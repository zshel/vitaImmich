/* vitaImmich unity build — part of one translation unit.
 * This file is #included by src/main.c (in order) and shares all
 * file-scope state with the other parts; it is NOT compiled on its own.
 * Only src/main.c is listed in CMakeLists.txt. Editors/clangd may flag
 * undefined symbols here because of that — those are expected.
 */

/* ------------------------------------------------------------------ */
/* map / "places" view                                                 */
/*                                                                     */
/* Browse geotagged photos on a world map. Markers come from Immich's  */
/* GET /api/map/markers (a flat {id,lat,lon} list). There is no raster */
/* base map: markers are projected with Web Mercator onto a dark       */
/* canvas with a faint lat/lon graticule, and nearby ones are grouped  */
/* into count "bubbles" (re-clustered every frame in screen space, so  */
/* zooming in splits a bubble apart). Pan with the d-pad / touch drag,  */
/* zoom with L/R / pinch, X acts on the bubble nearest the centre      */
/* (zoom into a cluster, or open a lone photo), O returns to the grid. */
/* ------------------------------------------------------------------ */

/* marker arrays, grown (doubling) by the fetch; read by the main thread
 * only once g_map_state flips to 1 (so a half-parsed list never draws) */
static double *g_map_lat;
static double *g_map_lon;
static char (*g_map_id)[40];
static int g_map_count;
static int g_map_cap;
static volatile int g_map_state;     /* 0 not fetched, 1 ok, -1 failed */
static volatile int g_map_fetching;  /* a worker thread is fetching now */

/* view: Web-Mercator centre in normalised [0,1] coords and the world's
 * on-screen pixel size (256 << zoom). g_map_view_init is cleared so the
 * first time the page opens (and after an account switch) it fits to the
 * markers' bounding box instead of showing a stale view. */
/* zoom limits — g_map_scale is the whole world's on-screen width in px.
 * MIN stops zoom-out once the map covers the screen with no black bars; the map
 * is taller-aspect than the screen, so the poles and a little longitude crop off
 * (pan to reach them) rather than leaving margins. MAX is about country/region
 * level — ~4x the deepest tile LOD (10496px), so the raster base map stays
 * legible rather than blurring into mush as the old deep caps did. */
#define MAP_SCALE_MIN 1200.0
#define MAP_SCALE_MAX 4.0e4
/* the base map's ocean colour — fills the view behind/around the tiles so the
 * margins (and any gap while tiles stream in) read as sea, not black */
#define MAP_OCEAN_BG RGBA8(217, 187, 149, 255)
static double g_map_cx = 0.5, g_map_cy = 0.5;
static double g_map_scale = MAP_SCALE_MIN;
static int g_map_view_init;
static int g_map_grid = 0;   /* overlay the computed graticule (toggle: TRIANGLE) */

#define MAP_PI 3.14159265358979323846

/* The source map (a mapchart.net world map, city-label variant) is a
 * near-conformal full-longitude Web Mercator: lon spans the image width,
 * latitude band MAP_IMG_MY0..MY1. These bounds were fit by least-squares against
 * the pixel-identical graticule export of the same map (meridian residual
 * <0.5px, parallel <2.3px over 7000px) — that sibling shares this one's exact
 * crop (verified: 0px phase-correlation shift), so the fit transfers directly.
 * The image runs ~0.45° past ±180 each side. */
#define MAP_LON0 (-180.40)   /* longitude at the image's left edge  */
#define MAP_LON1 (180.50)    /* longitude at the image's right edge */

/* lon/lat -> normalised image coords [0,1]. y is standard Web-Mercator (lat
 * clamped to the valid band); the image's merc-y bounds are MAP_IMG_MY0/MY1. */
static double merc_x(double lon)
{
	return (lon - MAP_LON0) / (MAP_LON1 - MAP_LON0);
}
static double merc_y(double lat)
{
	if (lat > 85.05113) lat = 85.05113;
	if (lat < -85.05113) lat = -85.05113;
	double s = sin(lat * MAP_PI / 180.0);
	return 0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * MAP_PI);
}

/* normalised merc coords -> screen pixels under the current view */
static void map_project(double mx, double my, float *sx, float *sy)
{
	*sx = (float)((mx - g_map_cx) * g_map_scale) + SCREEN_W / 2.0f;
	*sy = (float)((my - g_map_cy) * g_map_scale) + SCREEN_H / 2.0f;
}

static int grow_map(int need)
{
	int c = g_map_cap;
	if (need <= c)
		return 1;
	if (!GROW(g_map_lat, c, need) ||
	    !GROW(g_map_lon, c, need) ||
	    !(g_map_cap = GROW(g_map_id, c, need)))
		return 0;
	return 1;
}

/* parse a /api/map/markers response: a flat JSON array of objects, each
 * with a string "id" and numeric "lat"/"lon". returns the marker count,
 * or -1 on a parse error. (other fields — city/state/country — ignored.) */
static int parse_map_markers(const char *js, size_t len)
{
	jsmn_parser p;
	jsmn_init(&p);
	int nt = jsmn_parse(&p, js, len, NULL, 0);
	if (nt <= 0)
		return -1;
	jsmntok_t *t = malloc(sizeof(*t) * nt);
	if (!t)
		return -1;
	jsmn_init(&p);
	nt = jsmn_parse(&p, js, len, t, nt);
	if (nt <= 0 || t[0].type != JSMN_ARRAY) {
		free(t);
		return -1;
	}

	g_map_count = 0;
	int cur = -1, cur_obj = -1;
	for (int i = 1; i < nt; i++) {
		if (t[i].type == JSMN_OBJECT && t[i].parent == 0) {
			if (!grow_map(g_map_count + 1))
				break; /* out of memory: keep what we have */
			cur = g_map_count++;
			cur_obj = i;
			g_map_id[cur][0] = '\0';
			g_map_lat[cur] = 0.0;
			g_map_lon[cur] = 0.0;
			continue;
		}
		if (cur < 0 || t[i].type != JSMN_STRING || t[i].size != 1 ||
		    i + 1 >= nt || t[i].parent != cur_obj)
			continue;
		int kl = t[i].end - t[i].start;
		const char *k = js + t[i].start;
		jsmntok_t *v = &t[i + 1];
		int vl = v->end - v->start;
		if (kl == 2 && !strncmp(k, "id", 2) && vl > 0 &&
		    vl < (int)sizeof(g_map_id[0]))
			snprintf(g_map_id[cur], sizeof(g_map_id[0]), "%.*s",
				 vl, js + v->start);
		else if (kl == 3 && !strncmp(k, "lat", 3))
			g_map_lat[cur] = atof(js + v->start);
		else if (kl == 3 && !strncmp(k, "lon", 3))
			g_map_lon[cur] = atof(js + v->start);
	}
	free(t);

	/* drop trailing entries that somehow lack an id */
	while (g_map_count > 0 && g_map_id[g_map_count - 1][0] == '\0')
		g_map_count--;
	return g_map_count;
}

/* fetch all geotagged markers (GET /api/map/markers). sets g_map_state to
 * 1 on success / -1 on failure; runs on a worker thread so the page can
 * animate a throbber while the request is in flight. */
static void fetch_map_markers(void)
{
	char url[600];
	snprintf(url, sizeof(url), "%s/api/map/markers", g_server);
	membuf buf;
	long code;
	CURLcode res = http_request(url, NULL, &buf, &code, NULL, 0);
	if (res != CURLE_OK || code < 200 || code >= 300) {
		log_line("map markers: curl %d http %ld: %.160s", res, code,
			 buf.data ? buf.data : "");
		free(buf.data);
		__sync_synchronize();
		g_map_state = -1;
		return;
	}
	int n = parse_map_markers(buf.data, buf.size);
	free(buf.data);
	if (n < 0) {
		log_line("map markers: parse failed");
		__sync_synchronize();
		g_map_state = -1;
		return;
	}
	log_line("map markers: %d geotagged", n);
	__sync_synchronize();
	g_map_state = 1;
}

/* keep the centre on the map and the zoom within sane bounds */
static void map_clamp_view(void)
{
	if (g_map_scale < MAP_SCALE_MIN) g_map_scale = MAP_SCALE_MIN;  /* whole world */
	if (g_map_scale > MAP_SCALE_MAX) g_map_scale = MAP_SCALE_MAX;  /* neighbourhood */
	if (g_map_cx < 0.0) g_map_cx = 0.0;
	if (g_map_cx > 1.0) g_map_cx = 1.0;
	if (g_map_cy < 0.0) g_map_cy = 0.0;
	if (g_map_cy > 1.0) g_map_cy = 1.0;
}

/* frame the markers' bounding box (with margin), or the whole world if there
 * are none / they share a point */
static void map_fit_view(void)
{
	if (g_map_count <= 0) {
		g_map_cx = 0.5;
		g_map_cy = 0.5;
		g_map_scale = MAP_SCALE_MIN;
		map_clamp_view();
		return;
	}
	double minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9;
	for (int i = 0; i < g_map_count; i++) {
		double mx = merc_x(g_map_lon[i]), my = merc_y(g_map_lat[i]);
		if (mx < minx) minx = mx;
		if (mx > maxx) maxx = mx;
		if (my < miny) miny = my;
		if (my > maxy) maxy = my;
	}
	g_map_cx = (minx + maxx) / 2.0;
	g_map_cy = (miny + maxy) / 2.0;
	double w = maxx - minx, h = maxy - miny;
	if (w < 1e-9) w = 1e-9;
	if (h < 1e-9) h = 1e-9;
	double sx = SCREEN_W * 0.82 / w, sy = SCREEN_H * 0.82 / h;
	g_map_scale = sx < sy ? sx : sy;
	/* a single point fits at "infinite" zoom — pull back to the max zoom */
	if (g_map_scale > MAP_SCALE_MAX)
		g_map_scale = MAP_SCALE_MAX;
	map_clamp_view();
}

/* clusters, rebuilt every frame from the on-screen markers. markers are
 * bucketed into a fixed screen grid; each non-empty cell becomes one bubble
 * at its members' centroid. g_cl_rep is a member's marker index, used to open
 * the photo when a bubble holds just one. */
#define MAP_CELL     58.0f
#define MAP_MAXCLUST 600
static float g_cl_x[MAP_MAXCLUST], g_cl_y[MAP_MAXCLUST];
static int g_cl_gx[MAP_MAXCLUST], g_cl_gy[MAP_MAXCLUST]; /* merc-grid cell */
static int g_cl_n[MAP_MAXCLUST];
static int g_cl_rep[MAP_MAXCLUST];
static double g_cl_sx[MAP_MAXCLUST], g_cl_sy[MAP_MAXCLUST]; /* screen sums */
static float g_cl_x0[MAP_MAXCLUST], g_cl_y0[MAP_MAXCLUST]; /* member screen bbox */
static float g_cl_x1[MAP_MAXCLUST], g_cl_y1[MAP_MAXCLUST];
static int g_cl_count;

/* Group markers into bubbles. The grid cells are anchored in MAP (Web-Mercator)
 * space, not screen space, so panning leaves each marker in the same cell and
 * the bubbles simply translate with the map instead of reshuffling/jumping.
 *
 * The cell size is keyed off a scale SNAPPED to the nearest power-of-two octave,
 * not the live scale: the live scale changes every frame while zooming, which
 * drifts the cell boundaries so markers hop cells and the bubbles jitter. With
 * the snap, the grid is fixed across a ~1.4x zoom band and only re-forms once
 * per octave, so during a zoom the bubbles just glide/scale with the map. */
static void map_cluster(void)
{
	g_cl_count = 0;
	if (g_map_scale <= 0.0)
		return;
	double qscale = pow(2.0, round(log2(g_map_scale)));  /* octave-snapped */
	double cellsz = (double)MAP_CELL / qscale;       /* merc units per cell */
	for (int i = 0; i < g_map_count; i++) {
		double mx = merc_x(g_map_lon[i]), my = merc_y(g_map_lat[i]);
		float sx, sy;
		map_project(mx, my, &sx, &sy);
		/* only consider markers near the viewport (generous margin so a
		 * bubble straddling the edge keeps a steady count while panning) */
		if (sx < -90 || sx > SCREEN_W + 90 ||
		    sy < -90 || sy > SCREEN_H + 90)
			continue;
		int gx = (int)(mx / cellsz), gy = (int)(my / cellsz);
		int c = -1;
		for (int j = 0; j < g_cl_count; j++)
			if (g_cl_gx[j] == gx && g_cl_gy[j] == gy) {
				c = j;
				break;
			}
		if (c < 0) {
			if (g_cl_count >= MAP_MAXCLUST)
				continue; /* grid saturated: drop the overflow */
			c = g_cl_count++;
			g_cl_gx[c] = gx;
			g_cl_gy[c] = gy;
			g_cl_n[c] = 0;
			g_cl_sx[c] = g_cl_sy[c] = 0.0;
			g_cl_x0[c] = g_cl_x1[c] = sx;
			g_cl_y0[c] = g_cl_y1[c] = sy;
			g_cl_rep[c] = i;
		}
		g_cl_n[c]++;
		g_cl_sx[c] += sx;
		g_cl_sy[c] += sy;
		if (sx < g_cl_x0[c]) g_cl_x0[c] = sx;
		if (sx > g_cl_x1[c]) g_cl_x1[c] = sx;
		if (sy < g_cl_y0[c]) g_cl_y0[c] = sy;
		if (sy > g_cl_y1[c]) g_cl_y1[c] = sy;
	}
	for (int j = 0; j < g_cl_count; j++) {
		g_cl_x[j] = (float)(g_cl_sx[j] / g_cl_n[j]);
		g_cl_y[j] = (float)(g_cl_sy[j] / g_cl_n[j]);
	}
}

/* bubble radius (px) for a member count: lone markers are small dots, the
 * rest grow with log(count) so a 200-photo cluster isn't 200x a 2-photo one */
static float map_bubble_r(int count)
{
	if (count <= 1)
		return 7.0f;
	float r = 14.0f + 9.0f * log10f((float)count);
	return r > 40.0f ? 40.0f : r;
}

/* cluster nearest the screen centre (what X acts on), or -1 if none */
static int map_nearest_cluster(void)
{
	int best = -1;
	float bestd = 1e18f;
	for (int j = 0; j < g_cl_count; j++) {
		float dx = g_cl_x[j] - SCREEN_W / 2.0f;
		float dy = g_cl_y[j] - SCREEN_H / 2.0f;
		float d = dx * dx + dy * dy;
		if (d < bestd) {
			bestd = d;
			best = j;
		}
	}
	return best;
}

/* cluster whose bubble contains screen point (x,y), or -1: touch hit-test */
static int map_cluster_at(float x, float y)
{
	for (int j = 0; j < g_cl_count; j++) {
		float r = map_bubble_r(g_cl_n[j]) + 6.0f;
		float dx = g_cl_x[j] - x, dy = g_cl_y[j] - y;
		if (dx * dx + dy * dy <= r * r)
			return j;
	}
	return -1;
}

/* resolve a marker's asset to a display slot, opening it in the detail view.
 * the map lists the whole server, so the asset is often not among the pages
 * fetched for the grid — in that case fetch its metadata, append it to the
 * library and rebuild the timeline so it has a slot. main-thread only (same
 * pattern as fetch_page). returns 1 and fills *out_sel on success. */
static int map_open_marker(int marker_idx, int *out_sel)
{
	if (marker_idx < 0 || marker_idx >= g_map_count)
		return 0;
	const char *id = g_map_id[marker_idx];
	int idx = -1;
	for (int i = 0; i < g_asset_count; i++)
		if (!strcmp(g_asset_ids[i], id)) {
			idx = i;
			break;
		}

	if (idx < 0) {
		char url[600];
		snprintf(url, sizeof(url), "%s/api/assets/%s", g_server, id);
		membuf buf;
		long code;
		CURLcode r = http_request(url, NULL, &buf, &code, NULL, 0);
		if (r != CURLE_OK || code < 200 || code >= 300) {
			log_line("map open %s: curl %d http %ld", id, r, code);
			free(buf.data);
			return 0;
		}
		char dval[DATELEN] = "", tval[12] = "";
		json_get(buf.data, buf.size, "fileCreatedAt", dval, sizeof(dval));
		json_get(buf.data, buf.size, "type", tval, sizeof(tval));
		free(buf.data);
		if (!grow_assets(g_asset_count + 1))
			return 0;
		idx = g_asset_count++;
		snprintf(g_asset_ids[idx], sizeof(g_asset_ids[0]), "%s", id);
		snprintf(g_asset_dates[idx], sizeof(g_asset_dates[0]), "%s", dval);
		g_asset_ratio[idx] = 0.0f;          /* unknown: detail view fits anyway */
		g_asset_rot[idx] = 0;
		g_asset_is_video[idx] = !strncmp(tval, "VIDEO", 5);
		g_asset_local_backed[idx] = 0;
		g_asset_hidden[idx] = 0;
		g_thumb[idx] = NULL;
		g_thumb_failed[idx] = 0;
		g_tex_err[idx][0] = '\0';
		/* opening from the map shouldn't be hijacked by an active search */
		g_search_active = 0;
		rebuild_display();
		log_line("map open: fetched asset %s (%s)", id,
			 g_asset_is_video[idx] ? "video" : "photo");
	}
	int s = find_disp(SRC_SERVER, idx);
	if (s >= 0) {
		*out_sel = s;
		return 1;
	}
	return 0;
}

/* latitude band of the source image, in normalised Web-Mercator. NOT simply
 * image-height/width: the source's vertical scale differs ~0.4% from its
 * horizontal, so these were fit against the image's baked-in graticule (the
 * equator sits at y=3213 of 6352, ~37px below image centre). ~83.7°N .. -83.3°S. */
#define MAP_IMG_MY0 0.03814   /* top edge  (~83.7°N) */
#define MAP_IMG_MY1 0.95125   /* bottom edge (~-83.3°S) */

/* ------------------------------------------------------------------ */
/* raster tile pyramid (app0:maptiles.pak) — the LOD base map.         */
/* Generated from the MapChart PNG by tools/png_to_tiles.py: several    */
/* zoom levels, each cut into 256px JPEG tiles. We pick the level whose */
/* native width matches the on-screen map width (crisp), and draw only  */
/* the visible tiles, decoding them into pooled textures (the safe path */
/* — freeing textures mid-run crashes Vita3K's texture cache). Tiles    */
/* span mx[0,1] x my[MAP_IMG_MY0,MAP_IMG_MY1], same georeferencing as   */
/* the markers, so everything lines up.                                 */
/* ------------------------------------------------------------------ */
#define TILE_PX     256
#define MAP_MAXLEV  8
static void map_draw_grid(void);   /* fallback, defined below */
static FILE *g_pak;
static long g_pak_blob;            /* file offset where JPEG data begins */
static int g_pak_nlev;
static int g_lev_w[MAP_MAXLEV], g_lev_h[MAP_MAXLEV];     /* content px */
static int g_lev_cols[MAP_MAXLEV], g_lev_rows[MAP_MAXLEV];
static int g_lev_base[MAP_MAXLEV]; /* index of this level's first tile */
static unsigned int *g_ti_off, *g_ti_size;
static int g_ti_n;

#define TCACHE 80
static struct tile_slot {
	int lev, col, row;
	vita2d_texture *tex;
	unsigned use;
} g_tc[TCACHE];
static unsigned g_tc_clock;
static int g_tile_budget;          /* new tile decodes allowed this frame */

static void load_maptiles(void)
{
	FILE *f = fopen("app0:maptiles.pak", "rb");
	if (!f)
		return;
	char magic[4];
	unsigned int ver, tile, nlev;
	if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "MTIL", 4) ||
	    fread(&ver, 4, 1, f) != 1 || fread(&tile, 4, 1, f) != 1 ||
	    fread(&nlev, 4, 1, f) != 1 ||
	    tile != TILE_PX || nlev == 0 || nlev > MAP_MAXLEV) {
		fclose(f);
		return;
	}
	g_pak_nlev = (int)nlev;
	for (int L = 0; L < g_pak_nlev; L++) {
		unsigned int w, h, c, r;
		if (fread(&w, 4, 1, f) != 1 || fread(&h, 4, 1, f) != 1 ||
		    fread(&c, 4, 1, f) != 1 || fread(&r, 4, 1, f) != 1) {
			fclose(f);
			return;
		}
		g_lev_w[L] = w; g_lev_h[L] = h;
		g_lev_cols[L] = c; g_lev_rows[L] = r;
	}
	unsigned int nt;
	if (fread(&nt, 4, 1, f) != 1 || nt == 0 || nt > 200000) {
		fclose(f);
		return;
	}
	g_ti_off = malloc(nt * sizeof(*g_ti_off));
	g_ti_size = malloc(nt * sizeof(*g_ti_size));
	if (!g_ti_off || !g_ti_size) {
		free(g_ti_off); free(g_ti_size);
		g_ti_off = g_ti_size = NULL;
		fclose(f);
		return;
	}
	int curlev = -1;
	unsigned int i;
	for (i = 0; i < nt; i++) {
		unsigned short lev, col, row;
		unsigned int off, sz;
		if (fread(&lev, 2, 1, f) != 1 || fread(&col, 2, 1, f) != 1 ||
		    fread(&row, 2, 1, f) != 1 || fread(&off, 4, 1, f) != 1 ||
		    fread(&sz, 4, 1, f) != 1)
			break;
		g_ti_off[i] = off;
		g_ti_size[i] = sz;
		if ((int)lev != curlev && lev < g_pak_nlev) {
			curlev = lev;
			g_lev_base[curlev] = (int)i;
		}
	}
	g_pak_blob = ftell(f);   /* index fully read: blob starts here */
	g_ti_n = (int)i;
	g_pak = f;               /* kept open for on-demand tile reads */
	log_line("maptiles: %d levels, %d tiles", g_pak_nlev, g_ti_n);
}

/* read + decode one tile JPEG into a pooled texture (NULL on any failure) */
static vita2d_texture *tile_decode(int L, int col, int row)
{
	if (!g_pak || L < 0 || L >= g_pak_nlev ||
	    col < 0 || col >= g_lev_cols[L] || row < 0 || row >= g_lev_rows[L])
		return NULL;
	int ti = g_lev_base[L] + row * g_lev_cols[L] + col;
	if (ti < 0 || ti >= g_ti_n)
		return NULL;
	unsigned int off = g_ti_off[ti], sz = g_ti_size[ti];
	if (sz == 0 || sz > 4u * 1024 * 1024)
		return NULL;
	char *buf = malloc(sz);
	if (!buf)
		return NULL;
	if (fseek(g_pak, g_pak_blob + off, SEEK_SET) != 0 ||
	    fread(buf, 1, sz, g_pak) != sz) {
		free(buf);
		return NULL;
	}
	int w, h, comps;
	char err[64];
	unsigned char *pix = decode_jpeg_buf(buf, sz, TILE_PX, &w, &h, &comps,
					     err, sizeof(err));
	free(buf);
	if (!pix) {
		log_line("tile %d/%d/%d: %s", L, col, row, err);
		return NULL;
	}
	SceGxmTextureFormat fmt = (comps == 1) ?
		SCE_GXM_TEXTURE_FORMAT_U8_R111 :
		SCE_GXM_TEXTURE_FORMAT_U8U8U8_BGR;
	vita2d_texture *t = tex_acquire(w, h, fmt);
	if (t) {
		unsigned char *dst = vita2d_texture_get_datap(t);
		unsigned int stride = vita2d_texture_get_stride(t);
		unsigned int rb = w * comps;
		for (int y = 0; y < h; y++)
			memcpy(dst + y * stride, pix + y * rb, rb);
		vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR,
					   SCE_GXM_TEXTURE_FILTER_LINEAR);
	}
	free(pix);
	return t;
}

/* fetch a tile from the cache, optionally loading (budget-limited) on a miss */
static vita2d_texture *tile_get(int L, int col, int row, int may_load)
{
	for (int i = 0; i < TCACHE; i++)
		if (g_tc[i].tex && g_tc[i].lev == L &&
		    g_tc[i].col == col && g_tc[i].row == row) {
			g_tc[i].use = ++g_tc_clock;
			return g_tc[i].tex;
		}
	if (!may_load || g_tile_budget <= 0)
		return NULL;
	vita2d_texture *t = tile_decode(L, col, row);
	if (!t)
		return NULL;
	g_tile_budget--;
	int slot = -1;
	unsigned oldest = 0xffffffffu;
	for (int i = 0; i < TCACHE; i++) {
		if (!g_tc[i].tex) { slot = i; break; }
		if (g_tc[i].use < oldest) { oldest = g_tc[i].use; slot = i; }
	}
	if (g_tc[slot].tex)
		tex_release(g_tc[slot].tex);   /* recycle, never free */
	g_tc[slot].tex = t;
	g_tc[slot].lev = L;
	g_tc[slot].col = col;
	g_tc[slot].row = row;
	g_tc[slot].use = ++g_tc_clock;
	return t;
}

/* the LOD whose native width best matches the on-screen map width */
static int tile_pick_level(void)
{
	int L = 0;
	while (L < g_pak_nlev - 1 && g_lev_w[L] < (int)g_map_scale)
		L++;
	return L;
}

/* draw every visible tile of level L (missing ones leave the coarser level
 * below showing through, so there are no blank gaps while tiles stream in) */
static void map_draw_level(int L)
{
	if (L < 0 || L >= g_pak_nlev)
		return;
	double span = MAP_IMG_MY1 - MAP_IMG_MY0;
	double lw = g_lev_w[L], lh = g_lev_h[L];
	for (int r = 0; r < g_lev_rows[L]; r++) {
		for (int c = 0; c < g_lev_cols[L]; c++) {
			double mx0 = (c * TILE_PX) / lw;
			double mx1 = ((c + 1) * TILE_PX) / lw;
			double my0 = MAP_IMG_MY0 + ((r * TILE_PX) / lh) * span;
			double my1 = MAP_IMG_MY0 + (((r + 1) * TILE_PX) / lh) * span;
			float sx0, sy0, sx1, sy1;
			map_project(mx0, my0, &sx0, &sy0);
			map_project(mx1, my1, &sx1, &sy1);
			if (sx1 < -1 || sx0 > SCREEN_W + 1 ||
			    sy1 < -1 || sy0 > SCREEN_H + 1)
				continue;
			vita2d_texture *t = tile_get(L, c, r, 1);
			if (!t)
				continue;
			vita2d_draw_texture_scale(t, sx0, sy0,
				(sx1 - sx0) / TILE_PX, (sy1 - sy0) / TILE_PX);
		}
	}
}

/* the tiled base map: a coarse overview first (no gaps), then the chosen LOD */
static void map_draw_tiles(void)
{
	g_tile_budget = 6;   /* cap new decodes per frame (avoids hitching) */
	if (g_pak_nlev <= 0) {
		map_draw_grid();   /* fallback if the pak is missing */
		return;
	}
	map_draw_level(0);
	int L = tile_pick_level();
	if (L > 0)
		map_draw_level(L);
}

/* faint lat/lon graticule for orientation (the only "base map" there is) */
static void map_draw_grid(void)
{
	uint32_t c = RGBA8(40, 44, 54, 255);
	for (int lon = -180; lon <= 180; lon += 30) {
		float sx, sy;
		map_project(merc_x(lon), 0.0, &sx, &sy);
		if (sx >= 0 && sx <= SCREEN_W)
			vita2d_draw_rectangle(sx, 0, 1, SCREEN_H, c);
	}
	for (int lat = -75; lat <= 75; lat += 15) {
		float sx, sy;
		map_project(0.0, merc_y(lat), &sx, &sy);
		if (sy >= 0 && sy <= SCREEN_H)
			vita2d_draw_rectangle(0, sy, SCREEN_W, 1, c);
	}
}

/* Lat/lon graticule, projected with merc_x/merc_y + MAP_LON0/1 + MAP_IMG_MY0/1
 * (the same maths the markers use), drawn bright over the tiled base map at 10°
 * spacing. The current city-label base map has no printed grid, so this is the
 * only graticule; against the graticule export it coincided to ~1px, which is
 * how the georeferencing was validated. Prime meridian and equator are brighter
 * to anchor orientation. Toggle with TRIANGLE. */
static void map_draw_graticule(void)
{
	uint32_t reg = RGBA8(225, 45, 45, 140);    /* translucent red */
	uint32_t axis = RGBA8(255, 90, 90, 230);   /* lon 0 / equator */
	/* clip the overlay to the base-map rectangle (clamped to the screen) so
	 * each line spans only where the map is, not the black margins around it */
	float lx, ty, rx, by;
	map_project(0.0, MAP_IMG_MY0, &lx, &ty);   /* image top-left  */
	map_project(1.0, MAP_IMG_MY1, &rx, &by);   /* image bot-right */
	if (lx < 0) lx = 0;
	if (ty < 0) ty = 0;
	if (rx > SCREEN_W) rx = SCREEN_W;
	if (by > SCREEN_H) by = SCREEN_H;
	for (int lon = -180; lon <= 180 && by > ty; lon += 10) {
		float sx, sy;
		map_project(merc_x(lon), 0.0, &sx, &sy);
		if (sx >= 0 && sx <= SCREEN_W)
			vita2d_draw_rectangle(sx, ty, 1, by - ty,
					      lon == 0 ? axis : reg);
	}
	for (int lat = -80; lat <= 80 && rx > lx; lat += 10) {
		float sx, sy;
		map_project(0.0, merc_y(lat), &sx, &sy);
		if (sy >= 0 && sy <= SCREEN_H)
			vita2d_draw_rectangle(lx, sy, rx - lx, 1,
					      lat == 0 ? axis : reg);
	}
}

/* draw every cluster bubble; `sel` gets a white selection ring */
static void map_draw_clusters(int sel)
{
	uint32_t fill = RGBA8(94, 110, 215, 235);
	uint32_t dot  = RGBA8(120, 140, 235, 235);
	uint32_t white = RGBA8(245, 245, 250, 255);
	for (int j = 0; j < g_cl_count; j++) {
		float x = g_cl_x[j], y = g_cl_y[j];
		int cnt = g_cl_n[j];
		float r = map_bubble_r(cnt);
		if (j == sel)
			draw_disc(x, y, (r + 3.0f) * 2.0f, white);
		if (cnt <= 1) {
			draw_disc(x, y, r * 2.0f, dot);
			continue;
		}
		draw_disc(x, y, r * 2.0f, fill);
		char b[12];
		snprintf(b, sizeof(b), "%d", cnt);
		int w = text_width(0.8f, b);
		draw_text(x - w / 2.0f, y + 6.0f, white, 0.8f, b);
	}
}

/* ------------------------------------------------------------------ */
/* cluster gallery                                                     */
/*                                                                     */
/* Selecting a bubble shows every photo it groups on the SAME grid as  */
/* the timeline/search — a filtered view (g_gallery_*). Members already */
/* paged into the library are reused; the rest are appended as hidden   */
/* g_asset entries (id + thumb only) whose date/aspect the metadata     */
/* worker fills in lazily, so the justified grid sorts into month       */
/* sections and true aspect ratios as the data arrives.                 */
/* ------------------------------------------------------------------ */

/* g_asset index for an id: an existing entry, or a freshly appended hidden one
 * (lets the gallery show a marker not yet paged into the library). -1 on OOM. */
static int asset_for_id(const char *id)
{
	for (int i = 0; i < g_asset_count; i++)
		if (!strcmp(g_asset_ids[i], id))
			return i;
	if (!grow_assets(g_asset_count + 1))
		return -1;
	int idx = g_asset_count++;
	snprintf(g_asset_ids[idx], sizeof(g_asset_ids[0]), "%s", id);
	g_asset_dates[idx][0] = '\0';   /* "" = unresolved; meta_worker fills it */
	g_asset_ratio[idx] = 0.0f;
	g_asset_rot[idx] = 0;
	g_asset_is_video[idx] = 0;
	g_asset_local_backed[idx] = 0;
	g_asset_hidden[idx] = 1;        /* gallery-only: kept out of the timeline */
	g_thumb[idx] = NULL;
	g_thumb_failed[idx] = 0;
	g_tex_err[idx][0] = '\0';
	return idx;
}

/* open the gallery for cluster c: collect its members (same merc cell as the
 * bubble) into g_gallery_idx, then rebuild the grid to show them */
static void gallery_open(int c)
{
	g_gallery_count = 0;
	if (c < 0 || c >= g_cl_count || g_map_scale <= 0.0)
		return;
	double qscale = pow(2.0, round(log2(g_map_scale)));
	double cellsz = (double)MAP_CELL / qscale;
	int tgx = g_cl_gx[c], tgy = g_cl_gy[c];
	for (int i = 0; i < g_map_count; i++) {
		double mx = merc_x(g_map_lon[i]), my = merc_y(g_map_lat[i]);
		float sx, sy;
		map_project(mx, my, &sx, &sy);
		if (sx < -90 || sx > SCREEN_W + 90 ||
		    sy < -90 || sy > SCREEN_H + 90)
			continue;
		if ((int)(mx / cellsz) != tgx || (int)(my / cellsz) != tgy)
			continue;
		int idx = asset_for_id(g_map_id[i]);
		if (idx < 0 || !grow_gallery(g_gallery_count + 1))
			break;
		g_gallery_idx[g_gallery_count++] = idx;
	}
	g_gallery_active = 1;
	rebuild_display();
}

static void gallery_close(void)
{
	g_gallery_active = 0;
	rebuild_display();
}

/* lazy fill: apply a finished metadata result (re-laying out the grid so the
 * item drops into its month/aspect), then request the next missing one —
 * visible slots first. g_meta_target is a g_asset index, so it stays valid
 * across the rebuilds that reorder g_disp. */
static void gallery_meta_pump(int first_vis, int last_vis,
			      int *sel, float *scroll, float *target)
{
	if (g_meta_state == META_DONE) {
		int t = g_meta_target;
		if (t >= 0 && t < g_asset_count && g_meta_date[0]) {
			snprintf(g_asset_dates[t], sizeof(g_asset_dates[0]), "%s",
				 g_meta_date);
			g_asset_ratio[t] = g_meta_ratio;
			g_asset_rot[t] = g_meta_rot;
			g_asset_is_video[t] = (unsigned char)g_meta_is_video;
		} else if (t >= 0 && t < g_asset_count) {
			/* fetch failed: stamp a placeholder date so we don't
			 * spin on it forever (it just won't group by month) */
			snprintf(g_asset_dates[t], sizeof(g_asset_dates[0]),
				 "0000-00-00");
		}
		g_meta_state = META_IDLE;
		rebuild_keep_view(sel, scroll, target);
	}
	if (g_meta_state != META_IDLE)
		return;
	int pick = -1;
	for (int d = first_vis; pick < 0 && d >= 0 && d <= last_vis &&
	     d < g_disp_count; d++)
		if (g_disp[d].src == SRC_SERVER &&
		    g_asset_dates[g_disp[d].idx][0] == '\0')
			pick = g_disp[d].idx;
	for (int k = 0; pick < 0 && k < g_gallery_count; k++)
		if (g_asset_dates[g_gallery_idx[k]][0] == '\0')
			pick = g_gallery_idx[k];
	if (pick < 0)
		return;
	g_meta_target = pick;
	snprintf(g_meta_id, sizeof(g_meta_id), "%s", g_asset_ids[pick]);
	__sync_synchronize();
	g_meta_state = META_PENDING;
}
