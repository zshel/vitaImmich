/* vitaImmich unity build — part of one translation unit.
 * This file is #included by src/main.c (in order) and shares all
 * file-scope state with the other parts; it is NOT compiled on its own.
 * Only src/main.c is listed in CMakeLists.txt. Editors/clangd may flag
 * undefined symbols here because of that — those are expected.
 */

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

/* the server-side fix for a too-high-bitrate clip, appended to the relevant
 * error messages so the user knows what to change on their Immich install */
#define SERVER_FIX_STEPS \
	"To fix it on the server: in Immich open Administration -> Settings -> " \
	"Video Transcoding, set Transcode policy to \"All videos\" and Max " \
	"bitrate to 10000k (or Target resolution to 720p), save, then run the " \
	"\"Transcode video\" job under Administration -> Jobs."

/* draw an error screen until O is pressed. title and detail are word-wrapped
 * to the screen, so long messages no longer run off the edges. */
static void show_blocking_error(const char *title, const char *detail)
{
	SceCtrlData pad;
	do {
		sceCtrlPeekBufferPositive(0, &pad, 1);
		vita2d_start_drawing();
		vita2d_clear_screen();
		int y = draw_centered_wrapped(150, RGBA8(255, 80, 80, 255),
					      title, SCREEN_W - 80, 34);
		if (detail && detail[0])
			draw_centered_wrapped(y + 16, RGBA8(205, 205, 205, 255),
					      detail, SCREEN_W - 80, 30);
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
	auth_header(keyhdr, sizeof(keyhdr));
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
	curl_off_t dl = 0;
	if (res == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &dl);
	log_line("video dl %s: res=%d code=%ld bytes=%lld",
		 g_asset_ids[idx], res, code, (long long)dl);
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
	void *r = memalign(alignment, size);
	/* the AVC decoder's working memory comes through here; a NULL or a big
	 * alloc is the prime suspect for the player dying right after parse */
	if (!r || size >= 0x100000)
		log_line("video: av_alloc %u align %u -> %p", size, alignment, r);
	return r;
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
	int mr = sceGxmMapMemory(base, size,
			SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE);
	log_line("video: gpu_alloc %u bytes align %u -> %p (map 0x%08x)",
		 size, alignment, base, mr);
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
static volatile int g_av_audio_frames; /* diag: audio frames pulled this play */

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
			g_av_audio_frames++;
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
	g_av_audio_frames = 0;
	SceUID audio_thid = sceKernelCreateThread("video_audio",
						  video_audio_thread,
						  0x10000100, 64 * 1024,
						  0, 0, NULL);
	if (audio_thid >= 0)
		sceKernelStartThread(audio_thid, 0, NULL);

	/* AddSource parses asynchronously; give it ~5 s to start */
	int active = 0;
	int waited = 0;
	for (waited = 0; waited < 300 && !(active = sceAvPlayerIsActive(g_avp)); waited++)
		show_video_status("Starting video...");
	log_line("video: active=%d after %d polls", active, waited);

	/* log every stream the demuxer found (codec/dims) — a video-only stream
	 * has just one entry, which is fine; this pinpoints decode failures */
	{
		SceAvPlayerStreamInfo si;
		for (int s = 0; s < 4; s++) {
			memset(&si, 0, sizeof(si));
			if (sceAvPlayerGetStreamInfo(g_avp, s, &si) < 0)
				break;
			log_line("video: stream %d type=%u dur=%llu", s,
				 (unsigned)si.type, (unsigned long long)si.duration);
		}
	}

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
	int frames = 0;       /* decoded video frames seen this play */
	int loops = 0;        /* main-loop iterations */
	int started = 0;      /* a frame has been decoded (real playback began) */
	/* AddSource is async: the player briefly flips active->inactive->active
	 * while it spins up its decoders, so an early IsActive==false is NOT the
	 * end of the stream. Don't treat it as EOS until either a frame has been
	 * decoded or this startup grace window has elapsed. */
	uint64_t play_t0 = sceKernelGetProcessTimeWide();
	int last_act = -1;    /* diag: log IsActive transitions through the loop */
	unsigned int prev = 0xffffffff; /* swallow the X press that got us here */

	while (active) {
		loops++;
		{
			int a = sceAvPlayerIsActive(g_avp);
			if (a != last_act) {
				log_line("video: IsActive=%d at loop %d t=%llums", a,
					 loops,
					 (unsigned long long)((sceKernelGetProcessTimeWide()
							       - play_t0) / 1000));
				last_act = a;
			}
		}
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

		if (!sceAvPlayerIsActive(g_avp) &&
		    (started ||
		     sceKernelGetProcessTimeWide() - play_t0 > 8000000ULL)) {
			ended_eos = 1;
			break; /* end of stream (or never started within 8 s) */
		}

		if (sceAvPlayerGetVideoData(g_avp, &vframe[buf_idx])) {
			if (frames == 0)
				log_line("video: first frame %ux%u pData=%p",
					 vframe[buf_idx].details.video.width,
					 vframe[buf_idx].details.video.height,
					 vframe[buf_idx].pData);
			frames++;
			started = 1;
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

	log_line("video: loop exit frames=%d loops=%d eos=%d audioframes=%d",
		 frames, loops, ended_eos, g_av_audio_frames);

	if (!active) {
		log_line("video: player never became active");
		show_blocking_error("Could not play this video",
				    "The player never started. The Vita decodes "
				    "H.264/AAC only; if this is HEVC, set Immich "
				    "Video Transcoding to convert it to H.264.");
	} else if (frames == 0) {
		/* the player parsed the file and went active, allocated its
		 * decoder buffers, then aborted without ever emitting a frame.
		 * On hardware this happens when the H.264 stream overruns the
		 * limits of the level declared in its SPS (e.g. a ~19 Mbps clip
		 * tagged Level 3.1, whose ceiling is 14 Mbps): the hardware
		 * decoder sizes its buffers from the declared level and rejects
		 * the stream. Not fixable client-side — the transcode is the
		 * problem. Tell the user instead of holding on a black frame. */
		log_line("video: active but 0 frames decoded (HW decoder "
			 "rejected the stream, or running under Vita3K)");
		show_blocking_error("This video's bitrate is too high to play",
				    "The player started but produced no frames. On a real "
				    "Vita this means the clip's bitrate exceeds the H.264 "
				    "level it declares, so the hardware decoder rejects it. "
				    SERVER_FIX_STEPS
				    "\n(The Vita3K emulator also decodes no video at all.)");
	}

	/* reached the end of the video: hold on the last frame with a replay
	 * prompt instead of returning (which, for a server video, drops the user
	 * back to the detail screen and would re-download to play it again). X
	 * replays from the still-local file; O leaves. The decoder buffers behind
	 * `cur` are still valid here — teardown happens below, after this loop. */
	if (ended_eos && frames > 0) {
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

/* ---- pre-download bitrate probe ------------------------------------------
 * Before pulling the whole stream we fetch just the MP4 header (the small
 * `moov` box) and decide whether the Vita's hardware H.264 decoder will accept
 * it. Everything we need is in `moov`: `avcC` carries the declared H.264 level
 * (which the decoder sizes its buffers from and refuses to overrun), and the
 * total file size (from the range response) over the `mvhd` duration gives the
 * average bitrate. If that bitrate exceeds the level's ceiling we show the
 * "too high" message immediately instead of downloading the whole clip only to
 * fail at playback. Immich's transcodes are faststart (moov at the front), so
 * a small head request usually has it; if not, we try the tail. The estimate
 * is approximate (average vs the decoder's peak/buffer limit), so this only
 * blocks clear cases and otherwise defers to the real playback attempt. */

enum { PROBE_OK = 0, PROBE_TOO_HIGH = 1, PROBE_UNKNOWN = -1 };
#define PROBE_HEAD_BYTES (256 * 1024)

static uint32_t rd_be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t rd_be64(const unsigned char *p)
{
	return ((uint64_t)rd_be32(p) << 32) | rd_be32(p + 4);
}

/* locate a 4CC box tag in a buffer; returns a pointer to the tag (payload
 * follows immediately after the 4 bytes), or NULL. a plain scan is enough for
 * the tags we want (avcC/mvhd/moov) — collisions in real files are negligible. */
static const unsigned char *find_tag(const unsigned char *buf, size_t len,
				     const char *tag)
{
	if (len >= 4)
		for (size_t i = 0; i + 4 <= len; i++)
			if (buf[i] == (unsigned char)tag[0] &&
			    !memcmp(buf + i, tag, 4))
				return buf + i;
	return NULL;
}

/* the H.264 level's bitrate ceiling in Mbps (High-profile MaxBR). the decoder
 * keys its buffer sizing off this, so a stream above it is what gets rejected. */
static double level_ceiling_mbps(int lvl)
{
	switch (lvl) {
	case 30: return 12.5;
	case 31: return 17.5;
	case 32: return 25.0;
	case 40: case 41: case 42: return 62.5;
	case 50: return 168.75;
	case 51: case 52: return 300.0;
	default: return lvl >= 51 ? 300.0 : lvl >= 40 ? 62.5 : 17.5;
	}
}

/* capture the total file size from a 206's "Content-Range: bytes a-b/TOTAL" */
static size_t probe_hdr_cb(char *b, size_t s, size_t n, void *ud)
{
	size_t len = s * n;
	long long *total = ud;
	if (len > 14 && !strncasecmp(b, "Content-Range:", 14)) {
		char *slash = memchr(b, '/', len);
		if (slash)
			*total = strtoll(slash + 1, NULL, 10);
	}
	return len;
}

/* range GET [start,end] of the asset's playback stream into `out`; sets
 * *total to the full file size from Content-Range. returns the HTTP status,
 * or -1 on transport error. */
static int http_get_range(int idx, long start, long end, membuf *out,
			  long long *total)
{
	char url[700];
	snprintf(url, sizeof(url), "%s/api/assets/%s/video/playback",
		 g_server, g_asset_ids[idx]);
	CURL *curl = curl_easy_init();
	if (!curl)
		return -1;
	char keyhdr[300];
	auth_header(keyhdr, sizeof(keyhdr));
	struct curl_slist *hdrs = curl_slist_append(NULL, keyhdr);
	char range[64];
	snprintf(range, sizeof(range), "%ld-%ld", start, end);
	out->data = NULL;
	out->size = 0;
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(curl, CURLOPT_RANGE, range);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, probe_hdr_cb);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, total);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "vitaImmich/0.1 (PS Vita)");
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	if (g_resolve_list)
		curl_easy_setopt(curl, CURLOPT_RESOLVE, g_resolve_list);
	net_lock();
	CURLcode res = curl_easy_perform(curl);
	net_unlock();
	long code = 0;
	if (res == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_slist_free_all(hdrs);
	curl_easy_cleanup(curl);
	if (res != CURLE_OK) {
		free(out->data);
		out->data = NULL;
		return -1;
	}
	return (int)code;
}

/* decide from the header whether the clip is decodable. on TOO_HIGH, fills
 * *level (level_idc, e.g. 31) and *mbps (estimated average bitrate). */
static int probe_video_bitrate(int idx, int *level, double *mbps)
{
	membuf head;
	long long total = -1;
	int code = http_get_range(idx, 0, PROBE_HEAD_BYTES - 1, &head, &total);
	/* need a real partial response (206) with a known total to estimate */
	if (code != 206 || total <= 0) {
		log_line("video probe %s: no usable range (code=%d total=%lld)",
			 g_asset_ids[idx], code, total);
		free(head.data);
		return PROBE_UNKNOWN;
	}

	const unsigned char *sbuf = (const unsigned char *)head.data;
	size_t slen = head.size;
	membuf tail;
	tail.data = NULL;
	/* faststart files have moov up front; otherwise grab the tail */
	if (!find_tag(sbuf, slen, "moov") && total > (long long)head.size) {
		long ts = total > PROBE_HEAD_BYTES ? (long)(total - PROBE_HEAD_BYTES) : 0;
		long long t2 = -1;
		if (http_get_range(idx, ts, total - 1, &tail, &t2) == 206) {
			sbuf = (const unsigned char *)tail.data;
			slen = tail.size;
		}
	}

	const unsigned char *avcc = find_tag(sbuf, slen, "avcC");
	const unsigned char *mvhd = find_tag(sbuf, slen, "mvhd");
	int verdict = PROBE_UNKNOWN;
	/* avcc payload[3] = level_idc; mvhd (v1) reads a 64-bit field at +24,
	 * so the payload must have 32 bytes available */
	if (avcc && avcc + 4 + 4 <= sbuf + slen && mvhd && mvhd + 4 + 32 <= sbuf + slen) {
		int lvl = avcc[4 + 3];
		const unsigned char *m = mvhd + 4;
		uint32_t timescale;
		uint64_t duration;
		if (m[0] == 1) {              /* version 1: 64-bit times */
			timescale = rd_be32(m + 20);
			duration = rd_be64(m + 24);
		} else {                      /* version 0: 32-bit times */
			timescale = rd_be32(m + 12);
			duration = rd_be32(m + 16);
		}
		double secs = timescale ? (double)duration / timescale : 0;
		if (secs > 0.5 && lvl > 0) {
			double est = (double)total * 8.0 / secs / 1e6;
			double ceil_mbps = level_ceiling_mbps(lvl);
			verdict = est > ceil_mbps ? PROBE_TOO_HIGH : PROBE_OK;
			*level = lvl;
			*mbps = est;
			log_line("video probe %s: level %d.%d, ~%.1f Mbps "
				 "(%lld B / %.1fs), ceiling %.1f -> %s",
				 g_asset_ids[idx], lvl / 10, lvl % 10, est,
				 (long long)total, secs, ceil_mbps,
				 verdict == PROBE_TOO_HIGH ? "reject" : "ok");
		}
	}
	if (verdict == PROBE_UNKNOWN)
		log_line("video probe %s: header incomplete (avcC=%p mvhd=%p) - "
			 "deferring to playback", g_asset_ids[idx],
			 (const void *)avcc, (const void *)mvhd);
	free(head.data);
	free(tail.data);
	return verdict;
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

	/* check the header before committing to the full download, so an
	 * unplayable (too-high-bitrate) clip is caught up front */
	vita2d_start_drawing();
	vita2d_clear_screen();
	if (g_dl_bg) {
		draw_texture_fitted(g_dl_bg, 0, 0, SCREEN_W, SCREEN_H);
		vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, RGBA8(0, 0, 0, 130));
	}
	draw_centered(SCREEN_H / 2, RGBA8(255, 255, 255, 255), "Checking video…");
	vita2d_end_drawing();
	vita2d_swap_buffers();

	int level = 0;
	double mbps = 0;
	if (probe_video_bitrate(idx, &level, &mbps) == PROBE_TOO_HIGH) {
		char det[512];
		snprintf(det, sizeof(det),
			 "This clip is about %.0f Mbps but declares H.264 Level "
			 "%d.%d, whose bitrate ceiling the Vita's hardware decoder "
			 "enforces. " SERVER_FIX_STEPS,
			 mbps, level / 10, level % 10);
		show_blocking_error("This video's bitrate is too high to play", det);
		g_dl_bg = NULL;
		return;
	}

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

