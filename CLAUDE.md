# CLAUDE.md — vitaImmich

PS Vita Immich client (PoC). C sources in `src/`. It's a **unity build**:
`main.c` holds the includes, `#define`s and core globals, then `#include`s the
section files (`state.c`, `draw.c`, `config.c`, `net.c`, `jpeg.c`, `api.c`,
`thumbs.c`, `display.c`, `ui.c`, `video.c`, `sync.c`, `app.c`) in order. They
form **one translation unit** and share all file-scope state — there are no
headers or `extern` decls. **Only `src/main.c` is compiled** (CMakeLists lists
just it); the parts are textual slices, so add new code to the relevant part,
not to CMake. `jsmn.h` is vendored.
Built with the VitaSDK toolchain into a `.vpk`, run in the **Vita3K** emulator,
and driven headlessly through the **kwin-mcp** server. This file is the
end-to-end loop: **build → install → launch → drive → read logs.** For the
narrative version see `AgenticLoop.md`; full key map is in `controls.MD`.

App title id: **`VIMM00001`**.

---

## 1. Build the changes → a `.vpk`

After editing anything in `src/` (or `CMakeLists.txt`), rebuild.

**Preferred — Docker (reproducible), writes `build/vitaImmich.vpk`:**

```bash
./build.sh
```

> `build/` ends up root-owned. If the Docker runtime is broken (containerd shim
> `readlink /proc/self/exe: no such file or directory`), use the native fallback.

**Fallback — native host SDK, also writes `build/vitaImmich.vpk`:**

```bash
export VITASDK="$HOME/vitasdk"
export PATH="$VITASDK/bin:$PATH"
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" .
cmake --build build --parallel "$(nproc)"
```

> If a prior Docker build left `build/` root-owned, clear it first
> (`sudo rm -rf build`) so the native build can write there.

Whichever path you use, the next step installs that `.vpk`.

---

## 2. Install the rebuilt VPK into Vita3K

Installing = launch Vita3K once with the `.vpk` as an argument; it extracts and
auto-boots. `deploy.sh` does install-then-launch in one shot:

```bash
./deploy.sh    # installs ./build/vitaImmich.vpk, then launches VIMM00001 detached
```

Gotchas (these are why it must run from a script, not inline):

- Background it from a **script file** with a `sleep`/`wait` then `pkill`. Inline
  backgrounding from the agent's shell does not persist.
- Guard `pkill` with `|| true` — a no-match exit code aborts the chain.
- Vita3K preserves the **zip's internal mtime** on extract. To confirm a *fresh*
  install, compare the installed `eboot.bin` mtime against the vpk entry, not
  wall-clock:

  ```bash
  unzip -l build/vitaImmich.vpk | grep eboot
  stat -c 'eboot: %y' ~/.local/share/Vita3K/Vita3K/ux0/app/VIMM00001/eboot.bin
  ```

- Wipe a stale install: `vita3k -d VIMM00001`.

> `deploy.sh` launches detached on the **live desktop**. For agent-driven
> verification with screenshots, **skip `deploy.sh`'s launch** and instead launch
> inside the MCP's isolated session (step 3) — only do the *install* half first
> (`vita3k ./build/vitaImmich.vpk` for ~4s, then `pkill`).

---

## 3. Launch + drive via kwin-mcp (screenshots + keystrokes)

Launch the app **inside kwin-mcp's own isolated KWin session** — its screenshot
tool only works there. `session_connect` to the live desktop does **not** capture
(spectacle can't save non-interactively; KWin's `ScreenShot2` DBus rejects
unauthorized callers). The isolated session is headless: the human cannot watch
it live, which is exactly why we capture every frame to disk.

0. **Wipe the per-session frame folder first:**
   `rm -rf agent-run/frames && mkdir -p agent-run/frames`. (`agent-run/` is
   git-ignored; `frames/` is the throwaway capture folder — your curated keepers
   stay in `agent-run/` proper and are never touched.)
1. **`session_start`** with `app_command: "vita3k -r VIMM00001"`,
   `keep_screenshots: true`, **and `env: {"SDL_VIDEODRIVER": "wayland",
   "DISPLAY": ""}`** — starts the isolated session, launches the app, and keeps
   every captured frame after `session_stop`. Give it **~12s** to boot.

   > **The `env` is mandatory.** The session inherits the host's `DISPLAY=:1`, so
   > without it Vita3K's SDL backend picks X11 and renders to the *host* display —
   > the captured frames come back **blank white**, `focus_window "Vita3K"` fails
   > ("no application matching"), and keystrokes never reach the app. Forcing
   > `SDL_VIDEODRIVER=wayland` and clearing `DISPLAY` makes Vita3K render into the
   > MCP's Wayland session, where capture and input both work. Sanity check:
   > `list_windows` should show a `Vita3K` window and frames should be ~300–500 KB,
   > not ~8 KB.
2. **`focus_window` with `app_name: "Vita3K"` BEFORE every key.** Without focus,
   keypresses are dropped. They also drop intermittently — re-focus and re-press.
   **One key per call**; rapid bursts get coalesced/dropped.
3. **`keyboard_key`** with `screenshot_after_ms: <ms>` after **every** command,
   so the frame folder is a complete visual record.
4. **`session_stop`** when done, then `pkill -9 -f vita3k`. kwin-mcp always writes
   frames to `/tmp/kwin-mcp-screenshots-*/` (not configurable) — move them into the
   repo and clear tmp, then tell the human where they are:
   ```bash
   cp /tmp/kwin-mcp-screenshots-*/*.png agent-run/frames/ 2>/dev/null
   rm -rf /tmp/kwin-mcp-screenshots-*
   ```

### Key map (this app's actions; full list in `controls.MD`)

| Vita button | host key (kwin-mcp)          | in-app action        |
|-------------|------------------------------|----------------------|
| D-pad       | `Right` `Left` `Up` `Down`   | move grid / browse   |
| Cross (X)   | `x`                          | view / play / replay |
| Circle (O)  | `c`                          | back                 |
| Select      | `Shift_R`                    | sync                 |
| Start       | `Return`                     | exit                 |

---

## 4. Read the logs — verify from TEXT, not frames

**Capturing a frame is free** (PNGs on disk are not in context). **Reading a PNG
back into context is the expensive part.** So capture every command, but **do not
Read frames back** during the loop. Verify progress from the app's own **text**
log instead:

```
~/.local/share/Vita3K/Vita3K/ux0/data/vitaimmich/log.txt
```

This is the app's own log — it records page fetches, video open/play, scans, and
errors. Also available:

- MCP **`read_app_log`** with the launched PID.
- `/tmp/vita3k-runtime.log` (Vita3K runtime stdout/stderr from `deploy.sh`).
- `/tmp/vita3k-install.log` (install output).

**Only Read a frame** when a screenshot is genuinely the only way to resolve an
ambiguity (rare). Reading every frame is what bloats context — that's what this
rule avoids.

### Typical verify loop

1. Build (step 1) → install half of step 2.
2. `rm -rf agent-run/frames && mkdir -p agent-run/frames`, then `session_start`
   (`keep_screenshots: true`, `env` forcing Wayland) → wait ~12s.
3. `focus_window "Vita3K"`.
4. `keyboard_key "Right"` (+ `screenshot_after_ms`), stepping to the target tile,
   one key per call; `x` to open, `x` again to play.
5. Confirm what happened from `log.txt` — **not** the frames.
6. `session_stop` + `pkill`; move frames into the repo and clear tmp
   (`cp /tmp/kwin-mcp-screenshots-*/*.png agent-run/frames/ 2>/dev/null && rm -rf /tmp/kwin-mcp-screenshots-*`),
   then hand `agent-run/frames/` to the human as the visual record.

---
