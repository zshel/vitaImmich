/* vitaImmich unity build — part of one translation unit.
 * This file is #included by src/main.c (in order) and shares all
 * file-scope state with the other parts; it is NOT compiled on its own.
 * Only src/main.c is listed in CMakeLists.txt. Editors/clangd may flag
 * undefined symbols here because of that — those are expected.
 */

/* ------------------------------------------------------------------ */
/* networking                                                          */
/* ------------------------------------------------------------------ */

static char g_net_mem[1024 * 1024];

/* OpenSSL 1.0.2 is not thread-safe unless the application installs locking
 * callbacks. Three threads here do concurrent HTTPS (main page fetches, the
 * thumbnail worker, the sync thread); without these locks, overlapping TLS
 * work corrupts OpenSSL's shared state and crashes deep inside the cipher
 * (seen as wild reads/writes in CRYPTO_gcm128_decrypt while fast-scrolling,
 * which is exactly when a page fetch overlaps thumbnail downloads). */
static SceUID *g_ssl_locks;

/* vitasdk's OpenSSL 1.0.2 is built without thread support (opensslconf.h
 * defines no OPENSSL_THREADS), so even with locking callbacks installed,
 * concurrent TLS on multiple threads corrupts its global state — crashes
 * surface deep in the cipher (CRYPTO_gcm128_decrypt) when a page fetch on
 * the main thread overlaps a thumbnail download on the worker. Serialize
 * every curl_easy_perform behind this mutex instead. */
static SceUID g_net_mutex = -1;

static void net_lock(void)
{
	if (g_net_mutex >= 0)
		sceKernelLockMutex(g_net_mutex, 1, NULL);
}

static void net_unlock(void)
{
	if (g_net_mutex >= 0)
		sceKernelUnlockMutex(g_net_mutex, 1);
}

static void ssl_lock_cb(int mode, int n, const char *file, int line)
{
	(void)file; (void)line;
	if (mode & CRYPTO_LOCK)
		sceKernelLockMutex(g_ssl_locks[n], 1, NULL);
	else
		sceKernelUnlockMutex(g_ssl_locks[n], 1);
}

static unsigned long ssl_thread_id_cb(void)
{
	return (unsigned long)sceKernelGetThreadId();
}

static void ssl_locks_init(void)
{
	int n = CRYPTO_num_locks();
	g_ssl_locks = malloc(n * sizeof(SceUID));
	if (!g_ssl_locks)
		return;
	for (int i = 0; i < n; i++) {
		char name[32];
		snprintf(name, sizeof(name), "ssl_lock_%d", i);
		g_ssl_locks[i] = sceKernelCreateMutex(name, 0, 0, NULL);
	}
	CRYPTO_set_id_callback(ssl_thread_id_cb);
	CRYPTO_set_locking_callback(ssl_lock_cb);
	log_line("openssl locking callbacks installed (%d locks)", n);
}

static void net_init(void)
{
	g_net_mutex = sceKernelCreateMutex("net_mutex", 0, 0, NULL);
	ssl_locks_init();
	sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	if (sceNetShowNetstat() == SCE_NET_ERROR_ENOTINIT) {
		SceNetInitParam param = {
			.memory = g_net_mem,
			.size   = sizeof(g_net_mem),
			.flags  = 0,
		};
		sceNetInit(&param);
	}
	sceNetCtlInit();
	curl_global_init(CURL_GLOBAL_ALL);
}

typedef struct {
	char *data;
	size_t size;
} membuf;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	membuf *mb = userdata;
	size_t add = size * nmemb;
	char *p = realloc(mb->data, mb->size + add + 1);
	if (!p)
		return 0;
	mb->data = p;
	memcpy(mb->data + mb->size, ptr, add);
	mb->size += add;
	mb->data[mb->size] = '\0';
	return add;
}

/* verbose-log the first request via curl's debug stream so connection
 * problems land in log.txt with curl's own explanation */
static int g_verbose_request = 1;

static int curl_debug_cb(CURL *h, curl_infotype type, char *data,
			 size_t size, void *ud)
{
	if (type == CURLINFO_TEXT && size > 0) {
		char line[300];
		if (data[size - 1] == '\n')
			size--;
		snprintf(line, sizeof(line), "%.*s", (int)size, data);
		log_line("curl: %s", line);
	}
	return 0;
}

/* write the right auth header: a login token (Bearer) wins over an API key.
 * used by the worker/sync curl handles that build their own header lists. */
static void auth_header(char *buf, size_t n)
{
	if (g_token[0])
		snprintf(buf, n, "Authorization: Bearer %s", g_token);
	else
		snprintf(buf, n, "x-api-key: %s", g_apikey);
}

/* body == NULL -> GET, otherwise POST with a JSON body.
 * ctype (optional) receives the response Content-Type. */
static CURLcode http_request(const char *url, const char *body,
			     membuf *out, long *http_code,
			     char *ctype, size_t ctype_len)
{
	out->data = NULL;
	out->size = 0;
	*http_code = 0;
	if (ctype)
		ctype[0] = '\0';

	CURL *curl = curl_easy_init();
	if (!curl)
		return CURLE_FAILED_INIT;

	/* auth: a login token (Bearer) wins over an API key; during the login
	 * request itself both are empty, so no auth header is sent */
	char authhdr[320];
	struct curl_slist *hdrs = NULL;
	if (g_token[0]) {
		snprintf(authhdr, sizeof(authhdr), "Authorization: Bearer %s",
			 g_token);
		hdrs = curl_slist_append(hdrs, authhdr);
	} else if (g_apikey[0]) {
		snprintf(authhdr, sizeof(authhdr), "x-api-key: %s", g_apikey);
		hdrs = curl_slist_append(hdrs, authhdr);
	}
	hdrs = curl_slist_append(hdrs, "Accept: application/json, image/jpeg");
	if (body)
		hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "vitaImmich/0.1 (PS Vita)");
	/* PoC: no CA bundle on the Vita, skip TLS verification */
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	if (g_resolve_list)
		curl_easy_setopt(curl, CURLOPT_RESOLVE, g_resolve_list);
	if (body)
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	if (g_verbose_request) {
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
		curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_debug_cb);
	}

	net_lock(); /* TLS is single-threaded, see g_net_mutex */
	CURLcode res = curl_easy_perform(curl);
	net_unlock();
	if (g_verbose_request) {
		log_line("curl: perform result=%d (%s)", res, curl_easy_strerror(res));
		g_verbose_request = 0;
	}
	if (res == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
		if (ctype) {
			char *ct = NULL;
			curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
			if (ct)
				snprintf(ctype, ctype_len, "%s", ct);
		}
	}

	curl_slist_free_all(hdrs);
	curl_easy_cleanup(curl);
	return res;
}

/* copy the value of a top-level JSON key into `out` (raw token text, works
 * for both string and number values). returns 1 if found. small flat
 * responses only (server storage / version). */
static int json_get(const char *js, size_t len, const char *key,
		    char *out, size_t outlen)
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
	int kl = (int)strlen(key), got = 0;
	for (int i = 1; i + 1 < n; i++) {
		if (t[i].type == JSMN_STRING && t[i].parent == 0 &&
		    t[i].end - t[i].start == kl &&
		    !strncmp(js + t[i].start, key, kl)) {
			int vl = t[i + 1].end - t[i + 1].start;
			if (vl >= (int)outlen)
				vl = (int)outlen - 1;
			memcpy(out, js + t[i + 1].start, vl);
			out[vl] = '\0';
			got = 1;
			break;
		}
	}
	free(t);
	return got;
}

/* log in with email/password and stash the access token in g_token (sent as
 * Authorization: Bearer afterwards). returns 0 on success. */
static int do_login(char *err, size_t errlen)
{
	/* JSON-escape email + password into the request body */
	char esc[400];
	int e = 0;
	const char *fields[2] = { g_email, g_password };
	const char *names[2] = { "email", "password" };
	e += snprintf(esc + e, sizeof(esc) - e, "{");
	for (int f = 0; f < 2; f++) {
		e += snprintf(esc + e, sizeof(esc) - e, "%s\"%s\":\"",
			      f ? "," : "", names[f]);
		for (const char *p = fields[f]; *p && e < (int)sizeof(esc) - 8; p++) {
			unsigned char ch = (unsigned char)*p;
			if (ch == '"' || ch == '\\') {
				esc[e++] = '\\';
				esc[e++] = (char)ch;
			} else if (ch >= 0x20) {
				esc[e++] = (char)ch;
			}
		}
		e += snprintf(esc + e, sizeof(esc) - e, "\"");
	}
	e += snprintf(esc + e, sizeof(esc) - e, "}");

	char url[600];
	snprintf(url, sizeof(url), "%s/api/auth/login", g_server);
	membuf buf;
	long code;
	CURLcode r = http_request(url, esc, &buf, &code, NULL, 0);
	if (r != CURLE_OK) {
		snprintf(err, errlen, "%s", curl_easy_strerror(r));
		free(buf.data);
		return -1;
	}
	if (code < 200 || code >= 300) {
		snprintf(err, errlen, "HTTP %ld: %.80s", code,
			 buf.data ? buf.data : "");
		free(buf.data);
		return -1;
	}
	if (!buf.data || !json_get(buf.data, buf.size, "accessToken",
				   g_token, sizeof(g_token))) {
		snprintf(err, errlen, "no accessToken in response");
		free(buf.data);
		return -1;
	}
	free(buf.data);
	log_line("login ok: token %.8s... for %s", g_token, g_email);
	return 0;
}

/* GET a JSON endpoint, trying the modern path then a legacy fallback. fills
 * `out`/`code`; caller frees out->data. returns CURLcode of the call used. */
static CURLcode server_get(const char *path, const char *legacy,
			   membuf *out, long *code)
{
	char url[600];
	snprintf(url, sizeof(url), "%s%s", g_server, path);
	CURLcode r = http_request(url, NULL, out, code, NULL, 0);
	if (r == CURLE_OK && (*code < 200 || *code >= 300) && legacy) {
		free(out->data);
		snprintf(url, sizeof(url), "%s%s", g_server, legacy);
		r = http_request(url, NULL, out, code, NULL, 0);
	}
	return r;
}

/* fetch disk usage + server version into the g_srv_* fields (main thread) */
static void fetch_server_info(void)
{
	membuf buf;
	long code;

	CURLcode r = server_get("/api/server/storage",
				"/api/server-info/storage", &buf, &code);
	if (r == CURLE_OK && code >= 200 && code < 300 && buf.data) {
		char pct[24] = "";
		if (!json_get(buf.data, buf.size, "diskUse",
			      g_srv_use, sizeof(g_srv_use)))
			g_srv_use[0] = '\0';
		if (!json_get(buf.data, buf.size, "diskSize",
			      g_srv_total, sizeof(g_srv_total)))
			g_srv_total[0] = '\0';
		if (json_get(buf.data, buf.size, "diskUsagePercentage",
			     pct, sizeof(pct)))
			g_srv_pct = (int)(atof(pct) + 0.5);
		g_srv_state = 1;
	} else {
		log_line("server storage: curl %d http %ld", r, code);
		g_srv_state = -1;
	}
	free(buf.data);

	r = server_get("/api/server/version", "/api/server-info/version",
		       &buf, &code);
	if (r == CURLE_OK && code >= 200 && code < 300 && buf.data) {
		char mj[12] = "", mn[12] = "", pt[12] = "";
		json_get(buf.data, buf.size, "major", mj, sizeof(mj));
		json_get(buf.data, buf.size, "minor", mn, sizeof(mn));
		json_get(buf.data, buf.size, "patch", pt, sizeof(pt));
		snprintf(g_srv_version, sizeof(g_srv_version), "v%s.%s.%s",
			 mj[0] ? mj : "?", mn[0] ? mn : "?", pt[0] ? pt : "?");
	}
	free(buf.data);
}

