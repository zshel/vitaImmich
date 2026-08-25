/* vitaImmich unity build — part of one translation unit.
 * This file is #included by src/main.c (in order) and shares all
 * file-scope state with the other parts; it is NOT compiled on its own.
 * Only src/main.c is listed in CMakeLists.txt. Editors/clangd may flag
 * undefined symbols here because of that — those are expected.
 */

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
	enum { NSLOT = 14 }; /* a small fixed set of shapes (pill, buttons, ...) */
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
 * a small body rectangle filling between the side lobes. `k` erodes it
 * uniformly (px) so the outline variant is `filled && !filled(k)`. */
static int cloud_filled(float px, float py, int s, float k)
{
	float yb = s * 0.68f - k;
	int mid = sqrtf((px - s * 0.50f) * (px - s * 0.50f) +
			(py - s * 0.42f) * (py - s * 0.42f)) <= s * 0.22f - k;
	int lp = sqrtf((px - s * 0.34f) * (px - s * 0.34f) +
		       (py - s * 0.52f) * (py - s * 0.52f)) <= s * 0.16f - k;
	int rp = sqrtf((px - s * 0.66f) * (px - s * 0.66f) +
		       (py - s * 0.52f) * (py - s * 0.52f)) <= s * 0.16f - k;
	int base = (px >= s * 0.34f + k && px <= s * 0.66f - k &&
		    py >= s * 0.52f + k && py <= yb);
	return mid || lp || rp || base;
}

static int cov_cloud(float px, float py, int s)
{
	return cloud_filled(px, py, s, 0.0f);
}

/* a cloud outline (the filled shape minus an eroded copy) */
static int cov_cloud_line(float px, float py, int s)
{
	float w = s * 0.12f;
	if (w < 2.0f) w = 2.0f;
	return cloud_filled(px, py, s, 0.0f) && !cloud_filled(px, py, s, w);
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

/* a filled right-pointing play triangle */
static int cov_play(float px, float py, int s)
{
	return in_tri(px, py, s * 0.36f, s * 0.27f,
		      s * 0.36f, s * 0.73f, s * 0.76f, s * 0.5f);
}

/* cache one texture per glyph; all are drawn at a fixed size each frame */
static vita2d_texture *icon_tex(int which, int s)
{
	static vita2d_texture *cache[9];
	static int cs[9];
	if (which < 0 || which > 8)
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
				       which == 5 ? cov_cross :
				       which == 6 ? cov_circle :
				       which == 7 ? cov_play : cov_cloud_line;
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
#define ICON_PLAY   7
#define ICON_CLOUD_LINE 8

/* a DualShock-style face button: a dark disc (subtle lighter rim) centred at
 * (cx,cy) with the coloured glyph on top. each glyph keeps a per-call size. */
/* an anti-aliased filled disc baked into a white alpha mask (4x4 supersampled),
 * drawn tinted; replaces aliased vita2d_draw_fill_circle for UI discs */
static vita2d_texture *disc_tex(int d)
{
	/* keyed by size, no eviction: must be large enough to hold the fixed UI
	 * disc sizes (face buttons) plus the many cluster-bubble sizes, else a
	 * button drawn after the bubbles falls back to the aliased fill_circle */
	enum { NDISC = 24 };
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
#define BAR_PX  (BAR_M + 42.0f)            /* pill left x (logo to its left) */
#define BAR_W  (BTN1_CX - BTN_SZ / 2 - BTN_GAP - BAR_PX)  /* pill width */
#define BAR_CLEAR_CX (BAR_PX + BAR_W - 22.0f) /* centre of the clear chip */

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

/* draw the bar shifted vertically by `yoff` (Square slides it out of view);
 * `focus` highlights the focused element (1=field, 2=map, 3=cloud) */
static void draw_search_bar(float yoff, int focus)
{
	unsigned int field = RGBA8(38, 38, 42, 255);
	unsigned int hi = RGBA8(94, 110, 215, 255);   /* focus ring (indigo) */
	float by = BAR_Y + yoff, cy = BTN_CY + yoff;
	/* opaque strip so grid items scroll cleanly underneath the bar */
	vita2d_draw_rectangle(0, yoff, SCREEN_W, SEARCH_H, RGBA8(16, 16, 16, 255));
	/* Immich logo to the left of the search field */
	if (g_logo) {
		float ls = (BAR_H - 6.0f) / vita2d_texture_get_width(g_logo);
		vita2d_draw_texture_scale(g_logo, BAR_M, by + 3.0f, ls, ls);
	}
	/* focus ring behind the field */
	if (focus == 1) {
		vita2d_texture *ring = rounded_mask_tex((int)BAR_W + 6,
							(int)BAR_H + 6, BAR_R + 3);
		if (ring)
			vita2d_draw_texture_tint(ring, BAR_PX - 3, by - 3, hi);
	}
	/* borderless field: an AA rounded-rect mask tinted to the field colour */
	vita2d_texture *mask = rounded_mask_tex((int)BAR_W, (int)BAR_H, BAR_R);
	if (mask)
		vita2d_draw_texture_tint(mask, BAR_PX, by, field);
	else
		vita2d_draw_rectangle(BAR_PX, by, BAR_W, BAR_H, field);

	int isz = 24;
	vita2d_texture *icon = icon_tex(ICON_SEARCH, isz);
	if (icon)
		vita2d_draw_texture_tint(icon, BAR_PX + 22 - isz / 2.0f,
					 cy - isz / 2.0f,
					 RGBA8(190, 190, 196, 255));

	/* focus rings behind the placeholder buttons */
	if (focus == 2 || focus == 3) {
		vita2d_texture *r = rounded_mask_tex((int)BTN_SZ + 6,
						     (int)BTN_SZ + 6, 14.0f);
		float fcx = (focus == 2) ? BTN1_CX : BTN2_CX;
		if (r)
			vita2d_draw_texture_tint(r, fcx - (BTN_SZ + 6) / 2,
						 cy - (BTN_SZ + 6) / 2, hi);
	}
	/* placeholder buttons (map, cloud) for future features */
	draw_bar_button(BTN1_CX, cy, ICON_MAP);
	draw_bar_button(BTN2_CX, cy, ICON_CLOUD);

	float tx = BAR_PX + 44, ty = by + BAR_H - 13;
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
		     char *out, size_t outsz, int password)
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
	p.textBoxMode = password ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD :
				   SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
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

static void draw_hud(const char *text);   /* defined below */

/* persist server + credentials (and the loader's other settings) to config */
static void save_config(void)
{
	FILE *f = fopen(CONFIG_PATH, "w");
	if (!f)
		return;
	fprintf(f, "server=%s\n", g_server);
	fprintf(f, "apikey=%s\n", g_apikey);
	if (g_email[0])    fprintf(f, "email=%s\n", g_email);
	if (g_password[0]) fprintf(f, "password=%s\n", g_password);
	/* OAuth has no password to redo the login with, so its session token is
	 * the credential that gets saved instead */
	if (g_token[0] && !g_email[0]) fprintf(f, "token=%s\n", g_token);
	if (g_oauth_redirect[0]) fprintf(f, "oauth_redirect=%s\n", g_oauth_redirect);
	if (g_serverip[0]) fprintf(f, "serverip=%s\n", g_serverip);
	for (int i = 0; i < g_syncdir_count; i++)
		fprintf(f, "syncdir=%s\n", g_syncdirs[i]);
	if (g_syncmax_mb != 512)
		fprintf(f, "syncmaxmb=%ld\n", g_syncmax_mb);
	fclose(f);
}

/* give g_server an https:// scheme if it lacks one, strip a trailing slash */
static void normalize_server(void)
{
	size_t n = strlen(g_server);
	while (n > 0 && g_server[n - 1] == '/')
		g_server[--n] = '\0';
	if (g_server[0] && !strstr(g_server, "://")) {
		char tmp[512];
		snprintf(tmp, sizeof(tmp), "https://%s", g_server);
		snprintf(g_server, sizeof(g_server), "%s", tmp);
	}
}

/* renders `text` as a QR Code (via the vendored qrcodegen) into a cached
 * texture: black modules on white, with the standard 4-module quiet zone,
 * so a phone camera can find and decode it reliably regardless of what the
 * app draws behind it. keyed by string content — single slot, since the
 * OAuth screen only ever shows one address at a time. */
static vita2d_texture *qr_tex_for(const char *text)
{
	static vita2d_texture *cache;
	static char cache_text[1024];

	if (cache && !strcmp(cache_text, text))
		return cache;

	static uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
	static uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
	if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW,
				  qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
				  qrcodegen_Mask_AUTO, true))
		return NULL;

	int size = qrcodegen_getSize(qr);
	const int scale = 4;   /* raw px per module, before GPU minification */
	const int quiet = 4;   /* modules of white border (spec minimum) */
	int dim = (size + quiet * 2) * scale;

	vita2d_texture *t = vita2d_create_empty_texture_format(dim, dim,
		SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
	if (!t)
		return NULL;
	uint32_t *data = vita2d_texture_get_datap(t);
	int stride = (int)(vita2d_texture_get_stride(t) / 4);
	const uint32_t white = RGBA8(255, 255, 255, 255);
	const uint32_t black = RGBA8(0, 0, 0, 255);
	for (int y = 0; y < dim; y++) {
		int my = y / scale - quiet;
		for (int x = 0; x < dim; x++) {
			int mx = x / scale - quiet;
			int dark = mx >= 0 && mx < size && my >= 0 && my < size &&
				   qrcodegen_getModule(qr, mx, my);
			data[y * stride + x] = dark ? black : white;
		}
	}

	if (cache) {
		vita2d_wait_rendering_done();
		vita2d_free_texture(cache);
	}
	cache = t;
	snprintf(cache_text, sizeof(cache_text), "%s", text);
	return cache;
}

/* interactive OAuth sign-in: fetches the identity provider's authorization
 * URL from the server and lets the user paste back either the short "code"
 * value or the whole callback URL once they've completed the login
 * somewhere else. blocks. returns 1 and leaves the session token in g_token
 * on success; 0 otherwise (err is left empty if the user simply cancelled,
 * set to a message on failure). */
static int oauth_login_flow(char *err, size_t errlen, unsigned int frame)
{
	err[0] = '\0';

	char redirect[300];
	oauth_redirect_uri(redirect, sizeof(redirect));

	static char auth_url[1024];
	static char state[64];
	static char verifier[128];
	draw_loading("Starting OAuth...", frame);
	if (oauth_authorize(auth_url, sizeof(auth_url), state, sizeof(state),
			    verifier, sizeof(verifier), err, errlen) != 0)
		return 0;

	int sel = 0;               /* 0 = enter code/URL, 1 = cancel */
	static char input[512];
	char suberr[160] = "";
	unsigned int prev = 0;
	const float fx = 60, fw = 840, fh = 40, by = 380, cy = 430;
	int t_down = 0, t_drag = 0;
	float t_x = 0, t_y = 0, t_sx = 0, t_sy = 0;
	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		unsigned int pressed = pad.buttons & ~prev;
		prev = pad.buttons;
		frame++;

		if (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN)) sel ^= 1;
		if (pressed & SCE_CTRL_CIRCLE)
			return 0;

		int activate = (pressed & SCE_CTRL_CROSS) ? sel : -1;
		{
			SceTouchData td;
			sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
			if (td.reportNum > 0) {
				float tx = td.report[0].x * 0.5f;
				float ty = td.report[0].y * 0.5f;
				if (!t_down) {
					t_down = 1;
					t_drag = 0;
					t_sx = tx;
					t_sy = ty;
				} else if (fabsf(tx - t_sx) > 14 ||
					   fabsf(ty - t_sy) > 14) {
					t_drag = 1;
				}
				t_x = tx;
				t_y = ty;
			} else if (t_down) {
				t_down = 0;
				if (!t_drag && t_x >= fx && t_x <= fx + fw) {
					if (t_y >= by && t_y <= by + fh) {
						sel = 0;
						activate = 0;
					} else if (t_y >= cy && t_y <= cy + fh) {
						sel = 1;
						activate = 1;
					}
				}
			}
		}

		if (activate == 1)
			return 0; /* Cancel */
		if (activate == 0) {
			if (ime_input("Code (or full callback URL)", input,
				      input, sizeof(input), 0) && input[0]) {
				draw_loading("Signing in...", frame);
				if (oauth_exchange(redirect, state, verifier,
						   input, suberr,
						   sizeof(suberr)) == 0)
					return 1;
			}
			prev = 0xFFFFFFFF;
			t_down = 0;
			continue;
		}

		vita2d_start_drawing();
		vita2d_clear_screen();
		draw_centered(40, RGBA8(255, 255, 255, 255), "Sign in with OAuth");
		draw_centered(76, RGBA8(190, 190, 198, 255),
			"To continue signing in, please use another device.");

		/* QR code on the left, sized generously so its modules stay
		 * scannable from a phone camera; the address wrapped to its
		 * own column on the right so a long URL wraps onto multiple
		 * lines instead of running off the screen. */
		const int qr_x = 60, qr_y = 108, qr_dim = 210;
		vita2d_texture *qr = qr_tex_for(auth_url);
		if (qr) {
			float s = (float)qr_dim / vita2d_texture_get_width(qr);
			vita2d_draw_texture_scale(qr, (float)qr_x, (float)qr_y, s, s);
		}

		const int col_x = 300, col_w = 900 - 300;
		draw_text((float)col_x, 112.0f, RGBA8(150, 150, 158, 255), 1.0f,
			  "Address:");
		draw_left_wrapped(col_x, 136, RGBA8(140, 170, 250, 255),
				  auth_url, col_w, 22);

		vita2d_texture *eb = rounded_mask_tex((int)fw, (int)fh, 10.0f);
		if (eb)
			vita2d_draw_texture_tint(eb, fx, by,
				sel == 0 ? RGBA8(94, 110, 215, 255)
					 : RGBA8(38, 38, 42, 255));
		draw_centered((int)by + 26, RGBA8(245, 245, 250, 255),
			      "Enter code / URL");

		vita2d_texture *cb = rounded_mask_tex((int)fw, (int)fh, 10.0f);
		if (cb)
			vita2d_draw_texture_tint(cb, fx, cy,
				sel == 1 ? RGBA8(94, 110, 215, 255)
					 : RGBA8(38, 38, 42, 255));
		draw_centered((int)cy + 26, RGBA8(210, 210, 218, 255), "Cancel");

		if (suberr[0])
			draw_centered(SCREEN_H - 20, RGBA8(220, 120, 120, 255),
				      suberr);
		vita2d_end_drawing();
		vita2d_swap_buffers();
		sceDisplayWaitVblankStart();
	}
}

/* interactive sign-in: server + email + password via the on-screen keyboard,
 * or OAuth. blocks until a login succeeds, then saves the config. */
static void login_screen(void)
{
	int sel = 0;    /* 0 server, 1 email, 2 password, 3 log in, 4 demo, 5 oauth */
	char errmsg[160] = "";
	unsigned int prev = 0, frame = 0;
	/* field / button geometry, shared by the touch hit-test and the drawer */
	const float fx = 180, fw = 600, fh = 40, row0 = 186, rowgap = 50;
	const float by = row0 + 3 * rowgap + 4;   /* Log in button */
	const float dy = by + fh + 8;             /* Try demo button */
	const float ey = dy + fh + 8;             /* Log in with OAuth button */
	int lt_down = 0, lt_drag = 0;
	float lt_x = 0, lt_y = 0, lt_sx = 0, lt_sy = 0;
	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		unsigned int pressed = pad.buttons & ~prev;
		prev = pad.buttons;
		frame++;

		if (pressed & SCE_CTRL_UP)   sel = (sel + 5) % 6;
		if (pressed & SCE_CTRL_DOWN) sel = (sel + 1) % 6;

		/* activate an item with X, or by tapping it on the touchscreen */
		int activate = (pressed & SCE_CTRL_CROSS) ? sel : -1;
		{
			SceTouchData td;
			sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
			if (td.reportNum > 0) {
				float tx = td.report[0].x * 0.5f;
				float ty = td.report[0].y * 0.5f;
				if (!lt_down) {
					lt_down = 1;
					lt_drag = 0;
					lt_sx = tx;
					lt_sy = ty;
				} else if (fabsf(tx - lt_sx) > 14 ||
					   fabsf(ty - lt_sy) > 14) {
					lt_drag = 1;
				}
				lt_x = tx;
				lt_y = ty;
			} else if (lt_down) {
				lt_down = 0;
				if (!lt_drag && lt_x >= fx && lt_x <= fx + fw) {
					for (int i = 0; i < 3; i++) {
						float fy = row0 + i * rowgap;
						if (lt_y >= fy && lt_y <= fy + fh) {
							sel = i;
							activate = i;
						}
					}
					if (lt_y >= by && lt_y <= by + fh) {
						sel = 3;
						activate = 3;
					} else if (lt_y >= dy && lt_y <= dy + fh) {
						sel = 4;
						activate = 4;
					} else if (lt_y >= ey && lt_y <= ey + fh) {
						sel = 5;
						activate = 5;
					}
				}
			}
		}

		if (activate >= 0) {
			if (activate == 0) {
				ime_input("Server URL", g_server,
					  g_server, sizeof(g_server), 0);
				normalize_server();
			} else if (activate == 1) {
				ime_input("Email", g_email, g_email,
					  sizeof(g_email), 0);
			} else if (activate == 2) {
				ime_input("Password", g_password, g_password,
					  sizeof(g_password), 1);
			} else if (activate == 5) {
				g_token[0] = g_apikey[0] = '\0';
				normalize_server();
				if (!g_server[0]) {
					snprintf(errmsg, sizeof(errmsg),
						 "Enter a server first");
				} else {
					char oerr[160];
					if (oauth_login_flow(oerr, sizeof(oerr),
							     frame)) {
						save_config();
						return;
					}
					if (oerr[0])
						snprintf(errmsg, sizeof(errmsg),
							 "%s", oerr);
				}
			} else {
				/* activate 4 = Try demo: prefill the public demo */
				if (activate == 4) {
					snprintf(g_server, sizeof(g_server),
						 "https://demo.immich.app");
					snprintf(g_email, sizeof(g_email),
						 "demo@immich.app");
					snprintf(g_password, sizeof(g_password),
						 "demo");
				}
				g_token[0] = g_apikey[0] = '\0';
				normalize_server();
				if (!g_server[0] || !g_email[0] || !g_password[0]) {
					snprintf(errmsg, sizeof(errmsg),
						 "Enter server, email and password");
				} else {
					draw_loading("Signing in...", frame);
					char lerr[120];
					if (do_login(lerr, sizeof(lerr)) == 0) {
						save_config();
						return;
					}
					snprintf(errmsg, sizeof(errmsg),
						 "Login failed: %.100s", lerr);
				}
			}
			prev = 0xFFFFFFFF;   /* swallow buttons held in the dialog */
			lt_down = 0;
			continue;
		}

		vita2d_start_drawing();
		vita2d_clear_screen();
		if (g_logo) {
			float lw = vita2d_texture_get_width(g_logo);
			float s = 72.0f / lw;
			vita2d_draw_texture_scale(g_logo, SCREEN_W / 2.0f - 36.0f,
						  44.0f, s, s);
		}
		draw_centered(150, RGBA8(255, 255, 255, 255), "Sign in to Immich");

		const char *labels[3] = { "Server", "Email", "Password" };
		char pwmask[40];
		int pl = (int)strlen(g_password);
		if (pl > 24) pl = 24;
		memset(pwmask, '*', pl);
		pwmask[pl] = '\0';
		const char *vals[3];
		vals[0] = g_server[0] ? g_server : "";
		vals[1] = g_email[0] ? g_email : "";
		vals[2] = g_password[0] ? pwmask : "";

		for (int i = 0; i < 3; i++) {
			float fy = row0 + i * rowgap;
			if (sel == i) {
				vita2d_texture *r = rounded_mask_tex(
					(int)fw + 6, (int)fh + 6, 13.0f);
				if (r)
					vita2d_draw_texture_tint(r, fx - 3, fy - 3,
						RGBA8(94, 110, 215, 255));
			}
			vita2d_texture *bx = rounded_mask_tex((int)fw, (int)fh, 10.0f);
			if (bx)
				vita2d_draw_texture_tint(bx, fx, fy,
							 RGBA8(38, 38, 42, 255));
			draw_text(fx + 14, fy + 16, RGBA8(150, 150, 158, 255),
				  0.75f, labels[i]);
			draw_text(fx + 14, fy + 38, RGBA8(230, 230, 235, 255),
				  0.95f, vals[i]);
		}
		if (sel == 3) {
			vita2d_texture *r = rounded_mask_tex((int)fw + 6,
							     (int)fh + 6, 13.0f);
			if (r)
				vita2d_draw_texture_tint(r, fx - 3, by - 3,
							 RGBA8(120, 140, 235, 255));
		}
		vita2d_texture *lb = rounded_mask_tex((int)fw, (int)fh, 10.0f);
		if (lb)
			vita2d_draw_texture_tint(lb, fx, by,
				RGBA8(94, 110, 215, 255));
		draw_centered((int)by + 30, RGBA8(245, 245, 250, 255), "Log in");

		/* Try demo button */
		if (sel == 4) {
			vita2d_texture *r = rounded_mask_tex((int)fw + 6,
							     (int)fh + 6, 13.0f);
			if (r)
				vita2d_draw_texture_tint(r, fx - 3, dy - 3,
							 RGBA8(120, 140, 235, 255));
		}
		vita2d_texture *dbn = rounded_mask_tex((int)fw, (int)fh, 10.0f);
		if (dbn)
			vita2d_draw_texture_tint(dbn, fx, dy, RGBA8(50, 52, 60, 255));
		draw_centered((int)dy + 30, RGBA8(210, 210, 218, 255),
			      "Try demo");

		/* Log in with OAuth button */
		if (sel == 5) {
			vita2d_texture *r = rounded_mask_tex((int)fw + 6,
							     (int)fh + 6, 13.0f);
			if (r)
				vita2d_draw_texture_tint(r, fx - 3, ey - 3,
							 RGBA8(120, 140, 235, 255));
		}
		vita2d_texture *ob = rounded_mask_tex((int)fw, (int)fh, 10.0f);
		if (ob)
			vita2d_draw_texture_tint(ob, fx, ey, RGBA8(50, 52, 60, 255));
		draw_centered((int)ey + 26, RGBA8(210, 210, 218, 255),
			      "Log in with OAuth");

		if (errmsg[0])
			draw_centered(SCREEN_H - 46, RGBA8(220, 120, 120, 255),
				      errmsg);
		draw_hud("Up/Down select    X edit / log in");
		vita2d_end_drawing();
		vita2d_swap_buffers();
		sceDisplayWaitVblankStart();
	}
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

/* run the (blocking) smart search off the main thread so the grid can animate
 * a throbber while the request is in flight. the main thread only draws the
 * throbber meanwhile, so it never touches the display model this rebuilds. */
static int search_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;
	int r = run_smart_search(g_search_pending);
	__sync_synchronize();
	g_search_result = r;
	g_search_done = 1;
	return 0;
}

static void draw_hud(const char *text)
{
	(void)text;   /* bottom HUD bar removed */
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

/* grey panel (with a throbber) for a photo that hasn't loaded yet (mid-swipe,
 * or in the static view before even the low-res preview is in); sized to the
 * photo's fitted rect (from its aspect ratio) and offset horizontally by
 * `xoff`, so it matches where the real image lands */
static void draw_photo_placeholder(float xoff, int d, unsigned int frame)
{
	float ratio = disp_ratio(d);          /* w/h */
	float w = SCREEN_W, h = SCREEN_W / ratio;
	if (h > SCREEN_H) {
		h = SCREEN_H;
		w = SCREEN_H * ratio;
	}
	float x = (SCREEN_W - w) / 2.0f + xoff;
	float y = (SCREEN_H - h) / 2.0f;
	vita2d_draw_rectangle(x, y, w, h, RGBA8(38, 38, 44, 255));
	draw_throbber(x + w / 2.0f, y + h / 2.0f, 22.0f, frame);
}

/* translucent play button over a video's poster in the detail view (AA) */
static void draw_play_overlay(void)
{
	float cx = SCREEN_W / 2.0f, cy = SCREEN_H / 2.0f;
	draw_disc(cx, cy, 92.0f, RGBA8(20, 20, 20, 150));
	int gs = 56;
	vita2d_texture *p = icon_tex(ICON_PLAY, gs);
	if (p)
		vita2d_draw_texture_tint(p, cx - gs / 2.0f, cy - gs / 2.0f,
					 RGBA8(255, 255, 255, 235));
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

