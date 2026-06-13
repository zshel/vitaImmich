/* vitaImmich unity build — part of one translation unit.
 * This file is #included by src/main.c (in order) and shares all
 * file-scope state with the other parts; it is NOT compiled on its own.
 * Only src/main.c is listed in CMakeLists.txt. Editors/clangd may flag
 * undefined symbols here because of that — those are expected.
 */

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */

/* trim whitespace on both ends and a UTF-8 BOM; editors sneak these in,
 * and any of them makes curl fail with "unsupported protocol" */
static char *clean_line(char *s)
{
	if (!strncmp(s, "\xEF\xBB\xBF", 3))
		s += 3;
	while (*s == ' ' || *s == '\t')
		s++;
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
			 s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';
	return s;
}

static int load_config(void)
{
	FILE *f = fopen(CONFIG_PATH, "r");
	if (!f) {
		/* create a template so the user just has to edit it */
		f = fopen(CONFIG_PATH, "w");
		if (f) {
			fputs("server=https://demo.immich.app\n"
			      "# auth: an API key, OR an email + password login.\n"
			      "apikey=\n"
			      "email=demo@immich.app\n"
			      "password=demo\n"
			      "# serverip=192.168.1.100  (optional: LAN IP of the\n"
			      "#  server, for routers without NAT loopback)\n"
			      "# syncdir=ux0:picture  (optional, repeatable: folders\n"
			      "#  to scan for camera media; default ux0:picture and\n"
			      "#  ux0:video/CAMERA)\n"
			      "# syncmaxmb=512  (skip files bigger than this many\n"
			      "#  MB when syncing)\n", f);
			fclose(f);
		}
		return -1;
	}

	char line[512];
	while (fgets(line, sizeof(line), f)) {
		char *s = clean_line(line);
		if (!strncmp(s, "server=", 7))
			snprintf(g_server, sizeof(g_server), "%s", clean_line(s + 7));
		else if (!strncmp(s, "apikey=", 7))
			snprintf(g_apikey, sizeof(g_apikey), "%s", clean_line(s + 7));
		else if (!strncmp(s, "email=", 6))
			snprintf(g_email, sizeof(g_email), "%s", clean_line(s + 6));
		else if (!strncmp(s, "password=", 9))
			snprintf(g_password, sizeof(g_password), "%s", clean_line(s + 9));
		else if (!strncmp(s, "serverip=", 9))
			snprintf(g_serverip, sizeof(g_serverip), "%s", clean_line(s + 9));
		else if (!strncmp(s, "syncdir=", 8)) {
			if (g_syncdir_count < (int)(sizeof(g_syncdirs) /
						    sizeof(g_syncdirs[0]))) {
				char *d = clean_line(s + 8);
				size_t dl = strlen(d);
				while (dl > 0 && d[dl - 1] == '/')
					d[--dl] = '\0';
				if (dl > 0)
					snprintf(g_syncdirs[g_syncdir_count++],
						 sizeof(g_syncdirs[0]), "%s", d);
			}
		} else if (!strncmp(s, "syncmaxmb=", 10))
			g_syncmax_mb = atol(clean_line(s + 10));
	}
	fclose(f);

	/* strip trailing slash from server URL */
	size_t n = strlen(g_server);
	if (n > 0 && g_server[n - 1] == '/')
		g_server[n - 1] = '\0';

	/* need a server and either an API key or an email+password login */
	if (strstr(g_apikey, "PASTE_YOUR"))
		g_apikey[0] = '\0';
	int have_key = g_apikey[0] != '\0';
	int have_login = g_email[0] && g_password[0];
	if (!g_server[0] || (!have_key && !have_login))
		return -1;

	/* default to http:// when no scheme is given */
	if (!strstr(g_server, "://")) {
		char tmp[512];
		snprintf(tmp, sizeof(tmp), "http://%s", g_server);
		snprintf(g_server, sizeof(g_server), "%s", tmp);
	}
	if (strncasecmp(g_server, "http://", 7) && strncasecmp(g_server, "https://", 8))
		fatal_error(g_server,
			    "Bad server URL in config (must start with http:// or https://):");

	/* serverip= pins the hostname to a fixed IP (keeps Host header and
	 * TLS SNI intact, unlike putting the IP in the URL) */
	if (g_serverip[0]) {
		int https = !strncasecmp(g_server, "https://", 8);
		const char *host = strstr(g_server, "://") + 3;
		char hostname[256];
		size_t i = 0;
		while (host[i] && host[i] != '/' && host[i] != ':' &&
		       i < sizeof(hostname) - 1) {
			hostname[i] = host[i];
			i++;
		}
		hostname[i] = '\0';
		int port = https ? 443 : 80;
		if (host[i] == ':')
			port = atoi(host + i + 1);

		char resolve[400];
		snprintf(resolve, sizeof(resolve), "%s:%d:%s",
			 hostname, port, g_serverip);
		g_resolve_list = curl_slist_append(NULL, resolve);
		log_line("resolve pin: %s", resolve);
	}
	return 0;
}

