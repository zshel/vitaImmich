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
/* email/password login: an empty apikey + these triggers POST /api/auth/login
 * at startup; the returned access token is sent as Authorization: Bearer */
static char g_email[128];
static char g_password[128];
static char g_token[256];
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
static vita2d_texture *g_logo;       /* bundled Immich logo (app0:logo.png) */
/* cloud status icons (white, from the Immich SVGs): on server / on device /
 * on both */
static vita2d_texture *g_ic_server;
static vita2d_texture *g_ic_device;
static vita2d_texture *g_ic_both;

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
/* unity build: the rest of the program lives in section files,        */
/* #included here in dependency order so they form one translation     */
/* unit and share all the file-scope state declared above. There are   */
/* no headers/extern decls — each part is just the next slice of what  */
/* was one main.c. Only this file is compiled (see CMakeLists.txt).    */
/* ------------------------------------------------------------------ */

#include "state.c"     /* local media + sync state, array-grow helpers   */
#include "draw.c"      /* text/logo/error drawing helpers                */
#include "config.c"    /* config.txt load/save                          */
#include "net.c"       /* curl/openssl, auth headers, HTTP helpers       */
#include "jpeg.c"      /* libjpeg decode to packed pixels                */
#include "api.c"       /* immich REST endpoints                          */
#include "thumbs.c"    /* background thumbnail loader + texture pool     */
#include "display.c"   /* merged server+local timeline + grid layout     */
#include "ui.c"        /* grid/detail/search/login/cloud screens         */
#include "video.c"     /* video download + SceAvPlayer playback          */
#include "sync.c"      /* local scan/hash/check/upload + sync overview   */
#include "app.c"       /* main(): init, event loop, threads             */
