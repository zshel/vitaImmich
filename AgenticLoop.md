# Agentic loop: build, run & drive vitaImmich in Vita3K

This is the loop an agent uses to make a change, run it in the Vita3K emulator,
and verify it by sending input + reading screenshots — all headless, on KDE
Plasma (Wayland).

## One-time setup

- VitaSDK (community): native toolchain at `~/vitasdk` (`arm-vita-eabi-gcc`,
  `vita-pack-vpk`, toolchain at `share/vita.toolchain.cmake`).
- Vita3K installed and on `PATH` (`/usr/bin/vita3k`).
- The MCP that drives the GUI: `uv tool install kwin-mcp` (configured in
  `.mcp.json` as the `kwin-mcp` server).

## 1. Build the VPK → `build/vitaImmich.vpk`

Preferred (Docker, reproducible):

```bash
./build.sh        # writes build/vitaImmich.vpk  (build/ ends up root-owned)
```

Fallback when the Docker runtime is broken (e.g. containerd shim error
`readlink /proc/self/exe: no such file or directory`), build natively with the
host SDK into `build-native/` (git-ignored):

```bash
export VITASDK="$HOME/vitasdk"
export PATH=$VITASDK/bin:$PATH
cmake -B build-native -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake .
cmake --build build-native --parallel "$(nproc)"
```

## 2. Install the VPK into Vita3K

Installing = launch Vita3K once with the `.vpk` as an argument; it extracts and
auto-boots. Gotchas:

- Run it **from a script file**, backgrounded with a wait then killed. Inline
  backgrounding from the agent's shell does not persist.
- Guard `pkill` with `|| true` — a no-match exit code aborts the chain.
- Vita3K extracts files preserving the **zip's internal mtime**, so to confirm a
  fresh install compare the installed `eboot.bin` mtime against
  `unzip -l <vpk> | grep eboot`, not wall-clock time.

```bash
#!/bin/bash
cd "$(git rev-parse --show-toplevel)"
pkill -9 -f vita3k 2>/dev/null || true
sleep 1
vita3k ./build-native/vitaImmich.vpk >/tmp/vinst.log 2>&1 &   # or ./build/...
sleep 8
pkill -9 -f vita3k 2>/dev/null || true
sleep 1
grep -i "successfully" /tmp/vinst.log | tail -1
stat -c 'eboot: %y' ~/.local/share/Vita3K/Vita3K/ux0/app/VIMM00001/eboot.bin
```

App title id: **`VIMM00001`**. To wipe a stale install: `vita3k -d VIMM00001`.

## 3. Launch + drive via kwin-mcp

Launch the app **inside the MCP's own isolated KWin session** — its screenshot
tool works there. (Connecting to the live desktop with `session_connect` does
*not* work for capture: `spectacle` can't save non-interactively, and KWin's
`ScreenShot2` DBus API rejects unauthorized callers — so the isolated session is
the only reliable way to capture, and it is headless: the human cannot watch it
live on their monitor.)

- **Before starting, wipe the per-session frame folder** so each run is clean:
  `rm -rf agent-run/frames && mkdir -p agent-run/frames`. (`agent-run/` is
  git-ignored; `frames/` is the throwaway capture folder — keepers live in
  `agent-run/` proper and are never touched.)
- `session_start` with `app_command: "vita3k -r VIMM00001"`,
  `keep_screenshots: true`, **and `env: {"SDL_VIDEODRIVER": "wayland", "DISPLAY":
  ""}`** — starts an isolated Wayland session, launches the app, and keeps every
  captured frame on disk after `session_stop`. Without that `env` the frames come
  back blank white and input is dropped (host `DISPLAY=:1` sends SDL to X11). Give
  it ~12s to boot.
- **`focus_window` with `app_name: "Vita3K"` BEFORE sending keys.** Without focus
  the keypresses are dropped. Presses also drop intermittently — re-focus and
  re-press. Send one key per call; rapid bursts get coalesced/dropped.
- `keyboard_key` with `screenshot_after_ms: [ms]` after **every** command, so the
  frame folder is a complete visual record of the run for the human to scrub.
- `session_stop` when done (and `pkill -9 -f vita3k`). kwin-mcp always writes
  frames to `/tmp/kwin-mcp-screenshots-*/` (that path is not configurable); don't
  leave them there — **move them into the repo and clear the tmp folder**:
  ```bash
  cp /tmp/kwin-mcp-screenshots-*/*.png agent-run/frames/ 2>/dev/null
  rm -rf /tmp/kwin-mcp-screenshots-*
  ```
  Then tell the human the frames are in `agent-run/frames/`.

### Zero-context-cost capture (the standing rule)

Capturing a frame (writing the PNG) costs the agent **nothing** — files on disk
are not in context. The cost is only incurred when the agent **Reads** a PNG
back. So:

- **Capture every command** (`screenshot_after_ms`) — free, and it gives the
  human the full record.
- **Do not Read frames back** into context during the loop. Verify progress from
  the app's own **text** log instead (cheap), not from images:
  `~/.local/share/Vita3K/Vita3K/ux0/data/vitaimmich/log.txt`.
- Only Read a frame when a screenshot is genuinely the only way to resolve an
  ambiguity (rare). Reading every frame is what bloats context and slows the
  session — that is exactly what this rule avoids.

### Key mapping (full list in `controls.MD`)

Vita3K maps host keys → Vita buttons. The ones this app uses:

| Vita button | host key (kwin-mcp) | in-app action            |
|-------------|---------------------|--------------------------|
| D-pad       | `Right` `Left` `Up` `Down` | move grid / browse |
| Cross (X)   | `x`                 | view / play / replay     |
| Circle (O)  | `c`                 | back                     |
| Select      | `Shift_R`           | sync                     |
| Start       | `Return`            | exit                     |

### Typical verify loop (capture everything, Read nothing)

1. `rm -rf agent-run/frames && mkdir -p agent-run/frames`, then `session_start`
   (`keep_screenshots: true`, `env` forcing Wayland) → wait ~12s.
2. `focus_window "Vita3K"`.
3. `keyboard_key "Right"` (with `screenshot_after_ms`) … stepping to the target
   tile, one key per call.
4. `keyboard_key "x"` to open, `x` again to play.
5. Confirm what happened from the **text** log, not the frames:
   - `~/.local/share/Vita3K/Vita3K/ux0/data/vitaimmich/log.txt` (the app's own log
     — records page fetches, video open/play, scans, errors)
   - MCP `read_app_log` with the launched PID, or `/tmp/vita3k-runtime.log`.
6. On `session_stop`, move the frames out of `/tmp` and clear it:
   `cp /tmp/kwin-mcp-screenshots-*/*.png agent-run/frames/ 2>/dev/null && rm -rf /tmp/kwin-mcp-screenshots-*`.
   Hand `agent-run/frames/` to the human as the visual record.
