/* vitaImmich unity build — part of one translation unit.
 * This file is #included by src/main.c (in order) and shares all
 * file-scope state with the other parts; it is NOT compiled on its own.
 * Only src/main.c is listed in CMakeLists.txt. Editors/clangd may flag
 * undefined symbols here because of that — those are expected.
 */

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

/* startup loading screen: the Immich logo spinning, with text below it */
static void draw_loading(const char *text, unsigned int frame)
{
	vita2d_start_drawing();
	vita2d_clear_screen();
	float cx = SCREEN_W / 2.0f, cy = SCREEN_H / 2.0f - 30.0f;
	if (g_logo) {
		float lw = vita2d_texture_get_width(g_logo);
		float lh = vita2d_texture_get_height(g_logo);
		/* draw smaller than the source so the GPU minifies it (smooth
		 * edges) and spin slowly */
		float scale = 96.0f / lw;
		vita2d_draw_texture_scale_rotate_hotspot(g_logo, cx, cy,
			scale, scale, frame * 0.045f, lw / 2.0f, lh / 2.0f);
	}
	int w = text_width(1.0f, text);
	draw_text((SCREEN_W - w) / 2, cy + 90.0f,
		  RGBA8(225, 225, 230, 255), 1.0f, text);
	vita2d_end_drawing();
	vita2d_swap_buffers();
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

