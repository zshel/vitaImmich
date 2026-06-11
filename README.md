# vitaImmich

Minimal proof-of-concept [Immich](https://immich.app/) client for the PS Vita.
It connects to your Immich server and shows your photo library as a scrollable
chronological grid (newest first), with a full-screen viewer.

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

4. Relaunch the app.

## Controls

Grid view:

| Button        | Action                          |
| ------------- | ------------------------------- |
| D-pad         | Move selection (hold to repeat) |
| L / R         | Jump two rows up / down         |
| X             | Open photo full screen          |
| START         | Exit                            |

Full-screen view:

| Button        | Action                  |
| ------------- | ----------------------- |
| Left / Right  | Previous / next photo   |
| X             | Retry a failed photo    |
| O             | Back to grid            |
| START         | Exit                    |

More of the library is fetched automatically (100 photos at a time, newest
first) as you scroll toward the end. Errors are shown on screen and logged
to `ux0:data/vitaimmich/log.txt`.

## PoC limitations

- TLS certificate verification is disabled (no CA bundle is shipped), so
  prefer plain HTTP on a trusted LAN or treat HTTPS as unverified.
- Thumbnails stream in on a background thread (one at a time over a
  keep-alive connection); cells show a grey placeholder until loaded.
- Caps at 1000 photos per launch; no albums, search, video or upload.
