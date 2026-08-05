# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Home Assistant controller **firmware** for the CYD "Cheap Yellow Display"
(ESP32-2432S028R: ESP32-WROOM-32, 2.8" 320×240 ILI9341 TFT, XPT2046 resistive
touch). It controls exactly four bedroom devices — three IKEA TRADFRI
white-spectrum bulbs and a Sensibo Sky AC — over Home Assistant's REST API at
`192.168.1.117:8123`.

Scope is deliberately fixed: 4 rows, 23 buttons, no navigation, no menus, no
scrolling. **Resist turning this into a general HA dashboard.** If a request
needs a fifth device or a second page, say so explicitly rather than quietly
adding one.

This is an Arduino/PlatformIO C++ project, **not** a static web project like the
other repos in this parent directory. The `~/CLAUDE.md` conventions (HTML/CSS/JS
projects) do not apply here.

## Relationship to ../btcticker-cyd

`../btcticker-cyd` is a sibling firmware for **the same physical CYD unit**. Its
hardware-specific values were measured on that unit and are carried here
verbatim — treat them as calibration data, not as style choices:

- `platformio.ini` build flags, especially `TFT_RGB_ORDER=TFT_BGR` (this panel
  swaps red/blue) and `ILI9341_2_DRIVER`.
- `include/config.h` `TOUCH_*` calibration and the CYD pin map.
- `xptWrite()`/`xptRead()`/`readTouch()` in `main.cpp`.
- `src/ui/theme.h` palette.

Two constraints in the touch path cost real debugging time upstream and must
survive any refactor:
1. **Touch must not go through TFT_eSPI's `TOUCH_CS`.** Hardware SPI contention
   over CS 33 leaves touch dead on many CYD units. Never add `-D TOUCH_CS`.
2. **PENIRQ is unreliable** (stays high on many boards). Pressure (`z >=
   TOUCH_Z_MIN`) is the source of truth; the IRQ pin is logged for diagnostics only.

Deliberate divergence: btcticker talks to public HTTPS APIs via
`WiFiClientSecure`. HA here is local plain HTTP, so `src/net/ha.cpp` uses a bare
`WiFiClient`. Do not "restore" TLS — dropping the handshake is what makes taps
feel instant.

## Build / flash / debug

```sh
pio run              # compile only
pio run -t upload    # build + flash
pio device monitor   # serial log @ 115200
```

`upload_port`/`monitor_port` are pinned to `/dev/cu.usbserial-110` (this unit's
CH340 bridge, VID 0x1A86 / PID 0x7523). Use `cu.`, not `tty.`.

**Keep `upload_speed = 115200`** — faster rates corrupt the flash on this board.

There is no test suite. **`simulator.html` is the fast iteration path**: it
re-implements the layout geometry in canvas, flags clipping/overflow, and mirrors
`screenHitTest()`'s click mapping. Validate any layout change there before
flashing (`python3 -m http.server 8765`). **Keep its constants in sync with
`include/config.h`** — a drift between them makes it worse than useless.

`include/secrets.h` is gitignored. If it is missing the build fails; copy
`include/secrets.h.example`.

## Architecture

Everything runs on the single Arduino `loop()` task — **no RTOS tasks, no
locking**. `loop()` calls, in order: `updateNetState` → `servicePoll` →
`handleTouch` → `screenRender`, then `delay(20)`.

**Shared state is one global `AppState S`** (`include/state.h`), holding
`DeviceState dev[4]`. The poller and optimistic tap updates write it; the
renderer reads it. Single task, so plain reads/writes are safe. Each device
carries `known` (has any poll succeeded) and `okMs` (millis of last success).

**Polling** (`servicePoll()` in `main.cpp`). One entity per tick, round-robin, at
`HA_POLL_MS` — so each of the 4 devices refreshes every ~6s, which is the
worst-case latency for a change made elsewhere. Failures back off exponentially
(`RETRY_BASE_MS` 1s → `RETRY_MAX_MS` 60s). A tap schedules a **targeted
reconcile poll** that jumps the queue after `RECONCILE_MS`.

**Networking** (`src/net/ha.cpp`). `haPollDevice()` GETs
`/api/states/<entity_id>` and parses with an **ArduinoJson filter** off
`http.getStream()` (with `useHTTP10(true)` so responses aren't chunked), so a
light's full attribute blob never lands in heap. Service calls POST to
`/api/services/<domain>/<service>`; their response bodies are ignored because the
reconcile poll re-reads authoritative state anyway.

**Optimistic UI** (`doAction()` in `main.cpp`) is the load-bearing UX decision.
On tap it (1) applies the expected state locally and calls `screenRender()`,
(2) fires the blocking HTTP call, (3) either schedules a reconcile or **rolls
back from a saved copy** and sets `errMs`. IKEA Zigbee round-trips run 1–2s;
without step 1 every tap feels ignored. Preserve this ordering.

**Rendering** (`src/ui/screen.cpp`) is **dirty-region based**. `screenRender()`
runs every loop pass, but each row builds a `RowSnap` and `memcmp`s it against
the last-drawn one, returning early if unchanged. GFX/GLCD fonts don't paint
their own background, so each dirty region is `fillRect`-cleared first.

Two rendering details worth keeping:
- `drawFittedLabel()` tries Font 2, then a shorter form ("100" for "100%"), then
  Font 1. Font 2 is proportional, so don't replace this with a hand-measured
  width — it self-corrects when a label changes.
- Active buttons use **dark text on the cyan fill**, not white. White-on-cyan
  measures ~1.9:1 contrast; dark-on-cyan is ~9:1.

**Layout** lives entirely in the LAYOUT block of `include/config.h`. Rows are
`ROWS_Y0 + i*ROW_H`; 4 × 55 + 20 = 240 exactly. Change those `#define`s
together, not the arithmetic in `screen.cpp`.

## Conventions

- **Comments explain why, not what** — especially for anything carried from
  btcticker-cyd or worked around on real hardware. Preserve those notes.
- **Fail soft**: never blank a value on a failed poll. Keep the last one and dim
  it (`C_DIM`) once `DEVICE_STALE_MS` passes. A failed *service call* is
  different — it flashes the state text red via `errMs`.
- **Touch targets are the full 55px row band vertically**, not the 36px button
  strip. This is used in a dark bedroom; generous targets are intentional.
- **T+/T- are relative** and must no-op (setting `errMs`) until a real setpoint
  is known. Never guess a starting temperature.
- Active-highlight comparisons use tolerances (`PCT_MID_MIN`/`MAX` etc.) because
  HA round-trips `brightness_pct` through a 0–255 byte. Exact compares will not
  match.
