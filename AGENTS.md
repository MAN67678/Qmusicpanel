# AGENTS.md

Instructions for AI coding agents working in this repo. `devnote.txt` is the
living design document and wins over anything here; read it first
(§0 status, §5 idle/input, §6a text metrics, §6b design rules, §11 config).

## What this is

QPANEL — a standalone terminal music player. C++20, CMake, one binary, no
daemon. `src/` is layered: term (raw mode, diff buffer, input parser), ui
(widgets, palettes, glyphs, vector layer), audio (miniaudio), dsp (FFTW
spectrum), media (TagLib + stb + chafa), fs (lazy tree), app (layout, input
map, run loop). Vendored headers live in `vendor/`; `src/vendor_impl.cpp` is
the single TU that instantiates them.

## Build and verify

```sh
cmake -S . -B build && cmake --build build -j8          # release
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address -g"
cmake --build build-asan -j8
```

Anything that touches the UI must be verified through the pty harness, not by
reading code:

```sh
COLS=160 ROWS=45 WAIT_FOR=METADATA KEYS='\x1b[<0;8;16M' \
  python3 tools/ptykeys.py "build/qpanel $HOME/Music" 1.5 3 out.bin
/tmp/qpvenv/bin/python tools/screen_text_png.py out.bin shot.png 160 45
```

* `WAIT_FOR=<text>` is required: keystrokes sent before raw mode is engaged are
  echoed by the tty and never reach the app.
* `tools/render.py` has a hardcoded 92x28 grid — use pyte for other sizes.
* `screen_text_png.py` needs pyte + pillow: use `/tmp/qpvenv/bin/python`.
* Check 92x28, 120x33 and 160x45 at least; a size that "looks fine" in code is
  usually the one that clips.
* Run the ASan binary through the same click/key sequence before claiming done.

## Invariants — breaking one re-introduces a bug we already paid for

* **A cell is a column, not a codepoint.** Use `cpWidth` / `decodeUtf8` /
  `drawText` / `drawTextTrunc` from `ui/Text.hpp`. Wide glyphs are 2 columns
  with a `cont` half-cell; never iterate text by codepoint.
* **Draw only inside `contentRect(r)`.** It already adds the frame + title row
  and `pad_x/pad_y`; adding another `+2` is the double-offset bug that has
  appeared twice (deck rows, spectrum).
* **One content gutter: 2 cells.** Deck uses `INL = q.x+2` / `INR = q.x+q.w-3`,
  metadata uses `cx = in.x+2`, the spectrum canvas is inset 2, and tree depth 1
  is the base level. New widgets must match or the layout reads lopsided.
* **Threads.** The library tree is owned by the UI thread: workers may call
  `scan()` only, and `graft()` happens on the UI thread via `postUi`. Decoder
  swaps (`audio_.load`) run on the UI thread. The device callback never blocks.
* **Idle/spectrum state.** `fadeEased_` is recomputed every frame, never inside
  a "value changed" branch. Any action that asserts the visualizer must clear
  `idle_` (`vizShow()` / `vizHide()`) or the idle timer cancels it next frame.
* **Keys** come from `VKey` in `term/Terminal.hpp`; never write raw virtual key
  integers in the dispatcher.
* **Config** is read on the UI thread. Capture anything a worker needs (e.g.
  `artFallback`) before posting the job.

## Style

* Comments explain *why* (usually "the bug this prevents"), in the same voice as
  `devnote.txt`. No narration of the diff.
* Touch only what the task needs — no drive-by refactors, renames or reformat.
* After fixing something, add one bullet to `devnote.txt` §0b (cause → fix) and
  keep §11 in sync when a config key changes.
* Keep `--help`, `README.md` and the config skeleton honest when behaviour
  changes; stale docs here are treated as bugs.

## Repo rules

* No git credentials, SSH key or `gh` on this machine: **commit locally and hand
  the push command to the user**, never try to authenticate.
* Never commit `build/`, `build-asan/`, `*.bin` dumps or ad-hoc PNGs.
  `docs/screenshot.png` is committed on purpose.
* Do not rewrite `~/.config/qpanel/config.ini` while testing — it overrides the
  built-in defaults and silently invalidates screenshots.
* `[layout] art_split` is parsed for compatibility but unused: the art dock is
  sized from its height (`w = 2*h`). Don't document it as working.
