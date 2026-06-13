/* vitaImmich unity build — part of one translation unit.
 * This file is #included by src/main.c (in order) and shares all
 * file-scope state with the other parts; it is NOT compiled on its own.
 * Only src/main.c is listed in CMakeLists.txt. Editors/clangd may flag
 * undefined symbols here because of that — those are expected.
 */

/* ------------------------------------------------------------------ */
/* local camera media: scan, hash, check, upload                        */
/*                                                                      */
/* The scan (paths + stat only) runs on the main thread at startup; the */
/* slow work — SHA1 hashing, the bulk dup-check and uploads — runs on a */
/* second background thread so the UI never blocks. Item state lives in */
/* the volatile g_local_state[] machine; the sync thread is the only    */
/* writer except for the QUEUED handoff (main thread, guarded).         */
/* ------------------------------------------------------------------ */

/* build "YYYY-MM-DDTHH:MM:SS" (sortable) from a SceDateTime. fields are
 * masked to their printed width so the output can never overrun out[] */
static void datetime_sortable(const SceDateTime *t, char *out, size_t len)
{
	snprintf(out, len, "%04d-%02d-%02dT%02d:%02d:%02d",
		 t->year % 10000, t->month % 100, t->day % 100,
		 t->hour % 100, t->minute % 100, t->second % 100);
}

/* build ISO8601 with milliseconds + Z, as Immich's API expects */
static void datetime_iso(const SceDateTime *t, char *out, size_t len)
{
	snprintf(out, len, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
		 t->year % 10000, t->month % 100, t->day % 100,
		 t->hour % 100, t->minute % 100, t->second % 100);
}

static int ext_is(const char *name, const char *ext)
{
	size_t n = strlen(name), e = strlen(ext);
	return n > e && !strcasecmp(name + n - e, ext);
}

static int is_media_name(const char *name, int *is_video)
{
	if (ext_is(name, ".jpg") || ext_is(name, ".jpeg") || ext_is(name, ".png")) {
		*is_video = 0;
		return 1;
	}
	if (ext_is(name, ".mp4")) {
		*is_video = 1;
		return 1;
	}
	return 0;
}

/* set while re-scanning after a resume from sleep: scan_dir then only appends
 * genuinely-new files into the headroom (no realloc; the worker/sync threads
 * are reading the arrays) */
static int g_rescan;

/* recurse a directory (up to depth levels) collecting camera media */
static void scan_dir(const char *path, int depth)
{
	SceUID dfd = sceIoDopen(path);
	if (dfd < 0) {
		/* a safe (sandboxed) self gets an error here; the vpk must be
		 * built UNSAFE and unsafe homebrew enabled in HENkaku settings */
		log_line("scan: cannot open %s: 0x%08x", path, dfd);
		return;
	}

	SceIoDirent ent;
	memset(&ent, 0, sizeof(ent));
	while (sceIoDread(dfd, &ent) > 0) {
		if (ent.d_name[0] == '.')
			continue;

		/* build "path/name" by hand (bounded, no snprintf %s
		 * truncation warning); skip anything too long to store */
		size_t pl = strlen(path), nl = strlen(ent.d_name);
		if (pl + 1 + nl + 1 > sizeof(g_local_path[0]))
			continue;
		char full[sizeof(g_local_path[0])];
		memcpy(full, path, pl);
		full[pl] = '/';
		memcpy(full + pl + 1, ent.d_name, nl + 1);

		/* some IO drivers report directory-ness only in st_attr */
		if (SCE_S_ISDIR(ent.d_stat.st_mode) ||
		    SCE_SO_ISDIR(ent.d_stat.st_attr)) {
			if (depth > 0)
				scan_dir(full, depth - 1);
			continue;
		}

		int is_video;
		if (!is_media_name(ent.d_name, &is_video))
			continue;
		if (!strcmp(full, VIDEO_TMP_PATH)) /* our own temp file */
			continue;
		if (g_syncmax_mb > 0 &&
		    (long long)ent.d_stat.st_size > g_syncmax_mb * 1024LL * 1024LL) {
			log_line("scan: skip %s (%lld MB > syncmaxmb=%ld)", full,
				 (long long)ent.d_stat.st_size >> 20, g_syncmax_mb);
			continue;
		}

		/* rescan (resume from sleep): skip files we already know, and
		 * never realloc while the sync/worker threads are reading the
		 * arrays — only fill into the headroom reserved at startup */
		if (g_rescan) {
			int known = 0;
			for (int k = 0; k < g_local_count && !known; k++)
				known = !strcmp(g_local_path[k], full);
			if (known)
				continue;
			if (g_local_count >= g_local_cap) {
				log_line("rescan: no headroom at %d files",
					 g_local_count);
				break;
			}
		} else if (!grow_locals(g_local_count + 1)) {
			log_line("scan: out of memory at %d local files",
				 g_local_count);
			break;
		}
		int j = g_local_count;   /* fill, then publish (count++) */
		memcpy(g_local_path[j], full, pl + 1 + nl + 1);
		g_local_is_video[j] = is_video;
		g_local_size[j] = (long long)ent.d_stat.st_size;
		g_local_mtime[j] = ent.d_stat.st_mtime;
		g_local_ctime[j] = ent.d_stat.st_ctime;
		datetime_sortable(&ent.d_stat.st_mtime, g_local_date[j],
				  sizeof(g_local_date[j]));
		g_local_sha1[j][0] = '\0';
		g_local_server_id[j][0] = '\0';
		g_local_err[j][0] = '\0';
		g_local_state[j] = SYNC_UNSCANNED;
		__sync_synchronize();
		g_local_count = j + 1;
	}
	sceIoDclose(dfd);
	log_line("scan: %s: %d media so far", path, g_local_count);
}

/* scan the Vita's camera folders (main thread, fast: paths + stat only) */
static int g_photo0_mounted;

static void scan_local_media(void)
{
	g_local_count = 0;
	if (g_syncdir_count > 0) {
		for (int i = 0; i < g_syncdir_count; i++)
			scan_dir(g_syncdirs[i], 3);
	} else {
		/* ux0:picture is ACL-protected (EPERM) even for unsafe
		 * homebrew; the photo0: appdata mount is the sanctioned way
		 * in. Camera videos live there too (the Photos app owns
		 * them), so all of ux0:video — the movie collection — is
		 * left alone. */
		int mres = sceAppMgrAppDataMount(100, "photo0:");
		g_photo0_mounted = (mres == 0);
		log_line("scan: photo0 mount: 0x%08x", mres);
		scan_dir(g_photo0_mounted ? "photo0:" : "ux0:picture", 3);
		scan_dir("ux0:video/CAMERA", 3);
	}
	log_line("local scan: %d files", g_local_count);
}

/* append-only re-scan of the camera folders (resume from sleep). same dirs as
 * scan_local_media, but g_rescan keeps it from touching known entries or
 * reallocating. returns how many new files were appended. */
static int rescan_local_new(void)
{
	int before = g_local_count;
	g_rescan = 1;
	if (g_syncdir_count > 0) {
		for (int i = 0; i < g_syncdir_count; i++)
			scan_dir(g_syncdirs[i], 3);
	} else {
		scan_dir(g_photo0_mounted ? "photo0:" : "ux0:picture", 3);
		scan_dir("ux0:video/CAMERA", 3);
	}
	g_rescan = 0;
	int added = g_local_count - before;
	if (added > 0)
		log_line("rescan: %d new local file(s) after resume", added);
	return added;
}

static int sha1_file_hex(const char *path, char out[41])
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	SHA_CTX c;
	SHA1_Init(&c);
	static unsigned char buf[64 * 1024]; /* big, keep off the stack */
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		SHA1_Update(&c, buf, n);
	int err = ferror(f);
	fclose(f);
	if (err)
		return -1;
	unsigned char d[20];
	SHA1_Final(d, &c);
	for (int i = 0; i < 20; i++)
		snprintf(out + 2 * i, 3, "%02x", d[i]);
	out[40] = '\0';
	return 0;
}

static void sync_push_err(const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	snprintf(g_sync_errlog[g_sync_errn % 5], 160, "%s", buf);
	g_sync_errn++;
	log_line("sync: %s", buf);
}

/* one curl handle reused by the sync thread (bulk-check + uploads) */
static CURL *g_sync_curl;
static struct curl_slist *g_sync_hdrs;

static void sync_curl_init(void)
{
	g_sync_curl = curl_easy_init();
	char keyhdr[300];
	auth_header(keyhdr, sizeof(keyhdr));
	g_sync_hdrs = curl_slist_append(NULL, keyhdr);
	curl_easy_setopt(g_sync_curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(g_sync_curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(g_sync_curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(g_sync_curl, CURLOPT_USERAGENT, "vitaImmich/0.1 (PS Vita)");
	curl_easy_setopt(g_sync_curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(g_sync_curl, CURLOPT_SSL_VERIFYHOST, 0L);
	if (g_resolve_list)
		curl_easy_setopt(g_sync_curl, CURLOPT_RESOLVE, g_resolve_list);
}

/* parse a bulk-upload-check response: for each result, look up the local
 * item by its id (we send the local index as the id) and set its state */
static void parse_bulk_check(const char *js, size_t jslen)
{
	jsmn_parser parser;
	jsmn_init(&parser);
	int ntok = jsmn_parse(&parser, js, jslen, NULL, 0);
	if (ntok <= 0)
		return;
	jsmntok_t *tok = malloc(sizeof(jsmntok_t) * ntok);
	if (!tok)
		return;
	jsmn_init(&parser);
	ntok = jsmn_parse(&parser, js, jslen, tok, ntok);
	if (ntok <= 0) {
		free(tok);
		return;
	}

	/* walk objects, collecting id/action/reason/assetId per result */
	int cur_obj = -1, li = -1;
	char action[16], reason[24], assetid[40];
	for (int i = 0; i <= ntok; i++) {
		if (i == ntok || tok[i].type == JSMN_OBJECT) {
			/* flush the previous object's result */
			if (li >= 0 && li < g_local_count) {
				if (!strcmp(action, "reject") &&
				    !strcmp(reason, "duplicate")) {
					snprintf(g_local_server_id[li], 40,
						 "%s", assetid);
					g_local_state[li] = SYNC_BACKED_UP;
				} else {
					/* accept, or a non-duplicate reject we
					 * can still try to upload */
					g_local_state[li] = SYNC_LOCAL_ONLY;
				}
				g_need_rebuild = 1;
			}
			if (i == ntok)
				break;
			cur_obj = i;
			li = -1;
			action[0] = reason[0] = assetid[0] = '\0';
			continue;
		}
		if (tok[i].type != JSMN_STRING || tok[i].size != 1 ||
		    tok[i].parent != cur_obj || i + 1 >= ntok)
			continue;
		int klen = tok[i].end - tok[i].start;
		const char *k = js + tok[i].start;
		jsmntok_t *v = &tok[i + 1];
		int vlen = v->end - v->start;
		const char *vs = js + v->start;
		if (klen == 2 && !strncmp(k, "id", 2))
			li = atoi(vs); /* we sent the local index */
		else if (klen == 6 && !strncmp(k, "action", 6))
			snprintf(action, sizeof(action), "%.*s", vlen, vs);
		else if (klen == 6 && !strncmp(k, "reason", 6))
			snprintf(reason, sizeof(reason), "%.*s", vlen, vs);
		else if (klen == 7 && !strncmp(k, "assetId", 7))
			snprintf(assetid, sizeof(assetid), "%.*s", vlen, vs);
	}
	free(tok);
}

/* bulk-upload-check a batch of CHECKING items (indices in idxs[0..n)) */
static void bulk_check_batch(const int *idxs, int n)
{
	/* {"assets":[{"id":"<localidx>","checksum":"<hex>"},...]} */
	size_t cap = 64 + n * 80;
	char *body = malloc(cap);
	if (!body)
		return;
	int off = snprintf(body, cap, "{\"assets\":[");
	for (int i = 0; i < n; i++)
		off += snprintf(body + off, cap - off,
				"%s{\"id\":\"%d\",\"checksum\":\"%s\"}",
				i ? "," : "", idxs[i], g_local_sha1[idxs[i]]);
	off += snprintf(body + off, cap - off, "]}");

	char url[600];
	snprintf(url, sizeof(url), "%s/api/assets/bulk-upload-check", g_server);

	struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
	char keyhdr[300];
	auth_header(keyhdr, sizeof(keyhdr));
	hdrs = curl_slist_append(hdrs, keyhdr);

	membuf buf = { NULL, 0 };
	curl_easy_setopt(g_sync_curl, CURLOPT_URL, url);
	curl_easy_setopt(g_sync_curl, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(g_sync_curl, CURLOPT_POST, 1L);
	curl_easy_setopt(g_sync_curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(g_sync_curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(g_sync_curl, CURLOPT_TIMEOUT, 60L);
	net_lock(); /* TLS is single-threaded, see g_net_mutex */
	CURLcode res = curl_easy_perform(g_sync_curl);
	net_unlock();
	long code = 0;
	if (res == CURLE_OK)
		curl_easy_getinfo(g_sync_curl, CURLINFO_RESPONSE_CODE, &code);

	if (res == CURLE_OK && code >= 200 && code < 300 && buf.data) {
		parse_bulk_check(buf.data, buf.size);
	} else {
		/* leave them LOCAL_ONLY so the user can still try to upload */
		for (int i = 0; i < n; i++)
			g_local_state[idxs[i]] = SYNC_LOCAL_ONLY;
		sync_push_err("dup-check failed (curl %d, HTTP %ld)", res, code);
	}
	g_need_rebuild = 1;

	/* reset the handle for reuse (clear POSTFIELDS / restore defaults) */
	curl_easy_setopt(g_sync_curl, CURLOPT_HTTPHEADER, NULL);
	curl_easy_setopt(g_sync_curl, CURLOPT_POSTFIELDS, NULL);
	curl_slist_free_all(hdrs);
	free(buf.data);
	free(body);
}

static int upload_xfer_cb(void *ud, curl_off_t dl, curl_off_t dln,
			  curl_off_t ult, curl_off_t uln)
{
	g_ul_total = ult;
	g_ul_now = uln;
	return 0;
}

/* upload one local item; returns 0 on success, -1 on failure (err set) */
static int upload_item(int j, char *err, size_t errlen)
{
	const char *path = g_local_path[j];
	const char *name = strrchr(path, '/');
	name = name ? name + 1 : path;

	char created[40], modified[40], devid[300];
	datetime_iso(&g_local_ctime[j], created, sizeof(created));
	datetime_iso(&g_local_mtime[j], modified, sizeof(modified));
	snprintf(devid, sizeof(devid), "%s-%lld", name, g_local_size[j]);

	char url[600];
	snprintf(url, sizeof(url), "%s/api/assets", g_server);

	char keyhdr[300], sumhdr[80];
	auth_header(keyhdr, sizeof(keyhdr));
	snprintf(sumhdr, sizeof(sumhdr), "x-immich-checksum: %s", g_local_sha1[j]);
	struct curl_slist *hdrs = curl_slist_append(NULL, keyhdr);
	hdrs = curl_slist_append(hdrs, sumhdr);
	hdrs = curl_slist_append(hdrs, "Accept: application/json");

	curl_mime *mime = curl_mime_init(g_sync_curl);
	curl_mimepart *part;
	part = curl_mime_addpart(mime);
	curl_mime_name(part, "assetData");
	curl_mime_filedata(part, path); /* streams from disk */
	curl_mime_filename(part, name);
	part = curl_mime_addpart(mime);
	curl_mime_name(part, "deviceAssetId");
	curl_mime_data(part, devid, CURL_ZERO_TERMINATED);
	part = curl_mime_addpart(mime);
	curl_mime_name(part, "deviceId");
	curl_mime_data(part, "PS Vita", CURL_ZERO_TERMINATED);
	part = curl_mime_addpart(mime);
	curl_mime_name(part, "fileCreatedAt");
	curl_mime_data(part, created, CURL_ZERO_TERMINATED);
	part = curl_mime_addpart(mime);
	curl_mime_name(part, "fileModifiedAt");
	curl_mime_data(part, modified, CURL_ZERO_TERMINATED);
	part = curl_mime_addpart(mime);
	curl_mime_name(part, "filename");
	curl_mime_data(part, name, CURL_ZERO_TERMINATED);

	membuf buf = { NULL, 0 };
	g_ul_now = g_ul_total = 0;
	curl_easy_setopt(g_sync_curl, CURLOPT_URL, url);
	curl_easy_setopt(g_sync_curl, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(g_sync_curl, CURLOPT_MIMEPOST, mime);
	curl_easy_setopt(g_sync_curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(g_sync_curl, CURLOPT_TIMEOUT, 0L); /* big files */
	curl_easy_setopt(g_sync_curl, CURLOPT_LOW_SPEED_LIMIT, 512L);
	curl_easy_setopt(g_sync_curl, CURLOPT_LOW_SPEED_TIME, 60L);
	curl_easy_setopt(g_sync_curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(g_sync_curl, CURLOPT_XFERINFOFUNCTION, upload_xfer_cb);

	net_lock(); /* TLS is single-threaded, see g_net_mutex */
	CURLcode res = curl_easy_perform(g_sync_curl);
	net_unlock();
	long code = 0;
	if (res == CURLE_OK)
		curl_easy_getinfo(g_sync_curl, CURLINFO_RESPONSE_CODE, &code);

	int rc = -1;
	if (res != CURLE_OK) {
		snprintf(err, errlen, "curl %d: %s", res, curl_easy_strerror(res));
	} else if (code != 200 && code != 201) {
		snprintf(err, errlen, "HTTP %ld: %.80s", code,
			 buf.data ? buf.data : "");
	} else {
		/* parse {"id":"...","status":"created|duplicate"} */
		const char *p = buf.data ? strstr(buf.data, "\"id\"") : NULL;
		if (p) {
			p = strchr(p + 4, '"');
			if (p) {
				p++;
				int k = 0;
				while (p[k] && p[k] != '"' && k < 39) {
					g_local_server_id[j][k] = p[k];
					k++;
				}
				g_local_server_id[j][k] = '\0';
			}
		}
		rc = 0;
	}

	curl_easy_setopt(g_sync_curl, CURLOPT_MIMEPOST, NULL);
	curl_easy_setopt(g_sync_curl, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(g_sync_curl, CURLOPT_HTTPHEADER, NULL);
	curl_mime_free(mime);
	curl_slist_free_all(hdrs);
	free(buf.data);
	return rc;
}

/* the sync worker: hash everything, dup-check in batches, then drain the
 * upload queue (items the user moved to QUEUED from the sync screen) */
static int sync_thread(SceSize args, void *argp)
{
	sync_curl_init();

	for (;;) {
		/* idle while the main thread resets for an account switch */
		if (g_pause_bg) {
			g_sync_idle = 1;
			sceKernelDelayThread(8 * 1000);
			continue;
		}
		g_sync_idle = 0;

		/* 1. hash + dup-check a batch of freshly scanned files */
		int batch[100], nb = 0;
		for (int j = 0; j < g_local_count && nb < 100; j++) {
			if (g_local_state[j] != SYNC_UNSCANNED)
				continue;
			const char *name = strrchr(g_local_path[j], '/');
			name = name ? name + 1 : g_local_path[j];
			g_sync_phase = 1;
			snprintf(g_sync_activity, sizeof(g_sync_activity),
				 "hashing %.120s", name);
			g_local_state[j] = SYNC_HASHING;
			if (sha1_file_hex(g_local_path[j], g_local_sha1[j]) != 0) {
				snprintf(g_local_err[j], sizeof(g_local_err[j]),
					 "hash failed");
				g_local_state[j] = SYNC_FAILED;
				sync_push_err("hash failed: %s", name);
				continue;
			}
			g_local_state[j] = SYNC_CHECKING;
			batch[nb++] = j;
		}
		if (nb > 0) {
			g_sync_phase = 2;
			snprintf(g_sync_activity, sizeof(g_sync_activity),
				 "checking %d files on server", nb);
			bulk_check_batch(batch, nb);
			continue; /* loop back for the next batch */
		}

		/* 2. drain the upload queue */
		int up = -1;
		for (int j = 0; j < g_local_count; j++)
			if (g_local_state[j] == SYNC_QUEUED) {
				up = j;
				break;
			}
		if (up >= 0) {
			const char *name = strrchr(g_local_path[up], '/');
			name = name ? name + 1 : g_local_path[up];
			g_local_state[up] = SYNC_UPLOADING;
			g_sync_phase = 3;
			snprintf(g_sync_activity, sizeof(g_sync_activity),
				 "uploading %.118s", name);
			char err[96];
			if (upload_item(up, err, sizeof(err)) == 0) {
				g_local_state[up] = SYNC_BACKED_UP;
			} else {
				snprintf(g_local_err[up], sizeof(g_local_err[up]),
					 "%s", err);
				g_local_state[up] = SYNC_FAILED;
				sync_push_err("upload %s: %s", name, err);
			}
			g_need_rebuild = 1;
			continue;
		}

		/* nothing to do */
		g_sync_phase = 0;
		g_sync_activity[0] = '\0';
		sceKernelDelayThread(50 * 1000);
	}
	return 0;
}

/* main thread: queue every LOCAL_ONLY item for upload (the QUEUED handoff,
 * the one transition the main thread is allowed to make) */
static int queue_all_uploads(void)
{
	int n = 0;
	for (int j = 0; j < g_local_count; j++)
		if (g_local_state[j] == SYNC_LOCAL_ONLY) {
			g_local_state[j] = SYNC_QUEUED;
			n++;
		}
	__sync_synchronize();
	return n;
}

static void load_autobackup(void)
{
	FILE *f = fopen(AUTOBK_PATH, "r");
	if (!f)
		return;
	int v = 0;
	if (fscanf(f, "%d", &v) == 1)
		g_autobackup = (v != 0);
	fclose(f);
}

static void save_autobackup(void)
{
	FILE *f = fopen(AUTOBK_PATH, "w");
	if (f) {
		fputc(g_autobackup ? '1' : '0', f);
		fclose(f);
	}
}


/* ------------------------------------------------------------------ */
/* grid status badges + sync overview                                  */
/* ------------------------------------------------------------------ */

/* small colour dot in the top-left corner of a grid cell; a backed-up
 * item also gets a tiny white check, a local-only item an up-arrow */
static void draw_status_badge(float bx, float by, float bw, float bh,
			      int d, unsigned int frame)
{
	struct disp_item *it = &g_disp[d];
	int kind; /* 0 none, 1 cloud, 2 local, 3 busy, 4 backed, 5 failed */

	if (it->src == SRC_SERVER) {
		kind = g_asset_local_backed[it->idx] ? 4 : 1;
	} else {
		switch (g_local_state[it->idx]) {
		case SYNC_LOCAL_ONLY: kind = 2; break;
		case SYNC_QUEUED:
		case SYNC_UPLOADING:  kind = 3; break;
		case SYNC_BACKED_UP:  kind = 4; break;
		case SYNC_FAILED:     kind = 5; break;
		default:              kind = 0; break; /* hashing/checking */
		}
	}
	if (kind == 0)
		return;

	/* Immich cloud status icons (the bundled SVGs): cloud = on server only,
	 * cloud-off = on device only, cloud-done = on both. White, bottom-right
	 * of the cell, no background; a 1px dark shadow keeps it legible. */
	vita2d_texture *ic = (kind == 4) ? g_ic_both :
			     (kind == 1) ? g_ic_server : g_ic_device;
	if (!ic)
		return;
	const float isz = 26.0f;
	float ix = bx + bw - isz - 4;    /* bottom-right corner */
	float iy = by + bh - isz - 4;
	float s = isz / vita2d_texture_get_width(ic);
	uint32_t fg = (kind == 5) ? RGBA8(235, 90, 90, 255)
				  : RGBA8(255, 255, 255, 255);
	vita2d_draw_texture_tint_scale(ic, ix + 1, iy + 1, s, s,
				       RGBA8(0, 0, 0, 140));   /* shadow */
	vita2d_draw_texture_tint_scale(ic, ix, iy, s, s, fg);
}

/* count local items in a given state */
static int count_state(int st)
{
	int n = 0;
	for (int j = 0; j < g_local_count; j++)
		if (g_local_state[j] == st)
			n++;
	return n;
}

