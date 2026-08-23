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

/* ---- OAuth (authorization-code + PKCE) ------------------------------ */

/* base64url, no padding (RFC 4648 sec. 5): what Immich's OAuth endpoints
 * want for the PKCE code_verifier/code_challenge and our own state token */
static void b64url_encode(const unsigned char *in, size_t inlen,
			  char *out, size_t outsz)
{
	static const char tbl[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	size_t o = 0;
	for (size_t i = 0; i < inlen && o + 4 < outsz; i += 3) {
		unsigned int v = (unsigned int)in[i] << 16;
		int n = 1;
		if (i + 1 < inlen) { v |= (unsigned int)in[i + 1] << 8; n = 2; }
		if (i + 2 < inlen) { v |= in[i + 2];                    n = 3; }
		out[o++] = tbl[(v >> 18) & 0x3F];
		out[o++] = tbl[(v >> 12) & 0x3F];
		if (n > 1) out[o++] = tbl[(v >> 6) & 0x3F];
		if (n > 2) out[o++] = tbl[v & 0x3F];
	}
	out[o] = '\0';
}

/* a base64url-encoded random string with `nbytes` of entropy behind it */
static void rand_b64url(char *out, size_t outsz, int nbytes)
{
	unsigned char raw[64];
	if (nbytes > (int)sizeof(raw))
		nbytes = sizeof(raw);
	sceKernelGetRandomNumber(raw, nbytes);
	b64url_encode(raw, nbytes, out, outsz);
}

/* percent-encode a URL query value (RFC 3986 unreserved chars pass through) */
static void url_encode(const char *in, char *out, size_t outsz)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)in;
	     *p && o + 4 < outsz; p++) {
		if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
			out[o++] = (char)*p;
		} else {
			out[o++] = '%';
			out[o++] = hex[*p >> 4];
			out[o++] = hex[*p & 0xF];
		}
	}
	out[o] = '\0';
}

/* JSON-escape `src` into `dst` (quotes/backslashes/control chars) */
static void json_escape(char *dst, size_t dstsz, const char *src)
{
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)src;
	     *p && o + 2 < dstsz; p++) {
		if (*p == '"' || *p == '\\') {
			dst[o++] = '\\';
			dst[o++] = (char)*p;
		} else if (*p >= 0x20) {
			dst[o++] = (char)*p;
		}
	}
	dst[o] = '\0';
}

/* the redirect URI OAuth callbacks are sent to: the config override, or
 * Immich's own /api/oauth/mobile-redirect passthrough. Self-hosters who have
 * already set up OAuth for the official mobile app typically allow this
 * exact URI with their identity provider already (it's how Immich bridges a
 * plain https(s) redirect to the app.immich:// deep link), so it works with
 * no extra admin setup for most servers. */
static void oauth_redirect_uri(char *out, size_t outsz)
{
	if (g_oauth_redirect[0])
		snprintf(out, outsz, "%s", g_oauth_redirect);
	else
		snprintf(out, outsz, "%s/api/oauth/mobile-redirect", g_server);
}

/* start an OAuth login: ask the server for the identity provider's
 * authorization URL (POST /api/oauth/authorize). Fills `url` (open it in a
 * browser -- the Vita doesn't have one this app can drive) and `state`/
 * `verifier`, which must be replayed to oauth_exchange() below once the
 * provider has redirected back. returns 0 on success. */
static int oauth_authorize(char *url, size_t urlsz, char *state,
			   size_t statesz, char *verifier, size_t versz,
			   char *err, size_t errlen)
{
	char redirect[300];
	oauth_redirect_uri(redirect, sizeof(redirect));

	rand_b64url(state, statesz, 16);
	rand_b64url(verifier, versz, 32);

	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256((const unsigned char *)verifier, strlen(verifier), digest);
	char challenge[64];
	b64url_encode(digest, sizeof(digest), challenge, sizeof(challenge));

	char redirect_esc[400], state_esc[64], challenge_esc[96];
	json_escape(redirect_esc, sizeof(redirect_esc), redirect);
	json_escape(state_esc, sizeof(state_esc), state);
	json_escape(challenge_esc, sizeof(challenge_esc), challenge);

	char body[900];
	snprintf(body, sizeof(body),
		 "{\"redirectUri\":\"%s\",\"state\":\"%s\",\"codeChallenge\":\"%s\"}",
		 redirect_esc, state_esc, challenge_esc);

	char reqUrl[600];
	snprintf(reqUrl, sizeof(reqUrl), "%s/api/oauth/authorize", g_server);
	membuf buf;
	long code;
	CURLcode r = http_request(reqUrl, body, &buf, &code, NULL, 0);
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
	if (!buf.data || !json_get(buf.data, buf.size, "url", url, urlsz)) {
		snprintf(err, errlen, "no authorize url in response");
		free(buf.data);
		return -1;
	}
	free(buf.data);
	log_line("oauth authorize url: %.200s", url);
	return 0;
}

/* finish an OAuth login: exchange the identity provider's callback for a
 * session token (POST /api/oauth/callback), stashed in g_token like
 * do_login(). `input` is either the full callback URL the user landed on
 * (contains "://"), or just its "code" value -- reconstructed here into a
 * callback URL using the redirect URI + state this login started with, so
 * the user only has to retype the short code. returns 0 on success. */
static int oauth_exchange(const char *redirect, const char *state,
			  const char *verifier, const char *input,
			  char *err, size_t errlen)
{
	/* input comes from the IME dialog (max 128 UTF-16 chars) but every byte
	 * can percent-encode to 3, so code_enc needs headroom well past 128*3 */
	char cburl[1400];
	if (strstr(input, "://")) {
		snprintf(cburl, sizeof(cburl), "%s", input);
	} else {
		char code_enc[420], state_enc[96];
		url_encode(input, code_enc, sizeof(code_enc));
		url_encode(state, state_enc, sizeof(state_enc));
		snprintf(cburl, sizeof(cburl), "%s?code=%s&state=%s",
			 redirect, code_enc, state_enc);
	}

	char url_esc[1024], state_esc[64], verifier_esc[128];
	json_escape(url_esc, sizeof(url_esc), cburl);
	json_escape(state_esc, sizeof(state_esc), state);
	json_escape(verifier_esc, sizeof(verifier_esc), verifier);

	char body[1400];
	snprintf(body, sizeof(body),
		 "{\"url\":\"%s\",\"state\":\"%s\",\"codeVerifier\":\"%s\"}",
		 url_esc, state_esc, verifier_esc);

	char reqUrl[600];
	snprintf(reqUrl, sizeof(reqUrl), "%s/api/oauth/callback", g_server);
	membuf buf;
	long code;
	CURLcode r = http_request(reqUrl, body, &buf, &code, NULL, 0);
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
	log_line("oauth login ok: token %.8s...", g_token);
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

