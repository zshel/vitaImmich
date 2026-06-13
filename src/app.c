/* vitaImmich unity build — part of one translation unit.
 * This file is #included by src/main.c (in order) and shares all
 * file-scope state with the other parts; it is NOT compiled on its own.
 * Only src/main.c is listed in CMakeLists.txt. Editors/clangd may flag
 * undefined symbols here because of that — those are expected.
 */

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

enum { MODE_GRID, MODE_DETAIL, MODE_CLOUD, MODE_CLOUD_DETAILS };

/* the whole boot workload (auto sign-in, first library page, local media scan)
 * runs on this one thread so the main thread can animate a single, continuous
 * spinner from launch to grid instead of flashing a separate static screen per
 * step. g_boot_status is the caption it shows; g_boot_done signals completion;
 * g_boot_login_failed asks the main thread to drop to the interactive login. */
static char g_boot_status[48] = "Loading...";
static volatile int g_boot_done;
static volatile int g_boot_login_failed;
static int boot_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;
	/* stored email/password (no API key, not already signed in): try once */
	if (!g_apikey[0] && g_email[0] && !g_token[0]) {
		snprintf(g_boot_status, sizeof(g_boot_status), "Signing in...");
		char lerr[160];
		if (do_login(lerr, sizeof(lerr)) != 0) {
			g_boot_login_failed = 1;
			__sync_synchronize();
			g_boot_done = 1;
			return 0;
		}
	}
	snprintf(g_boot_status, sizeof(g_boot_status), "Loading your library...");
	fetch_page(0);
	snprintf(g_boot_status, sizeof(g_boot_status), "Scanning local media...");
	scan_local_media();
	/* reserve headroom so a resume-from-sleep rescan can append new camera
	 * files without reallocating while the worker/sync threads read */
	grow_locals(g_local_count + 512);
	rebuild_display();
	__sync_synchronize();
	g_boot_done = 1;
	return 0;
}

/* fetch the cloud page's server info off the main thread, so the page can
 * animate a throbber while the request is in flight (fetch_server_info sets
 * g_srv_state to 1/-1 when it finishes) */
static int srvinfo_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;
	fetch_server_info();
	return 0;
}

/* after a logout + sign-in (possibly a different account): drop the old
 * library/local lists and reload, with the sync thread paused and the
 * thumbnail worker drained so nothing reads/writes the arrays mid-reset.
 * main thread only. */
static void reset_for_account_change(void)
{
	draw_loading("Switching account...", 0);

	/* pause the sync thread (it reads g_local_* directly) */
	g_pause_bg = 1;
	for (int i = 0; i < 300 && !g_sync_idle; i++)
		sceKernelDelayThread(10 * 1000);

	/* drain any in-flight thumbnail request so a late REQ_DONE can't write
	 * g_thumb[] at an index that's about to become invalid */
	for (int i = 0; i < 300 && g_req_state == REQ_PENDING; i++)
		sceKernelDelayThread(10 * 1000);
	if (g_req_state == REQ_DONE) {
		free(g_req_pix); g_req_pix = NULL;
		free(g_req_raw); g_req_raw = NULL;
	}
	g_req_state = REQ_IDLE;
	g_req_idx = -1;
	__sync_synchronize();

	/* recycle every thumbnail and drop the old library + local lists */
	vita2d_wait_rendering_done();
	for (int i = 0; i < g_asset_count; i++)
		if (g_thumb[i]) {
			tex_release(g_thumb[i]);
			g_thumb[i] = NULL;
			g_thumb_failed[i] = 0;
		}
	for (int j = 0; j < g_local_count; j++)
		if (g_local_thumb[j]) {
			tex_release(g_local_thumb[j]);
			g_local_thumb[j] = NULL;
		}
	g_asset_count = 0;
	g_local_count = 0;
	g_disp_count = 0;
	g_sect_count = 0;
	g_next_page = 1;
	g_search_active = 0;
	g_search_count = 0;
	g_srv_state = 0;   /* re-fetch the cloud page's server info */
	g_srv_fetching = 0;
	__sync_synchronize();

	/* reload from the new account (sync still paused), then resume */
	fetch_page(0);
	scan_local_media();
	grow_locals(g_local_count + 512);
	rebuild_display();
	g_pause_bg = 0;
}

int main(void)
{
	vita2d_init();
	vita2d_set_clear_color(RGBA8(16, 16, 16, 255));
	g_font = vita2d_load_default_pgf();
	g_ttf = vita2d_load_font_file("app0:font.ttf");
	g_logo = vita2d_load_PNG_file("app0:logo.png");
	g_ic_server = vita2d_load_PNG_file("app0:cloud_server.png");
	g_ic_device = vita2d_load_PNG_file("app0:cloud_device.png");
	g_ic_both   = vita2d_load_PNG_file("app0:cloud_both.png");
	/* linear (bilinear) sampling so these scale/rotate smoothly instead of
	 * showing jagged stair-stepped edges (point sampling is the default) */
	vita2d_texture *icons[] = { g_logo, g_ic_server, g_ic_device, g_ic_both };
	for (unsigned i = 0; i < sizeof(icons) / sizeof(icons[0]); i++)
		if (icons[i])
			vita2d_texture_set_filters(icons[i],
				SCE_GXM_TEXTURE_FILTER_LINEAR,
				SCE_GXM_TEXTURE_FILTER_LINEAR);

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

	int have_cfg = (load_config() == 0);

	/* log the exact bytes of the server URL; invisible characters in the
	 * config show up here when curl complains about the protocol */
	{
		char hex[3 * 64 + 1] = "";
		int n = strlen(g_server);
		for (int i = 0; i < n && i < 64; i++)
			sprintf(hex + 3 * i, "%02x ", (unsigned char)g_server[i]);
		log_line("server='%s' len=%d hex=%s", g_server, n, hex);
	}

	/* auth: an API key needs nothing; missing config drops to interactive
	 * sign-in here. stored email/password is tried inside the boot thread. */
	if (!have_cfg)
		login_screen();

	/* one continuous spinner: the boot thread signs in (if needed), fetches
	 * the first page and scans local media while the main thread animates the
	 * logo with a single, never-resetting frame counter. */
	g_boot_done = 0;
	g_boot_login_failed = 0;
	SceUID bt = sceKernelCreateThread("boot", boot_thread,
					  0x10000100, 256 * 1024, 0, 0, NULL);
	if (bt >= 0) {
		sceKernelStartThread(bt, 0, NULL);
		unsigned int bf = 0;
		while (!g_boot_done) {
			draw_loading(g_boot_status, bf++);
			sceDisplayWaitVblankStart();
		}
	} else {
		/* fallback: run the same steps blocking, without the spinner */
		fetch_page(1);
		scan_local_media();
		grow_locals(g_local_count + 512);
		rebuild_display();
	}
	/* a stored-credential sign-in failed: drop to interactive login (main
	 * thread), then load blocking */
	if (g_boot_login_failed) {
		login_screen();
		fetch_page(1);
		scan_local_media();
		grow_locals(g_local_count + 512);
		rebuild_display();
	}
	if (g_asset_count == 0)
		fatal_error(NULL, "Server returned no assets (see log.txt)");

	SceUID worker = sceKernelCreateThread("thumb_loader", worker_thread,
					      0x10000100, 256 * 1024, 0, 0, NULL);
	if (worker >= 0)
		sceKernelStartThread(worker, 0, NULL);

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
	int bar_focus = 0;      /* search bar focused (d-pad up from the top row) */
	uint64_t last_tick = 0; /* wall-clock (incl. sleep) to detect resume */
	/* periodic check for photos added to the server while we run */
	uint64_t last_poll = sceKernelGetProcessTimeWide();
	int grid_last_sel = -1;   /* sel at the previous frame */
	int grid_settle = 0;      /* frames since sel last changed */
	int rl_idle = 1000;       /* frames since the last R/L month jump */
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

		/* resume from sleep: wall-clock (RTC) keeps running while the
		 * Vita is suspended, so a big jump between frames means we just
		 * woke — re-scan the camera folders for photos taken meanwhile */
		{
			SceRtcTick rt;
			if (sceRtcGetCurrentTick(&rt) == 0) {
				if (last_tick &&
				    rt.tick - last_tick > 3000000ULL) {
					if (rescan_local_new() > 0)
						rebuild_keep_view(&sel, &scroll,
								  &target);
				}
				last_tick = rt.tick;
			}
		}

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

		/* SELECT opens the cloud / backup page from the grid or detail */
		if ((pressed & SCE_CTRL_SELECT) &&
		    (mode == MODE_GRID || mode == MODE_DETAIL)) {
			mode = MODE_CLOUD;
			continue;
		}

		if (mode == MODE_GRID) {
			/* search bar: TRIANGLE opens the keyboard, CIRCLE (or
			 * the bar's clear chip) drops back to the timeline. a
			 * tap on the bar sets these too; handled after touch. */
			int do_search_open = (pressed & SCE_CTRL_TRIANGLE) != 0;
			int do_search_clear = g_search_active &&
					      (pressed & SCE_CTRL_CIRCLE) != 0;
			int do_open_cloud = 0; /* tap on the cloud button */

			/* SQUARE slides the top bar in/out (when not focused);
			 * while focused it drops focus back to the grid */
			if ((pressed & SCE_CTRL_SQUARE) && !bar_focus)
				bar_shown = !bar_shown;
			float bar_t = bar_shown ? 0.0f : (float)SEARCH_H;
			bar_hidden += (bar_t - bar_hidden) * 0.3f;
			if (fabsf(bar_t - bar_hidden) < 0.5f)
				bar_hidden = bar_t;
			/* the d-pad brings the selection square back — but not
			 * while the bar is focused; a touch drag hides it again */
			if ((nav & dirs) && !bar_focus)
				show_sel = 1;

			if (bar_focus) {
				/* bar focused (1=field, 2=map, 3=cloud): left/right
				 * move across the elements, down or [] returns to
				 * the grid, X activates the focused element */
				show_sel = 0;
				if (pressed & SCE_CTRL_SQUARE) {
					/* drop to the grid AND hide the bar */
					bar_focus = 0;
					show_sel = 1;
					bar_shown = 0;
				} else if (nav & SCE_CTRL_DOWN) {
					bar_focus = 0;
					show_sel = 1;
				} else {
					if ((nav & SCE_CTRL_LEFT) && bar_focus > 1)
						bar_focus--;
					if ((nav & SCE_CTRL_RIGHT) && bar_focus < 3)
						bar_focus++;
					if (pressed & SCE_CTRL_CROSS) {
						if (bar_focus == 1)
							do_search_open = 1;
						else if (bar_focus == 3)
							do_open_cloud = 1;
						/* map (2): placeholder, no-op */
					}
				}
			} else {
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
			if (nav & SCE_CTRL_UP) {
				int nr = nav_row(sel, -1);
				if (nr == sel) {        /* already on the top row */
					bar_focus = 1;
					bar_shown = 1;  /* reveal the bar if hidden */
					show_sel = 0;
				} else {
					sel = nr;
				}
			}
			if ((pressed & (SCE_CTRL_RTRIGGER |
					SCE_CTRL_LTRIGGER)) &&
			    g_disp_count > 0) {
				int dir = (pressed & SCE_CTRL_RTRIGGER) ?
					  +1 : -1;
				/* Only the FIRST deliberate jump after a pause
				 * may pull pages. rl_idle counts frames since the
				 * last R/L jump; a settle threshold alone isn't
				 * enough because mashing R at >12-frame intervals
				 * lets grid_settle creep back to 12 between
				 * presses, firing the blocking fetch+rebuild loop
				 * on every press. Gate on the PREVIOUS idle span
				 * (before resetting it for this press), so spam
				 * jumps stay purely in-memory and the catch-up
				 * fetch waits until you actually stop. */
				int can_pull = (rl_idle >= 30);
				rl_idle = 0;
				/* jumping down: the next month may simply not
				 * be fetched yet — pull pages until a new month
				 * shows up (or the library ends). search results
				 * aren't paginated, so skip the pull. */
				if (dir > 0 && !g_search_active && can_pull) {
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
			}  /* end !bar_focus */

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
			if (rl_idle < 1000)
				rl_idle++;
			int scrolling_fast = (grid_settle < 12);

			if ((pressed & SCE_CTRL_CROSS) && g_disp_count > 0 &&
			    !bar_focus) {
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
			if (!scrolling_fast && rl_idle >= 12 && !g_search_active &&
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
						bar_focus = 0;  /* touching cancels bar focus */
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
						} else if (touch_x > BAR_PX + BAR_W) {
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
						q, sizeof(q), 0);
					if (ok && q[0]) {
						/* run the search on a worker thread and
						 * spin a throbber while it's in flight */
						snprintf(g_search_pending,
							 sizeof(g_search_pending), "%s", q);
						g_search_done = 0;
						SceUID stid = sceKernelCreateThread("search",
							search_thread, 0x10000100,
							128 * 1024, 0, 0, NULL);
						int nres;
						if (stid >= 0) {
							sceKernelStartThread(stid, 0, NULL);
							char cap[160];
							snprintf(cap, sizeof(cap),
								 "Searching \"%s\"...", q);
							unsigned int sf = 0;
							while (!g_search_done) {
								vita2d_start_drawing();
								vita2d_clear_screen();
								draw_throbber(SCREEN_W / 2.0f,
									SCREEN_H / 2.0f - 16.0f,
									22.0f, sf++);
								draw_centered(SCREEN_H / 2 + 36,
									RGBA8(200, 200, 200, 255),
									cap);
								vita2d_end_drawing();
								vita2d_swap_buffers();
							}
							nres = g_search_result;
						} else {
							show_status("Searching \"%s\"...", q);
							nres = run_smart_search(q);
						}
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
				draw_status_badge(bx, by, bw, bh, i, frame);
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
			draw_search_bar(-bar_hidden, bar_focus);

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

			/* zoom with the triggers or the right stick (push up to
			 * zoom in, down to zoom out), continuous while held */
			if (!is_video && detail_tex) {
				if (pad.buttons & SCE_CTRL_RTRIGGER)
					zoom *= 1.04f;
				if (pad.buttons & SCE_CTRL_LTRIGGER)
					zoom /= 1.04f;
				float rz = (128 - pad.ry) / 128.0f; /* up positive */
				if (rz > 0.18f || rz < -0.18f)
					zoom *= 1.0f + rz * 0.05f;
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
					else
						draw_photo_placeholder(slide_x,
								       sel, frame);
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
					else
						draw_photo_placeholder(nx, nb, frame);
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
				 * image is still on its way; no preview yet ->
				 * a grey placeholder sized to the photo */
				vita2d_texture *th = disp_thumb(sel);
				if (th)
					draw_texture_fitted(th, 0, 0,
							    SCREEN_W, SCREEN_H);
				else
					draw_photo_placeholder(0, sel, frame);
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
			/* throbber centred over the preview while the full-res
			 * image is still loading */
			if (detail_loading && disp_thumb(sel) && !zoomed && !sliding)
				draw_throbber(SCREEN_W / 2.0f, SCREEN_H / 2.0f,
					      24.0f, frame);

			vita2d_end_drawing();
			vita2d_swap_buffers();
		} else if (mode == MODE_CLOUD) {
			/* server + backup overview, opened from the cloud
			 * button on the search bar */
			if (g_srv_state == 0) {
				/* fetch on a worker thread so the page animates a
				 * throbber instead of freezing on the request */
				if (!g_srv_fetching) {
					g_srv_fetching = 1;
					SceUID st = sceKernelCreateThread("srvinfo",
						srvinfo_thread, 0x10000100,
						64 * 1024, 0, 0, NULL);
					if (st >= 0)
						sceKernelStartThread(st, 0, NULL);
					else
						fetch_server_info(); /* fallback */
				}
				vita2d_start_drawing();
				vita2d_clear_screen();
				draw_throbber(SCREEN_W / 2.0f,
					      SCREEN_H / 2.0f - 16.0f, 22.0f, frame);
				draw_centered(SCREEN_H / 2 + 36,
					      RGBA8(200, 200, 200, 255),
					      "Loading server info...");
				if (pressed & SCE_CTRL_CIRCLE)
					mode = MODE_GRID;
				vita2d_end_drawing();
				vita2d_swap_buffers();
				continue;
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
			int do_logout = 0;
			/* O / Back button in the top-right corner */
			float bkd = 36, bkcx = SCREEN_W - 30, bkcy = 36;
			/* Log out button in the top-left corner */
			float lox = 16, loy = 18, low = 120, loh = 36;

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
					} else if (touch_x >= lox &&
						   touch_x <= lox + low &&
						   touch_y >= loy &&
						   touch_y <= loy + loh) {
						do_logout = 1; /* top-left Log out */
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
			if (do_logout) {
				/* best-effort server logout, clear credentials,
				 * persist, then return to the sign-in screen */
				membuf lb;
				long lc;
				char lu[600];
				snprintf(lu, sizeof(lu), "%s/api/auth/logout",
					 g_server);
				http_request(lu, "{}", &lb, &lc, NULL, 0);
				free(lb.data);
				g_token[0] = g_apikey[0] = '\0';
				g_email[0] = g_password[0] = '\0';
				g_serverip[0] = '\0';  /* drop the old LAN-IP pin */
				save_config();
				login_screen();   /* blocks until re-signed-in */
				/* drop the old account's library + reload */
				reset_for_account_change();
				sel = 0;
				scroll = target = 0;
				grid_last_sel = -1;
				for (int k = 0; k < 2; k++) {
					if (pf_tex[k]) {
						vita2d_wait_rendering_done();
						vita2d_free_texture(pf_tex[k]);
						pf_tex[k] = NULL;
					}
					pf_d[k] = -1;
				}
				mode = MODE_GRID;
				detail_idx = -1;
				prev_buttons = 0xFFFFFFFF; /* swallow held keys */
				touch_active = 0;
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

			/* top-left Log out button */
			{
				vita2d_texture *lo = rounded_mask_tex((int)low,
								      (int)loh, 10.0f);
				if (lo)
					vita2d_draw_texture_tint(lo, lox, loy,
						RGBA8(60, 46, 52, 255));
				int w = text_width(0.9f, "Log out");
				draw_text(lox + (low - w) / 2, loy + loh - 13,
					  RGBA8(235, 200, 205, 255), 0.9f, "Log out");
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

			draw_centered(SCREEN_H - 56, RGBA8(150, 150, 158, 255),
				      "Made by SadsArches with love");
			draw_hud("X / tap details    O back");
			vita2d_end_drawing();
			vita2d_swap_buffers();
		} else if (mode == MODE_CLOUD_DETAILS) {
			/* scrollable list of not-backed-up local files + status */
			float bkd = 36, bkcx = SCREEN_W - 30, bkcy = 36;
			int do_back = (pressed & SCE_CTRL_CIRCLE) != 0;
			/* touch: tap the top-right Back button */
			{
				SceTouchData td;
				sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1);
				if (td.reportNum > 0) {
					float tx = td.report[0].x * 0.5f;
					float ty = td.report[0].y * 0.5f;
					if (!touch_active) {
						touch_active = 1;
						if (ty < bkcy + bkd && tx > bkcx - bkd)
							do_back = 1;
					}
				} else {
					touch_active = 0;
				}
			}
			if (do_back) {
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
				draw_text(SCREEN_W - 220, y, sc, 0.9f, stx);
				y += rowh;
				drawn++;
			}
			if (total_unbacked == 0)
				draw_centered(SCREEN_H / 2,
					      RGBA8(120, 200, 120, 255),
					      "Everything is backed up");

			/* scroll position: "first-last of total" + a scrollbar */
			if (total_unbacked > rows) {
				snprintf(line, sizeof(line), "%d-%d of %d",
					 cloud_scroll + 1, cloud_scroll + drawn,
					 total_unbacked);
				draw_centered(66, RGBA8(150, 150, 158, 255), line);

				float trx = SCREEN_W - 14, ttop = top - 2;
				float tht = (float)rows * rowh;
				vita2d_draw_rectangle(trx, ttop, 4, tht,
						      RGBA8(60, 60, 66, 255));
				float th_h = tht * (float)rows / total_unbacked;
				if (th_h < 18)
					th_h = 18;
				float th_y = ttop + (tht - th_h) *
					     (float)cloud_scroll / maxscroll;
				vita2d_draw_rectangle(trx, th_y, 4, th_h,
						      RGBA8(160, 160, 170, 255));
			}

			/* top-right Back button (also press O) */
			draw_ps_button(bkcx, bkcy, bkd, ICON_CIRCLE,
				       RGBA8(235, 90, 85, 255), 26);

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
