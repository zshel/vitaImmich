/* vitaImmich unity build — part of one translation unit.
 * This file is #included by src/main.c (in order) and shares all
 * file-scope state with the other parts; it is NOT compiled on its own.
 * Only src/main.c is listed in CMakeLists.txt. Editors/clangd may flag
 * undefined symbols here because of that — those are expected.
 */

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
	/* map cluster gallery: show exactly the selected bubble's members (server
	 * assets), date-grouped like the timeline. Members whose metadata hasn't
	 * resolved yet have an empty date and sort to the end until it arrives. */
	if (g_gallery_active) {
		if (!grow_disp(g_gallery_count > 0 ? g_gallery_count : 1))
			return;
		int n = 0;
		for (int k = 0; k < g_gallery_count; k++) {
			g_disp[n].src = SRC_SERVER;
			g_disp[n].idx = g_gallery_idx[k];
			n++;
		}
		g_disp_count = n;
		qsort(g_disp, n, sizeof(g_disp[0]), disp_cmp);
		layout_grid();
		return;
	}

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
		if (g_asset_hidden[i])
			continue;   /* gallery-only asset: not in the timeline */
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

