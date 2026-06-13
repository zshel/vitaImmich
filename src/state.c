/* vitaImmich unity build — part of one translation unit.
 * This file is #included by src/main.c (in order) and shares all
 * file-scope state with the other parts; it is NOT compiled on its own.
 * Only src/main.c is listed in CMakeLists.txt. Editors/clangd may flag
 * undefined symbols here because of that — those are expected.
 */

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

/* account switch (logout -> login): pause the sync thread while the main
 * thread drops the old library and reloads, so it can't read/realloc the
 * arrays mid-reset */
static volatile int g_pause_bg;    /* request the sync thread to idle */
static volatile int g_sync_idle;   /* sync thread acknowledges it idled */

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

