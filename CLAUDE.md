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

Constraints in the touch path that cost real debugging time and must survive any
refactor:

1. **Touch must not go through TFT_eSPI's `TOUCH_CS`.** Hardware SPI contention
   over CS 33 leaves touch dead on many CYD units. Never add `-D TOUCH_CS`.
2. **`TOUCH_SWAP_XY` is 1 on this unit, and that is load-bearing.** btcticker's
   `readTouch()` builds `bestSx` from the 0xD1 (Y-command) samples, which is
   backwards here. With the swap wrong, every tap collapses into the left third
   of the screen — only buttons 0-2 respond and the AC row is unreachable. The
   ranges look almost identical either way (208/3717 vs btcticker's 200/3700),
   so this is easy to misdiagnose as a range problem. It is not.
3. **PENIRQ *is* reliable on this unit** — the opposite of btcticker's note. Every
   noise sample reads `irq=0`, every real contact `irq=1`, so `TOUCH_REQUIRE_IRQ`
   gates on it. Combined with `TOUCH_Z_MIN` this is a two-signal check, and it
   needs to stay that way: a phantom tap here does not merely misdraw, it fires a
   real service call and changes a light. That happened.
4. **`TOUCH_Z_MIN` must sit above the measured noise floor, not at it.** btcticker's
   95 was inside this firmware's noise band (idle reaches ~130) and fired phantom
   taps. Real contact reads 2100-2500, so 400 sits in the dead zone between.
   `touch idle: rawZ=..` logging exists so the floor can be re-measured rather
   than guessed — its absence was why the first diagnosis was wrong.

Recalibrate with `pio run -e calib -t upload`, never by hand. It fits 4
crosshair points, prints a ready-to-paste `TOUCH_*` block, then shows a verify
screen so accuracy is confirmed before spending a flash cycle.

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

**Polling** (`servicePoll()` in `main.cpp`). **All four devices refresh together**
in one `haPollAll()` call every `HA_POLL_MS`, so nothing is ever more than 1.5 s
stale. Failures back off exponentially (`RETRY_BASE_MS` 1s → `RETRY_MAX_MS` 60s).
A tap sets `reconcilePend` to pull the next refresh forward by `RECONCILE_MS`.

**Networking** (`src/net/ha.cpp`). `haPollAll()` POSTs a Jinja template to
`/api/template` and gets back a ~50-byte delimited string. This replaced a
round-robin of 4× `GET /api/states/<id>`; measured 64.0 ms / 2403 B → 14.2 ms /
50 B, and per-device freshness 6 s → 1.5 s (verified: external changes detected
in 1009–1036 ms).

Three non-obvious things about that template:
- It **must not use Jinja `{%- ... -%}` statement tags.** Those contain `%`,
  which `snprintf` eats as a format specifier when the entity IDs are
  substituted. It is deliberately loop-free for this reason.
- Climate temperatures travel in **tenths**, because `|int` would truncate a 0.5
  `target_temp_step` or a 24.5 setpoint to something wrong.
- Parsing uses `sscanf` into fixed buffers, so **the poll path allocates nothing**.
  Don't reintroduce ArduinoJson here — that was the point.

The template response is logged **only when it changes**, which is how external
changes and poll latency get verified without watching the screen.

Service calls POST to `/api/services/<domain>/<service>`; their response bodies
are ignored because the reconcile refresh re-reads authoritative state anyway.

**Every HA call blocks `loop()`**, so a timeout is also a UI-freeze budget —
touch and rendering stop while one is outstanding. Hence `HTTP_CONNECT_MS` 1200 /
`HTTP_READ_MS` 1500 (~20× the measured 16-64 ms LAN round trip) and
`haBreakerOpen()`, which makes taps fail fast rather than each paying another
timeout. A single 4000 ms timeout on both phases once froze the screen for up to
8 s per tap.

**Optimistic UI** (`doAction()` in `main.cpp`) is the load-bearing UX decision.
On tap it (1) applies the expected state locally and calls `screenRender()`,
(2) fires the blocking HTTP call, (3) either schedules a reconcile or **rolls
back from a saved copy** and sets `errMs`. IKEA Zigbee round-trips run 1–2s;
without step 1 every tap feels ignored. Preserve this ordering.

**Rendering** (`src/ui/screen.cpp`) is **dirty-region based**, tracked at three
granularities. `screenRender()` runs every loop pass and repaints only what
changed:
- The **status bar** is two independent halves (dots / freshness text). It also
  deliberately shows a static `LIVE` rather than a counting age — every poll
  resets `haOkMs`, so a live age oscillated `0↔1` and repainted the full bar
  several times a second. Visible flicker. Don't put a live counter back.
- Each **row's text line** is compared by its rendered string.
- Each **button** is compared by its *visual* state (`BtnVis`), not by underlying
  values — so two brightness values mapping to the same highlight cost nothing,
  and a change repaints 2 buttons instead of all 6.

GFX/GLCD fonts don't paint their own background, so each dirty region is
`fillRect`-cleared first. The row separator sits at `top - 2`, outside every
clear rect, so it is painted once.

**Backlight ordering is a trap.** The LEDC setup in `setup()` **must** come after
`screenBegin()`: `TFT_eSPI::init()` does `pinMode(TFT_BL, OUTPUT); digitalWrite(
TFT_BL, TFT_BACKLIGHT_ON)` (`TFT_eSPI.cpp:786`), reclaiming GPIO 21 and detaching
any PWM. Configuring LEDC first leaves `BL_DUTY` silently ignored at 100%.

Two more rendering details worth keeping:
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
- **Never let an unreachable device read as a normal state.** HA reports
  `unavailable`/`unknown`, which naively collapses to `on == false` and renders as
  a plain `OFF` — indistinguishable from a healthy bulb that is off.
  `DeviceState::avail` exists for this: the row shows `UNAVAILABLE` in red and
  every button greys out, because there is no current state to highlight.
- **Touch targets are the full 55px row band vertically**, not the 36px button
  strip. This is used in a dark bedroom; generous targets are intentional.
- **T+/T- are relative** and must no-op (setting `errMs`) until a real setpoint
  is known. Never guess a starting temperature.
- Active-highlight comparisons use tolerances (`PCT_MID_MIN`/`MAX` etc.) because
  HA round-trips `brightness_pct` through a 0–255 byte. Exact compares will not
  match.
