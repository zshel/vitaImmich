/*
 * vitaImmich — minimal proof-of-concept Immich client for the PS Vita.
 *
 * Reads server URL + API key from ux0:data/vitaimmich/config.txt and shows
 * the library as a scrollable chronological grid (newest first).
 * X opens a photo full screen, O goes back. Videos are downloaded to the
 * memory card and played with SceAvPlayer (hardware MP4/H.264 decoding).
 *
 * It also scans the Vita's own camera media (ux0:picture, ux0:video),
 * hashes it (SHA1) on a background sync thread, asks the server which files
 * are already backed up (POST /api/assets/bulk-upload-check) and can upload
 * the rest (POST /api/assets, multipart). Local files are merged into the
 * same date-sorted grid with a per-cell status badge (cloud-only / local-
 * only / backed-up). SELECT opens a sync overview where uploads are started.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <setjmp.h>
#include <malloc.h>

#include <psp2/appmgr.h>
#include <psp2/audioout.h>
#include <psp2/avplayer.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/touch.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/rtc.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>

#include <vita2d.h>
#include <curl/curl.h>
#include <jpeglib.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>

#define JSMN_STATIC
#define JSMN_PARENT_LINKS
#include "jsmn.h"

#define SCREEN_W 960
#define SCREEN_H 544

#define COLS       5     /* rough items per row, for prefetch distances only */
#define ROW_H      200   /* justified-grid row height; widths follow aspect */
#define CELL_PAD   3
#define HEADER_H   40            /* month/year band height in the grid */
#define SEARCH_H   56            /* pinned Immich-style search bar at the top */
#define THUMB_MAX  256   /* decode grid thumbs down to <= this dimension */
#define FULL_MAX   4096  /* GXM texture size limit */

#define PAGE_SIZE  100
#define DATELEN    20            /* "YYYY-MM-DDTHH:MM:SS" + NUL, sortable */
#define APP_VERSION "1.0"        /* keep in step with CMakeLists VITA_VERSION */

#define CONFIG_DIR  "ux0:data/vitaimmich"
#define CONFIG_PATH CONFIG_DIR "/config.txt"
#define LOG_PATH    CONFIG_DIR "/log.txt"
#define AUTOBK_PATH CONFIG_DIR "/autobackup.txt"
#define VIDEO_TMP_PATH CONFIG_DIR "/video.mp4"

/* give curl/jpeg decoding plenty of heap */
int _newlib_heap_size_user = 192 * 1024 * 1024;

static char g_server[512];
static char g_apikey[256];
static char g_serverip[64];
/* optional DNS pin ("host:port:ip") for routers without NAT loopback */
static struct curl_slist *g_resolve_list;

/* auto-backup: when set, newly-found local-only photos are queued for upload
 * automatically (toggled on the cloud page, persisted to AUTOBK_PATH) */
static int g_autobackup;

/* folders scanned for camera media (config syncdir=, with defaults), and
 * a size cap so a movie collection in ux0:video isn't hashed/synced */
static char g_syncdirs[8][192];
static int g_syncdir_count;
static long g_syncmax_mb = 512;

/* vitasdk's freetype is built with bzip2 support but the SDK ships no
 * libbz2; we never load bzip2-compressed fonts, so failing stubs satisfy
 * the linker */
int BZ2_bzDecompressInit(void *strm, int verbosity, int small);
int BZ2_bzDecompress(void *strm);
int BZ2_bzDecompressEnd(void *strm);
int BZ2_bzDecompressInit(void *strm, int verbosity, int small) { return -1; }
int BZ2_bzDecompress(void *strm) { return -1; }
int BZ2_bzDecompressEnd(void *strm) { return -1; }

static vita2d_pgf *g_font;           /* system font, fallback */
static vita2d_font *g_ttf;           /* bundled Overpass (Immich's face) */

/* server-asset arrays. dynamically grown (doubling) as pages are fetched, so
 * the library is bounded only by memory, not a fixed cap. grown on the main
 * thread; the thumb worker never indexes these (it reads dedicated request
 * buffers), so a realloc here can't dangle a pointer under it. */
static char (*g_asset_ids)[40];
static char (*g_asset_dates)[DATELEN]; /* YYYY-MM-DDTHH:MM:SS */
static float *g_asset_ratio;           /* display w/h from exif, 0 = unknown */
static unsigned char *g_asset_rot;     /* raw exif orientation (0 = none) */
static unsigned char *g_asset_is_video;
/* set during display rebuild: a backed-up local file matches this server
 * asset, so its grid cell shows the green "backed up" badge */
static unsigned char *g_asset_local_backed;
static int g_asset_count;
static int g_asset_cap;
static int g_next_page = 1; /* 0 = no more pages */

static vita2d_texture **g_thumb;
static int *g_thumb_failed;
static char (*g_tex_err)[160];

/* ------------------------------------------------------------------ */
/* local camera media + sync state                                     */
/* ------------------------------------------------------------------ */

/* per-item sync state machine. transitions are written by the sync
 * thread, except QUEUED which the main thread sets on a LOCAL_ONLY item
 * (guarded the same way as the thumb worker's g_req_state handoff). */
enum {
	SYNC_UNSCANNED,  /* found on disk, not hashed yet */
	SYNC_HASHING,    /* sync thread is computing SHA1 */
	SYNC_CHECKING,   /* hashed, awaiting/under bulk-upload-check */
	SYNC_LOCAL_ONLY, /* not on the server */
	SYNC_QUEUED,     /* user asked to upload (main-thread write) */
	SYNC_UPLOADING,  /* sync thread is POSTing it */
	SYNC_BACKED_UP,  /* exists on the server */
	SYNC_FAILED,     /* upload/check error (see g_local_err) */
};

/* local camera-media arrays. grown (doubling) during the one-shot startup
 * scan, then fixed for the rest of the run, so the sync thread can read them
 * without a realloc moving the storage out from under it. */
static char (*g_local_path)[256];
static char (*g_local_date)[DATELEN]; /* sortable mtime */
static char (*g_local_sha1)[41];      /* lowercase hex */
static char (*g_local_server_id)[40]; /* assetId once backed up */
static char (*g_local_err)[96];
static SceDateTime *g_local_mtime;
static SceDateTime *g_local_ctime;
static long long *g_local_size;
static unsigned char *g_local_is_video;
static volatile int *g_local_state;
static int g_local_count;
static int g_local_cap;

static vita2d_texture **g_local_thumb;
static int *g_local_thumb_failed;

/* merged, date-desc display order of server + local items */
enum { SRC_SERVER, SRC_LOCAL };
struct disp_item { unsigned char src; int idx; };
static struct disp_item *g_disp;
static int g_disp_count;
static int g_disp_cap;

/* per-display-slot grid layout (filled by layout_grid): pixel position and
 * width/height of the cell (justified rows: width follows the photo's
 * aspect, the row is stretched to span the full screen width), plus the
 * month/year section headers above each month's run */
static float *g_item_x;
static float *g_item_y;
static float *g_item_w;
static float *g_item_h;
struct sect_hdr { float y; char label[24]; };
static struct sect_hdr *g_sect;
static int g_sect_count;
static int g_sect_cap;
static float g_content_h; /* total scrollable height incl. headers */

/* free-text smart (CLIP) search. when active the grid shows only the matching
 * server assets — still date-grouped under month headers like the timeline —
 * instead of the full merged library; TRIANGLE opens the keyboard, CIRCLE (or
 * the bar's clear chip) returns to the timeline. g_search_idx holds indices
 * into the g_asset_* arrays (those never reorder, only grow), in the order the
 * server returned them. */
static int g_search_active;
static char g_search_query[128];
static int *g_search_idx;
static int g_search_count;
static int g_search_cap;

/* server info shown on the cloud/backup page; fetched lazily the first time
 * the page is opened. g_srv_state: 0 not fetched, 1 ok, -1 failed. */
static int g_srv_state;
static char g_srv_use[40];      /* disk used, human ("1.4 TiB") */
static char g_srv_total[40];    /* disk size, human ("1.8 TiB") */
static int g_srv_pct;           /* disk usage percentage */
static char g_srv_version[40];  /* "v1.119.0" */

/* Texture recycling pool. Freeing a texture unmaps its memblock, and the
 * GPU side (notably Vita3K's texture cache, which re-reads guest memory of
 * cached textures at its own pace) may still touch it afterwards — freeing
 * evicted thumbnails crashes intermittently no matter how long the free is
 * deferred. So thumbnails are never freed while browsing: released textures
 * go into this pool and get reused for the next thumb with the same
 * dimensions+format (Immich previews come in a handful of sizes, so the hit
 * rate is high and the pool stays small). */
#define TEXPOOL_MAX 160
static vita2d_texture *g_texpool[TEXPOOL_MAX];
static int g_texpool_n;

static void tex_release(vita2d_texture *tex);
static vita2d_texture *tex_acquire(int w, int h, SceGxmTextureFormat fmt);

/* the sync thread asks the main thread to rebuild g_disp (which the main
 * thread also reads every frame) by setting this; one writer per side */
static volatile int g_need_rebuild;

/* sync thread -> sync overview screen (loose: status text only) */
static volatile int g_sync_phase; /* 0 idle, 1 hashing, 2 checking, 3 uploading */
static char g_sync_activity[160];
static volatile curl_off_t g_ul_now, g_ul_total;
static char g_sync_errlog[5][160];
static volatile int g_sync_errn;

static void log_line(const char *fmt, ...);

/* grow *pp to hold `need` elements of `elem` bytes, doubling from `cur` cap;
 * zero the freshly added tail (callers rely on NULL thumbs / zero state).
 * returns the new capacity, or 0 on allocation failure. */
static int grow_array(void **pp, int cur, int need, size_t elem)
{
	if (need <= cur)
		return cur;
	int cap = cur ? cur : 64;
	while (cap < need)
		cap *= 2;
	void *np = realloc(*pp, (size_t)cap * elem);
	if (!np)
		return 0;
	memset((char *)np + (size_t)cur * elem, 0, (size_t)(cap - cur) * elem);
	*pp = np;
	return cap;
}

#define GROW(arr, capvar, need) \
	grow_array((void **)&(arr), (capvar), (need), sizeof(*(arr)))

/* ensure the server-asset arrays hold at least `need` entries */
static int grow_assets(int need)
{
	int c = g_asset_cap;
	if (need <= c)
		return 1;
	if (!GROW(g_asset_ids, c, need) ||
	    !GROW(g_asset_dates, c, need) ||
	    !GROW(g_asset_ratio, c, need) ||
	    !GROW(g_asset_rot, c, need) ||
	    !GROW(g_asset_is_video, c, need) ||
	    !GROW(g_asset_local_backed, c, need) ||
	    !GROW(g_thumb, c, need) ||
	    !GROW(g_thumb_failed, c, need) ||
	    !(g_asset_cap = GROW(g_tex_err, c, need)))
		return 0;
	return 1;
}

/* ensure the local-media arrays hold at least `need` entries */
static int grow_locals(int need)
{
	int c = g_local_cap;
	if (need <= c)
		return 1;
	if (!GROW(g_local_path, c, need) ||
	    !GROW(g_local_date, c, need) ||
	    !GROW(g_local_sha1, c, need) ||
	    !GROW(g_local_server_id, c, need) ||
	    !GROW(g_local_err, c, need) ||
	    !GROW(g_local_mtime, c, need) ||
	    !GROW(g_local_ctime, c, need) ||
	    !GROW(g_local_size, c, need) ||
	    !GROW(g_local_is_video, c, need) ||
	    !GROW(g_local_state, c, need) ||
	    !GROW(g_local_thumb, c, need) ||
	    !(g_local_cap = GROW(g_local_thumb_failed, c, need)))
		return 0;
	return 1;
}

/* the merged timeline + its per-slot grid positions share one capacity */
static int grow_disp(int need)
{
	int c = g_disp_cap;
	if (need <= c)
		return 1;
	if (!GROW(g_disp, c, need) ||
	    !GROW(g_item_x, c, need) ||
	    !GROW(g_item_w, c, need) ||
	    !GROW(g_item_h, c, need) ||
	    !(g_disp_cap = GROW(g_item_y, c, need)))
		return 0;
	return 1;
}

static int grow_sect(int need)
{
	if (need <= g_sect_cap)
		return 1;
	int cap = GROW(g_sect, g_sect_cap, need);
	if (!cap)
		return 0;
	g_sect_cap = cap;
	return 1;
}

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

/* one Overpass instance per pixel size: vita2d's glyph atlas caches every
 * glyph at the first size it is drawn at and reuses that bitmap for other
 * sizes, so mixing sizes in one font renders letters at the wrong size (an
 * "E" cached at 20 px showing up oversized inside 17 px text). */
static vita2d_font *ttf_for(unsigned int px)
{
	static vita2d_font *cache[8];
	static unsigned int cache_px[8];
	static int n;

	for (int i = 0; i < n; i++)
		if (cache_px[i] == px)
			return cache[i];
	if (n < 8) {
		vita2d_font *f = vita2d_load_font_file("app0:font.ttf");
		if (f) {
			cache[n] = f;
			cache_px[n] = px;
			n++;
			return f;
		}
	}
	return g_ttf; /* shared instance, better than nothing */
}

/* text via the bundled Overpass TTF (Immich's typeface), falling back to
 * the system PGF font if the TTF failed to load. the pgf "scale" the call
 * sites use maps to a pixel size (1.0 ~ 20 px). */
static void draw_text(float x, float y, uint32_t color, float scale,
		      const char *text)
{
	if (g_ttf) {
		unsigned int px = (unsigned int)(scale * 20.0f + 0.5f);
		vita2d_font_draw_text(ttf_for(px), (int)x, (int)y, color,
				      px, text);
	} else {
		vita2d_pgf_draw_text(g_font, x, y, color, scale, text);
	}
}

static int text_width(float scale, const char *text)
{
	if (g_ttf) {
		unsigned int px = (unsigned int)(scale * 20.0f + 0.5f);
		return vita2d_font_text_width(ttf_for(px), px, text);
	}
	return vita2d_pgf_text_width(g_font, scale, text);
}

static void draw_centered(int y, uint32_t color, const char *text)
{
	int w = text_width(1.0f, text);
	draw_text((SCREEN_W - w) / 2, y, color, 1.0f, text);
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

	log_line("FATAL: %s | %s", buf, detail ? detail : "-");

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
				draw_text(40, y, RGBA8(200, 200, 200, 255), 1.0f, line);
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
			fputs("server=http://192.168.1.100:2283\n"
			      "apikey=PASTE_YOUR_IMMICH_API_KEY_HERE\n"
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

	if (!g_server[0] || !g_apikey[0] || strstr(g_apikey, "PASTE_YOUR"))
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
			net_lock(); /* TLS is single-threaded, see g_net_mutex */
			res = curl_easy_perform(curl);
			net_unlock();
			if (res == CURLE_OK)
				curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
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

/* ------------------------------------------------------------------ */
/* merged display model (server + local items)                         */
/* ------------------------------------------------------------------ */

static const char *disp_date(int d)
{
	struct disp_item *it = &g_disp[d];
	return it->src == SRC_LOCAL ? g_local_date[it->idx]
				    : g_asset_dates[it->idx];
}

static int disp_is_video(int d)
{
	struct disp_item *it = &g_disp[d];
	return it->src == SRC_LOCAL ? g_local_is_video[it->idx]
				    : g_asset_is_video[it->idx];
}

static vita2d_texture *disp_thumb(int d)
{
	struct disp_item *it = &g_disp[d];
	return it->src == SRC_LOCAL ? g_local_thumb[it->idx]
				    : g_thumb[it->idx];
}

static int disp_thumb_failed(int d)
{
	struct disp_item *it = &g_disp[d];
	return it->src == SRC_LOCAL ? g_local_thumb_failed[it->idx]
				    : g_thumb_failed[it->idx];
}

/* request a thumb for anything not yet loaded or failed; local videos get a
 * poster frame extracted on the worker thread, same as photos */
static int disp_wants_thumb(int d)
{
	return !disp_thumb(d) && !disp_thumb_failed(d);
}

/* is some server asset id present among the fetched assets? */
static int server_has_id(const char *id)
{
	if (!id[0])
		return 0;
	for (int i = 0; i < g_asset_count; i++)
		if (!strcmp(g_asset_ids[i], id))
			return 1;
	return 0;
}

static int disp_cmp(const void *a, const void *b)
{
	const struct disp_item *x = a, *y = b;
	const char *dx = x->src == SRC_LOCAL ? g_local_date[x->idx]
					     : g_asset_dates[x->idx];
	const char *dy = y->src == SRC_LOCAL ? g_local_date[y->idx]
					     : g_asset_dates[y->idx];
	return strcmp(dy, dx); /* newest first */
}

/* turn a sortable "YYYY-MM-..." date into a "Month YYYY" section title */
static void month_label(const char *date, char *out, size_t len)
{
	static const char *const mon[] = {
		"", "January", "February", "March", "April", "May", "June",
		"July", "August", "September", "October", "November", "December"
	};
	int y = 0, m = 0;
	if (strlen(date) >= 7) {
		y = (date[0] - '0') * 1000 + (date[1] - '0') * 100 +
		    (date[2] - '0') * 10 + (date[3] - '0');
		m = (date[5] - '0') * 10 + (date[6] - '0');
	}
	if (m < 1 || m > 12)
		snprintf(out, len, "Unknown date");
	else
		snprintf(out, len, "%s %d", mon[m], y);
}

/* display aspect ratio (w/h) of a slot; local camera media and assets the
 * server reported no exif for fall back to 4:3 */
static float disp_ratio(int d)
{
	struct disp_item *it = &g_disp[d];
	float r = (it->src == SRC_SERVER) ? g_asset_ratio[it->idx] : 0.0f;
	return r > 0.0f ? r : 4.0f / 3.0f;
}

/* place row items [start, end) at *y. when justifying, the whole row is
 * scaled so it spans the full screen width (the row height grows with it,
 * capped so a sparse row can't blow up); a month's trailing partial row
 * stays at its natural size, left-aligned. */
static void layout_finish_row(int start, int end, float *y, float natw,
			      int justify)
{
	float scale = 1.0f;
	if (justify && natw > 0.0f) {
		scale = (float)SCREEN_W / natw;
		if (scale > 1.55f)
			scale = 1.55f;
	}
	float h = ROW_H * scale;
	float x = 0.0f;
	for (int i = start; i < end; i++) {
		g_item_x[i] = x;
		g_item_y[i] = *y;
		g_item_w[i] *= scale;
		g_item_h[i] = h;
		x += g_item_w[i];
	}
	*y += h;
}

/* compute each display slot's grid cell + the month/year header bands.
 * justified rows like the Immich web timeline: each item's width follows
 * its aspect ratio, a row wraps when the next item no longer fits, and the
 * closed row is stretched to fill the full screen width. the timeline is
 * date-desc sorted, so a run of equal "YYYY-MM" is one month; every month
 * starts on a fresh row under its own header. */
static void layout_grid(void)
{
	g_sect_count = 0;
	/* start below the pinned search bar so the first month header clears
	 * it at the top of the scroll range */
	float y = SEARCH_H, natw = 0.0f;
	int row_start = 0;
	char curkey[8] = "";

	for (int d = 0; d < g_disp_count; d++) {
		const char *date = disp_date(d);
		char key[8];
		snprintf(key, sizeof(key), "%.7s", date); /* YYYY-MM */

		if (strcmp(key, curkey) != 0) {
			if (d > row_start) /* month's partial last row */
				layout_finish_row(row_start, d, &y, natw, 0);
			row_start = d;
			natw = 0.0f;
			if (grow_sect(g_sect_count + 1)) {
				g_sect[g_sect_count].y = y;
				month_label(date, g_sect[g_sect_count].label,
					    sizeof(g_sect[0].label));
				g_sect_count++;
			}
			y += HEADER_H;
			snprintf(curkey, sizeof(curkey), "%s", key);
		}

		float w = ROW_H * disp_ratio(d);
		if (w < ROW_H * 0.4f)
			w = ROW_H * 0.4f;  /* keep extreme portraits tappable */
		if (w > SCREEN_W)
			w = SCREEN_W;      /* panoramas: one per row */
		if (natw > 0.0f && natw + w > SCREEN_W) {
			layout_finish_row(row_start, d, &y, natw, 1);
			row_start = d;
			natw = 0.0f;
		}
		g_item_w[d] = w; /* natural width; scaled when the row closes */
		natw += w;
	}
	if (g_disp_count > row_start)
		layout_finish_row(row_start, g_disp_count, &y, natw, 0);
	g_content_h = y;
}

/* rebuild the merged, date-desc display order. main-thread only (the grid
 * reads g_disp every frame). a backed-up local file that matches a fetched
 * server asset is shown once, as the server asset (green badge). */
static void rebuild_display(void)
{
	/* search mode: the merged timeline is replaced by the result set
	 * (server assets only, date-grouped); local media isn't folded in */
	if (g_search_active) {
		if (!grow_disp(g_search_count > 0 ? g_search_count : 1))
			return;
		int n = 0;
		for (int k = 0; k < g_search_count; k++) {
			g_disp[n].src = SRC_SERVER;
			g_disp[n].idx = g_search_idx[k];
			n++;
		}
		g_disp_count = n;
		qsort(g_disp, n, sizeof(g_disp[0]), disp_cmp);
		layout_grid();
		return;
	}

	for (int i = 0; i < g_asset_count; i++)
		g_asset_local_backed[i] = 0;

	if (!grow_disp(g_asset_count + g_local_count))
		return; /* keep the previous layout if we can't size the new one */

	int n = 0;
	for (int i = 0; i < g_asset_count; i++) {
		g_disp[n].src = SRC_SERVER;
		g_disp[n].idx = i;
		n++;
	}
	for (int j = 0; j < g_local_count; j++) {
		if (g_local_state[j] == SYNC_BACKED_UP &&
		    server_has_id(g_local_server_id[j])) {
			/* fold into the matching server cell */
			for (int i = 0; i < g_asset_count; i++)
				if (!strcmp(g_asset_ids[i], g_local_server_id[j])) {
					g_asset_local_backed[i] = 1;
					break;
				}
			continue;
		}
		g_disp[n].src = SRC_LOCAL;
		g_disp[n].idx = j;
		n++;
	}
	g_disp_count = n;
	qsort(g_disp, n, sizeof(g_disp[0]), disp_cmp);
	layout_grid();
}

static int find_disp(unsigned char src, int idx);

/* rebuild the display after the asset set changed, keeping the view glued
 * to the photo the selection is on: the selection follows the item, and the
 * scroll/target keep their offset relative to it, so a page fetch or poll
 * relayout doesn't visibly move the grid. */
static void rebuild_keep_view(int *sel, float *scroll, float *target)
{
	int had = (*sel >= 0 && *sel < g_disp_count);
	struct disp_item keep = had ? g_disp[*sel] : (struct disp_item){ 0, 0 };
	float dt = had ? *target - g_item_y[*sel] : 0.0f;
	float ds = had ? *scroll - g_item_y[*sel] : 0.0f;
	rebuild_display();
	if (had) {
		int ns = find_disp(keep.src, keep.idx);
		if (ns >= 0) {
			*sel = ns;
			*target = g_item_y[ns] + dt;
			*scroll = g_item_y[ns] + ds;
		}
	}
	if (*sel >= g_disp_count)
		*sel = g_disp_count - 1;
	if (*sel < 0)
		*sel = 0;
}

/* first item of the month-run after (dir>0, older) or before (dir<0, newer)
 * the one containing display slot d; the timeline is "YYYY-MM"-grouped and
 * date-descending, so a month is a contiguous run of equal 7-char prefixes */
static int month_jump(int d, int dir)
{
	if (g_disp_count == 0)
		return 0;
	if (d < 0)
		d = 0;
	if (d >= g_disp_count)
		d = g_disp_count - 1;
	const char *cur = disp_date(d);
	int i = d;
	if (dir > 0) {
		while (i < g_disp_count - 1) {
			i++;
			if (strncmp(disp_date(i), cur, 7))
				return i; /* first item of the next month */
		}
		return g_disp_count - 1; /* already in the last month */
	}
	/* back to the start of the current month-run */
	while (i > 0 && !strncmp(disp_date(i - 1), cur, 7))
		i--;
	if (i == 0)
		return 0;
	/* then to the start of the previous (newer) month-run */
	const char *prev = disp_date(i - 1);
	i--;
	while (i > 0 && !strncmp(disp_date(i - 1), prev, 7))
		i--;
	return i;
}

/* display slot whose grid cell contains the (screen-x, world-y) point,
 * or -1: touch hit-testing for tap-to-open */
static int item_at(float x, float wy)
{
	for (int i = 0; i < g_disp_count; i++) {
		if (g_item_y[i] > wy)
			break; /* item y is non-decreasing */
		if (wy < g_item_y[i] + g_item_h[i] &&
		    x >= g_item_x[i] && x < g_item_x[i] + g_item_w[i])
			return i;
	}
	return -1;
}

/* move the selection one row up (dir<0) or down (dir>0), landing on the
 * item whose cell is horizontally nearest — rows hold a variable number
 * of items now, so the fixed columns arithmetic no longer applies */
static int nav_row(int sel, int dir)
{
	if (sel < 0 || sel >= g_disp_count)
		return sel;
	float cy = g_item_y[sel];
	float cx = g_item_x[sel] + g_item_w[sel] / 2.0f;
	int i = sel;
	/* step off the current row */
	while (i + dir >= 0 && i + dir < g_disp_count &&
	       g_item_y[i] == cy)
		i += dir;
	if (g_item_y[i] == cy)
		return sel; /* already on the first/last row */
	/* pick the horizontally nearest item of that row */
	float ry = g_item_y[i];
	int best = i;
	float bestd = -1.0f;
	for (; i >= 0 && i < g_disp_count && g_item_y[i] == ry; i += dir) {
		float d = g_item_x[i] + g_item_w[i] / 2.0f - cx;
		if (d < 0)
			d = -d;
		if (bestd < 0 || d < bestd) {
			bestd = d;
			best = i;
		}
	}
	return best;
}

/* display slot nearest a world-y (used to keep the selection inside the
 * viewport while a touch drag/fling moves the grid) */
static int item_near(float wy)
{
	int last = -1;
	for (int i = 0; i < g_disp_count; i++) {
		if (g_item_y[i] + g_item_h[i] > wy)
			return i;
		last = i;
	}
	return last;
}

/* find the display slot for a given source item (after a rebuild reorders
 * things, to keep the selection on the same photo) */
static int find_disp(unsigned char src, int idx)
{
	for (int i = 0; i < g_disp_count; i++)
		if (g_disp[i].src == src && g_disp[i].idx == idx)
			return i;
	return -1;
}

/* pick the most useful thumbnail to load next: selection, then visible,
 * then prefetch a couple of rows below and above the viewport */
static int pick_next_load(int sel, int first_vis, int last_vis)
{
	if (sel >= 0 && sel < g_disp_count && disp_wants_thumb(sel))
		return sel;
	for (int i = first_vis; i >= 0 && i <= last_vis && i < g_disp_count; i++)
		if (disp_wants_thumb(i))
			return i;
	for (int i = last_vis + 1; i <= last_vis + COLS && i < g_disp_count; i++)
		if (i >= 0 && disp_wants_thumb(i))
			return i;
	for (int i = first_vis - 1; i >= first_vis - COLS && i >= 0; i--)
		if (disp_wants_thumb(i))
			return i;
	return -1;
}

/* hand the worker a thumbnail request for display slot d (g_req_state must
 * be REQ_IDLE); local videos get their poster extracted */
static void req_issue_thumb(int d)
{
	struct disp_item *ti = &g_disp[d];
	g_req_idx = ti->idx;
	g_req_src = ti->src;
	g_req_detail = 0;
	if (ti->src == SRC_LOCAL) {
		snprintf(g_req_path, sizeof(g_req_path), "%s",
			 g_local_path[ti->idx]);
		g_req_is_video = g_local_is_video[ti->idx];
	} else {
		snprintf(g_req_id, sizeof(g_req_id), "%s",
			 g_asset_ids[ti->idx]);
		g_req_is_video = 0;
	}
	__sync_synchronize();
	g_req_state = REQ_PENDING;
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

/* the largest pan offset (px) that keeps the scaled image covering the screen
 * edge it's panned away from; 0 when the image is smaller than the screen */
static float pan_limit(float scaled, float screen)
{
	float lim = (scaled - screen) / 2.0f;
	return lim > 0.0f ? lim : 0.0f;
}

/* draw the detail image fit-to-screen, then scaled by `zoom` about the centre
 * and shifted by (panx, pany). pan is clamped to keep the image on screen. */
static void draw_texture_zoom(vita2d_texture *tex, float zoom,
			      float *panx, float *pany)
{
	float w = vita2d_texture_get_width(tex);
	float h = vita2d_texture_get_height(tex);
	float fit = (SCREEN_W / w < SCREEN_H / h) ? SCREEN_W / w : SCREEN_H / h;
	float s = fit * zoom;
	float dw = w * s, dh = h * s;

	float lx = pan_limit(dw, SCREEN_W), ly = pan_limit(dh, SCREEN_H);
	if (*panx > lx) *panx = lx;
	if (*panx < -lx) *panx = -lx;
	if (*pany > ly) *pany = ly;
	if (*pany < -ly) *pany = -ly;

	vita2d_draw_texture_scale(tex, (SCREEN_W - dw) / 2.0f + *panx,
				  (SCREEN_H - dh) / 2.0f + *pany, s, s);
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

/* ---- Immich-style top search bar ----------------------------------- */

/* an anti-aliased filled rounded rectangle, baked once into a white alpha
 * mask texture (4x4 supersampled corner coverage) and drawn tinted to the
 * wanted colour. vita2d has no AA primitives, so the coverage mask + the
 * texture's bilinear sampling are what soften the corners. */
static vita2d_texture *rounded_mask_tex(int w, int h, float r)
{
	enum { NSLOT = 6 };  /* a small fixed set of shapes (pill, buttons, ...) */
	static vita2d_texture *cache[NSLOT];
	static int cw[NSLOT], ch[NSLOT];
	static float cr[NSLOT];
	static int n;
	for (int i = 0; i < n; i++)
		if (cw[i] == w && ch[i] == h && cr[i] == r)
			return cache[i];
	if (n >= NSLOT)
		return NULL;
	vita2d_texture *t = vita2d_create_empty_texture_format(w, h,
		SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
	if (!t)
		return NULL;
	uint32_t *data = vita2d_texture_get_datap(t);
	int stride = (int)(vita2d_texture_get_stride(t) / 4); /* pixels/row */
	memset(data, 0, (size_t)stride * h * 4); /* keep row padding transparent */
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int hits = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++) {
					float px = x + (sx + 0.5f) / 4.0f;
					float py = y + (sy + 0.5f) / 4.0f;
					/* nearest point of the corner-centre
					 * rectangle inset by r */
					float qx = px < r ? r :
						(px > w - r ? w - r : px);
					float qy = py < r ? r :
						(py > h - r ? h - r : py);
					float dx = px - qx, dy = py - qy;
					if (dx * dx + dy * dy <= r * r)
						hits++;
				}
			data[y * stride + x] = RGBA8(255, 255, 255,
						     hits * 255 / 16);
		}
	}
	cache[n] = t;
	cw[n] = w;
	ch[n] = h;
	cr[n] = r;
	n++;
	return t;
}

/* ---- AA glyph icons (magnifier / map pin / cloud) -------------------- */
/* each is a coverage predicate over icon-local pixel coords (0..s); bake_icon
 * supersamples it 4x4 into a white alpha mask, drawn tinted to the wanted
 * colour. one cached texture per icon (called at a fixed size each frame). */

static float seg_dist(float px, float py, float ax, float ay,
		      float bx, float by)
{
	float dx = bx - ax, dy = by - ay;
	float l2 = dx * dx + dy * dy;
	float t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
	if (t < 0) t = 0;
	if (t > 1) t = 1;
	float qx = ax + dx * t, qy = ay + dy * t;
	return sqrtf((px - qx) * (px - qx) + (py - qy) * (py - qy));
}

static int in_tri(float px, float py, float ax, float ay, float bx, float by,
		  float cx, float cy)
{
	float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
	float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
	float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
	int neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
	int pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
	return !(neg && pos);
}

/* shared glyph line thickness (pixels), so every outline reads the same */
#define GLYPH_W 3.0f

/* a thin ring outline with a straight, round-capped handle off the lower right */
static int cov_search(float px, float py, int s)
{
	float R = s * 0.30f, W = GLYPH_W, hw = W * 0.5f, c45 = 0.70710678f;
	float ccx = s * 0.40f, ccy = s * 0.40f;
	float dr = sqrtf((px - ccx) * (px - ccx) + (py - ccy) * (py - ccy));
	int ring = (dr <= R && dr >= R - W);
	float ax = ccx + c45 * (R - hw), ay = ccy + c45 * (R - hw);
	float bx = ccx + c45 * (R + s * 0.42f), by = ccy + c45 * (R + s * 0.42f);
	return ring || seg_dist(px, py, ax, ay, bx, by) <= hw;
}

/* a filled map marker: a round head (with a hole) tapering to a point */
static int cov_map(float px, float py, int s)
{
	float cx = s * 0.5f, hcy = s * 0.40f, Rp = s * 0.30f, tipY = s * 0.95f;
	float dHead = sqrtf((px - cx) * (px - cx) + (py - hcy) * (py - hcy));
	int solid = (dHead <= Rp) ||
		    in_tri(px, py, cx - Rp * 0.78f, hcy + Rp * 0.30f,
			   cx + Rp * 0.78f, hcy + Rp * 0.30f, cx, tipY);
	return solid && dHead >= Rp * 0.42f;   /* punch the hole */
}

/* a filled cloud: three lobes tangent to a flat bottom (rounded sides), with
 * a small body rectangle filling between the side lobes. all kept well inside
 * the icon bounds so nothing clips at the edge. */
static int cov_cloud(float px, float py, int s)
{
	float yb = s * 0.68f;   /* flat bottom */
	int mid = sqrtf((px - s * 0.50f) * (px - s * 0.50f) +
			(py - s * 0.42f) * (py - s * 0.42f)) <= s * 0.22f;
	int lp = sqrtf((px - s * 0.34f) * (px - s * 0.34f) +
		       (py - s * 0.52f) * (py - s * 0.52f)) <= s * 0.16f;
	int rp = sqrtf((px - s * 0.66f) * (px - s * 0.66f) +
		       (py - s * 0.52f) * (py - s * 0.52f)) <= s * 0.16f;
	int base = (px >= s * 0.34f && px <= s * 0.66f &&
		    py >= s * 0.52f && py <= yb);
	return mid || lp || rp || base;
}

static vita2d_texture *bake_icon(int s, int (*inside)(float, float, int))
{
	vita2d_texture *t = vita2d_create_empty_texture_format(s, s,
		SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
	if (!t)
		return NULL;
	uint32_t *data = vita2d_texture_get_datap(t);
	int stride = (int)(vita2d_texture_get_stride(t) / 4);
	memset(data, 0, (size_t)stride * s * 4);
	for (int y = 0; y < s; y++)
		for (int x = 0; x < s; x++) {
			int hits = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++)
					hits += inside(x + (sx + 0.5f) / 4.0f,
						       y + (sy + 0.5f) / 4.0f, s);
			data[y * stride + x] = RGBA8(255, 255, 255,
						     hits * 255 / 16);
		}
	return t;
}

/* PlayStation triangle button glyph: an up-pointing outline, fixed-width edges */
static int cov_tri(float px, float py, int s)
{
	float ax = s * 0.5f, ay = s * 0.10f;   /* lifted up a bit */
	float bx = s * 0.14f, by = s * 0.80f;
	float cx = s * 0.86f, cy = s * 0.80f;
	if (!in_tri(px, py, ax, ay, bx, by, cx, cy))
		return 0;
	float d = fminf(seg_dist(px, py, ax, ay, bx, by),
		  fminf(seg_dist(px, py, bx, by, cx, cy),
			seg_dist(px, py, cx, cy, ax, ay)));
	return d <= GLYPH_W;   /* band just inside the edges */
}

/* PlayStation square button glyph: an outline square, fixed-width border */
static int cov_sq(float px, float py, int s)
{
	float lo = s * 0.18f, hi = s * 0.82f;
	int outer = px >= lo && px <= hi && py >= lo && py <= hi;
	int inner = px >= lo + GLYPH_W && px <= hi - GLYPH_W &&
		    py >= lo + GLYPH_W && py <= hi - GLYPH_W;
	return outer && !inner;
}

/* PlayStation cross button glyph: a thick X — two rotated rectangles, so the
 * arm ends are flat and perpendicular (sharp), not box-clipped or rounded. */
static int cov_cross(float px, float py, int s)
{
	float hw = GLYPH_W * 0.5f;        /* half stroke */
	float half = s * 0.30f;           /* arm half-length */
	float dx = px - s * 0.5f, dy = py - s * 0.5f;
	float a = (dx + dy) * 0.70710678f;   /* along one diagonal */
	float b = (dx - dy) * 0.70710678f;   /* along the other */
	return (fabsf(b) <= hw && fabsf(a) <= half) ||
	       (fabsf(a) <= hw && fabsf(b) <= half);
}

/* PlayStation circle button glyph: an outline ring, fixed-width stroke */
static int cov_circle(float px, float py, int s)
{
	float cx = s * 0.5f, cy = s * 0.5f, R = s * 0.38f, W = GLYPH_W;
	float d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
	return d <= R && d >= R - W;
}

/* cache one texture per glyph; all are drawn at a fixed size each frame */
static vita2d_texture *icon_tex(int which, int s)
{
	static vita2d_texture *cache[7];
	static int cs[7];
	if (which < 0 || which > 6)
		return NULL;
	if (cache[which] && cs[which] == s)
		return cache[which];
	if (cache[which]) {
		vita2d_wait_rendering_done();
		vita2d_free_texture(cache[which]);
		cache[which] = NULL;
	}
	int (*fn)(float, float, int) = which == 0 ? cov_search :
				       which == 1 ? cov_map :
				       which == 2 ? cov_cloud :
				       which == 3 ? cov_tri :
				       which == 4 ? cov_sq :
				       which == 5 ? cov_cross : cov_circle;
	cache[which] = bake_icon(s, fn);
	cs[which] = s;
	return cache[which];
}
#define ICON_SEARCH 0
#define ICON_MAP    1
#define ICON_CLOUD  2
#define ICON_TRI    3
#define ICON_SQUARE 4
#define ICON_CROSS  5
#define ICON_CIRCLE 6

/* a DualShock-style face button: a dark disc (subtle lighter rim) centred at
 * (cx,cy) with the coloured glyph on top. each glyph keeps a per-call size. */
/* an anti-aliased filled disc baked into a white alpha mask (4x4 supersampled),
 * drawn tinted; replaces aliased vita2d_draw_fill_circle for UI discs */
static vita2d_texture *disc_tex(int d)
{
	enum { NDISC = 8 };
	static vita2d_texture *cache[NDISC];
	static int cd[NDISC], n;
	for (int i = 0; i < n; i++)
		if (cd[i] == d)
			return cache[i];
	if (n >= NDISC)
		return NULL;
	vita2d_texture *t = vita2d_create_empty_texture_format(d, d,
		SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
	if (!t)
		return NULL;
	uint32_t *data = vita2d_texture_get_datap(t);
	int stride = (int)(vita2d_texture_get_stride(t) / 4);
	memset(data, 0, (size_t)stride * d * 4);
	float c = d / 2.0f, r = d / 2.0f;
	for (int y = 0; y < d; y++)
		for (int x = 0; x < d; x++) {
			int hits = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++) {
					float dx = x + (sx + 0.5f) / 4.0f - c;
					float dy = y + (sy + 0.5f) / 4.0f - c;
					if (dx * dx + dy * dy <= r * r)
						hits++;
				}
			data[y * stride + x] = RGBA8(255, 255, 255, hits * 255 / 16);
		}
	cache[n] = t;
	cd[n] = d;
	n++;
	return t;
}

/* a filled (AA) disc at (cx,cy), tinted */
static void draw_disc(float cx, float cy, float d, uint32_t col)
{
	vita2d_texture *t = disc_tex((int)(d + 0.5f));
	if (t)
		vita2d_draw_texture_tint(t, cx - (int)(d + 0.5f) / 2.0f,
					 cy - (int)(d + 0.5f) / 2.0f, col);
	else
		vita2d_draw_fill_circle(cx, cy, d / 2.0f, col);
}

static void draw_ps_button(float cx, float cy, float d, int icon,
			   uint32_t tint, int gs)
{
	draw_disc(cx, cy, d, RGBA8(46, 48, 56, 255));   /* flat dark face */
	vita2d_texture *g = icon_tex(icon, gs);
	if (g)
		vita2d_draw_texture_tint(g, cx - gs / 2.0f, cy - gs / 2.0f, tint);
}

/* pixel geometry of the search pill + the two buttons on its right, shared
 * by the drawer and the tap test */
#define BAR_M  12.0f                       /* left/right margin */
#define BAR_Y  8.0f                        /* top of the pill */
#define BAR_H  40.0f                       /* pill height */
#define BAR_R  12.0f                       /* corner radius */
#define BTN_SZ 40.0f                       /* button (rounded square) size */
#define BTN_GAP 10.0f                      /* gap between the two buttons */
#define BTN2_CX ((float)SCREEN_W - BAR_M - BTN_SZ / 2)   /* cloud (rightmost) */
#define BTN1_CX (BTN2_CX - BTN_SZ - BTN_GAP)             /* map */
#define BTN_CY  (BAR_Y + BAR_H / 2)
#define BAR_W  (BTN1_CX - BTN_SZ / 2 - BTN_GAP - BAR_M)  /* pill width */
#define BAR_CLEAR_CX (BAR_M + BAR_W - 22.0f) /* centre of the clear chip */

/* one future-feature button: rounded-square background + a centred AA glyph */
static void draw_bar_button(float cx, float cy, int icon)
{
	vita2d_texture *bg = rounded_mask_tex((int)BTN_SZ, (int)BTN_SZ, 11.0f);
	if (bg)
		vita2d_draw_texture_tint(bg, cx - BTN_SZ / 2, cy - BTN_SZ / 2,
					 RGBA8(46, 46, 52, 255));
	int isz = 26;
	vita2d_texture *g = icon_tex(icon, isz);
	if (g)
		vita2d_draw_texture_tint(g, cx - isz / 2.0f, cy - isz / 2.0f,
					 RGBA8(190, 190, 196, 255));
}

/* draw the bar shifted vertically by `yoff` (Square slides it out of view) */
static void draw_search_bar(float yoff)
{
	unsigned int field = RGBA8(38, 38, 42, 255);
	float by = BAR_Y + yoff, cy = BTN_CY + yoff;
	/* opaque strip so grid items scroll cleanly underneath the bar */
	vita2d_draw_rectangle(0, yoff, SCREEN_W, SEARCH_H, RGBA8(16, 16, 16, 255));
	/* borderless field: an AA rounded-rect mask tinted to the field colour */
	vita2d_texture *mask = rounded_mask_tex((int)BAR_W, (int)BAR_H, BAR_R);
	if (mask)
		vita2d_draw_texture_tint(mask, BAR_M, by, field);
	else
		vita2d_draw_rectangle(BAR_M, by, BAR_W, BAR_H, field);

	int isz = 24;
	vita2d_texture *icon = icon_tex(ICON_SEARCH, isz);
	if (icon)
		vita2d_draw_texture_tint(icon, BAR_M + 22 - isz / 2.0f,
					 cy - isz / 2.0f,
					 RGBA8(190, 190, 196, 255));

	/* placeholder buttons (map, cloud) for future features */
	draw_bar_button(BTN1_CX, cy, ICON_MAP);
	draw_bar_button(BTN2_CX, cy, ICON_CLOUD);

	float tx = BAR_M + 44, ty = by + BAR_H - 13;
	if (g_search_active && g_search_query[0]) {
		char q[64];
		snprintf(q, sizeof(q), "%.40s", g_search_query);
		draw_text(tx, ty, RGBA8(235, 235, 240, 255), 0.95f, q);
		/* press O (or tap here) to clear: just the red ring glyph, no
		 * button disc/border on the pill */
		{
			int gs = 26;
			vita2d_texture *og = icon_tex(ICON_CIRCLE, gs);
			if (og)
				vita2d_draw_texture_tint(og,
					BAR_CLEAR_CX - gs / 2.0f, cy - gs / 2.0f,
					RGBA8(235, 90, 85, 255));
		}
	} else {
		draw_text(tx, ty, RGBA8(140, 140, 148, 255), 0.95f,
			  "Search your photos");
	}
}

/* minimal UTF-8 <-> UTF-16 (BMP) for the on-screen keyboard buffers */
static void utf8_to_utf16(const char *s, uint16_t *out, int outcap)
{
	int n = 0;
	while (*s && n < outcap - 1) {
		unsigned char c = (unsigned char)s[0];
		unsigned int cp;
		if (c < 0x80) {
			cp = c; s += 1;
		} else if ((c >> 5) == 0x6 && (s[1] & 0xC0) == 0x80) {
			cp = ((c & 0x1F) << 6) | (s[1] & 0x3F); s += 2;
		} else if ((c >> 4) == 0xE && (s[1] & 0xC0) == 0x80 &&
			   (s[2] & 0xC0) == 0x80) {
			cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) |
			     (s[2] & 0x3F); s += 3;
		} else {
			cp = '?'; s += 1;
		}
		out[n++] = (uint16_t)(cp > 0xFFFF ? '?' : cp);
	}
	out[n] = 0;
}

static void utf16_to_utf8(const uint16_t *s, char *out, int outcap)
{
	int n = 0;
	for (; *s; s++) {
		unsigned int cp = *s;
		if (cp < 0x80) {
			if (n + 1 >= outcap) break;
			out[n++] = (char)cp;
		} else if (cp < 0x800) {
			if (n + 2 >= outcap) break;
			out[n++] = (char)(0xC0 | (cp >> 6));
			out[n++] = (char)(0x80 | (cp & 0x3F));
		} else {
			if (n + 3 >= outcap) break;
			out[n++] = (char)(0xE0 | (cp >> 12));
			out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[n++] = (char)(0x80 | (cp & 0x3F));
		}
	}
	out[n] = '\0';
}

/* bring up the system on-screen keyboard (a common dialog). returns 1 and
 * fills `out` (UTF-8) when the user confirms, 0 if they cancelled. blocks,
 * driving the dialog each frame; the worker thread keeps running, which is
 * fine since we touch no shared arrays here. */
static int ime_input(const char *title, const char *initial,
		     char *out, size_t outsz)
{
	static uint16_t title16[SCE_IME_DIALOG_MAX_TITLE_LENGTH];
	static uint16_t init16[129];
	static uint16_t buf16[129];
	utf8_to_utf16(title, title16, SCE_IME_DIALOG_MAX_TITLE_LENGTH);
	utf8_to_utf16(initial ? initial : "", init16, 129);
	memset(buf16, 0, sizeof(buf16));

	SceImeDialogParam p;
	sceImeDialogParamInit(&p);
	p.supportedLanguages = 0;            /* allow all installed languages */
	p.languagesForced = 0;
	p.type = SCE_IME_TYPE_DEFAULT;
	p.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
	p.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
	p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
	p.title = title16;
	p.maxTextLength = 128;
	p.initialText = init16;
	p.inputTextBuffer = buf16;
	p.enterLabel = SCE_IME_ENTER_LABEL_SEARCH;

	if (sceImeDialogInit(&p) < 0)
		return 0;

	int confirmed = 0;
	for (;;) {
		vita2d_start_drawing();
		vita2d_clear_screen();
		vita2d_end_drawing();
		vita2d_common_dialog_update();
		vita2d_swap_buffers();
		sceDisplayWaitVblankStart();

		if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
			continue;
		SceImeDialogResult res;
		memset(&res, 0, sizeof(res));
		sceImeDialogGetResult(&res);
		if (res.button == SCE_IME_DIALOG_BUTTON_ENTER) {
			utf16_to_utf8(buf16, out, (int)outsz);
			confirmed = 1;
		}
		sceImeDialogTerm();
		break;
	}
	return confirmed;
}

/* run an Immich smart (CLIP) search and switch the grid to its results.
 * main thread only: it appends any newly-seen assets to the g_asset_* arrays
 * (like fetch_page) and rebuilds g_disp from the result set. returns the
 * number of distinct results shown (>=0), or -1 on a request/parse failure. */
static int run_smart_search(const char *query)
{
	char esc[256]; /* JSON-escape the query into the request body */
	int e = 0;
	for (const char *p = query; *p && e < (int)sizeof(esc) - 2; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == '"' || c == '\\') {
			esc[e++] = '\\';
			esc[e++] = (char)c;
		} else if (c >= 0x20) {
			esc[e++] = (char)c;
		} else {
			esc[e++] = ' ';
		}
	}
	esc[e] = '\0';

	char url[600];
	snprintf(url, sizeof(url), "%s/api/search/smart", g_server);
	char body[320];
	/* withExif so results carry width/height — without it every asset has
	 * no aspect ratio and the justified grid collapses to uniform columns */
	snprintf(body, sizeof(body), "{\"query\":\"%s\",\"withExif\":true}", esc);

	membuf buf;
	long code;
	CURLcode res = http_request(url, body, &buf, &code, NULL, 0);
	if (res != CURLE_OK || code < 200 || code >= 300) {
		log_line("smart search: curl %d http %ld: %.160s", res, code,
			 buf.data ? buf.data : "");
		free(buf.data);
		return -1;
	}

	char err[160];
	int before = g_asset_count;
	int added = parse_assets(buf.data, buf.size, err, sizeof(err));
	free(buf.data);
	if (added < 0) {
		log_line("smart search parse: %s", err);
		return -1;
	}

	/* the freshly-parsed tail [before, count) is the result set in server
	 * order. capture the ids before dedup compacts that tail around, then
	 * resolve each id to its surviving index in g_asset_*. */
	int raw = g_asset_count - before;
	static char (*ids)[40];
	static int ids_cap;
	if (raw > ids_cap) {
		void *np = realloc(ids, sizeof(ids[0]) * raw);
		if (!np)
			return -1;
		ids = np;
		ids_cap = raw;
	}
	for (int i = 0; i < raw; i++)
		memcpy(ids[i], g_asset_ids[before + i], sizeof(ids[0]));

	dedup_new_assets(before);

	if (!(g_search_cap = GROW(g_search_idx, g_search_cap,
				  raw > 0 ? raw : 1)))
		return -1;
	g_search_count = 0;
	for (int i = 0; i < raw; i++) {
		int idx = -1;
		for (int j = 0; j < g_asset_count; j++)
			if (!strcmp(g_asset_ids[j], ids[i])) {
				idx = j;
				break;
			}
		if (idx < 0)
			continue;
		int seen = 0;
		for (int k = 0; k < g_search_count && !seen; k++)
			seen = (g_search_idx[k] == idx);
		if (!seen)
			g_search_idx[g_search_count++] = idx;
	}

	g_search_active = 1;
	snprintf(g_search_query, sizeof(g_search_query), "%s", query);
	rebuild_display();
	log_line("smart search '%s': %d result(s)", query, g_search_count);
	return g_search_count;
}

static void draw_hud(const char *text)
{
	vita2d_draw_rectangle(0, SCREEN_H - 32, SCREEN_W, 32, RGBA8(0, 0, 0, 180));
	draw_text(10, SCREEN_H - 9,
			     RGBA8(255, 255, 255, 255), 1.0f, text);
}

/* spinning throbber: a ring of dots with a brightness tail */
static void draw_throbber(float cx, float cy, float r, unsigned int frame)
{
	for (int i = 0; i < 12; i++) {
		float a = (float)i * (6.2831853f / 12.0f);
		int phase = ((int)(frame / 3) - i) % 12;
		if (phase < 0)
			phase += 12;
		unsigned char al = (unsigned char)(255 - phase * 18);
		vita2d_draw_fill_circle(cx + cosf(a) * r, cy + sinf(a) * r,
					r / 5.5f,
					RGBA8(255, 255, 255, al));
	}
}

/* translucent play button over a video's poster in the detail view */
static void draw_play_overlay(void)
{
	float cx = SCREEN_W / 2.0f, cy = SCREEN_H / 2.0f;
	vita2d_draw_fill_circle(cx, cy, 46.0f, RGBA8(20, 20, 20, 150));
	vita2d_color_vertex *v =
		vita2d_pool_memalign(3 * sizeof(*v), sizeof(*v));
	if (!v)
		return;
	unsigned int col = RGBA8(255, 255, 255, 220);
	v[0] = (vita2d_color_vertex){ cx - 14.0f, cy - 24.0f, 0.5f, col };
	v[1] = (vita2d_color_vertex){ cx - 14.0f, cy + 24.0f, 0.5f, col };
	v[2] = (vita2d_color_vertex){ cx + 28.0f, cy, 0.5f, col };
	vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 3);
}

static void draw_error_detail(int idx)
{
	draw_centered(220, RGBA8(255, 80, 80, 255), "Failed to load this photo");
	draw_text(40, 270, RGBA8(200, 200, 200, 255),
			     1.0f, g_asset_ids[idx]);
	const char *e = g_tex_err[idx];
	int len = strlen(e), y = 300;
	const int per_line = 76;
	for (int off = 0; off < len && y < 500; off += per_line, y += 26) {
		char line[80];
		snprintf(line, sizeof(line), "%.*s", per_line, e + off);
		draw_text(40, y, RGBA8(200, 200, 200, 255), 1.0f, line);
	}
}

/* ------------------------------------------------------------------ */
/* video download + playback                                           */
/*                                                                     */
/* The asset's transcoded playback stream is downloaded to the memory  */
/* card, then played with SceAvPlayer (the OS's hardware MP4/H.264     */
/* decoder). Frame buffers are CDRAM memblocks mapped into GXM, so     */
/* decoded NV12 frames are drawn zero-copy by pointing a YUV texture   */
/* at them. Audio runs on its own thread; sceAvPlayerGetVideoData      */
/* paces video against it internally.                                  */
/* ------------------------------------------------------------------ */

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/* draw an error screen until O is pressed */
static void show_blocking_error(const char *title, const char *detail)
{
	SceCtrlData pad;
	do {
		sceCtrlPeekBufferPositive(0, &pad, 1);
		vita2d_start_drawing();
		vita2d_clear_screen();
		draw_centered(230, RGBA8(255, 80, 80, 255), title);
		if (detail && detail[0])
			draw_centered(280, RGBA8(200, 200, 200, 255), detail);
		draw_centered(510, RGBA8(160, 160, 160, 255), "O back");
		vita2d_end_drawing();
		vita2d_swap_buffers();
	} while (!(pad.buttons & SCE_CTRL_CIRCLE));
}

static size_t file_write_cb(void *ptr, size_t size, size_t nmemb, void *ud)
{
	return fwrite(ptr, size, nmemb, (FILE *)ud);
}

static uint64_t g_dl_last_draw;
/* poster/thumb drawn behind the download progress and the player's startup
 * (the seconds AVPlayer spends parsing before the first frame); set by the
 * play call sites, valid for the whole modal download + playback */
static vita2d_texture *g_dl_bg;

/* display rotation (degrees clockwise) of the video being played; from the
 * asset's exif orientation, so portrait recordings play upright */
static int g_video_rot;

static int ori_to_deg(int o)
{
	switch (o) {
	case 3: case 4: return 180;
	case 5: case 6: return 90;
	case 7: case 8: return 270;
	default:        return 0;
	}
}

/* fit a (possibly rotated) video frame to the screen */
static void draw_video_frame(vita2d_texture *tex)
{
	float w = vita2d_texture_get_width(tex);
	float h = vita2d_texture_get_height(tex);
	if (g_video_rot == 0) {
		draw_texture_fitted(tex, 0, 0, SCREEN_W, SCREEN_H);
		return;
	}
	int side = (g_video_rot == 90 || g_video_rot == 270);
	float ew = side ? h : w, eh = side ? w : h;
	float sc = ((float)SCREEN_W / ew < (float)SCREEN_H / eh) ?
		   (float)SCREEN_W / ew : (float)SCREEN_H / eh;
	vita2d_draw_texture_scale_rotate(tex, SCREEN_W / 2.0f, SCREEN_H / 2.0f,
					 sc, sc,
					 (float)g_video_rot *
					 (3.14159265f / 180.0f));
}

/* full-screen status with the video's poster behind it */
static void show_video_status(const char *text)
{
	vita2d_start_drawing();
	vita2d_clear_screen();
	if (g_dl_bg) {
		draw_texture_fitted(g_dl_bg, 0, 0, SCREEN_W, SCREEN_H);
		vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H,
				      RGBA8(0, 0, 0, 130));
	}
	draw_centered(SCREEN_H / 2, RGBA8(255, 255, 255, 255), text);
	vita2d_end_drawing();
	vita2d_swap_buffers();
}

static int dl_progress_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow,
			  curl_off_t ultotal, curl_off_t ulnow)
{
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(0, &pad, 1);
	if (pad.buttons & SCE_CTRL_CIRCLE)
		return 1; /* abort the transfer */

	/* curl calls this very often; only redraw every 100 ms */
	uint64_t now = sceKernelGetProcessTimeWide();
	if (now - g_dl_last_draw < 100 * 1000)
		return 0;
	g_dl_last_draw = now;

	char buf[128];
	if (dltotal > 0)
		snprintf(buf, sizeof(buf), "Downloading video... %.1f / %.1f MB",
			 dlnow / (1024.0 * 1024.0), dltotal / (1024.0 * 1024.0));
	else
		snprintf(buf, sizeof(buf), "Downloading video... %.1f MB",
			 dlnow / (1024.0 * 1024.0));

	vita2d_start_drawing();
	vita2d_clear_screen();
	if (g_dl_bg) {
		/* the video's poster behind the progress, dimmed */
		draw_texture_fitted(g_dl_bg, 0, 0, SCREEN_W, SCREEN_H);
		vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H,
				      RGBA8(0, 0, 0, 130));
	}
	draw_centered(SCREEN_H / 2 - 20, RGBA8(255, 255, 255, 255), buf);
	if (dltotal > 0) {
		float frac = (float)dlnow / (float)dltotal;
		vita2d_draw_rectangle(180, SCREEN_H / 2 + 10, 600, 14,
				      RGBA8(60, 60, 60, 255));
		vita2d_draw_rectangle(180, SCREEN_H / 2 + 10, 600.0f * frac, 14,
				      RGBA8(120, 200, 120, 255));
	}
	draw_centered(SCREEN_H / 2 + 60, RGBA8(160, 160, 160, 255), "O cancel");
	vita2d_end_drawing();
	vita2d_swap_buffers();
	return 0;
}

/* download the playback stream of asset idx to VIDEO_TMP_PATH.
 * returns 0 on success, 1 if the user cancelled, -1 on error. */
static int download_video(int idx, char *err, size_t errlen)
{
	char url[700];
	snprintf(url, sizeof(url), "%s/api/assets/%s/video/playback",
		 g_server, g_asset_ids[idx]);

	FILE *f = fopen(VIDEO_TMP_PATH, "wb");
	if (!f) {
		snprintf(err, errlen, "cannot create %s", VIDEO_TMP_PATH);
		return -1;
	}

	CURL *curl = curl_easy_init();
	if (!curl) {
		fclose(f);
		snprintf(err, errlen, "curl init failed");
		return -1;
	}

	char keyhdr[300];
	snprintf(keyhdr, sizeof(keyhdr), "x-api-key: %s", g_apikey);
	struct curl_slist *hdrs = curl_slist_append(NULL, keyhdr);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	/* no overall timeout (videos can be big); abort on a 30 s stall */
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, dl_progress_cb);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "vitaImmich/0.1 (PS Vita)");
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	if (g_resolve_list)
		curl_easy_setopt(curl, CURLOPT_RESOLVE, g_resolve_list);

	g_dl_last_draw = 0;
	net_lock(); /* TLS is single-threaded, see g_net_mutex */
	CURLcode res = curl_easy_perform(curl);
	net_unlock();
	long code = 0;
	if (res == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(hdrs);
	curl_easy_cleanup(curl);
	fclose(f);

	if (res == CURLE_ABORTED_BY_CALLBACK) {
		sceIoRemove(VIDEO_TMP_PATH);
		return 1;
	}
	if (res != CURLE_OK) {
		snprintf(err, errlen, "curl error %d: %s", res,
			 curl_easy_strerror(res));
		sceIoRemove(VIDEO_TMP_PATH);
		return -1;
	}
	if (code < 200 || code >= 300) {
		snprintf(err, errlen, "HTTP %ld", code);
		sceIoRemove(VIDEO_TMP_PATH);
		return -1;
	}
	return 0;
}

static void *av_alloc(void *p, uint32_t alignment, uint32_t size)
{
	return memalign(alignment, size);
}

static void av_free(void *p, void *ptr)
{
	free(ptr);
}

/* video frame buffers must be GPU-visible: CDRAM memblocks mapped into GXM */
static void *av_gpu_alloc(void *p, uint32_t alignment, uint32_t size)
{
	if (alignment < 0x40000)
		alignment = 0x40000; /* CDRAM memblocks are 256 KiB granular */
	size = ALIGN_UP(size, alignment);

	SceKernelAllocMemBlockOpt opt;
	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
	opt.alignment = alignment;
	SceUID mb = sceKernelAllocMemBlock("vimm_vframe",
					   SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
					   size, &opt);
	if (mb < 0) {
		log_line("video: frame alloc failed (%u bytes): 0x%08x", size, mb);
		return NULL;
	}
	void *base = NULL;
	sceKernelGetMemBlockBase(mb, &base);
	sceGxmMapMemory(base, size,
			SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
	return base;
}

static void av_gpu_free(void *p, void *ptr)
{
	SceUID mb = sceKernelFindMemBlockByAddr(ptr, 0);
	sceGxmUnmapMemory(ptr);
	if (mb >= 0)
		sceKernelFreeMemBlock(mb);
}

/* load the SceAvPlayer module once. shared by playback and poster extraction
 * (either thread can be the first to need it); returns 0 once loaded, or a
 * negative module error. a second load returns ALREADY_LOADED, so the latch
 * keeps that from being mistaken for a failure. */
static volatile int g_avplayer_loaded;

static int load_avplayer_module(void)
{
	if (g_avplayer_loaded)
		return 0;
	int mret = sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
	log_line("video: load AVPLAYER module: 0x%08x", mret);
	if (mret < 0)
		return mret;
	g_avplayer_loaded = 1;
	return 0;
}

/* Extract the first video frame of an MP4 as a tightly-packed RGB888 buffer,
 * nearest-neighbour downscaled so the long side is <= THUMB_MAX. Runs on the
 * WORKER thread: no vita2d/GXM drawing, only SceAvPlayer + the av_gpu_alloc
 * memblock helper (which the architecture permits off the main thread).
 * Returns a malloc'd buffer (caller frees) with w/h set, or NULL on failure
 * (err set, and the failing avplayer step logged). */
static unsigned char *extract_video_poster(const char *path, int *w, int *h,
					   char *err, size_t errlen)
{
	if (load_avplayer_module() < 0) {
		snprintf(err, errlen, "avplayer module load failed");
		log_line("poster %s: module load failed", path);
		return NULL;
	}

	SceAvPlayerInitData init;
	memset(&init, 0, sizeof(init));
	init.memoryReplacement.allocate          = av_alloc;
	init.memoryReplacement.deallocate        = av_free;
	init.memoryReplacement.allocateTexture   = av_gpu_alloc;
	init.memoryReplacement.deallocateTexture = av_gpu_free;
	init.basePriority = 0xA0;
	init.numOutputVideoFrameBuffers = 2;
	init.autoStart = 1;
	init.defaultLanguage = "eng";

	/* like play_video_file: the handle is a context pointer, init failure
	 * is 0/NULL rather than a negative error */
	SceAvPlayerHandle avp = sceAvPlayerInit(&init);
	if (avp == 0) {
		snprintf(err, errlen, "sceAvPlayerInit returned NULL");
		log_line("poster %s: sceAvPlayerInit returned NULL", path);
		return NULL;
	}
	int ret = sceAvPlayerAddSource(avp, path);
	if (ret < 0) {
		snprintf(err, errlen, "sceAvPlayerAddSource 0x%08x", ret);
		log_line("poster %s: sceAvPlayerAddSource 0x%08x", path, ret);
		sceAvPlayerClose(avp);
		return NULL;
	}

	/* the source parses asynchronously; wait ~5 s for it to become active */
	int active = 0;
	for (int i = 0; i < 500 && !(active = sceAvPlayerIsActive(avp)); i++)
		sceKernelDelayThread(10 * 1000);
	if (!active) {
		snprintf(err, errlen, "never became active (codec?)");
		log_line("poster %s: never became active", path);
		sceAvPlayerStop(avp);
		sceAvPlayerClose(avp);
		return NULL;
	}

	/* poll ~5 s for the first decoded frame */
	SceAvPlayerFrameInfo frame;
	int got = 0;
	for (int i = 0; i < 500; i++) {
		memset(&frame, 0, sizeof(frame));
		if (sceAvPlayerGetVideoData(avp, &frame)) {
			got = 1;
			break;
		}
		if (!sceAvPlayerIsActive(avp))
			break;
		sceKernelDelayThread(10 * 1000);
	}
	if (!got) {
		snprintf(err, errlen, "no video frame");
		log_line("poster %s: sceAvPlayerGetVideoData timed out", path);
		sceAvPlayerStop(avp);
		sceAvPlayerClose(avp);
		return NULL;
	}

	int sw = (int)frame.details.video.width;
	int sh = (int)frame.details.video.height;
	if (sw <= 0 || sh <= 0) {
		snprintf(err, errlen, "bad frame size %dx%d", sw, sh);
		log_line("poster %s: bad frame size %dx%d", path, sw, sh);
		sceAvPlayerStop(avp);
		sceAvPlayerClose(avp);
		return NULL;
	}

	/* integer downscale step so the long side lands <= THUMB_MAX */
	int step = 1;
	while ((sw > sh ? sw : sh) / step > THUMB_MAX)
		step++;
	int dw = sw / step, dh = sh / step;
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;

	unsigned char *rgb = malloc((size_t)dw * dh * 3);
	if (!rgb) {
		snprintf(err, errlen, "out of memory (%dx%d)", dw, dh);
		log_line("poster %s: out of memory (%dx%d)", path, dw, dh);
		sceAvPlayerStop(avp);
		sceAvPlayerClose(avp);
		return NULL;
	}

	/* NV12: Y plane (sw*sh bytes), then interleaved U,V at half resolution
	 * (row pitch sw, one U,V pair per 2x2 luma block). nearest-neighbour
	 * sample + integer BT.601 YUV->RGB. byte order matches decode_jpeg_buf
	 * (R,G,B), which the U8U8U8_BGR texture in consume_worker_result wants. */
	const unsigned char *yp = (const unsigned char *)frame.pData;
	const unsigned char *uvp = yp + (size_t)sw * sh;
	unsigned char *o = rgb;
	for (int dy = 0; dy < dh; dy++) {
		int sy = dy * step;
		const unsigned char *yrow = yp + (size_t)sy * sw;
		const unsigned char *uvrow = uvp + (size_t)(sy / 2) * sw;
		for (int dx = 0; dx < dw; dx++) {
			int sx = dx * step;
			int Y = yrow[sx];
			int U = uvrow[(sx & ~1)];
			int V = uvrow[(sx & ~1) + 1];
			int C = Y - 16, D = U - 128, E = V - 128;
			int R = (298 * C + 409 * E + 128) >> 8;
			int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
			int B = (298 * C + 516 * D + 128) >> 8;
			*o++ = R < 0 ? 0 : R > 255 ? 255 : R;
			*o++ = G < 0 ? 0 : G > 255 ? 255 : G;
			*o++ = B < 0 ? 0 : B > 255 ? 255 : B;
		}
	}

	*w = dw;
	*h = dh;
	sceAvPlayerStop(avp);
	sceAvPlayerClose(avp);
	return rgb;
}

static SceAvPlayerHandle g_avp;
static volatile int g_av_audio_run;

/* sceAudioOutOutput blocks until the previous chunk drains, so this
 * thread is naturally paced by the audio hardware */
static int video_audio_thread(SceSize args, void *argp)
{
	int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, 1024, 48000,
				       SCE_AUDIO_OUT_MODE_STEREO);
	if (port < 0) {
		log_line("video: audio port open failed: 0x%08x", port);
		return 0;
	}
	int vol[2] = { 32767, 32767 };
	sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH |
				   SCE_AUDIO_VOLUME_FLAG_R_CH, vol);

	SceAvPlayerFrameInfo frame;
	memset(&frame, 0, sizeof(frame));
	while (g_av_audio_run) {
		if (sceAvPlayerIsActive(g_avp) &&
		    sceAvPlayerGetAudioData(g_avp, &frame)) {
			sceAudioOutSetConfig(port, -1,
					     frame.details.audio.sampleRate,
					     frame.details.audio.channelCount == 1 ?
					     SCE_AUDIO_OUT_MODE_MONO :
					     SCE_AUDIO_OUT_MODE_STEREO);
			sceAudioOutOutput(port, frame.pData);
		} else {
			sceKernelDelayThread(1000);
		}
	}
	sceAudioOutReleasePort(port);
	return 0;
}

/* play a local MP4 full screen; returns when it ends or O is pressed */
static void play_video_file(const char *path)
{
	if (load_avplayer_module() < 0) {
		show_blocking_error("Could not load the video player module",
				    "sceSysmoduleLoadModule failed");
		return;
	}

	/* claim the hardware decoder: announce ourselves, then wait (bounded,
	 * ~12 s — longer than extraction's worst-case timeouts) for any
	 * in-flight worker poster extraction to finish. lock-order mirror of
	 * the worker guard — we set our flag before waiting, so the worker
	 * sees it and backs off; we only ever wait here, never hold a lock
	 * the worker is also waiting on, so the two cannot deadlock. */
	g_player_active = 1;
	__sync_synchronize();
	for (int i = 0; i < 720 && g_poster_active; i++)
		show_video_status("Starting video...");

	/* replay re-inits the player from the same (already-local) file, so the
	 * decoder claim above is taken once and held across replays. */
	int want_replay;
replay:
	want_replay = 0;

	SceAvPlayerInitData init;
	memset(&init, 0, sizeof(init));
	init.memoryReplacement.allocate          = av_alloc;
	init.memoryReplacement.deallocate        = av_free;
	init.memoryReplacement.allocateTexture   = av_gpu_alloc;
	init.memoryReplacement.deallocateTexture = av_gpu_free;
	init.basePriority = 0xA0;
	/* four rotating frame buffers (not two): GPU rendering is async, so two
	 * let the decoder recycle a buffer the GPU is still sampling, which
	 * tears/glitches. 1080p NV12 is ~3.2 MB/buffer, so 4 ~= 13 MB CDRAM. */
	init.numOutputVideoFrameBuffers = 4;
	init.autoStart = 1;
	init.defaultLanguage = "eng";

	/* the handle is a pointer to the player context (so often has bit 31
	 * set); init failure returns 0, not a negative error code */
	g_avp = sceAvPlayerInit(&init);
	if (g_avp == 0) {
		log_line("video: sceAvPlayerInit failed");
		show_blocking_error("Could not start the video player",
				    "sceAvPlayerInit returned NULL");
		g_player_active = 0;
		__sync_synchronize();
		return;
	}
	log_line("video: player handle 0x%08x", g_avp);
	int ret = sceAvPlayerAddSource(g_avp, path);
	if (ret < 0) {
		char det[64];
		snprintf(det, sizeof(det), "sceAvPlayerAddSource: 0x%08x", ret);
		log_line("video: %s", det);
		sceAvPlayerClose(g_avp);
		show_blocking_error("Could not open the video file", det);
		g_player_active = 0;
		__sync_synchronize();
		return;
	}

	g_av_audio_run = 1;
	SceUID audio_thid = sceKernelCreateThread("video_audio",
						  video_audio_thread,
						  0x10000100, 64 * 1024,
						  0, 0, NULL);
	if (audio_thid >= 0)
		sceKernelStartThread(audio_thid, 0, NULL);

	/* AddSource parses asynchronously; give it ~5 s to start */
	int active = 0;
	for (int i = 0; i < 300 && !(active = sceAvPlayerIsActive(g_avp)); i++)
		show_video_status("Starting video...");

	/* AddSource parses asynchronously, so the stream duration is usually not
	 * available yet at this point; keep re-querying it in the loop below until
	 * it becomes known instead of latching the initial 0. */
	uint64_t duration = 0;

	/* four rotating frame wrappers (matching numOutputVideoFrameBuffers):
	 * while the GPU still samples one frame the decoder fills another, and
	 * the deeper ring keeps it from reusing a buffer that is still in flight.
	 * the wrappers never own memory (the decoder's CDRAM buffers do), so we
	 * must never vita2d_free_texture them. */
	vita2d_texture vtex[4];
	SceAvPlayerFrameInfo vframe[4];
	memset(vtex, 0, sizeof(vtex));
	memset(vframe, 0, sizeof(vframe));
	int buf_idx = 0;
	vita2d_texture *cur = NULL;

	int paused = 0;
	int ended_eos = 0;
	unsigned int prev = 0xffffffff; /* swallow the X press that got us here */

	while (active) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		unsigned int pressed = pad.buttons & ~prev;
		prev = pad.buttons;

		if (pressed & SCE_CTRL_CIRCLE)
			break;
		if (pressed & SCE_CTRL_CROSS) {
			if (paused)
				sceAvPlayerResume(g_avp);
			else
				sceAvPlayerPause(g_avp);
			paused = !paused;
		}
		if (!paused && (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))) {
			uint64_t t = sceAvPlayerCurrentTime(g_avp);
			if (pressed & SCE_CTRL_RIGHT)
				t += 10000;
			else
				t = t > 10000 ? t - 10000 : 0;
			if (duration == 0 || t < duration)
				sceAvPlayerJumpToTime(g_avp, t);
		}

		if (!sceAvPlayerIsActive(g_avp)) {
			ended_eos = 1;
			break; /* end of stream */
		}

		if (sceAvPlayerGetVideoData(g_avp, &vframe[buf_idx])) {
			sceGxmTextureInitLinear(&vtex[buf_idx].gxm_tex,
						vframe[buf_idx].pData,
						SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1,
						vframe[buf_idx].details.video.width,
						vframe[buf_idx].details.video.height,
						0);
			cur = &vtex[buf_idx];
			buf_idx = (buf_idx + 1) & 3;
		}

		vita2d_start_drawing();
		vita2d_clear_screen();
		if (cur)
			draw_video_frame(cur);
		else if (g_dl_bg)
			/* poster until the first decoded frame arrives */
			draw_texture_fitted(g_dl_bg, 0, 0, SCREEN_W, SCREEN_H);

		if (duration == 0) {
			SceAvPlayerStreamInfo sinfo;
			memset(&sinfo, 0, sizeof(sinfo));
			if (sceAvPlayerGetStreamInfo(g_avp, 0, &sinfo) >= 0)
				duration = sinfo.duration;
		}

		unsigned int cs = (unsigned int)(sceAvPlayerCurrentTime(g_avp) / 1000);
		unsigned int ds = (unsigned int)(duration / 1000);
		char hud[160];
		snprintf(hud, sizeof(hud),
			 "%u:%02u / %u:%02u%s    < > seek 10s    X %s    O back",
			 cs / 60, cs % 60, ds / 60, ds % 60,
			 paused ? "  [paused]" : "",
			 paused ? "resume" : "pause");
		draw_hud(hud);

		vita2d_end_drawing();
		vita2d_swap_buffers();
	}

	if (!active) {
		log_line("video: player never became active (unsupported codec?)");
		show_blocking_error("Could not play this video",
				    "The Vita plays MP4 (H.264/AAC) only; "
				    "check Immich transcoding settings.");
	}

	/* reached the end of the video: hold on the last frame with a replay
	 * prompt instead of returning (which, for a server video, drops the user
	 * back to the detail screen and would re-download to play it again). X
	 * replays from the still-local file; O leaves. The decoder buffers behind
	 * `cur` are still valid here — teardown happens below, after this loop. */
	if (ended_eos) {
		unsigned int eprev = 0xffffffff;
		for (;;) {
			SceCtrlData pad;
			sceCtrlPeekBufferPositive(0, &pad, 1);
			unsigned int pressed = pad.buttons & ~eprev;
			eprev = pad.buttons;

			if (pressed & SCE_CTRL_CIRCLE)
				break;
			if (pressed & SCE_CTRL_CROSS) {
				want_replay = 1;
				break;
			}

			vita2d_start_drawing();
			vita2d_clear_screen();
			if (cur)
				draw_video_frame(cur);
			draw_hud("ended    X replay    O back");
			vita2d_end_drawing();
			vita2d_swap_buffers();
		}
	}

	g_av_audio_run = 0;
	if (audio_thid >= 0) {
		sceKernelWaitThreadEnd(audio_thid, NULL, NULL);
		sceKernelDeleteThread(audio_thid);
	}
	/* Close() frees the frame the GPU may still be sampling */
	vita2d_wait_rendering_done();
	sceAvPlayerStop(g_avp);
	sceAvPlayerClose(g_avp);

	if (want_replay)
		goto replay; /* re-init from the same file; no re-download */

	/* release the decoder so the worker can resume poster extraction */
	g_player_active = 0;
	__sync_synchronize();
}

/* download + play asset idx, cleaning up the temp file afterwards */
static void view_video(int idx)
{
	/* the video's poster behind the download progress; the detail view
	 * pre-sets its sharper full-res poster, the grid falls back to the
	 * asset thumb. the modal download keeps the texture alive. */
	if (!g_dl_bg)
		g_dl_bg = g_thumb[idx];
	g_video_rot = ori_to_deg(g_asset_rot[idx]);

	char err[160];
	int r = download_video(idx, err, sizeof(err));
	if (r == 0) {
		log_line("video %s: downloaded, playing", g_asset_ids[idx]);
		play_video_file(VIDEO_TMP_PATH);
	} else if (r < 0) {
		log_line("video %s: %s", g_asset_ids[idx], err);
		show_blocking_error("Failed to download video", err);
	}
	g_dl_bg = NULL;
	sceIoRemove(VIDEO_TMP_PATH);
}

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

		if (!grow_locals(g_local_count + 1)) {
			log_line("scan: out of memory at %d local files",
				 g_local_count);
			break;
		}
		int j = g_local_count++;
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
	snprintf(keyhdr, sizeof(keyhdr), "x-api-key: %s", g_apikey);
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
	snprintf(keyhdr, sizeof(keyhdr), "x-api-key: %s", g_apikey);
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
	snprintf(keyhdr, sizeof(keyhdr), "x-api-key: %s", g_apikey);
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
static void draw_status_badge(float bx, float by, int d, unsigned int frame)
{
	struct disp_item *it = &g_disp[d];
	uint32_t col;
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

	switch (kind) {
	case 1: col = RGBA8(120, 150, 200, 255); break; /* cloud: blue-grey */
	case 2: col = RGBA8(240, 160, 40, 255);  break; /* local: orange */
	case 3: /* busy: blinking orange */
		col = (frame / 20) & 1 ? RGBA8(240, 160, 40, 255)
				       : RGBA8(120, 90, 30, 255);
		break;
	case 4: col = RGBA8(80, 200, 100, 255);  break; /* backed: green */
	default: col = RGBA8(220, 70, 70, 255);  break; /* failed: red */
	}

	float cx = bx + 14, cy = by + 14, r = 9;
	vita2d_draw_fill_circle(cx, cy, r + 2, RGBA8(0, 0, 0, 160));
	vita2d_draw_fill_circle(cx, cy, r, col);

	uint32_t w = RGBA8(255, 255, 255, 255);
	if (kind == 4) { /* check mark */
		vita2d_draw_rectangle(cx - 4, cy, 3, 5, w);
		vita2d_draw_rectangle(cx - 2, cy + 2, 3, 3, w);
		vita2d_draw_rectangle(cx, cy - 1, 3, 6, w);
		vita2d_draw_rectangle(cx + 2, cy - 4, 3, 5, w);
	} else if (kind == 2 || kind == 3) { /* up arrow */
		vita2d_draw_rectangle(cx - 1, cy - 4, 3, 9, w);
		vita2d_draw_rectangle(cx - 4, cy - 1, 3, 3, w);
		vita2d_draw_rectangle(cx + 2, cy - 1, 3, 3, w);
	}
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

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

enum { MODE_GRID, MODE_DETAIL, MODE_CLOUD, MODE_CLOUD_DETAILS };

int main(void)
{
	vita2d_init();
	vita2d_set_clear_color(RGBA8(16, 16, 16, 255));
	g_font = vita2d_load_default_pgf();
	g_ttf = vita2d_load_font_file("app0:font.ttf");

	/* on-screen keyboard (smart search). the IME runs as a common dialog;
	 * the config tells it the system language + enter/cancel button map */
	sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
	{
		SceCommonDialogConfigParam cfg;
		sceCommonDialogConfigParamInit(&cfg);
		sceCommonDialogSetConfigParam(&cfg);
	}

	/* analog mode so the left stick reports lx/ly for detail-view panning */
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
	/* front touch: drag scrolls the grid, a tap opens the item */
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
				 SCE_TOUCH_SAMPLING_STATE_START);

	/* start a fresh debug log each launch */
	sceIoMkdir(CONFIG_DIR, 0777);
	FILE *lf = fopen(LOG_PATH, "w");
	if (lf)
		fclose(lf);

	/* Pre-grow the newlib heap while still single-threaded. vitasdk's
	 * newlib maps heap memblocks on demand as sbrk grows; on Vita3K the
	 * page-mapping done by one thread races other threads' JIT'd memory
	 * accesses, so a heap growth during a thumbnail download or page
	 * fetch intermittently faults on the freshly mapped pages (crash
	 * locations wander: OpenSSL GCM, libjpeg, strlen). Touching a large
	 * block now commits the arena up front, so the heap never grows once
	 * the worker/sync threads exist. dlmalloc keeps the sbrk'd arena
	 * after the free, which is also headroom AVPlayer's allocator needs
	 * for video playback anyway. */
	{
		/* never trim: a trimmed top chunk would shrink sbrk and unmap
		 * the pages again, reintroducing runtime heap growth */
		mallopt(M_TRIM_THRESHOLD, 0x7fffffff);
		size_t want = 96 * 1024 * 1024;
		void *ball = NULL;
		while (want >= 8 * 1024 * 1024 && !(ball = malloc(want)))
			want /= 2;
		if (ball) {
			memset(ball, 0, want);
			free(ball);
		}
		log_line("heap pre-grown: %u MB", (unsigned)(want >> 20));
	}

	show_status("Starting network...");
	net_init();

	/* runtime truth about what the linked libcurl supports */
	{
		curl_version_info_data *vi = curl_version_info(CURLVERSION_NOW);
		log_line("libcurl %s | ssl: %s", vi->version,
			 vi->ssl_version ? vi->ssl_version : "(none)");
		char plist[300] = "";
		for (const char *const *pp = vi->protocols; *pp; pp++) {
			strncat(plist, *pp, sizeof(plist) - strlen(plist) - 2);
			strncat(plist, " ", sizeof(plist) - strlen(plist) - 1);
		}
		log_line("protocols: %s", plist);
	}

	if (load_config() != 0)
		fatal_error("Edit it with VitaShell, then restart the app. "
			    "Create the API key in the Immich web UI under "
			    "Account Settings > API Keys.",
			    "Set your server and API key in " CONFIG_PATH);

	/* log the exact bytes of the server URL; invisible characters in the
	 * config show up here when curl complains about the protocol */
	{
		char hex[3 * 64 + 1] = "";
		int n = strlen(g_server);
		for (int i = 0; i < n && i < 64; i++)
			sprintf(hex + 3 * i, "%02x ", (unsigned char)g_server[i]);
		log_line("server='%s' len=%d hex=%s", g_server, n, hex);
	}

	show_status("Loading library from %s ...", g_server);
	fetch_page(1);
	if (g_asset_count == 0)
		fatal_error(NULL, "Server returned no assets");

	SceUID worker = sceKernelCreateThread("thumb_loader", worker_thread,
					      0x10000100, 256 * 1024, 0, 0, NULL);
	if (worker >= 0)
		sceKernelStartThread(worker, 0, NULL);

	/* scan the Vita's own camera media (fast: paths + stat only), build
	 * the merged timeline, then let the sync thread hash + dup-check it */
	show_status("Scanning local media...");
	scan_local_media();
	rebuild_display();

	SceUID syncw = sceKernelCreateThread("sync_worker", sync_thread,
					     0x10000100, 256 * 1024, 0, 0, NULL);
	if (syncw >= 0)
		sceKernelStartThread(syncw, 0, NULL);

	load_autobackup();

	int mode = MODE_GRID;
	int sel = 0;
	int cloud_scroll = 0;   /* first visible row on the upload-details list */
	int show_sel = 1;       /* selection square: shown for d-pad, hidden for touch */
	int bar_shown = 1;      /* top search bar visible */
	float bar_hidden = 0.0f;/* animated pixels the bar is slid up (0..SEARCH_H) */
	/* periodic check for photos added to the server while we run */
	uint64_t last_poll = sceKernelGetProcessTimeWide();
	int grid_last_sel = -1;   /* sel at the previous frame */
	int grid_settle = 0;      /* frames since sel last changed */
	float scroll = 0.0f, target = 0.0f;
	unsigned int prev_buttons = 0;
	unsigned int held_frames = 0;
	unsigned int frame = 0;

	vita2d_texture *detail_tex = NULL;
	int detail_idx = -1;
	int detail_failed = 0;
	int detail_loading = 0;  /* full-res load in flight on the worker */
	int detail_issued = 0;   /* the worker request has been handed over */
	float zoom = 1.0f, panx = 0.0f, pany = 0.0f;

	/* front-touch state (grid: drag scrolls, tap opens; detail: swipe
	 * changes photo, drag pans when zoomed) */
	int touch_active = 0;   /* finger currently down */
	int touch_dragged = 0;  /* moved past the tap threshold */
	int touch_on_bar = 0;   /* contact began on the search bar */
	float touch_x = 0, touch_y = 0;       /* last position, screen px */
	float touch_start_x = 0, touch_start_y = 0, touch_start_scroll = 0;
	float touch_panx0 = 0, touch_pany0 = 0; /* pan at touch start (zoomed) */
	float touch_vel = 0;    /* px/frame at the moment of release */

	/* detail-view photo slide: the current photo's horizontal offset.
	 * a swipe maps it 1:1 to the finger; on release (or d-pad browse) it
	 * animates to +-SCREEN_W, the selection switches, and the neighbor's
	 * thumbnail rides alongside the whole way. */
	float slide_x = 0, slide_goal = 0;
	int slide_anim = 0;

	/* detail-view neighbor prefetch: while the current photo is settled,
	 * the worker quietly fetches the full-res images (or video posters)
	 * of sel+1 / sel-1 so browsing lands sharp instantly */
	vita2d_texture *pf_tex[2] = { NULL, NULL };
	int pf_d[2] = { -1, -1 };  /* display slot each cache slot holds */
	int pf_pending = -1;       /* slot being fetched right now, -1 none */

	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		unsigned int pressed = pad.buttons & ~prev_buttons;
		prev_buttons = pad.buttons;

		frame++;

		/* auto-backup: while enabled, queue any newly-found local-only
		 * photos for upload (~1/s; the sync thread drains the queue).
		 * this covers both launch and photos found mid-session. */
		if (g_autobackup && frame % 60 == 0 &&
		    count_state(SYNC_LOCAL_ONLY) > 0)
			queue_all_uploads();

		/* the sync thread asks us (the only writer of g_disp) to rebuild
		 * the merged timeline after a status change; keep the selection
		 * on the same photo across the reorder */
		if (g_need_rebuild) {
			g_need_rebuild = 0;
			rebuild_keep_view(&sel, &scroll, &target);
			detail_idx = -1; /* re-decode: item under sel may differ */
			/* display slots were reshuffled: the prefetch cache's
			 * slot keys are meaningless now */
			for (int k = 0; k < 2; k++) {
				if (pf_tex[k]) {
					vita2d_wait_rendering_done();
					vita2d_free_texture(pf_tex[k]);
					pf_tex[k] = NULL;
				}
				pf_d[k] = -1;
			}
		}

		/* d-pad auto-repeat for fast scrolling */
		const unsigned int dirs = SCE_CTRL_UP | SCE_CTRL_DOWN |
					  SCE_CTRL_LEFT | SCE_CTRL_RIGHT;
		held_frames = (pad.buttons & dirs) ? held_frames + 1 : 0;
		unsigned int nav = pressed;
		if (held_frames > 18 && held_frames % 5 == 0)
			nav |= pad.buttons & dirs;

		if (mode == MODE_GRID) {
			/* search bar: TRIANGLE opens the keyboard, CIRCLE (or
			 * the bar's clear chip) drops back to the timeline. a
			 * tap on the bar sets these too; handled after touch. */
			int do_search_open = (pressed & SCE_CTRL_TRIANGLE) != 0;
			int do_search_clear = g_search_active &&
					      (pressed & SCE_CTRL_CIRCLE) != 0;
			int do_open_cloud = 0; /* tap on the cloud button */

			/* SQUARE slides the top bar in/out; animate the offset */
			if (pressed & SCE_CTRL_SQUARE)
				bar_shown = !bar_shown;
			float bar_t = bar_shown ? 0.0f : (float)SEARCH_H;
			bar_hidden += (bar_t - bar_hidden) * 0.3f;
			if (fabsf(bar_t - bar_hidden) < 0.5f)
				bar_hidden = bar_t;
			/* the d-pad (and month jumps) bring the selection square
			 * back; a touch drag hides it again (set below) */
			if (nav & dirs)
				show_sel = 1;

			if (nav & SCE_CTRL_RIGHT)
				sel++;
			if (nav & SCE_CTRL_LEFT)
				sel--;
			if (sel < 0)
				sel = 0;
			if (sel >= g_disp_count)
				sel = g_disp_count - 1;
			if (nav & SCE_CTRL_DOWN)
				sel = nav_row(sel, +1);
			if (nav & SCE_CTRL_UP)
				sel = nav_row(sel, -1);
			if ((pressed & (SCE_CTRL_RTRIGGER |
					SCE_CTRL_LTRIGGER)) &&
			    g_disp_count > 0) {
				int dir = (pressed & SCE_CTRL_RTRIGGER) ?
					  +1 : -1;
				/* jumping down: the next month may simply not
				 * be fetched yet — pull pages until a new
				 * month shows up (or the library ends). search
				 * results aren't paginated, so skip the pull. */
				if (dir > 0 && !g_search_active) {
					int guard = 0;
					while (g_next_page > 0 && guard++ < 10 &&
					       !strncmp(disp_date(month_jump(sel, +1)),
							disp_date(sel), 7)) {
						if (fetch_page(0) <= 0)
							break;
						rebuild_keep_view(&sel, &scroll,
								  &target);
					}
				}
				sel = month_jump(sel, dir);
				/* put the jumped-to month just below the pinned
				 * search bar, header band included */
				if (sel >= 0 && sel < g_disp_count)
					target = g_item_y[sel] - HEADER_H - SEARCH_H;
			}

			/* Track how long the selection has held still. A held
			 * d-pad auto-repeats one row every few frames; while it's
			 * moving we suspend the expensive/fragile work below:
			 *  - thumbnail texture create/free (rapid GPU map+unmap
			 *    churn fragments the address space until vita2d returns
			 *    a partially-mapped texture and the draw faults), and
			 *  - page fetching (otherwise a held key races sel to the
			 *    end and fires fetch+rebuild every frame — a request
			 *    storm that hammers the server and drives that churn).
			 * Everything resumes once you stop. Auto-repeat period is
			 * 5 frames, so a threshold of 12 covers it. */
			if (sel != grid_last_sel) {
				grid_last_sel = sel;
				grid_settle = 0;
			} else if (grid_settle < 1000) {
				grid_settle++;
			}
			int scrolling_fast = (grid_settle < 12);

			if ((pressed & SCE_CTRL_CROSS) && g_disp_count > 0) {
				/* photos and videos both open the detail
				 * view; a video shows its poster with the
				 * play button there */
				mode = MODE_DETAIL;
				zoom = 1.0f;
				panx = pany = 0.0f;
				slide_x = slide_goal = 0;
				slide_anim = 0;
			}

			/* fetch the next page when the selection settles near the
			 * end (gated on !scrolling_fast, see above). no loading
			 * screen: the grid stays up, the HUD's "+" already says
			 * more is coming, and the fetch only blocks briefly */
			if (!scrolling_fast && !g_search_active &&
			    g_next_page > 0 &&
			    sel >= g_disp_count - COLS * 4) {
				if (fetch_page(0) > 0)
					rebuild_keep_view(&sel, &scroll,
							  &target);
			}

			/* photos added to the server show up by themselves:
			 * poll the newest page every ~30 s while the grid is
			 * settled, keeping the selection on the same photo
			 * across the relayout */
			uint64_t pnow = sceKernelGetProcessTimeWide();
			if (!scrolling_fast && !touch_active && !g_search_active &&
			    pnow - last_poll > 30ULL * 1000 * 1000) {
				last_poll = pnow;
				if (check_new_assets() > 0)
					rebuild_keep_view(&sel, &scroll,
							  &target);
			}

			/* front touch: drag scrolls the timeline directly, a
			 * fling keeps it gliding, a tap opens the item under
			 * the finger. the front panel reports 2x screen px. */
			{
				SceTouchData td;
				sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
				float maxs = g_content_h - SCREEN_H;
				if (maxs < 0)
					maxs = 0;
				if (td.reportNum > 0) {
					float tx = td.report[0].x * 0.5f;
					float ty = td.report[0].y * 0.5f;
					if (!touch_active) {
						touch_active = 1;
						touch_dragged = 0;
						touch_start_x = tx;
						touch_start_y = ty;
						touch_start_scroll = scroll;
						touch_vel = 0;
						/* a contact on the (visible) bar opens
						 * the keyboard instead of scrolling */
						touch_on_bar =
							(ty < (float)SEARCH_H - bar_hidden);
					} else if (!touch_on_bar) {
						float dy = ty - touch_start_y;
						if (!touch_dragged &&
						    (dy > 14.0f || dy < -14.0f))
							touch_dragged = 1;
						if (touch_dragged) {
							/* touch-scroll hides the
							 * selection square */
							show_sel = 0;
							float ns = touch_start_scroll - dy;
							if (ns < 0)
								ns = 0;
							if (ns > maxs)
								ns = maxs;
							touch_vel = ns - scroll;
							scroll = target = ns;
							/* keep the selection inside
							 * the dragged viewport */
							int c = item_near(ns +
								(SEARCH_H + SCREEN_H) / 2);
							if (c >= 0)
								sel = c;
						}
					}
					touch_x = tx;
					touch_y = ty;
				} else if (touch_active) {
					touch_active = 0;
					if (touch_on_bar) {
						/* tap on the bar: the map/cloud
						 * buttons are placeholders (no-op),
						 * the clear chip clears an active
						 * search, the field opens the
						 * keyboard */
						if (touch_x > BTN2_CX - BTN_SZ / 2) {
							/* cloud button: open the
							 * server/backup page */
							do_open_cloud = 1;
						} else if (touch_x > BAR_M + BAR_W) {
							/* map button (placeholder) */
						} else if (g_search_active &&
						    touch_x > BAR_CLEAR_CX - 16)
							do_search_clear = 1;
						else
							do_search_open = 1;
						touch_on_bar = 0;
					} else if (!touch_dragged) {
						/* tap: open the item under it
						 * (videos show their poster +
						 * play button in the detail) */
						int i = item_at(touch_x,
								touch_y + scroll + bar_hidden);
						if (i >= 0) {
							sel = i;
							mode = MODE_DETAIL;
							zoom = 1.0f;
							panx = pany = 0.0f;
							slide_x = slide_goal = 0;
							slide_anim = 0;
						}
					} else if (touch_vel > 2.0f ||
						   touch_vel < -2.0f) {
						/* fling: glide on; aim the
						 * selection at the destination
						 * so scroll-follow doesn't
						 * fight the glide */
						target = scroll + touch_vel * 18.0f;
						if (target < 0)
							target = 0;
						if (target > maxs)
							target = maxs;
						int c = item_near(target +
							(SEARCH_H + SCREEN_H) / 2);
						if (c >= 0)
							sel = c;
					}
				}

				if (do_open_cloud) {
					mode = MODE_CLOUD;
					continue;
				}

				/* act on a search request from TRIANGLE/CIRCLE
				 * or a tap on the bar. both block this frame
				 * (keyboard / request), so restart the loop. */
				if (do_search_clear) {
					g_search_active = 0;
					g_search_query[0] = '\0';
					rebuild_display();
					sel = 0;
					scroll = target = 0;
					detail_idx = -1;
					grid_last_sel = -1;
					for (int k = 0; k < 2; k++) {
						if (pf_tex[k]) {
							vita2d_wait_rendering_done();
							vita2d_free_texture(pf_tex[k]);
							pf_tex[k] = NULL;
						}
						pf_d[k] = -1;
					}
					continue;
				}
				if (do_search_open) {
					char q[128] = "";
					int ok = ime_input("Search your photos",
						g_search_active ? g_search_query : "",
						q, sizeof(q));
					if (ok && q[0]) {
						show_status("Searching \"%s\"...", q);
						int nres = run_smart_search(q);
						if (nres >= 0) {
							sel = 0;
							scroll = target = 0;
							detail_idx = -1;
							grid_last_sel = -1;
							for (int k = 0; k < 2; k++) {
								if (pf_tex[k]) {
									vita2d_wait_rendering_done();
									vita2d_free_texture(pf_tex[k]);
									pf_tex[k] = NULL;
								}
								pf_d[k] = -1;
							}
						} else {
							show_blocking_error(
								"Search failed",
								"Smart search may be "
								"disabled on the server "
								"(see log.txt).");
						}
					}
					/* swallow buttons held during the dialog */
					prev_buttons = 0xFFFFFFFF;
					touch_active = touch_on_bar = 0;
					continue;
				}
			}

			/* scroll follows the selection, using the precomputed
			 * per-item y so month headers shift it correctly. when
			 * moving up, reveal the header band above the item. */
			float sel_y = (sel >= 0 && sel < g_disp_count) ?
				      g_item_y[sel] : 0.0f;
			/* reveal the header band below the pinned search bar */
			if (sel_y - HEADER_H - SEARCH_H < target)
				target = sel_y - HEADER_H - SEARCH_H;
			float sel_h = (sel >= 0 && sel < g_disp_count) ?
				      g_item_h[sel] : ROW_H;
			if (sel_y + sel_h > target + SCREEN_H)
				target = sel_y + sel_h - SCREEN_H;
			float max_scroll = g_content_h - SCREEN_H;
			if (max_scroll < 0)
				max_scroll = 0;
			if (target < 0)
				target = 0;
			if (target > max_scroll)
				target = max_scroll;
			scroll += (target - scroll) * 0.35f;
			/* on-screen scroll: the hidden bar shifts the grid up so
			 * it fills the vacated strip (the reveal-header math in
			 * scroll-follow cancels out, so it still uses `scroll`) */
			float escroll = scroll + bar_hidden;

			/* visible index range (item y is non-decreasing with i),
			 * computed in the same pass as thumb eviction below */
			int first_vis = -1, last_vis = -1;

			/* evict thumbs far outside the viewport (never the one
			 * the worker is currently loading); evicted textures
			 * are recycled through the pool, see tex_release */
			for (int i = 0; i < g_disp_count; i++) {
				float dy = g_item_y[i] - escroll;
				if (dy > -g_item_h[i] && dy < SCREEN_H) {
					if (first_vis < 0)
						first_vis = i;
					last_vis = i;
				}
				/* free thumbs well outside the viewport (only when
				 * not mid-scroll, see scrolling_fast above) */
				if (!scrolling_fast && disp_thumb(i) &&
				    (dy < -1.25f * SCREEN_H || dy > 1.75f * SCREEN_H)) {
					struct disp_item *it = &g_disp[i];
					/* don't free a thumb the worker is filling */
					if (g_req_state != REQ_IDLE &&
					    g_req_idx == it->idx &&
					    g_req_src == it->src)
						continue;
					/* recycle, never free: the GPU side may
					 * still touch a freed texture's memory */
					tex_release(disp_thumb(i));
					if (it->src == SRC_LOCAL)
						g_local_thumb[it->idx] = NULL;
					else
						g_thumb[it->idx] = NULL;
				}
			}

			if (first_vis < 0) { /* nothing matched (empty grid) */
				first_vis = 0;
				last_vis = g_disp_count - 1;
			}

			/* collect finished download, then hand the worker
			 * the next most useful thumbnail */
			if (g_req_state == REQ_DONE) {
				__sync_synchronize();
				consume_worker_result();
			}
			if (g_req_state == REQ_IDLE && !scrolling_fast) {
				int next = pick_next_load(sel, first_vis, last_vis);
				if (next >= 0) {
					struct disp_item *it = &g_disp[next];
					g_req_idx = it->idx;
					g_req_src = it->src;
					g_req_detail = 0;
					/* copy the payload so the worker needn't
					 * index arrays we may realloc */
					if (it->src == SRC_LOCAL) {
						snprintf(g_req_path, sizeof(g_req_path),
							 "%s", g_local_path[it->idx]);
						g_req_is_video =
							g_local_is_video[it->idx];
					} else {
						snprintf(g_req_id, sizeof(g_req_id),
							 "%s", g_asset_ids[it->idx]);
						g_req_is_video = 0;
					}
					__sync_synchronize();
					g_req_state = REQ_PENDING;
				}
			}

			vita2d_start_drawing();
			vita2d_clear_screen();

			/* month/year header bands */
			for (int h = 0; h < g_sect_count; h++) {
				float hy = g_sect[h].y - escroll;
				if (hy + HEADER_H < 0 || hy > SCREEN_H)
					continue;
				draw_text(10, hy + HEADER_H - 12,
						     RGBA8(255, 255, 255, 255),
						     1.1f, g_sect[h].label);
				vita2d_draw_rectangle(10, hy + HEADER_H - 6,
						      SCREEN_W - 20, 2,
						      RGBA8(90, 90, 90, 255));
			}

			for (int i = first_vis; i >= 0 && i <= last_vis; i++) {
				float x = g_item_x[i];
				float y = g_item_y[i] - escroll;
				float bx = x + CELL_PAD, by = y + CELL_PAD;
				float bw = g_item_w[i] - 2 * CELL_PAD;
				float bh = g_item_h[i] - 2 * CELL_PAD;

				vita2d_texture *th = disp_thumb(i);
				if (th)
					draw_texture_fitted(th, bx, by, bw, bh);
				else if (disp_thumb_failed(i))
					vita2d_draw_rectangle(bx, by, bw, bh,
							      RGBA8(90, 30, 30, 255));
				else
					vita2d_draw_rectangle(bx, by, bw, bh,
							      RGBA8(40, 40, 40, 255));
				if (disp_is_video(i)) {
					vita2d_draw_rectangle(bx, by + bh - 26,
							      64, 26,
							      RGBA8(0, 0, 0, 170));
					draw_text(bx + 6,
							     by + bh - 7,
							     RGBA8(255, 255, 255, 255),
							     0.85f, "VIDEO");
				}
				draw_status_badge(bx, by, i, frame);
				if (i == sel && show_sel)
					draw_sel_outline(x + 2, y + 2,
							 g_item_w[i] - 4,
							 g_item_h[i] - 4);
			}

			if (g_search_active && g_disp_count == 0)
				draw_centered(SCREEN_H / 2,
					      RGBA8(160, 160, 165, 255),
					      "No results");

			/* pinned Immich-style search bar, slid up by bar_hidden
			 * (Square toggles it); covers items up to its lower edge */
			draw_search_bar(-bar_hidden);

			char hud[200];
			if (g_search_active)
				snprintf(hud, sizeof(hud),
					 "%d / %d results   /\\ edit  O clear  "
					 "X view  [] bar",
					 g_disp_count > 0 ? sel + 1 : 0,
					 g_disp_count);
			else
				snprintf(hud, sizeof(hud),
					 "%d / %d%s   %.10s   X view  /\\ search  "
					 "[] bar",
					 sel + 1, g_disp_count,
					 g_next_page > 0 ? "+" : "",
					 g_disp_count > 0 ? disp_date(sel) : "");
			draw_hud(hud);

			vita2d_end_drawing();
			vita2d_swap_buffers();
		} else if (mode == MODE_DETAIL) {
			struct disp_item it = g_disp[sel];
			int is_local = (it.src == SRC_LOCAL);
			int is_video = disp_is_video(sel);

			/* zoom with the triggers (continuous while held) */
			if (!is_video && detail_tex) {
				if (pad.buttons & SCE_CTRL_RTRIGGER)
					zoom *= 1.04f;
				if (pad.buttons & SCE_CTRL_LTRIGGER)
					zoom /= 1.04f;
				if (zoom > 8.0f)
					zoom = 8.0f;
				if (zoom < 1.0f) {
					zoom = 1.0f;
					panx = pany = 0.0f;
				}
			}
			int zoomed = (zoom > 1.001f);

			if (zoomed) {
				/* pan with the left stick + d-pad; don't browse */
				float dx = (pad.lx - 128) / 128.0f;
				float dy = (pad.ly - 128) / 128.0f;
				if (dx > 0.15f || dx < -0.15f)
					panx -= dx * 16.0f;
				if (dy > 0.15f || dy < -0.15f)
					pany -= dy * 16.0f;
				if (pad.buttons & SCE_CTRL_RIGHT) panx -= 12.0f;
				if (pad.buttons & SCE_CTRL_LEFT)  panx += 12.0f;
				if (pad.buttons & SCE_CTRL_UP)    pany += 12.0f;
				if (pad.buttons & SCE_CTRL_DOWN)  pany -= 12.0f;
			} else if (!slide_anim && !touch_active) {
				/* browse with a slide animation; the selection
				 * switches when the slide completes */
				if (nav & SCE_CTRL_RIGHT && sel < g_disp_count - 1) {
					slide_anim = 1;
					slide_goal = -SCREEN_W;
				}
				if (nav & SCE_CTRL_LEFT && sel > 0) {
					slide_anim = 1;
					slide_goal = SCREEN_W;
				}
			}

			/* front touch: when zoomed a drag pans the photo 1:1;
			 * otherwise a horizontal swipe drags the photo (and its
			 * neighbor) 1:1 and a release past the threshold flips
			 * to it, anything less springs back */
			{
				SceTouchData td;
				sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
				if (td.reportNum > 0) {
					float tx = td.report[0].x * 0.5f;
					float ty = td.report[0].y * 0.5f;
					if (!touch_active) {
						touch_active = 1;
						touch_dragged = 0;
						touch_start_x = tx;
						touch_start_y = ty;
						touch_panx0 = panx;
						touch_pany0 = pany;
					} else {
						float dx = tx - touch_start_x;
						float dy = ty - touch_start_y;
						if (!touch_dragged &&
						    (dx > 12.0f || dx < -12.0f ||
						     dy > 12.0f || dy < -12.0f))
							touch_dragged = 1;
						if (touch_dragged && zoomed) {
							panx = touch_panx0 + dx;
							pany = touch_pany0 + dy;
						} else if (touch_dragged &&
							   !slide_anim) {
							slide_x = dx;
							/* nothing beyond the ends */
							if (sel <= 0 && slide_x > 0)
								slide_x = 0;
							if (sel >= g_disp_count - 1 &&
							    slide_x < 0)
								slide_x = 0;
						}
					}
					touch_x = tx;
					touch_y = ty;
				} else if (touch_active) {
					touch_active = 0;
					if (touch_dragged && !zoomed && !slide_anim) {
						if (slide_x < -SCREEN_W / 4.0f &&
						    sel < g_disp_count - 1)
							slide_goal = -SCREEN_W;
						else if (slide_x > SCREEN_W / 4.0f &&
							 sel > 0)
							slide_goal = SCREEN_W;
						else
							slide_goal = 0; /* spring back */
						slide_anim = 1;
					} else if (!touch_dragged && is_video &&
						   !slide_anim) {
						/* tap on the poster plays it */
						g_dl_bg = detail_tex ?
							detail_tex :
							disp_thumb(sel);
						g_video_rot = 0;
						if (is_local)
							play_video_file(g_local_path[it.idx]);
						else
							view_video(it.idx);
						g_dl_bg = NULL;
						do {
							sceCtrlPeekBufferPositive(0, &pad, 1);
							sceKernelDelayThread(10 * 1000);
						} while (pad.buttons);
						prev_buttons = 0;
						continue;
					}
				}
			}

			/* run the slide animation; at the end the selection
			 * moves and the (centered) neighbor becomes current */
			if (slide_anim && !touch_active) {
				slide_x += (slide_goal - slide_x) * 0.35f;
				float dd = slide_goal - slide_x;
				if (dd < 4.0f && dd > -4.0f) {
					if (slide_goal < -1.0f && sel < g_disp_count - 1)
						sel++;
					else if (slide_goal > 1.0f && sel > 0)
						sel--;
					slide_x = 0;
					slide_goal = 0;
					slide_anim = 0;
				}
			}
			if (pressed & SCE_CTRL_CIRCLE) {
				/* back to the grid; if a full-res load is in
				 * flight its result gets discarded there */
				mode = MODE_GRID;
				detail_idx = -1;
				detail_loading = 0;
				slide_x = slide_goal = 0;
				slide_anim = 0;
				for (int k = 0; k < 2; k++) {
					if (pf_tex[k]) {
						vita2d_wait_rendering_done();
						vita2d_free_texture(pf_tex[k]);
						pf_tex[k] = NULL;
					}
					pf_d[k] = -1;
				}
				continue;
			}
			if (pressed & SCE_CTRL_CROSS) {
				if (is_video) {
					/* play even if the poster fetch failed */
					g_dl_bg = detail_tex ?
						detail_tex : disp_thumb(sel);
					g_video_rot = 0;
					if (is_local)
						play_video_file(g_local_path[it.idx]);
					else
						view_video(it.idx);
					g_dl_bg = NULL;
					/* wait for the buttons used inside the
					 * player to be released, so they don't
					 * also act on this screen */
					do {
						sceCtrlPeekBufferPositive(0, &pad, 1);
						sceKernelDelayThread(10 * 1000);
					} while (pad.buttons);
					prev_buttons = 0;
					continue;
				}
				if (detail_failed)
					detail_idx = -1; /* retry */
			}

			/* left/right may have moved the selection; refresh the
			 * item we load + draw below (the X handler above acted
			 * on the item shown when the frame started) */
			it = g_disp[sel];
			is_local = (it.src == SRC_LOCAL);
			is_video = disp_is_video(sel);

			/* collect a finished neighbor prefetch (and drop it
			 * if browsing already moved past its neighborhood) */
			if (pf_pending >= 0 && g_req_state == REQ_DONE) {
				__sync_synchronize();
				vita2d_texture *t = consume_detail_result();
				int keep = (pf_pending == sel - 1 ||
					    pf_pending == sel + 1 ||
					    pf_pending == sel);
				if (t && keep) {
					int slot = pf_tex[0] ? 1 : 0;
					if (pf_tex[slot]) {
						vita2d_wait_rendering_done();
						vita2d_free_texture(pf_tex[slot]);
					}
					pf_tex[slot] = t;
					pf_d[slot] = pf_pending;
				} else if (t) {
					vita2d_wait_rendering_done();
					vita2d_free_texture(t);
				}
				pf_pending = -1;
			}

			if (detail_idx != sel) {
				zoom = 1.0f; /* reset view for the new photo */
				panx = pany = 0.0f;
				if (detail_tex) {
					/* the GPU may still be drawing the
					 * previous frame with this texture */
					vita2d_wait_rendering_done();
					vita2d_free_texture(detail_tex);
					detail_tex = NULL;
				}
				detail_failed = 0;
				/* photos (and server videos, whose poster is
				 * the same preview endpoint) load async on the
				 * worker; the blown-up grid thumb shows
				 * meanwhile and O stays responsive */
				detail_loading = (!is_video ||
						  it.src == SRC_SERVER);
				detail_issued = 0;
				detail_idx = sel;
				/* prefetched? take it and skip the load */
				for (int k = 0; k < 2; k++) {
					if (pf_d[k] == sel && pf_tex[k]) {
						detail_tex = pf_tex[k];
						pf_tex[k] = NULL;
						pf_d[k] = -1;
						detail_loading = 0;
					}
				}
				/* drop cache entries no longer adjacent */
				for (int k = 0; k < 2; k++) {
					if (pf_d[k] >= 0 &&
					    (pf_d[k] < sel - 1 ||
					     pf_d[k] > sel + 1)) {
						vita2d_wait_rendering_done();
						vita2d_free_texture(pf_tex[k]);
						pf_tex[k] = NULL;
						pf_d[k] = -1;
					}
				}
			}

			/* drive the async full-res load */
			if (detail_loading) {
				if (!detail_issued) {
					if (pf_pending < 0 &&
					    g_req_state == REQ_DONE) {
						__sync_synchronize();
						consume_worker_result();
					}
					if (g_req_state == REQ_IDLE &&
					    disp_wants_thumb(sel)) {
						/* no low-res preview at all yet
						 * (opened mid-scroll): fetch the
						 * quick thumb first so something
						 * shows; the full-res request
						 * follows right after */
						req_issue_thumb(sel);
					} else if (g_req_state == REQ_IDLE) {
						g_req_idx = it.idx;
						g_req_src = it.src;
						g_req_detail = 1;
						g_req_is_video = 0;
						if (is_local)
							snprintf(g_req_path,
								 sizeof(g_req_path), "%s",
								 g_local_path[it.idx]);
						else
							snprintf(g_req_id,
								 sizeof(g_req_id), "%s",
								 g_asset_ids[it.idx]);
						__sync_synchronize();
						g_req_state = REQ_PENDING;
						detail_issued = 1;
					}
				} else if (g_req_state == REQ_DONE) {
					__sync_synchronize();
					detail_tex = consume_detail_result();
					detail_loading = 0;
					detail_failed = (detail_tex == NULL);
				}
			} else if (pf_pending < 0) {
				/* a stale detail result (the user moved on
				 * while it was in flight) would wedge the
				 * request slot: discard it */
				if (g_req_state == REQ_DONE) {
					__sync_synchronize();
					consume_worker_result();
				}
				if (g_req_state != REQ_IDLE ||
				    slide_anim || touch_active)
					goto pf_skip;
				/* the current item's own thumb first (e.g. a
				 * local video opened before its poster was
				 * extracted), then the neighbors': their grid
				 * thumbs are what the slide shows and what
				 * fills the screen the instant a swipe lands.
				 * the full-res prefetch follows. */
				if (disp_wants_thumb(sel)) {
					req_issue_thumb(sel);
					goto pf_skip; /* request slot taken */
				}
				int want[2] = { sel + 1, sel - 1 };
				for (int k = 0; k < 2; k++) {
					int t = want[k];
					if (t < 0 || t >= g_disp_count)
						continue;
					if (!disp_wants_thumb(t))
						continue;
					req_issue_thumb(t);
					goto pf_skip; /* request slot taken */
				}
				for (int k = 0; k < 2; k++) {
					int t = want[k];
					if (t < 0 || t >= g_disp_count)
						continue;
					if (pf_d[0] == t || pf_d[1] == t)
						continue;
					struct disp_item *ti = &g_disp[t];
					if (ti->src == SRC_LOCAL &&
					    g_local_is_video[ti->idx])
						continue; /* no poster re-extract */
					g_req_idx = ti->idx;
					g_req_src = ti->src;
					g_req_detail = 1;
					g_req_is_video = 0;
					if (ti->src == SRC_LOCAL)
						snprintf(g_req_path,
							 sizeof(g_req_path), "%s",
							 g_local_path[ti->idx]);
					else
						snprintf(g_req_id,
							 sizeof(g_req_id), "%s",
							 g_asset_ids[ti->idx]);
					__sync_synchronize();
					g_req_state = REQ_PENDING;
					pf_pending = t;
					break;
				}
			}
pf_skip:

			vita2d_start_drawing();
			vita2d_clear_screen();

			int sliding = (slide_anim ||
				       slide_x > 0.5f || slide_x < -0.5f);
			if (sliding) {
				/* current photo offset by the slide, neighbor
				 * riding alongside (its grid thumb blown up;
				 * the full image loads once it lands) */
				if (detail_tex)
					draw_texture_fitted(detail_tex, slide_x,
							    0, SCREEN_W, SCREEN_H);
				else {
					vita2d_texture *th = disp_thumb(sel);
					if (th)
						draw_texture_fitted(th, slide_x, 0,
								    SCREEN_W,
								    SCREEN_H);
				}
				int going_next = (slide_x < 0 || slide_goal < 0);
				int nb = going_next ? sel + 1 : sel - 1;
				float nx = slide_x +
					   (going_next ? SCREEN_W : -SCREEN_W);
				if (nb >= 0 && nb < g_disp_count) {
					/* prefer the prefetched full-res */
					vita2d_texture *th = NULL;
					if (pf_d[0] == nb)
						th = pf_tex[0];
					else if (pf_d[1] == nb)
						th = pf_tex[1];
					if (!th)
						th = disp_thumb(nb);
					if (th)
						draw_texture_fitted(th, nx, 0,
								    SCREEN_W,
								    SCREEN_H);
				}
			} else if (is_video) {
				/* video: its poster with a play button (the
				 * poster is the detail load for server videos,
				 * the grid thumb for local ones) */
				vita2d_texture *poster = detail_tex ?
					detail_tex : disp_thumb(sel);
				if (poster)
					draw_texture_fitted(poster, 0, 0,
							    SCREEN_W, SCREEN_H);
				draw_play_overlay();
			} else if (detail_tex) {
				draw_texture_zoom(detail_tex, zoom, &panx, &pany);
			} else if (detail_loading) {
				/* low-res grid thumb blown up while the full
				 * image is still on its way */
				vita2d_texture *th = disp_thumb(sel);
				if (th)
					draw_texture_fitted(th, 0, 0,
							    SCREEN_W, SCREEN_H);
				else
					draw_throbber(SCREEN_W / 2.0f,
						      SCREEN_H / 2.0f,
						      28.0f, frame);
			} else if (it.src == SRC_SERVER) {
				draw_error_detail(it.idx);
			} else {
				draw_centered(SCREEN_H / 2,
					      RGBA8(255, 80, 80, 255),
					      "Failed to load this photo");
			}

			const char *xhint = is_video ? "X play    " :
					    detail_failed ? "X retry    " : "";
			char hud[200];
			if (zoom > 1.001f)
				snprintf(hud, sizeof(hud),
					 "%d / %d    %.0f%%    stick/d-pad pan    L/R zoom    O back",
					 sel + 1, g_disp_count, zoom * 100.0f);
			else
				snprintf(hud, sizeof(hud),
					 "%d / %d    %.19s    < > browse  L/R zoom  %sO back",
					 sel + 1, g_disp_count,
					 g_disp_count > 0 ? disp_date(sel) : "",
					 xhint);
			draw_hud(hud);
			/* ...and a discreet corner throbber while the
			 * full-res image is still on its way — but only when
			 * a preview is showing; with no preview at all the
			 * big centered throbber is already up */
			if (detail_loading && disp_thumb(sel))
				draw_throbber(SCREEN_W - 36.0f, 36.0f,
					      14.0f, frame);

			vita2d_end_drawing();
			vita2d_swap_buffers();
		} else if (mode == MODE_CLOUD) {
			/* server + backup overview, opened from the cloud
			 * button on the search bar */
			if (g_srv_state == 0) {
				show_status("Loading server info...");
				fetch_server_info();
			}

			int total = g_local_count;
			int backed = count_state(SYNC_BACKED_UP);
			int remain = total - backed;

			/* auto-upload toggle switch + on-screen action buttons
			 * (also [] = toggle, X = details, /\ = upload) */
			float tgw = 60, tgh = 30, tgx = 470, tgy = 350;
			float bth = 46, bty = 408;
			float dbx = 56, dbw = 300;   /* See details (X glyph left)  */
			float ubx = 402, ubw = 300;  /* Upload all  (/\ glyph left) */
			int go_details = (pressed & SCE_CTRL_CROSS) != 0;
			int do_upload = (pressed & SCE_CTRL_TRIANGLE) != 0;
			int do_toggle = (pressed & SCE_CTRL_SQUARE) != 0;
			int do_back = (pressed & SCE_CTRL_CIRCLE) != 0;
			/* O / Back button in the top-right corner */
			float bkd = 36, bkcx = SCREEN_W - 30, bkcy = 36;

			/* touch: tap the button to open the details list */
			{
				SceTouchData td;
				sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
				if (td.reportNum > 0) {
					float tx = td.report[0].x * 0.5f;
					float ty = td.report[0].y * 0.5f;
					if (!touch_active) {
						touch_active = 1;
						touch_dragged = 0;
						touch_start_x = tx;
						touch_start_y = ty;
					} else if (fabsf(tx - touch_start_x) > 12 ||
						   fabsf(ty - touch_start_y) > 12) {
						touch_dragged = 1;
					}
					touch_x = tx;
					touch_y = ty;
				} else if (touch_active) {
					touch_active = 0;
					if (touch_dragged) {
						/* ignore drags */
					} else if (touch_y < bkcy + bkd &&
						   touch_x > bkcx - bkd - 70) {
						do_back = 1;   /* top-right Back */
					} else if (touch_y >= tgy - 6 &&
						   touch_y <= tgy + tgh + 6 &&
						   touch_x >= 60 && touch_x <= tgx + tgw) {
						do_toggle = 1;
					} else if (touch_y >= bty &&
						   touch_y <= bty + bth) {
						if (touch_x >= dbx &&
						    touch_x <= dbx + dbw)
							go_details = 1;
						else if (touch_x >= ubx &&
							 touch_x <= ubx + ubw)
							do_upload = 1;
					}
				}
			}

			if (do_back) {
				mode = MODE_GRID;
				continue;
			}
			if (do_toggle) {
				g_autobackup = !g_autobackup;
				save_autobackup();
				if (g_autobackup)
					queue_all_uploads();
				log_line("cloud: auto-upload %s",
					 g_autobackup ? "on" : "off");
			}
			if (do_upload) {
				int q = queue_all_uploads();
				log_line("cloud: queued %d uploads", q);
			}
			if (go_details) {
				mode = MODE_CLOUD_DETAILS;
				cloud_scroll = 0;
				continue;
			}

			vita2d_start_drawing();
			vita2d_clear_screen();
			draw_centered(40, RGBA8(255, 255, 255, 255),
				      "Server & Backup");

			/* top-right O / Back button: "Back" label then the circle */
			{
				int bw = text_width(0.95f, "Back");
				draw_text(bkcx - bkd / 2 - 8 - bw, bkcy + 6,
					  RGBA8(220, 220, 225, 255), 0.95f, "Back");
				draw_ps_button(bkcx, bkcy, bkd, ICON_CIRCLE,
					       RGBA8(235, 90, 85, 255), 26);
			}

			char line[300];
			uint32_t c = RGBA8(220, 220, 220, 255);
			uint32_t dim = RGBA8(150, 150, 155, 255);
			int y = 92;

			/* server storage with a usage bar */
			if (g_srv_state == 1 && g_srv_total[0]) {
				snprintf(line, sizeof(line),
					 "Server storage: %s of %s used (%d%%)",
					 g_srv_use[0] ? g_srv_use : "?",
					 g_srv_total, g_srv_pct);
				draw_text(60, y, c, 1.0f, line);
				y += 24;
				float bw = 600, bh = 14;
				vita2d_draw_rectangle(60, y, bw, bh,
						      RGBA8(48, 48, 54, 255));
				float f = g_srv_pct / 100.0f;
				if (f < 0) f = 0;
				if (f > 1) f = 1;
				vita2d_draw_rectangle(60, y, bw * f, bh,
						      RGBA8(94, 110, 215, 255));
				y += 40;
			} else {
				draw_text(60, y, dim, 1.0f,
					  "Server storage: unavailable");
				y += 40;
			}

			snprintf(line, sizeof(line), "Server version: %s",
				 g_srv_version[0] ? g_srv_version : "unknown");
			draw_text(60, y, c, 1.0f, line); y += 30;
			snprintf(line, sizeof(line), "Server URL: %.48s", g_server);
			draw_text(60, y, c, 1.0f, line); y += 30;
			snprintf(line, sizeof(line), "App version: %s", APP_VERSION);
			draw_text(60, y, c, 1.0f, line); y += 44;

			draw_text(60, y, RGBA8(255, 255, 255, 255), 1.0f,
				  "On this PS Vita"); y += 30;
			snprintf(line, sizeof(line), "  Images detected: %d", total);
			draw_text(60, y, c, 1.0f, line); y += 28;
			snprintf(line, sizeof(line), "  Backed up: %d", backed);
			draw_text(60, y, RGBA8(120, 200, 120, 255), 1.0f, line);
			y += 28;
			snprintf(line, sizeof(line), "  Remaining: %d", remain);
			draw_text(60, y, RGBA8(220, 180, 120, 255), 1.0f, line);

			/* auto-upload toggle switch */
			draw_text(60, tgy + 21, c, 1.0f, "Auto-upload new photos");
			vita2d_texture *tg = rounded_mask_tex((int)tgw, (int)tgh,
							      tgh / 2.0f);
			uint32_t trk = g_autobackup ? RGBA8(90, 180, 110, 255)
						    : RGBA8(70, 70, 76, 255);
			if (tg)
				vita2d_draw_texture_tint(tg, tgx, tgy, trk);
			else
				vita2d_draw_rectangle(tgx, tgy, tgw, tgh, trk);
			float kx = g_autobackup ? tgx + tgw - tgh / 2.0f
						: tgx + tgh / 2.0f;
			draw_disc(kx, tgy + tgh / 2.0f, tgh - 6.0f,
				  RGBA8(245, 245, 250, 255));
			draw_text(tgx + tgw + 14, tgy + 21,
				  g_autobackup ? RGBA8(120, 200, 120, 255) : dim,
				  0.95f, g_autobackup ? "ON" : "OFF");
			/* PS square button cueing the [] shortcut, left of switch */
			draw_ps_button(tgx - 23, tgy + tgh / 2, 38, ICON_SQUARE,
				       RGBA8(232, 120, 175, 255), 31);

			/* See details + Upload all buttons */
			vita2d_texture *db = rounded_mask_tex((int)dbw, (int)bth, 10.0f);
			if (db)
				vita2d_draw_texture_tint(db, dbx, bty,
							 RGBA8(46, 46, 52, 255));
			else
				vita2d_draw_rectangle(dbx, bty, dbw, bth,
						      RGBA8(46, 46, 52, 255));
			snprintf(line, sizeof(line), "See details (%d remaining)", remain);
			draw_text(dbx + 18, bty + bth - 16,
				  RGBA8(230, 230, 235, 255), 0.95f, line);
			/* PS cross button cueing the X shortcut, left of details */
			draw_ps_button(dbx - 23, bty + bth / 2, 38, ICON_CROSS,
				       RGBA8(120, 150, 235, 255), 37);

			vita2d_texture *ub = rounded_mask_tex((int)ubw, (int)bth, 10.0f);
			/* the upload button reads as active (indigo) when there's
			 * something to send, dimmed when everything is backed up */
			uint32_t uc = remain > 0 ? RGBA8(94, 110, 215, 255)
						 : RGBA8(46, 46, 52, 255);
			if (ub)
				vita2d_draw_texture_tint(ub, ubx, bty, uc);
			else
				vita2d_draw_rectangle(ubx, bty, ubw, bth, uc);
			draw_text(ubx + 18, bty + bth - 16,
				  RGBA8(245, 245, 250, 255), 0.95f, "Upload all");
			/* PS triangle button cueing the /\ shortcut, left of upload */
			draw_ps_button(ubx - 23, bty + bth / 2, 38, ICON_TRI,
				       RGBA8(95, 205, 130, 255), 28);

			draw_hud("X / tap details    O back");
			vita2d_end_drawing();
			vita2d_swap_buffers();
		} else if (mode == MODE_CLOUD_DETAILS) {
			/* scrollable list of not-backed-up local files + status */
			if (pressed & SCE_CTRL_CIRCLE) {
				mode = MODE_CLOUD;
				continue;
			}

			const int top = 86, rowh = 28, rows = (SCREEN_H - top - 40) / rowh;
			int total_unbacked = g_local_count - count_state(SYNC_BACKED_UP);
			int maxscroll = total_unbacked - rows;
			if (maxscroll < 0)
				maxscroll = 0;
			if (nav & SCE_CTRL_DOWN)
				cloud_scroll++;
			if (nav & SCE_CTRL_UP)
				cloud_scroll--;
			if (cloud_scroll > maxscroll)
				cloud_scroll = maxscroll;
			if (cloud_scroll < 0)
				cloud_scroll = 0;

			vita2d_start_drawing();
			vita2d_clear_screen();
			char line[300];
			snprintf(line, sizeof(line), "Not backed up (%d)",
				 total_unbacked);
			draw_centered(40, RGBA8(255, 255, 255, 255), line);

			/* walk local files, skipping backed-up ones, render the
			 * window [cloud_scroll, cloud_scroll+rows) */
			int seen = 0, drawn = 0, y = top;
			for (int j = 0; j < g_local_count && drawn < rows; j++) {
				if (g_local_state[j] == SYNC_BACKED_UP)
					continue;
				if (seen++ < cloud_scroll)
					continue;
				const char *p = g_local_path[j];
				const char *slash = strrchr(p, '/');
				const char *name = slash ? slash + 1 : p;
				int st = g_local_state[j];
				const char *stx = st == SYNC_HASHING ? "hashing" :
						  st == SYNC_UPLOADING ? "uploading" :
						  st == SYNC_FAILED ? "failed" :
						  "waiting";
				uint32_t sc = st == SYNC_UPLOADING ?
						RGBA8(120, 200, 120, 255) :
					      st == SYNC_HASHING ?
						RGBA8(220, 210, 120, 255) :
					      st == SYNC_FAILED ?
						RGBA8(220, 120, 120, 255) :
						RGBA8(170, 170, 178, 255);
				snprintf(line, sizeof(line), "%.46s", name);
				draw_text(40, y, RGBA8(220, 220, 220, 255),
					  0.9f, line);
				draw_text(SCREEN_W - 170, y, sc, 0.9f, stx);
				y += rowh;
				drawn++;
			}
			if (total_unbacked == 0)
				draw_centered(SCREEN_H / 2,
					      RGBA8(120, 200, 120, 255),
					      "Everything is backed up");

			draw_hud("Up/Down scroll    O back");
			vita2d_end_drawing();
			vita2d_swap_buffers();
		}

		/* Pace the loop to the display (~60 Hz). vita2d's swap doesn't
		 * block on vblank, so without this the loop free-runs at
		 * whatever speed the host allows — which makes the auto-repeat
		 * race the selection forward absurdly fast, fires a page-fetch
		 * storm, and churns thumbnail textures hard enough to fault the
		 * GPU. Pacing it also makes the frame-count throttling above
		 * behave the same here as on real hardware. */
		sceDisplayWaitVblankStart();
	}

	vita2d_wait_rendering_done();
	for (int i = 0; i < g_texpool_n; i++) /* drain the recycle pool */
		vita2d_free_texture(g_texpool[i]);
	g_texpool_n = 0;
	for (int k = 0; k < 2; k++)
		if (pf_tex[k])
			vita2d_free_texture(pf_tex[k]);
	if (detail_tex)
		vita2d_free_texture(detail_tex);
	for (int i = 0; i < g_asset_count; i++)
		if (g_thumb[i])
			vita2d_free_texture(g_thumb[i]);
	for (int i = 0; i < g_local_count; i++)
		if (g_local_thumb[i])
			vita2d_free_texture(g_local_thumb[i]);
	if (g_ttf)
		vita2d_free_font(g_ttf);
	vita2d_free_pgf(g_font);
	vita2d_fini();
	curl_global_cleanup();
	if (g_photo0_mounted)
		sceAppMgrUmount("photo0:");
	sceKernelExitProcess(0);
	return 0;
}
