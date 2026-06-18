/* vitaImmich unity build — part of one translation unit.
 * This file is #included by src/main.c (in order) and shares all
 * file-scope state with the other parts; it is NOT compiled on its own.
 * Only src/main.c is listed in CMakeLists.txt. Editors/clangd may flag
 * undefined symbols here because of that — those are expected.
 */

/* ------------------------------------------------------------------ */
/* immich API                                                          */
/* ------------------------------------------------------------------ */

/* append assets from a search/metadata (or search/random) response */
static int parse_assets(const char *js, size_t jslen, char *err, size_t errlen)
{
	jsmn_parser parser;
	jsmn_init(&parser);
	int ntok = jsmn_parse(&parser, js, jslen, NULL, 0);
	if (ntok <= 0) {
		snprintf(err, errlen, "JSON parse error (%d)", ntok);
		return -1;
	}

	jsmntok_t *tok = malloc(sizeof(jsmntok_t) * ntok);
	if (!tok) {
		snprintf(err, errlen, "out of memory (%d tokens)", ntok);
		return -1;
	}
	jsmn_init(&parser);
	ntok = jsmn_parse(&parser, js, jslen, tok, ntok);
	if (ntok <= 0) {
		free(tok);
		snprintf(err, errlen, "JSON parse error (%d)", ntok);
		return -1;
	}

	/* locate the asset array: root itself, or assets.items */
	int arr_idx = -1;
	if (tok[0].type == JSMN_ARRAY) {
		arr_idx = 0;
	} else {
		int assets_obj = -1;
		for (int i = 1; i < ntok - 1; i++)
			if (tok[i].type == JSMN_STRING && tok[i].size == 1 &&
			    tok[i].parent == 0 &&
			    tok[i].end - tok[i].start == 6 &&
			    !strncmp(js + tok[i].start, "assets", 6) &&
			    tok[i + 1].type == JSMN_OBJECT) {
				assets_obj = i + 1;
				break;
			}
		for (int i = assets_obj + 1; assets_obj > 0 && i < ntok - 1; i++)
			if (tok[i].type == JSMN_STRING && tok[i].size == 1 &&
			    tok[i].parent == assets_obj &&
			    tok[i].end - tok[i].start == 5 &&
			    !strncmp(js + tok[i].start, "items", 5) &&
			    tok[i + 1].type == JSMN_ARRAY) {
				arr_idx = i + 1;
				break;
			}
	}
	if (arr_idx < 0) {
		free(tok);
		snprintf(err, errlen, "unexpected response shape");
		return -1;
	}

	/* walk tokens in document order; keys whose parent is the current
	 * top-level object belong to that asset (exifInfo is the one nested
	 * object we descend into, for the photo's display aspect ratio) */
	int added = 0;
	int cur = -1, cur_obj = -1, exif_obj = -1;
	int exw = 0, exh = 0, exori = 0;
	for (int i = arr_idx + 1; i < ntok; i++) {
		if (tok[i].type == JSMN_OBJECT && tok[i].parent == arr_idx) {
			if (cur >= 0 && exw > 0 && exh > 0)
				g_asset_ratio[cur] = (exori >= 5 && exori <= 8) ?
					(float)exh / exw : (float)exw / exh;
			if (cur >= 0)
				g_asset_rot[cur] = (unsigned char)exori;
			if (!grow_assets(g_asset_count + 1))
				break; /* out of memory: keep what we have */
			cur = g_asset_count++;
			cur_obj = i;
			exif_obj = -1;
			exw = exh = exori = 0;
			added++;
			g_asset_ids[cur][0] = '\0';
			g_asset_dates[cur][0] = '\0';
			g_asset_ratio[cur] = 0.0f;
			g_asset_rot[cur] = 0;
			g_asset_is_video[cur] = 0;
			g_asset_hidden[cur] = 0;
			continue;
		}
		if (cur < 0 || tok[i].type != JSMN_STRING || tok[i].size != 1 ||
		    i + 1 >= ntok)
			continue;

		int klen = tok[i].end - tok[i].start;
		const char *k = js + tok[i].start;
		jsmntok_t *val = &tok[i + 1];
		int vlen = val->end - val->start;

		if (tok[i].parent == exif_obj) {
			/* numbers may also arrive as strings; atoi covers both
			 * and yields 0 on null */
			if (klen == 14 && !strncmp(k, "exifImageWidth", 14))
				exw = atoi(js + val->start);
			else if (klen == 15 && !strncmp(k, "exifImageHeight", 15))
				exh = atoi(js + val->start);
			else if (klen == 11 && !strncmp(k, "orientation", 11))
				exori = atoi(js + val->start);
			continue;
		}
		if (tok[i].parent != cur_obj)
			continue;

		if (klen == 8 && !strncmp(k, "exifInfo", 8) &&
		    val->type == JSMN_OBJECT) {
			exif_obj = i + 1;
			continue;
		}
		if (val->type != JSMN_STRING)
			continue;

		if (klen == 2 && !strncmp(k, "id", 2) &&
		    vlen > 0 && vlen < (int)sizeof(g_asset_ids[0]))
			snprintf(g_asset_ids[cur], sizeof(g_asset_ids[0]),
				 "%.*s", vlen, js + val->start);
		else if (klen == 13 && !strncmp(k, "fileCreatedAt", 13) && vlen >= 10)
			/* keep "YYYY-MM-DDTHH:MM:SS" (or just the date) so the
			 * merged grid can be sorted against local files */
			snprintf(g_asset_dates[cur], sizeof(g_asset_dates[0]),
				 "%.*s", vlen < DATELEN - 1 ? vlen : DATELEN - 1,
				 js + val->start);
		else if (klen == 4 && !strncmp(k, "type", 4))
			g_asset_is_video[cur] =
				(vlen == 5 && !strncmp(js + val->start, "VIDEO", 5));
	}
	if (cur >= 0 && exw > 0 && exh > 0)
		g_asset_ratio[cur] = (exori >= 5 && exori <= 8) ?
			(float)exh / exw : (float)exw / exh;
	if (cur >= 0)
		g_asset_rot[cur] = (unsigned char)exori;
	free(tok);

	/* drop trailing entries that somehow lack an id */
	while (g_asset_count > 0 && g_asset_ids[g_asset_count - 1][0] == '\0') {
		g_asset_count--;
		added--;
	}
	return added;
}

/* fetch the next page of the library, newest first; returns assets added */
/* compact away just-parsed entries [before, g_asset_count) whose id already
 * exists below `before` (the server's pages shift when assets are added or
 * removed, so refetches can overlap). order among the new tail doesn't
 * matter — the display rebuild sorts by date — and none of these have
 * textures yet. returns how many genuinely new entries remain. */
static int dedup_new_assets(int before)
{
	int n = g_asset_count;
	for (int i = before; i < n; ) {
		int dup = 0;
		for (int j = 0; j < before && !dup; j++)
			dup = !strcmp(g_asset_ids[j], g_asset_ids[i]);
		if (!dup) {
			i++;
			continue;
		}
		n--;
		if (i != n) {
			memcpy(g_asset_ids[i], g_asset_ids[n],
			       sizeof(g_asset_ids[0]));
			memcpy(g_asset_dates[i], g_asset_dates[n],
			       sizeof(g_asset_dates[0]));
			g_asset_ratio[i] = g_asset_ratio[n];
			g_asset_rot[i] = g_asset_rot[n];
			g_asset_is_video[i] = g_asset_is_video[n];
		}
	}
	g_asset_count = n;
	return n - before;
}

static int fetch_page(int is_first)
{
	if (g_next_page <= 0)
		return 0;

	char url[600];
	snprintf(url, sizeof(url), "%s/api/search/metadata", g_server);
	char body[128];
	snprintf(body, sizeof(body),
		 "{\"page\":%d,\"size\":%d,\"order\":\"desc\",\"withExif\":true}",
		 g_next_page, PAGE_SIZE);

	membuf buf;
	long code;
	CURLcode res = http_request(url, body, &buf, &code, NULL, 0);
	if (res != CURLE_OK) {
		if (is_first) {
			char det[700];
			snprintf(det, sizeof(det), "%s -- URL: %s",
				 curl_easy_strerror(res), url);
			fatal_error(det, "Connection failed (curl error %d)", res);
		}
		log_line("page %d: curl error %d", g_next_page, res);
		g_next_page = 0;
		free(buf.data);
		return 0;
	}
	if (code < 200 || code >= 300) {
		if (is_first)
			fatal_error(buf.data, "Server returned HTTP %ld", code);
		log_line("page %d: HTTP %ld: %.200s", g_next_page, code,
			 buf.data ? buf.data : "");
		g_next_page = 0;
		free(buf.data);
		return 0;
	}

	char err[160];
	int before = g_asset_count;
	int added = parse_assets(buf.data, buf.size, err, sizeof(err));
	if (added < 0) {
		if (is_first)
			fatal_error(buf.data, "Could not parse server response: %s", err);
		log_line("page %d: %s", g_next_page, err);
		g_next_page = 0;
		free(buf.data);
		return 0;
	}
	free(buf.data);

	/* a short (raw) page means we reached the end of the library; the
	 * dedup below only filters overlap from server-side page drift */
	g_next_page = (added == PAGE_SIZE) ? g_next_page + 1 : 0;
	int fresh = dedup_new_assets(before);
	int with_ratio = 0;
	for (int i = g_asset_count - fresh; i < g_asset_count; i++)
		with_ratio += (g_asset_ratio[i] > 0.0f);
	log_line("page fetched: +%d assets (total %d, %d with exif ratio)",
		 fresh, g_asset_count, with_ratio);
	return fresh;
}

/* poll the newest page for photos added to the server since we fetched;
 * duplicates of already-known assets are dropped, so only genuinely new
 * items remain appended. main-thread only (mutates the asset arrays).
 * returns the number of new assets. */
static int check_new_assets(void)
{
	char url[600];
	snprintf(url, sizeof(url), "%s/api/search/metadata", g_server);
	char body[160];
	snprintf(body, sizeof(body),
		 "{\"page\":1,\"size\":%d,\"order\":\"desc\",\"withExif\":true}",
		 PAGE_SIZE);

	membuf buf;
	long code;
	CURLcode res = http_request(url, body, &buf, &code, NULL, 0);
	if (res != CURLE_OK || code < 200 || code >= 300) {
		free(buf.data);
		return 0;
	}

	char err[160];
	int before = g_asset_count;
	int added = parse_assets(buf.data, buf.size, err, sizeof(err));
	free(buf.data);
	if (added <= 0)
		return 0;

	int fresh = dedup_new_assets(before);
	if (fresh > 0)
		log_line("poll: %d new asset(s) on the server", fresh);
	return fresh;
}

/* parse a single /api/assets/{id} object for the fields the grid needs to lay
 * an item out: capture date, display aspect (from exif), orientation, video
 * flag. returns 1 if the date was found. (parse_assets handles arrays; this is
 * the one-object form used by the gallery's lazy metadata resolver.) */
static int parse_one_asset(const char *js, size_t len, char *date, size_t datelen,
			   float *ratio, unsigned char *rot, int *is_video)
{
	jsmn_parser p;
	jsmn_init(&p);
	int n = jsmn_parse(&p, js, len, NULL, 0);
	if (n <= 0)
		return 0;
	jsmntok_t *t = malloc(sizeof(*t) * n);
	if (!t)
		return 0;
	jsmn_init(&p);
	n = jsmn_parse(&p, js, len, t, n);
	if (n <= 0 || t[0].type != JSMN_OBJECT) {
		free(t);
		return 0;
	}
	date[0] = '\0';
	*ratio = 0.0f;
	*rot = 0;
	*is_video = 0;
	int exif_obj = -1, exw = 0, exh = 0, exori = 0, got = 0;
	for (int i = 1; i + 1 < n; i++) {
		if (t[i].type != JSMN_STRING || t[i].size != 1)
			continue;
		int kl = t[i].end - t[i].start;
		const char *k = js + t[i].start;
		jsmntok_t *v = &t[i + 1];
		int vl = v->end - v->start;
		if (t[i].parent == 0 && kl == 8 && !strncmp(k, "exifInfo", 8) &&
		    v->type == JSMN_OBJECT) {
			exif_obj = i + 1;
			continue;
		}
		if (t[i].parent == exif_obj) {
			if (kl == 14 && !strncmp(k, "exifImageWidth", 14))
				exw = atoi(js + v->start);
			else if (kl == 15 && !strncmp(k, "exifImageHeight", 15))
				exh = atoi(js + v->start);
			else if (kl == 11 && !strncmp(k, "orientation", 11))
				exori = atoi(js + v->start);
			continue;
		}
		if (t[i].parent != 0)
			continue;
		if (kl == 13 && !strncmp(k, "fileCreatedAt", 13) && vl >= 10) {
			int cl = vl < (int)datelen - 1 ? vl : (int)datelen - 1;
			memcpy(date, js + v->start, cl);
			date[cl] = '\0';
			got = 1;
		} else if (kl == 4 && !strncmp(k, "type", 4)) {
			*is_video = (vl >= 5 && !strncmp(js + v->start, "VIDEO", 5));
		}
	}
	if (exw > 0 && exh > 0)
		*ratio = (exori >= 5 && exori <= 8) ?
			(float)exh / exw : (float)exw / exh;
	*rot = (unsigned char)exori;
	free(t);
	return got;
}

/* background metadata resolver thread: fetches one asset's date/aspect at a
 * time (the gallery's lazy fill). One-slot handoff like the thumb worker —
 * main writes g_meta_id + flips to PENDING, we fetch+parse and flip to DONE. */
static int meta_worker(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	for (;;) {
		if (g_meta_state != META_PENDING) {
			sceKernelDelayThread(4000);
			continue;
		}
		char url[600];
		snprintf(url, sizeof(url), "%s/api/assets/%s", g_server, g_meta_id);
		membuf buf;
		long code;
		CURLcode r = http_request(url, NULL, &buf, &code, NULL, 0);
		g_meta_date[0] = '\0';
		g_meta_ratio = 0.0f;
		g_meta_rot = 0;
		g_meta_is_video = 0;
		if (r == CURLE_OK && code >= 200 && code < 300 && buf.data)
			parse_one_asset(buf.data, buf.size, g_meta_date,
					sizeof(g_meta_date), &g_meta_ratio,
					&g_meta_rot, &g_meta_is_video);
		free(buf.data);
		__sync_synchronize();
		g_meta_state = META_DONE;
	}
	return 0;
}


