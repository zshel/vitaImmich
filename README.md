# vitaImmich

Minimal proof-of-concept [Immich](https://immich.app/) client for the PS Vita.
It connects to your Immich server and shows your library (photos and videos)
as a scrollable chronological grid (newest first), with a full-screen viewer
and hardware-accelerated video playback.

## Requirements

- A PS Vita with HENkaku/h-encore and VitaShell
- An Immich server reachable from the Vita's Wi-Fi (recent Immich version —
  it uses `POST /api/search/random` and `GET /api/assets/{id}/thumbnail`)
- An Immich API key (web UI → Account Settings → API Keys)

## Building

Needs [VitaSDK](https://vitasdk.org/) with the `libvita2d`, `curl`, `openssl`,
`zstd`, `zlib`, `libpng`, `libjpeg-turbo` and `freetype` packages installed
(via `vdpm`).

```sh
export VITASDK=$HOME/vitasdk
export PATH=$VITASDK/bin:$PATH
cmake -B build -S .
cmake --build build
```

This produces `build/vitaImmich.vpk`.

## Installing & configuring

1. Copy `vitaImmich.vpk` to the Vita and install it with VitaShell.
2. Launch it once — it creates `ux0:data/vitaimmich/config.txt`.
3. Edit that file (VitaShell → SELECT for FTP makes this easy):

   ```
   server=http://192.168.1.100:2283
   apikey=your-immich-api-key
   ```

   If your server sits behind a public domain (reverse proxy) and your
   router does not support NAT loopback, add the server's LAN IP so the
   domain is pinned to it while on your home network:

   ```
   server=https://immich.example.com
   apikey=your-immich-api-key
   serverip=192.168.1.100
   ```

   This keeps the Host header and TLS SNI on the domain (so the reverse
   proxy still routes correctly) but connects to the LAN IP directly.

4. Relaunch the app.

## Controls

Grid view:

| Button        | Action                          |
| ------------- | ------------------------------- |
| D-pad         | Move selection (hold to repeat) |
| L / R         | Jump two rows up / down         |
| X             | Open item full screen           |
| SELECT        | Open the sync overview          |
| START         | Exit                            |

Full-screen view:

| Button        | Action                            |
| ------------- | --------------------------------- |
| Left / Right  | Previous / next item              |
| X             | Play video / retry a failed photo |
| O             | Back to grid                      |
| SELECT        | Open the sync overview            |
| START         | Exit                              |

Sync overview (SELECT):

| Button        | Action                              |
| ------------- | ----------------------------------- |
| X             | Queue all local-only files to upload|
| O / SELECT    | Back                                |
| START         | Exit                                |

Video playback:

| Button        | Action            |
| ------------- | ----------------- |
| X             | Pause / resume    |
| Left / Right  | Seek ±10 seconds  |
| O             | Stop and go back  |

More of the library is fetched automatically (100 photos at a time, newest
first) as you scroll toward the end. Errors are shown on screen and logged
to `ux0:data/vitaimmich/log.txt`.

## Syncing the Vita's camera media

On startup the app mounts the Photos app's storage (`photo0:`, i.e. the
ACL-protected `ux0:picture` where camera shots and recordings live) via
`sceAppMgrAppDataMount`, scans it (plus `ux0:video/CAMERA`, a few levels
deep; `.jpg/.jpeg/.png/.mp4`) and merges those files into the same
date-sorted grid as your server library.
Files larger than 512 MB are skipped so a movie collection in `ux0:video`
is never hashed or offered for upload. Both knobs live in `config.txt`:
`syncdir=<folder>` (repeatable, replaces the default scan locations) and
`syncmaxmb=<MB>` (the size cap; 0 disables it).
A small badge in the top-left corner of each cell shows its status:

- blue-grey dot — **cloud only** (a server asset not present on the Vita)
- orange up-arrow — **local only** (on the Vita, not yet on the server)
- blinking orange — **queued / uploading**
- green check — **backed up** (exists on both; shown once, as the server asset)
- red dot — **failed** (see the sync overview for the reason)

A background thread hashes each local file (SHA1) and asks the server which
ones already exist (`POST /api/assets/bulk-upload-check`). Hashing and the
duplicate check start automatically. Uploads do **not** start on their own:
open the sync overview with **SELECT** and press **X** to queue every
local-only file. Each upload is a `POST /api/assets` multipart request with
an `x-immich-checksum` header for fast server-side dedup; progress is shown
in the overview.

## PoC limitations

- TLS certificate verification is disabled (no CA bundle is shipped), so
  prefer plain HTTP on a trusted LAN or treat HTTPS as unverified.
- Thumbnails stream in on a background thread (one at a time over a
  keep-alive connection); cells show a grey placeholder until loaded.
- Videos are downloaded in full to `ux0:data/vitaimmich/video.mp4` before
  playback (deleted afterwards), so they need free space on the memory card
  and a moment to start. Playback uses the Vita's hardware decoder via
  SceAvPlayer, so only MP4 (H.264/AAC) plays — Immich's transcoded
  `video/playback` stream is used, which is H.264 with default server
  settings.
- Caps at 1000 server assets and 500 local camera files per launch; no
  albums or search.
- Local media sync is one-way (Vita → server). It scans `ux0:picture` and
  `ux0:video/CAMERA` by default (the standard camera locations), recursing
  ~2 levels; use `syncdir=` in the config to scan other folders.
- Local videos have no poster-frame thumbnail (the Vita has no still
  decoder for arbitrary MP4 frames), so their grid cell is a dark
  placeholder with the VIDEO badge; opening one still plays it.
- Hashing 500 files at startup takes a while; it runs in the background so
  browsing is never blocked, but statuses fill in gradually.
- `deviceId` is hard-coded to "PS Vita" and uploads are not associated with
  an album.
