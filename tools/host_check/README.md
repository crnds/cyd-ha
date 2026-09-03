# Host-compiled equivalence harness

Regression net for the structural refactor tracked in this repo — **not** a
test of pixel output. `simulator.html` already owns visual fidelity to
hardware (real glyph decode, byte-identical to `TFT_eSPI::drawChar`); this
harness exists because `simulator.html` mirrors geometry constants, not the
C++ dirty-region logic in `src/ui/screen.cpp`, and no CYD is attached to this
machine to verify that logic by flashing it.

## What it does

`driver.cpp` `#include`s `src/ui/screen.cpp` directly into its own
translation unit — the functions being checked (`btnActive`, `scenePlan`,
`pctMatches`, `kelvinMatches`, `iconVis`, `bulbHue`, `btnRect`/`briRect`/
`nightRect`/`tabRect`, `screenHitTest`) are `static`, so this is the only way
to reach them from outside `screen.cpp` without changing its linkage.
`gfx.cpp`/`icons.cpp`/`widgets.cpp`/`theme.cpp` are compiled and linked
normally, unmodified. `stub/Arduino.h` and `stub/TFT_eSPI.h` stand in for the
real headers (found via `-I stub` placed first), so the whole thing links and
runs as an ordinary host binary — no ESP32 toolchain involved.

Two things the stubs are deliberately NOT trying to be:

- `TFT_eSPI::textWidth()` always returns 0. None of the target *logic*
  functions above measure text — that's drawing-adjacent work this harness
  doesn't touch — so this only needs to be deterministic, not accurate.
  Font-metric fidelity is `simulator.html`'s job.
- `getLocalTime()` always returns false (unsynced / `"--:--"`). The clock
  isn't in the target list either.

Every other `TFT_eSPI` method is a no-op that increments a global
`hostDrawCalls` counter. That counter is what lets `driver.cpp` verify the
dirty-region *early-out* itself, not just the pure functions: call
`drawDeviceCard()`/`drawStatusRoom()` twice with unchanged inputs and assert
the second call's delta is 0; change one field and assert it's positive. That
directly exercises the class of bug CLAUDE.md warns about — a time-derived
flag (`stale`/`err`/press-flash) missing from a dirty-region compare leaves a
control's appearance stuck.

`driver.cpp` sweeps a representative (not exhaustive) state space and prints
one deterministic line per case to stdout — except `screenHitTest`, which
*is* swept exhaustively (all 320×240 points × 3 pages) but reduced to an
FNV-1a hash per page plus a handful of named sample points, so a 230K-point
sweep doesn't dwarf the rest of the golden file for no readability gain.

## Running it

```sh
./build.sh            # build + run, print to stdout
./build.sh --golden   # build + run, overwrite golden.txt
./build.sh --diff     # build + run, diff against golden.txt (exit 1 on mismatch)
```

`golden.txt` is committed. After any change to `src/ui/screen.cpp` (or the
files it calls into), run `--diff` first: a clean diff means the change is
behaviour-preserving *with respect to what this harness checks* — it does not
by itself certify a refactor step done for other reasons (readability,
dedup). An intentional behaviour change means re-running `--golden` and
saying so in the commit message.

`out/` is build output (object files, the binary, scratch diffs) and is
gitignored.
