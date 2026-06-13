# vitaImmich — Goals

Task list for the current round of work. Checked items are done.

## -1. UI polish round 2 (current)
- [x] Reload images taken by the camera while the app was asleep: detect
      resume via an RTC wall-clock jump (>3 s) and append-only re-scan the
      camera folders (headroom reserved at startup so no realloc races the
      worker/sync threads).
- [x] Pressing up on the d-pad (from the top row) focuses the search bar
      (indigo ring); down returns to the grid, X opens the keyboard.
- [x] Immich logo (bundled app0:logo.png, white bg made transparent) to the
      left of the search bar.
- [x] "Made by SadsArches with love" credit on the cloud menu.
- [x] Spinning Immich logo on the startup loading screen (first page fetched
      on a thread so the logo actually spins), loading text below.
- [x] Anti-alias the video play overlay (AA disc + baked play-triangle glyph).
- [x] O (back) circle button top-right in the detail view (tappable + O).

## 0. UI polish (current round)
- [x] Hide the white selection square while touch-scrolling; show it again
      when the selection moves with the d-pad.
- [x] Square (□) hides/shows the top search bar with a sliding animation.
      The hidden bar slides up and the grid shifts to fill the vacated strip
      (escroll = scroll + bar_hidden); the scroll-follow math cancels out.
- [x] Search results must use the justified/dynamic grid layout: smart search
      now requests withExif so results carry width/height (without it every
      asset had no ratio and the grid collapsed to uniform 3 columns).

## 1. Make asset/local storage scalable (remove 1000 / 500 caps)
- [x] Replace fixed `MAX_ASSETS` (1000) server-asset arrays with dynamically
      grown (realloc, doubling) arrays.
- [x] Replace fixed `MAX_LOCAL` (500) local-media arrays with dynamically grown
      arrays (grown during the startup scan, fixed afterward).
- [x] Replace the fixed `g_disp[DISP_MAX]` merged-timeline array with a grown
      array.
- [x] Decouple the thumbnail worker thread from the growable arrays (pass the
      request id/path through dedicated buffers) so a realloc on the main
      thread can never dangle a pointer the worker is reading.

## 2. Zoom and pan in the image viewer
- [x] Enable analog-stick sampling.
- [x] R trigger zoom in / L trigger zoom out in the detail view.
- [x] Left stick pans when zoomed in; pan is clamped to the image bounds.
- [x] Zoom/pan reset when moving to another photo; browsing left/right is
      disabled while zoomed.

## 3. Group the gallery by month and year
- [x] Compute a grid layout with section headers inserted whenever the
      month/year of the (date-sorted) timeline changes.
- [x] Draw "Month YYYY" headers; render items under the right header.
- [x] Scroll, selection-follow, visible-range and thumb eviction all use the
      computed per-item positions.

## 4. Show more relevant info in the SELECT (sync) menu
- [x] Photo vs video breakdown for server + local.
- [x] Total local media size, backed-up size and percentage.
- [x] Details for the currently selected item (name, date, size, state).

## 5. Fix the fast-scroll crash (pre-existing, reproduced in Vita3K)
- [x] Root cause: freeing a thumbnail texture unmaps its memblock while the
      GPU side (Vita3K's texture cache re-reads guest memory of cached
      textures) may still touch it — random-looking crashes deep in OpenSSL /
      libjpeg / strlen whenever eviction ran during a scroll. Fixed by
      recycling thumbnails through a texture pool (`tex_acquire`/`tex_release`)
      instead of ever freeing them mid-run.
- [x] Pace the main loop with `sceDisplayWaitVblankStart()` — without it the
      loop free-runs in the emulator, racing the held d-pad into a page-fetch
      storm (30+ requests/second against the server).
- [x] Suspend thumbnail loads/eviction and page fetches while the selection is
      still moving (`scrolling_fast` settle counter).
- [x] Serialize all `curl_easy_perform` calls behind one mutex — vitasdk's
      OpenSSL 1.0.2 is built without thread support, so concurrent TLS from
      the main/worker/sync threads is unsafe (also installed OpenSSL locking
      callbacks for defense in depth).
- [x] Pre-grow the newlib heap (96 MB, trim disabled) at boot so heap
      memblock mapping never happens while multiple threads run.
- [x] Verified: repeated 10–12 s held-scroll cycles to photo 1780+ across
      multiple pages, zero GPU faults / zero crashes (previous builds died
      within ~10 s).

## 6. Open bugs / polish
- [x] R (month jump) lands on the next month, but when more pages load the
      view shifts away from the landing spot — fixed: rebuild_keep_view pins
      the scroll/selection to the same photo across every rebuild (page
      fetch, poll, sync), and R now fetches ahead until the next month is
      actually loaded before jumping.
- [x] R scrolls the jumped-to month to the top of the screen, header band
      included.
- [x] Portrait videos play sideways — fixed: the asset's exif orientation
      is kept (g_asset_rot) and the player rotates decoded frames upright
      (draw_video_frame).
- [x] Replaced the "loading..." texts with a spinning throbber: centered
      when there is no preview at all, a small corner one while the full-res
      loads over the thumb.
