/*
 * vitaImmich — minimal proof-of-concept Immich client for the PS Vita.
 *
 * Reads server URL + API key from ux0:data/vitaimmich/config.txt and shows
 * the photo library as a scrollable chronological grid (newest first).
 * X opens a photo full screen, O goes back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <vita2d.h>
#include <curl/curl.h>
#include <jpeglib.h>

#define JSMN_STATIC
#define JSMN_PARENT_LINKS
#include "jsmn.h"

#define SCREEN_W 960
#define SCREEN_H 544

#define COLS       4
#define CELL_W     240
#define CELL_H     240
#define CELL_PAD   6
#define THUMB_MAX  256   /* decode grid thumbs down to <= this dimension */
#define FULL_MAX   4096  /* GXM texture size limit */

#define PAGE_SIZE  100
#define MAX_ASSETS 1000

#define CONFIG_DIR  "ux0:data/vitaimmich"
#define CONFIG_PATH CONFIG_DIR "/config.txt"
#define LOG_PATH    CONFIG_DIR "/log.txt"

/* give curl/jpeg decoding plenty of heap */
int _newlib_heap_size_user = 192 * 1024 * 1024;

static char g_server[512];
static char g_apikey[256];

static vita2d_pgf *g_font;

static char g_asset_ids[MAX_ASSETS][40];
static char g_asset_dates[MAX_ASSETS][11]; /* YYYY-MM-DD */
static int g_asset_count;
static int g_next_page = 1; /* 0 = no more pages */

static vita2d_texture *g_thumb[MAX_ASSETS];
static int g_thumb_failed[MAX_ASSETS];
static char g_tex_err[MAX_ASSETS][160];

static void log_line(const char *fmt, ...)
{
	FILE *f = fopen(LOG_PATH, "a");
	if (!f)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

/* ------------------------------------------------------------------ */
/* drawing helpers                                                     */
/* ------------------------------------------------------------------ */

static void draw_centered(int y, uint32_t color, const char *text)
{
	int w = vita2d_pgf_text_width(g_font, 1.0f, text);
	vita2d_pgf_draw_text(g_font, (SCREEN_W - w) / 2, y, color, 1.0f, text);
}

static void show_status(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	vita2d_start_drawing();
	vita2d_clear_screen();
	draw_centered(SCREEN_H / 2, RGBA8(255, 255, 255, 255), buf);
	vita2d_end_drawing();
	vita2d_swap_buffers();
}

/* show an error (plus optional detail lines) until START is pressed, then exit */
static void fatal_error(const char *detail, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_START)
			break;

		vita2d_start_drawing();
		vita2d_clear_screen();
		draw_centered(180, RGBA8(255, 80, 80, 255), buf);
		if (detail && detail[0]) {
			const int chars_per_line = 80;
			int len = strlen(detail);
			int y = 230;
			for (int off = 0; off < len && y < 480; off += chars_per_line, y += 26) {
				char line[96];
				snprintf(line, sizeof(line), "%.*s", chars_per_line, detail + off);
				vita2d_pgf_draw_text(g_font, 40, y, RGBA8(200, 200, 200, 255), 1.0f, line);
			}
		}
		draw_centered(510, RGBA8(160, 160, 160, 255), "Press START to exit");
		vita2d_end_drawing();
		vita2d_swap_buffers();
	}

	vita2d_fini();
	sceKernelExitProcess(0);
}

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */

static void strip_eol(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
		s[--n] = '\0';
}

static int load_config(void)
{
	FILE *f = fopen(CONFIG_PATH, "r");
	if (!f) {
		/* create a template so the user just has to edit it */
		f = fopen(CONFIG_PATH, "w");
		if (f) {
			fputs("server=http://192.168.1.100:2283\n"
			      "apikey=PASTE_YOUR_IMMICH_API_KEY_HERE\n", f);
			fclose(f);
		}
		return -1;
	}

	char line[512];
	while (fgets(line, sizeof(line), f)) {
		strip_eol(line);
		if (!strncmp(line, "server=", 7))
			snprintf(g_server, sizeof(g_server), "%s", line + 7);
		else if (!strncmp(line, "apikey=", 7))
			snprintf(g_apikey, sizeof(g_apikey), "%s", line + 7);
	}
	fclose(f);

	/* strip trailing slash from server URL */
	size_t n = strlen(g_server);
	if (n > 0 && g_server[n - 1] == '/')
		g_server[n - 1] = '\0';

	if (!g_server[0] || !g_apikey[0] || strstr(g_apikey, "PASTE_YOUR"))
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* networking                                                          */
/* ------------------------------------------------------------------ */

static char g_net_mem[1024 * 1024];

static void net_init(void)
{
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

	char keyhdr[300];
	snprintf(keyhdr, sizeof(keyhdr), "x-api-key: %s", g_apikey);

	struct curl_slist *hdrs = NULL;
	hdrs = curl_slist_append(hdrs, keyhdr);
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
	if (body)
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);

	CURLcode res = curl_easy_perform(curl);
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

/* ------------------------------------------------------------------ */
/* JPEG decoding                                                       */
/*                                                                     */
/* libjpeg directly instead of vita2d_load_JPEG_buffer: vita2d rejects */
/* any JPEG whose first marker isn't APP0/APP1, but Immich previews    */
/* start with an APP2 ICC-profile marker (ff d8 ff e2). Also lets us   */
/* downscale at decode time for grid thumbnails, and a longjmp error   */
/* handler keeps corrupt data from exit()ing the app.                  */
/* ------------------------------------------------------------------ */

struct jpeg_jmp_err {
	struct jpeg_error_mgr mgr;
	jmp_buf jb;
	char msg[JMSG_LENGTH_MAX];
};

static void jpeg_jmp_error_exit(j_common_ptr cinfo)
{
	struct jpeg_jmp_err *e = (struct jpeg_jmp_err *)cinfo->err;
	cinfo->err->format_message(cinfo, e->msg);
	longjmp(e->jb, 1);
}

static vita2d_texture *decode_jpeg(const void *data, size_t size, int maxdim,
				   char *err, size_t errlen)
{
	struct jpeg_decompress_struct ji;
	struct jpeg_jmp_err jerr;
	vita2d_texture *tex = NULL;

	ji.err = jpeg_std_error(&jerr.mgr);
	jerr.mgr.error_exit = jpeg_jmp_error_exit;
	if (setjmp(jerr.jb)) {
		snprintf(err, errlen, "libjpeg: %s", jerr.msg);
		jpeg_destroy_decompress(&ji);
		if (tex)
			vita2d_free_texture(tex);
		return NULL;
	}

	jpeg_create_decompress(&ji);
	jpeg_mem_src(&ji, (void *)data, size);
	jpeg_read_header(&ji, 1);

	unsigned int longer = ji.image_width > ji.image_height ?
			      ji.image_width : ji.image_height;
	ji.scale_num = 1;
	ji.scale_denom = 1;
	while (longer / ji.scale_denom > (unsigned int)maxdim && ji.scale_denom < 8)
		ji.scale_denom *= 2;

	jpeg_start_decompress(&ji);

	if (ji.output_components != 1 && ji.output_components != 3) {
		snprintf(err, errlen, "unsupported JPEG: %d components (colorspace %d)",
			 ji.output_components, ji.jpeg_color_space);
		jpeg_abort_decompress(&ji);
		jpeg_destroy_decompress(&ji);
		return NULL;
	}

	SceGxmTextureFormat fmt = (ji.output_components == 1) ?
		SCE_GXM_TEXTURE_FORMAT_U8_R111 : SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR;
	tex = vita2d_create_empty_texture_format(ji.output_width,
						 ji.output_height, fmt);
	if (!tex) {
		snprintf(err, errlen, "texture alloc failed (%ux%u)",
			 ji.output_width, ji.output_height);
		jpeg_abort_decompress(&ji);
		jpeg_destroy_decompress(&ji);
		return NULL;
	}

	unsigned char *row = vita2d_texture_get_datap(tex);
	unsigned int stride = vita2d_texture_get_stride(tex);
	while (ji.output_scanline < ji.output_height) {
		jpeg_read_scanlines(&ji, &row, 1);
		row += stride;
	}

	jpeg_finish_decompress(&ji);
	jpeg_destroy_decompress(&ji);
	return tex;
}

/* decode into a tightly packed malloc'd pixel buffer; no vita2d/GXM
 * calls, so this is safe to run on the loader thread */
static unsigned char *decode_jpeg_buf(const void *data, size_t size, int maxdim,
				      int *w, int *h, int *comps,
				      char *err, size_t errlen)
{
	struct jpeg_decompress_struct ji;
	struct jpeg_jmp_err jerr;
	unsigned char *pix = NULL;

	ji.err = jpeg_std_error(&jerr.mgr);
	jerr.mgr.error_exit = jpeg_jmp_error_exit;
	if (setjmp(jerr.jb)) {
		snprintf(err, errlen, "libjpeg: %s", jerr.msg);
		jpeg_destroy_decompress(&ji);
		free(pix);
		return NULL;
	}

	jpeg_create_decompress(&ji);
	jpeg_mem_src(&ji, (void *)data, size);
	jpeg_read_header(&ji, 1);

	unsigned int longer = ji.image_width > ji.image_height ?
			      ji.image_width : ji.image_height;
	ji.scale_num = 1;
	ji.scale_denom = 1;
	while (longer / ji.scale_denom > (unsigned int)maxdim && ji.scale_denom < 8)
		ji.scale_denom *= 2;

	jpeg_start_decompress(&ji);

	if (ji.output_components != 1 && ji.output_components != 3) {
		snprintf(err, errlen, "unsupported JPEG: %d components (colorspace %d)",
			 ji.output_components, ji.jpeg_color_space);
		jpeg_abort_decompress(&ji);
		jpeg_destroy_decompress(&ji);
		return NULL;
	}

	unsigned int rowbytes = ji.output_width * ji.output_components;
	pix = malloc(rowbytes * ji.output_height);
	if (!pix) {
		snprintf(err, errlen, "out of memory (%ux%u)",
			 ji.output_width, ji.output_height);
		jpeg_abort_decompress(&ji);
		jpeg_destroy_decompress(&ji);
		return NULL;
	}

	unsigned char *row = pix;
	while (ji.output_scanline < ji.output_height) {
		jpeg_read_scanlines(&ji, &row, 1);
		row += rowbytes;
	}

	*w = ji.output_width;
	*h = ji.output_height;
	*comps = ji.output_components;
	jpeg_finish_decompress(&ji);
	jpeg_destroy_decompress(&ji);
	return pix;
}

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
	 * top-level object belong to that asset (nested objects have a
	 * different parent, so owner.id etc. are skipped) */
	int added = 0;
	int cur = -1, cur_obj = -1;
	for (int i = arr_idx + 1; i < ntok; i++) {
		if (tok[i].type == JSMN_OBJECT && tok[i].parent == arr_idx) {
			if (g_asset_count >= MAX_ASSETS)
				break;
			cur = g_asset_count++;
			cur_obj = i;
			added++;
			g_asset_ids[cur][0] = '\0';
			g_asset_dates[cur][0] = '\0';
			continue;
		}
		if (cur < 0 || tok[i].type != JSMN_STRING || tok[i].size != 1 ||
		    tok[i].parent != cur_obj || i + 1 >= ntok)
			continue;

		int klen = tok[i].end - tok[i].start;
		const char *k = js + tok[i].start;
		jsmntok_t *val = &tok[i + 1];
		int vlen = val->end - val->start;
		if (val->type != JSMN_STRING)
			continue;

		if (klen == 2 && !strncmp(k, "id", 2) &&
		    vlen > 0 && vlen < (int)sizeof(g_asset_ids[0]))
			snprintf(g_asset_ids[cur], sizeof(g_asset_ids[0]),
				 "%.*s", vlen, js + val->start);
		else if (klen == 13 && !strncmp(k, "fileCreatedAt", 13) && vlen >= 10)
			snprintf(g_asset_dates[cur], sizeof(g_asset_dates[0]),
				 "%.10s", js + val->start);
	}
	free(tok);

	/* drop trailing entries that somehow lack an id */
	while (g_asset_count > 0 && g_asset_ids[g_asset_count - 1][0] == '\0') {
		g_asset_count--;
		added--;
	}
	return added;
}

/* fetch the next page of the library, newest first; returns assets added */
static int fetch_page(int is_first)
{
	if (g_next_page <= 0 || g_asset_count >= MAX_ASSETS)
		return 0;

	char url[600];
	snprintf(url, sizeof(url), "%s/api/search/metadata", g_server);
	char body[128];
	snprintf(body, sizeof(body),
		 "{\"page\":%d,\"size\":%d,\"type\":\"IMAGE\",\"order\":\"desc\"}",
		 g_next_page, PAGE_SIZE);

	membuf buf;
	long code;
	CURLcode res = http_request(url, body, &buf, &code, NULL, 0);
	if (res != CURLE_OK) {
		if (is_first)
			fatal_error(curl_easy_strerror(res),
				    "Connection failed (curl error %d)", res);
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

	/* a short page means we reached the end of the library */
	g_next_page = (added == PAGE_SIZE) ? g_next_page + 1 : 0;
	log_line("page fetched: +%d assets (total %d)", added, g_asset_count);
	return added;
}

/* download the preview JPEG of an asset and decode it to <= maxdim px */
static vita2d_texture *load_image(int idx, int maxdim)
{
	char *err = g_tex_err[idx];
	const size_t errlen = sizeof(g_tex_err[idx]);

	char url[700];
	snprintf(url, sizeof(url), "%s/api/assets/%s/thumbnail?size=preview",
		 g_server, g_asset_ids[idx]);

	membuf buf;
	long code;
	char ctype[80];
	CURLcode res = http_request(url, NULL, &buf, &code, ctype, sizeof(ctype));

	if (res != CURLE_OK) {
		snprintf(err, errlen, "curl error %d: %s", res, curl_easy_strerror(res));
		log_line("%s: %s", g_asset_ids[idx], err);
		free(buf.data);
		return NULL;
	}
	if (code < 200 || code >= 300) {
		snprintf(err, errlen, "HTTP %ld: %.100s", code,
			 buf.data ? buf.data : "(empty body)");
		log_line("%s: %s", g_asset_ids[idx], err);
		free(buf.data);
		return NULL;
	}
	if (buf.size == 0) {
		snprintf(err, errlen, "HTTP %ld but empty body (type %s)", code, ctype);
		log_line("%s: %s", g_asset_ids[idx], err);
		free(buf.data);
		return NULL;
	}

	vita2d_texture *tex = NULL;
	const unsigned char *p = (const unsigned char *)buf.data;
	if (buf.size > 2 && p[0] == 0xff && p[1] == 0xd8) {
		tex = decode_jpeg(buf.data, buf.size, maxdim, err, errlen);
	} else if (buf.size > 8 && !memcmp(p, "\x89PNG", 4)) {
		tex = vita2d_load_PNG_buffer(buf.data);
		if (!tex)
			snprintf(err, errlen, "PNG decode failed; %u bytes",
				 (unsigned)buf.size);
	} else if (buf.size > 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WEBP", 4)) {
		snprintf(err, errlen, "got WebP (unsupported); %u bytes, type %s",
			 (unsigned)buf.size, ctype);
	} else {
		snprintf(err, errlen,
			 "unknown format; %u bytes, type %s, magic %02x %02x %02x %02x",
			 (unsigned)buf.size, ctype, p[0], p[1],
			 buf.size > 2 ? p[2] : 0, buf.size > 3 ? p[3] : 0);
	}
	if (!tex)
		log_line("%s: %s", g_asset_ids[idx], err);
	free(buf.data);
	return tex;
}

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
static unsigned char *g_req_pix;   /* decoded pixels, or NULL on failure */
static int g_req_w, g_req_h, g_req_comps;
static char *g_req_raw;            /* raw body when it's a PNG */
static size_t g_req_raw_size;

static int worker_thread(SceSize args, void *argp)
{
	CURL *curl = curl_easy_init();
	char keyhdr[300];
	snprintf(keyhdr, sizeof(keyhdr), "x-api-key: %s", g_apikey);
	struct curl_slist *hdrs = curl_slist_append(NULL, keyhdr);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "vitaImmich/0.1 (PS Vita)");
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

	for (;;) {
		if (g_req_state != REQ_PENDING) {
			sceKernelDelayThread(2000);
			continue;
		}
		int idx = g_req_idx;
		char *err = g_tex_err[idx];
		const size_t errlen = sizeof(g_tex_err[idx]);
		g_req_pix = NULL;
		g_req_raw = NULL;

		char url[700];
		snprintf(url, sizeof(url), "%s/api/assets/%s/thumbnail?size=preview",
			 g_server, g_asset_ids[idx]);

		membuf buf = { NULL, 0 };
		long code = 0;
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
		CURLcode res = curl_easy_perform(curl);
		if (res == CURLE_OK)
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

		if (res != CURLE_OK) {
			snprintf(err, errlen, "curl error %d: %s",
				 res, curl_easy_strerror(res));
		} else if (code < 200 || code >= 300) {
			snprintf(err, errlen, "HTTP %ld: %.100s", code,
				 buf.data ? buf.data : "(empty body)");
		} else if (buf.size == 0) {
			snprintf(err, errlen, "HTTP %ld but empty body", code);
		} else {
			const unsigned char *p = (const unsigned char *)buf.data;
			if (buf.size > 2 && p[0] == 0xff && p[1] == 0xd8) {
				g_req_pix = decode_jpeg_buf(buf.data, buf.size,
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
			log_line("%s: %s", g_asset_ids[idx], err);

		__sync_synchronize();
		g_req_state = REQ_DONE;
	}
	return 0;
}

/* main-thread side: turn a finished worker result into a texture */
static void consume_worker_result(void)
{
	int idx = g_req_idx;
	vita2d_texture *tex = NULL;

	if (g_req_pix) {
		SceGxmTextureFormat fmt = (g_req_comps == 1) ?
			SCE_GXM_TEXTURE_FORMAT_U8_R111 :
			SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR;
		tex = vita2d_create_empty_texture_format(g_req_w, g_req_h, fmt);
		if (tex) {
			unsigned char *dst = vita2d_texture_get_datap(tex);
			unsigned int stride = vita2d_texture_get_stride(tex);
			unsigned int rowbytes = g_req_w * g_req_comps;
			for (int y = 0; y < g_req_h; y++)
				memcpy(dst + y * stride,
				       g_req_pix + y * rowbytes, rowbytes);
		} else {
			snprintf(g_tex_err[idx], sizeof(g_tex_err[idx]),
				 "texture alloc failed (%dx%d)", g_req_w, g_req_h);
		}
		free(g_req_pix);
		g_req_pix = NULL;
	} else if (g_req_raw) {
		tex = vita2d_load_PNG_buffer(g_req_raw);
		if (!tex)
			snprintf(g_tex_err[idx], sizeof(g_tex_err[idx]),
				 "PNG decode failed; %u bytes",
				 (unsigned)g_req_raw_size);
		free(g_req_raw);
		g_req_raw = NULL;
	}

	g_thumb[idx] = tex;
	g_thumb_failed[idx] = (tex == NULL);
	g_req_state = REQ_IDLE;
}

/* pick the most useful thumbnail to load next: selection, then visible,
 * then prefetch a couple of rows below and above the viewport */
static int pick_next_load(int sel, int first_vis, int last_vis)
{
	if (!g_thumb[sel] && !g_thumb_failed[sel])
		return sel;
	for (int i = first_vis; i >= 0 && i <= last_vis && i < g_asset_count; i++)
		if (!g_thumb[i] && !g_thumb_failed[i])
			return i;
	for (int i = last_vis + 1; i <= last_vis + 2 * COLS && i < g_asset_count; i++)
		if (i >= 0 && !g_thumb[i] && !g_thumb_failed[i])
			return i;
	for (int i = first_vis - 1; i >= first_vis - 2 * COLS && i >= 0; i--)
		if (!g_thumb[i] && !g_thumb_failed[i])
			return i;
	return -1;
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static void draw_texture_fitted(vita2d_texture *tex, float x, float y,
				float box_w, float box_h)
{
	float w = vita2d_texture_get_width(tex);
	float h = vita2d_texture_get_height(tex);
	float s = (box_w / w < box_h / h) ? box_w / w : box_h / h;
	vita2d_draw_texture_scale(tex, x + (box_w - w * s) / 2.0f,
				  y + (box_h - h * s) / 2.0f, s, s);
}

static void draw_sel_outline(float x, float y, float w, float h)
{
	const uint32_t c = RGBA8(255, 255, 255, 255);
	const float t = 3.0f;
	vita2d_draw_rectangle(x, y, w, t, c);
	vita2d_draw_rectangle(x, y + h - t, w, t, c);
	vita2d_draw_rectangle(x, y, t, h, c);
	vita2d_draw_rectangle(x + w - t, y, t, h, c);
}

static void draw_hud(const char *text)
{
	vita2d_draw_rectangle(0, SCREEN_H - 32, SCREEN_W, 32, RGBA8(0, 0, 0, 180));
	vita2d_pgf_draw_text(g_font, 10, SCREEN_H - 9,
			     RGBA8(255, 255, 255, 255), 1.0f, text);
}

static void draw_error_detail(int idx)
{
	draw_centered(220, RGBA8(255, 80, 80, 255), "Failed to load this photo");
	vita2d_pgf_draw_text(g_font, 40, 270, RGBA8(200, 200, 200, 255),
			     1.0f, g_asset_ids[idx]);
	const char *e = g_tex_err[idx];
	int len = strlen(e), y = 300;
	const int per_line = 76;
	for (int off = 0; off < len && y < 500; off += per_line, y += 26) {
		char line[80];
		snprintf(line, sizeof(line), "%.*s", per_line, e + off);
		vita2d_pgf_draw_text(g_font, 40, y, RGBA8(200, 200, 200, 255), 1.0f, line);
	}
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

enum { MODE_GRID, MODE_DETAIL };

int main(void)
{
	vita2d_init();
	vita2d_set_clear_color(RGBA8(16, 16, 16, 255));
	g_font = vita2d_load_default_pgf();

	/* start a fresh debug log each launch */
	sceIoMkdir(CONFIG_DIR, 0777);
	FILE *lf = fopen(LOG_PATH, "w");
	if (lf)
		fclose(lf);

	show_status("Starting network...");
	net_init();

	if (load_config() != 0)
		fatal_error("Edit it with VitaShell, then restart the app. "
			    "Create the API key in the Immich web UI under "
			    "Account Settings > API Keys.",
			    "Set your server and API key in " CONFIG_PATH);

	show_status("Loading library from %s ...", g_server);
	fetch_page(1);
	if (g_asset_count == 0)
		fatal_error(NULL, "Server returned no image assets");

	SceUID worker = sceKernelCreateThread("thumb_loader", worker_thread,
					      0x10000100, 256 * 1024, 0, 0, NULL);
	if (worker >= 0)
		sceKernelStartThread(worker, 0, NULL);

	int mode = MODE_GRID;
	int sel = 0;
	float scroll = 0.0f, target = 0.0f;
	unsigned int prev_buttons = 0;
	unsigned int held_frames = 0;

	vita2d_texture *detail_tex = NULL;
	int detail_idx = -1;
	int detail_failed = 0;

	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		unsigned int pressed = pad.buttons & ~prev_buttons;
		prev_buttons = pad.buttons;

		if (pad.buttons & SCE_CTRL_START)
			break;

		/* d-pad auto-repeat for fast scrolling */
		const unsigned int dirs = SCE_CTRL_UP | SCE_CTRL_DOWN |
					  SCE_CTRL_LEFT | SCE_CTRL_RIGHT;
		held_frames = (pad.buttons & dirs) ? held_frames + 1 : 0;
		unsigned int nav = pressed;
		if (held_frames > 18 && held_frames % 5 == 0)
			nav |= pad.buttons & dirs;

		if (mode == MODE_GRID) {
			if (nav & SCE_CTRL_RIGHT)
				sel++;
			if (nav & SCE_CTRL_LEFT)
				sel--;
			if (nav & SCE_CTRL_DOWN)
				sel += COLS;
			if (nav & SCE_CTRL_UP)
				sel -= COLS;
			if (pressed & SCE_CTRL_RTRIGGER)
				sel += COLS * 2; /* page down */
			if (pressed & SCE_CTRL_LTRIGGER)
				sel -= COLS * 2;
			if (sel < 0)
				sel = 0;
			if (sel >= g_asset_count)
				sel = g_asset_count - 1;

			if (pressed & SCE_CTRL_CROSS)
				mode = MODE_DETAIL;

			/* fetch the next page when selection nears the end */
			if (g_next_page > 0 &&
			    sel >= g_asset_count - COLS * 4) {
				show_status("Loading more photos... (%d so far)",
					    g_asset_count);
				fetch_page(0);
			}

			/* scroll follows the selection */
			float sel_y = (float)(sel / COLS) * CELL_H;
			if (sel_y < target)
				target = sel_y;
			if (sel_y + CELL_H > target + SCREEN_H)
				target = sel_y + CELL_H - SCREEN_H;
			int rows = (g_asset_count + COLS - 1) / COLS;
			float max_scroll = rows * CELL_H - SCREEN_H;
			if (max_scroll < 0)
				max_scroll = 0;
			if (target < 0)
				target = 0;
			if (target > max_scroll)
				target = max_scroll;
			scroll += (target - scroll) * 0.35f;

			int first_vis = ((int)scroll / CELL_H) * COLS;
			int last_vis = (((int)scroll + SCREEN_H) / CELL_H + 1) * COLS - 1;
			if (last_vis >= g_asset_count)
				last_vis = g_asset_count - 1;

			/* evict thumbs far outside the viewport (never the one
			 * the worker is currently loading); wait for the GPU
			 * before the first free in case a recently drawn
			 * texture is still referenced by an in-flight frame */
			int waited = 0;
			for (int i = 0; i < g_asset_count; i++) {
				if (!g_thumb[i] ||
				    (g_req_state != REQ_IDLE && i == g_req_idx))
					continue;
				float dy = (float)(i / COLS) * CELL_H - scroll;
				if (dy < -2.5f * SCREEN_H || dy > 3.5f * SCREEN_H) {
					if (!waited) {
						vita2d_wait_rendering_done();
						waited = 1;
					}
					vita2d_free_texture(g_thumb[i]);
					g_thumb[i] = NULL;
				}
			}

			/* collect finished download, then hand the worker
			 * the next most useful thumbnail */
			if (g_req_state == REQ_DONE) {
				__sync_synchronize();
				consume_worker_result();
			}
			if (g_req_state == REQ_IDLE) {
				int next = pick_next_load(sel, first_vis, last_vis);
				if (next >= 0) {
					g_req_idx = next;
					__sync_synchronize();
					g_req_state = REQ_PENDING;
				}
			}

			vita2d_start_drawing();
			vita2d_clear_screen();

			for (int i = first_vis; i >= 0 && i <= last_vis; i++) {
				float x = (float)(i % COLS) * CELL_W;
				float y = (float)(i / COLS) * CELL_H - scroll;
				float bx = x + CELL_PAD, by = y + CELL_PAD;
				float bw = CELL_W - 2 * CELL_PAD;
				float bh = CELL_H - 2 * CELL_PAD;

				if (g_thumb[i])
					draw_texture_fitted(g_thumb[i], bx, by, bw, bh);
				else if (g_thumb_failed[i])
					vita2d_draw_rectangle(bx, by, bw, bh,
							      RGBA8(90, 30, 30, 255));
				else
					vita2d_draw_rectangle(bx, by, bw, bh,
							      RGBA8(40, 40, 40, 255));
				if (i == sel)
					draw_sel_outline(x + 2, y + 2,
							 CELL_W - 4, CELL_H - 4);
			}

			char hud[160];
			snprintf(hud, sizeof(hud),
				 "%d / %d%s    %s    X view    START exit",
				 sel + 1, g_asset_count, g_next_page > 0 ? "+" : "",
				 g_asset_dates[sel]);
			draw_hud(hud);

			vita2d_end_drawing();
			vita2d_swap_buffers();
		} else { /* MODE_DETAIL */
			if (nav & SCE_CTRL_RIGHT && sel < g_asset_count - 1)
				sel++;
			if (nav & SCE_CTRL_LEFT && sel > 0)
				sel--;
			if (pressed & SCE_CTRL_CIRCLE) {
				mode = MODE_GRID;
				continue;
			}
			if ((pressed & SCE_CTRL_CROSS) && detail_failed)
				detail_idx = -1; /* retry */

			if (detail_idx != sel) {
				if (detail_tex) {
					/* the GPU may still be drawing the
					 * previous frame with this texture */
					vita2d_wait_rendering_done();
					vita2d_free_texture(detail_tex);
					detail_tex = NULL;
				}
				show_status("Loading photo %d/%d ...",
					    sel + 1, g_asset_count);
				detail_tex = load_image(sel, FULL_MAX);
				detail_failed = (detail_tex == NULL);
				detail_idx = sel;
			}

			vita2d_start_drawing();
			vita2d_clear_screen();

			if (detail_tex)
				draw_texture_fitted(detail_tex, 0, 0,
						    SCREEN_W, SCREEN_H);
			else
				draw_error_detail(sel);

			char hud[160];
			snprintf(hud, sizeof(hud),
				 "%d / %d    %s    < > browse    %sO back    START exit",
				 sel + 1, g_asset_count, g_asset_dates[sel],
				 detail_failed ? "X retry    " : "");
			draw_hud(hud);

			vita2d_end_drawing();
			vita2d_swap_buffers();
		}
	}

	vita2d_wait_rendering_done();
	if (detail_tex)
		vita2d_free_texture(detail_tex);
	for (int i = 0; i < g_asset_count; i++)
		if (g_thumb[i])
			vita2d_free_texture(g_thumb[i]);
	vita2d_free_pgf(g_font);
	vita2d_fini();
	curl_global_cleanup();
	sceKernelExitProcess(0);
	return 0;
}
