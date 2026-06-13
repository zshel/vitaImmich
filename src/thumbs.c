/* vitaImmich unity build — part of one translation unit.
 * This file is #included by src/main.c (in order) and shares all
 * file-scope state with the other parts; it is NOT compiled on its own.
 * Only src/main.c is listed in CMakeLists.txt. Editors/clangd may flag
 * undefined symbols here because of that — those are expected.
 */

/* ------------------------------------------------------------------ */
/* background thumbnail loader                                         */
/*                                                                     */
/* One worker thread with a single request slot. The worker downloads  */
/* (reusing one curl handle, so the HTTP connection stays alive) and   */
/* decodes JPEGs to a plain pixel buffer; the main thread turns the    */
/* result into a GXM texture. Handoff is a tiny state machine:         */
/* IDLE -> (main writes request) PENDING -> (worker writes result)     */
/* DONE -> (main consumes) IDLE.                                       */
/* ------------------------------------------------------------------ */

enum { REQ_IDLE, REQ_PENDING, REQ_DONE };

static volatile int g_req_state = REQ_IDLE;
static volatile int g_req_idx = -1;
static volatile int g_req_src = SRC_SERVER; /* SRC_SERVER or SRC_LOCAL */
static volatile int g_req_detail;  /* full-res decode for the detail view */
/* the request payload, copied here by the main thread before REQ_PENDING so
 * the worker never indexes the growable per-asset / per-local arrays (which
 * the main thread may realloc while a request is in flight). */
static char g_req_id[40];          /* server asset id  */
static char g_req_path[256];       /* local file path  */
static int g_req_is_video;
static char g_req_err[160];        /* worker -> main error text */
static unsigned char *g_req_pix;   /* decoded pixels, or NULL on failure */
static int g_req_w, g_req_h, g_req_comps;
static char *g_req_raw;            /* raw body when it's a PNG */
static size_t g_req_raw_size;

/* SceAvPlayer + the hardware video decoder must never run two instances at
 * once. g_player_active is owned by play_video_file (main thread); the worker
 * sets g_poster_active around video-poster extraction. The two sides use a
 * fixed lock order: each sets its own flag, then tests the other's (the worker
 * backs off if the player won; play_video_file only ever waits, bounded). The
 * one that set its flag first is seen by the other, so a mutual wait — main
 * blocked on the worker while the worker is blocked on main — cannot occur. */
static volatile int g_player_active;
static volatile int g_poster_active;

/* extract the first frame of a local MP4 as a packed RGB888 thumbnail;
 * defined down in the video section, worker-thread safe (no vita2d calls) */
static unsigned char *extract_video_poster(const char *path, int *w, int *h,
					   char *err, size_t errlen);

static int worker_thread(SceSize args, void *argp)
{
	CURL *curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "vitaImmich/0.1 (PS Vita)");
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	if (g_resolve_list)
		curl_easy_setopt(curl, CURLOPT_RESOLVE, g_resolve_list);

	for (;;) {
		if (g_req_state != REQ_PENDING) {
			sceKernelDelayThread(2000);
			continue;
		}
		/* the request is fully described by the g_req_* buffers the main
		 * thread filled before REQ_PENDING; never touch the growable
		 * arrays here (they may move under us). */
		int src = g_req_src;
		char *err = g_req_err;
		const size_t errlen = sizeof(g_req_err);
		err[0] = '\0';
		const char *label = (src == SRC_LOCAL) ? g_req_path : g_req_id;
		g_req_pix = NULL;
		g_req_raw = NULL;

		/* local videos have no JPEG to read: decode a poster frame
		 * instead. guard against a concurrently playing video — set
		 * our flag first, then yield while the player holds the
		 * decoder (lock order mirrors play_video_file, see above). */
		if (src == SRC_LOCAL && g_req_is_video) {
			g_poster_active = 1;
			__sync_synchronize();
			while (g_player_active) {
				g_poster_active = 0;
				__sync_synchronize();
				sceKernelDelayThread(50 * 1000);
				g_poster_active = 1;
				__sync_synchronize();
			}
			g_req_pix = extract_video_poster(g_req_path,
							 &g_req_w, &g_req_h,
							 err, errlen);
			if (g_req_pix)
				g_req_comps = 3;
			g_poster_active = 0;
			__sync_synchronize();
			if (!g_req_pix)
				log_line("thumb %s: %s", label, err);
			__sync_synchronize();
			g_req_state = REQ_DONE;
			continue;
		}

		membuf buf = { NULL, 0 };
		long code = 0;
		CURLcode res = CURLE_OK;

		if (src == SRC_LOCAL) {
			/* read the camera JPEG/PNG off the memory card */
			FILE *lf = fopen(g_req_path, "rb");
			if (!lf) {
				snprintf(err, errlen, "open failed");
			} else {
				fseek(lf, 0, SEEK_END);
				long sz = ftell(lf);
				fseek(lf, 0, SEEK_SET);
				if (sz > 0) {
					buf.data = malloc(sz + 1);
					if (buf.data) {
						buf.size = fread(buf.data, 1, sz, lf);
						buf.data[buf.size] = '\0';
					}
				}
				fclose(lf);
				if (!buf.data)
					snprintf(err, errlen, "read failed (%ld bytes)", sz);
			}
		} else {
			char url[700];
			snprintf(url, sizeof(url),
				 "%s/api/assets/%s/thumbnail?size=preview",
				 g_server, g_req_id);
			curl_easy_setopt(curl, CURLOPT_URL, url);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
			/* build auth per request so an account switch (new token)
			 * takes effect immediately */
			char keyhdr[300];
			auth_header(keyhdr, sizeof(keyhdr));
			struct curl_slist *hdrs = curl_slist_append(NULL, keyhdr);
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
			curl_easy_setopt(curl, CURLOPT_RESOLVE, g_resolve_list);
			net_lock(); /* TLS is single-threaded, see g_net_mutex */
			res = curl_easy_perform(curl);
			net_unlock();
			if (res == CURLE_OK)
				curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
			curl_slist_free_all(hdrs);
		}

		if (src == SRC_SERVER && res != CURLE_OK) {
			snprintf(err, errlen, "curl error %d: %s",
				 res, curl_easy_strerror(res));
		} else if (src == SRC_SERVER && (code < 200 || code >= 300)) {
			snprintf(err, errlen, "HTTP %ld: %.100s", code,
				 buf.data ? buf.data : "(empty body)");
		} else if (buf.size == 0) {
			if (!err[0])
				snprintf(err, errlen, "empty body");
		} else {
			const unsigned char *p = (const unsigned char *)buf.data;
			if (buf.size > 2 && p[0] == 0xff && p[1] == 0xd8) {
				g_req_pix = decode_jpeg_buf(buf.data, buf.size,
							    g_req_detail ?
								FULL_MAX :
								THUMB_MAX,
							    &g_req_w, &g_req_h,
							    &g_req_comps,
							    err, errlen);
			} else if (buf.size > 8 && !memcmp(p, "\x89PNG", 4)) {
				/* hand the raw bytes to the main thread,
				 * vita2d's PNG loader isn't used off-thread */
				g_req_raw = buf.data;
				g_req_raw_size = buf.size;
				buf.data = NULL;
			} else if (buf.size > 12 && !memcmp(p, "RIFF", 4) &&
				   !memcmp(p + 8, "WEBP", 4)) {
				snprintf(err, errlen, "got WebP (unsupported); %u bytes",
					 (unsigned)buf.size);
			} else {
				snprintf(err, errlen,
					 "unknown format; %u bytes, magic %02x %02x %02x %02x",
					 (unsigned)buf.size, p[0], p[1],
					 buf.size > 2 ? p[2] : 0,
					 buf.size > 3 ? p[3] : 0);
			}
		}
		free(buf.data);
		if (!g_req_pix && !g_req_raw)
			log_line("thumb %s: %s", label, err);

		__sync_synchronize();
		g_req_state = REQ_DONE;
	}
	return 0;
}

/* build a texture from the worker's decoded result; `pooled` reuses the
 * thumbnail recycle pool, the detail view creates/frees its own */
static vita2d_texture *req_build_texture(char *err, size_t errlen, int pooled)
{
	vita2d_texture *tex = NULL;

	if (g_req_pix) {
		SceGxmTextureFormat fmt = (g_req_comps == 1) ?
			SCE_GXM_TEXTURE_FORMAT_U8_R111 :
			SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR;
		tex = pooled ? tex_acquire(g_req_w, g_req_h, fmt) :
		      vita2d_create_empty_texture_format(g_req_w, g_req_h, fmt);
		if (tex) {
			unsigned char *dst = vita2d_texture_get_datap(tex);
			unsigned int stride = vita2d_texture_get_stride(tex);
			unsigned int rowbytes = g_req_w * g_req_comps;
			for (int y = 0; y < g_req_h; y++)
				memcpy(dst + y * stride,
				       g_req_pix + y * rowbytes, rowbytes);
		} else {
			snprintf(err, errlen,
				 "texture alloc failed (%dx%d)", g_req_w, g_req_h);
		}
		free(g_req_pix);
		g_req_pix = NULL;
	} else if (g_req_raw) {
		tex = vita2d_load_PNG_buffer(g_req_raw);
		if (!tex)
			snprintf(err, errlen, "PNG decode failed; %u bytes",
				 (unsigned)g_req_raw_size);
		free(g_req_raw);
		g_req_raw = NULL;
	}
	/* bilinear, not nearest: thumbs get blown up in the detail view and
	 * full-res photos get scaled down to the screen */
	if (tex)
		vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR,
					   SCE_GXM_TEXTURE_FILTER_LINEAR);
	return tex;
}

/* main-thread side: turn a finished worker result into a grid thumbnail */
static void consume_worker_result(void)
{
	if (g_req_detail) {
		/* a detail-view load whose result nobody wants anymore (the
		 * user backed out to the grid mid-load): discard it */
		free(g_req_pix);
		g_req_pix = NULL;
		free(g_req_raw);
		g_req_raw = NULL;
		g_req_state = REQ_IDLE;
		return;
	}

	int idx = g_req_idx;
	int src = g_req_src;
	char errbuf[64];
	char *err = (src == SRC_LOCAL) ? errbuf : g_tex_err[idx];
	size_t errlen = (src == SRC_LOCAL) ? sizeof(errbuf) :
		    sizeof(g_tex_err[0]);
	/* carry the worker's error text across (it wrote into g_req_err so it
	 * never touched the growable arrays); main-side failures below overwrite */
	snprintf(err, errlen, "%s", g_req_err);
	vita2d_texture *tex = req_build_texture(err, errlen, 1);

	if (src == SRC_LOCAL) {
		g_local_thumb[idx] = tex;
		g_local_thumb_failed[idx] = (tex == NULL);
	} else {
		g_thumb[idx] = tex;
		g_thumb_failed[idx] = (tex == NULL);
	}
	g_req_state = REQ_IDLE;
}

/* main-thread side: turn a finished detail-view load into its texture */
static vita2d_texture *consume_detail_result(void)
{
	int src = g_req_src;
	char errbuf[160];
	char *err = (src == SRC_LOCAL) ? errbuf : g_tex_err[g_req_idx];
	size_t errlen = (src == SRC_LOCAL) ? sizeof(errbuf) :
		    sizeof(g_tex_err[0]);
	snprintf(err, errlen, "%s", g_req_err);
	vita2d_texture *tex = req_build_texture(err, errlen, 0);
	if (!tex)
		log_line("detail %s: %s",
			 src == SRC_LOCAL ? g_req_path : g_req_id, err);
	g_req_state = REQ_IDLE;
	return tex;
}

/* ------------------------------------------------------------------ */
/* texture recycling pool (see g_texpool above)                        */
/* ------------------------------------------------------------------ */

/* release a texture into the pool; never vita2d_free_texture mid-run */
static void tex_release(vita2d_texture *tex)
{
	if (!tex)
		return;
	if (g_texpool_n < TEXPOOL_MAX) {
		g_texpool[g_texpool_n++] = tex;
		return;
	}
	/* pool full: freeing is the crash hazard, so prefer leaking the
	 * oldest entry's slot over freeing. Shouldn't happen in practice
	 * (steady-state live+pooled thumbs stay well under the cap). */
	log_line("texpool full, leaking a texture");
}

/* get a texture of exactly w x h in `fmt`, reusing a pooled one if any */
static vita2d_texture *tex_acquire(int w, int h, SceGxmTextureFormat fmt)
{
	for (int i = 0; i < g_texpool_n; i++) {
		vita2d_texture *t = g_texpool[i];
		if ((int)vita2d_texture_get_width(t) == w &&
		    (int)vita2d_texture_get_height(t) == h &&
		    vita2d_texture_get_format(t) == fmt) {
			g_texpool[i] = g_texpool[--g_texpool_n];
			return t;
		}
	}
	return vita2d_create_empty_texture_format(w, h, fmt);
}

