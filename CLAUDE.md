# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Home Assistant controller **firmware** for the CYD "Cheap Yellow Display"
(ESP32-2432S028R: ESP32-WROOM-32, 2.8" 320×240 ILI9341 TFT, XPT2046 resistive
touch). It controls exactly four bedroom devices — three IKEA TRADFRI
white-spectrum bulbs and a Sensibo Sky AC — over Home Assistant's REST API at
`192.168.1.117:8123`.

Scope is still deliberately fixed, just no longer to a single screen. It is now
**exactly three pages**, reached from a tab bar in the header:

1. **Devices** — 4 cards, one per entity, 23 controls.
2. **Scenes** — macros over the three bulbs, in a scrolling 3-column grid of
   88×64 tiles. Five are defined; the page is built for far more. No new
   entities.
3. **Settings** — 4 board-level knobs (backlight, night mode, night schedule,
   screen flip). Nothing here touches Home Assistant.

**The UI runs on a design system, not per-screen styling.** Colour and type
tokens live in `src/ui/theme.h` / `src/ui/gfx.h`; all geometry (spacing scale,
radii, control sizes) lives in the LAYOUT block of `include/config.h`. Components
come from `src/ui/widgets.cpp` and icons from `src/ui/icons.cpp`. A change to how
something *looks* belongs in a token or a component, never in a page — the pages
are layout and state derivation only. See "Design system" below.

**No drill-down pickers, no sub-pages, and no fourth page.** One tap still acts,
everywhere.

**Scrolling is now allowed on Scenes, and only on Scenes.** That is a deliberate
reversal of this file's former blanket ban, made on request so the scene list can
grow to 100+ without a redesign — not an oversight, and not a general licence.
Devices and Settings still show every control at once, and both still
`static_assert` that their rows tile the body exactly. A request to scroll either
of *those* is the thing to push back on.

**Resist turning this into a general HA dashboard.** If a request needs a fifth
device or a fourth page, say so explicitly rather than quietly adding one.

An earlier version of this file forbade navigation outright, and the one before
this forbade scrolling. Both were retired on purpose — when the tab bar landed,
and when the scene grid did. Neither was overlooked.

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

`src/ui/theme.h`'s palette **used to be** on that list and no longer is. It was
carried over for family resemblance, not measured on hardware, and it has been
deliberately rebuilt as a semantic token set (see "Design system"). The HA cyan
accent and the dark-slate ground are kept so the two devices still read as one
family; everything else is new. Do not "restore" the old `C_MUTED` / `C_GREEN` /
`C_RED` names — they described appearance rather than role, which is exactly why
three of them ended up doing four jobs each.

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

**`src/ui/logo_ha.h` is generated, not hand-written.** It holds Home Assistant's
launch-screen mark as a 96×96 RLE-encoded palette bitmap for the boot splash
(1366 B + palette, versus 18 KB raw). No SVG rasteriser is installed on this
machine — no rsvg, inkscape, ImageMagick or PIL — so the pipeline runs through a
browser:

```sh
# 1. rasterise: open scripts/gen_ha_logo.html, which renders the SVG to a canvas
#    and POSTs the encoded result to scripts/logo_data.json
# 2. convert:
python3 scripts/gen_logo_header.py
```

`scripts/logo_data.json` is committed so step 2 can be re-run without a browser.
The generator asserts the RLE covers exactly W×H pixels, so a truncated capture
fails loudly rather than producing a corrupt bitmap. The palette's background
entry is exactly `C_BG`, which is why the logo blits as an opaque rectangle with
no transparency handling.

There is no test suite. **`simulator.html` is the fast iteration path**: it
re-implements the layout geometry in canvas, flags clipping/overflow, and mirrors
`screenHitTest()`'s click mapping. Validate any layout change there before
flashing (`python3 -m http.server 8765`). **Keep its constants in sync with
`include/config.h`** — a drift between them makes it worse than useless.

Three things about it are load-bearing, not conveniences:

- **It carries the real font metrics.** `ADV_*` are the exact `xAdvance` tables
  lifted from TFT_eSPI's `Fonts/GFXFF/` headers, and `gtext()` draws character by
  character at those advances, so `textW()` here is byte-identical to
  `tft.textWidth()` on the panel. That is the only reason its clipping warnings
  can be trusted: the previous version approximated with `13px system-ui` and
  could neither confirm nor deny a 1px overflow. If a face is ever swapped, both
  the tables here and `ROLE_ASC`/`ROLE_DESC` in `gfx.cpp` must be re-extracted —
  the layout in `config.h` is derived from those numbers.
- **It re-checks every `static_assert` from `screen.cpp`**, plus two things the
  compiler cannot see: that a card's line-1 descenders stop before the control
  row, and that its dirty rect covers those descenders. It also asserts the night
  palette's red ladder is collision-free (see "Night mode").
- **State is deep-linkable** — `?page=1&night=1&scenes=100&stale=1&ha=0&mode=heat`
  and so on, listed at the bottom of the file. That is what makes a specific case
  reproducible, and it is how the design was reviewed:

```sh
python3 -m http.server 8765
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new \
  --disable-gpu --virtual-time-budget=2500 --window-size=1010,820 \
  --screenshot=/tmp/devices.png "http://localhost:8765/simulator.html?page=0"
```

There is also a headless assertion harness pattern worth knowing: `eval()` the
file's `<script>` against a Proxy-stubbed canvas context (all drawing becomes a
no-op) plus `document`/`location` stubs, then drive `page`, `set`, `dev` and
`sceneStress()` directly and read `log.innerHTML`. That sweeps every page, both
palettes, all 32 scroll offsets at 100 scenes and every fault state in about a
second, with no browser.

`include/secrets.h` is gitignored. If it is missing the build fails; copy
`include/secrets.h.example`.

## Design system

Five files, layered, each with one job. The point of the split is that a visual
change has exactly one correct home:

| file | owns | rule |
|---|---|---|
| `src/ui/theme.h/.cpp` | 16 semantic colour tokens + night mapping | no page picks a hex value |
| `src/ui/gfx.h/.cpp` | the `TFT_eSPI` handle, 4 type roles, fitted/truncated text | **only `src/ui/*` may include it** |
| `include/config.h` LAYOUT | spacing scale, radii, every rect | `simulator.html` mirrors this |
| `src/ui/widgets.cpp` | card, chip, swatch, toggle, stepper, value, tab, pip | one `vis` byte in, pixels out |
| `src/ui/icons.cpp` | 10 primitive-drawn glyphs on one 15×15 grid | no bitmaps, no decoration |

**Geometry is in `config.h` and not in `theme.h`, on purpose.** Splitting the
tokens across two files looks like a wart until you remember `simulator.html`
mirrors `config.h` and nothing else — putting sizes next to colours would give
the layout two sources of truth, which is the failure that file exists to catch.

**Colour tokens are semantic, and that is what fixed the old palette.** The
previous names described appearance (`C_MUTED`, `C_GREEN`, `C_RED`), so each ended
up serving several unrelated roles and none could be changed without changing all
of them. Now: `SURFACE`/`ELEVATED` (a card, then a control on it),
`BORDER`/`DIVIDER`, `TEXT`/`TEXT2`/`TEXT3`, `DISABLED` vs `DIM` (no state to show
vs a stale reading), `ACCENT`, and `SUCCESS`/`WARNING`/`ERROR`.
**`ACCENT` is the only saturated colour in a resting UI** — if everything is
highlighted, nothing is — and the three status colours appear only when there is
something to say.

**Type is four roles over two faces.** The build already carried `LOAD_GFXFF`, so
this uses TFT_eSPI's bundled proportional FreeSans instead of its bitmap fonts:
`F_NUM` (Bold 12pt, the AC setpoint alone), `F_TITLE` (Bold 9pt, card titles and
the clock), `F_BODY` (9pt, everything live), `F_MICRO` (GLCD 6×8, last-resort fit
only). Hierarchy comes from **weight and text tier, not more sizes** — at 320×240
a third size reads as a ransom note. Two consequences to know:

- **9pt is the floor.** GFX fonts only scale by integer `textsize`, so there is no
  face between FreeSans 9pt and the 6×8 GLCD bitmap. That is why a settings
  caption is the same *size* as its title and separated by weight and colour
  instead; it is a real constraint, not an oversight.
- **Name the faces in `gfx.cpp` and nowhere else.** `TFT_eSPI.h` pulls in
  `gfxfont.h`, which declares all 48 bundled fonts with internal linkage in every
  TU that sees it. So an explicit `#include <Fonts/GFXFF/...>` is a *redefinition
  error*, and merely referencing a face from a second `.cpp` puts a second copy of
  its glyph bitmaps in flash. Verified: the three faces appear once each
  (`nm | grep FreeSans` → `_ZL…`), ~7.8 KB total.

**Every component honours the full state set** — `BV_INACTIVE / BV_ACTIVE /
BV_ACTIVE_OFF / BV_PRESSED / BV_DISABLED / BV_ERR` — and resolves it through one `ctlColour()`
table rather than branching locally, so a pressed chip and a pressed chevron
cannot come out looking like different interactions. The table's shape is the rule:
**every selected state is a solid fill with dark text, and only WHICH fill depends
on what was selected.** Four decisions in there:

- **An inactive control has no border** (`edge == fill`). The old page outlined all
  23 controls at once; deleting those outlines, not changing any colour, is what
  made the page quiet.
- **`BV_ACTIVE_OFF` exists because OFF is not an accomplishment.** A cyan OFF chip
  made the quietest state on the Devices page the loudest mark on it, and with three
  bulbs off, three of the four cards lit up. `C_NEUTRAL` grey still reads as
  selected — 9 night-steps and ~2.75:1 day luminance clear of `C_ELEVATED` — without
  claiming anything is happening. It is a **vis state, not a colour the caller
  passes in**, which is what keeps the invariant above in this one table.
- **`BV_DISABLED` recedes into the card** rather than greying out on top of it.
  There is no state to show, so the control must not look like it is showing one.
- **`BV_PRESSED` is a full-brightness fill**, deliberately the loudest thing the
  UI ever draws, because it has to land within `PRESS_FLASH_MS` and before HA has
  answered. It is the only such fill.

**New `BtnVis` values go on the END of the enum.** The raw ordinal is what every
page's snapshot stores and compares against next frame's, so inserting in the middle
silently changes what "unchanged" means.

**Icons are drawn from primitives, never stored.** Three reasons, and the first is
not thrift: night mode swaps the palette at runtime, so a baked RGB565 sprite
would render in day colours over a red-only UI — the exact problem `themeMap()`
exists to paper over for the one bitmap that *is* baked (the splash logo). They
also need no generator (`logo_ha.h` needs a browser in the loop), and stroke
weight stays even by construction. **Every icon is used and every one carries
state**; nothing was added because a row looked bare.

## Architecture

Everything runs on the single Arduino `loop()` task — **no RTOS tasks, no
locking**. `loop()` calls, in order: `updateNetState` → `servicePoll` →
`serviceNightSchedule` → `applySettings` → `handleTouch` → `screenRender`, then
`delay(20)`. `applySettings()` sits before `handleTouch()` so a scheduled flip
lands before the tap that follows it is mapped.

**Shared state is one global `AppState S`** (`include/state.h`), holding
`DeviceState dev[4]`, the current `page`, and the persisted `Settings set`. The
poller and optimistic tap updates write it; the renderer reads it. Single task,
so plain reads/writes are safe. Each device carries `known` (has any poll
succeeded) and `okMs` (millis of last success).

**Touch dispatch is one tagged `Hit`** (`state.h`), not a device/button pair:
`{ HitKind kind; int16_t idx; int8_t sub; }` over `HIT_TAB` / `HIT_ROW` /
`HIT_SCENE` / `HIT_SCROLL` / `HIT_SETTING`. `idx` is 16-bit for `HIT_SCENE`
alone — it carries an absolute index into a table meant to reach 100+, where an
`int8_t` would wrap. `HitKind` lives in `state.h` rather than `screen.h` because
`AppState` needs it for the press flash, and `state.h → screen.h → state.h`
would be circular. **`handleTouch()`'s `S.dev[h.idx].name` log must stay inside
the `HIT_ROW` branch** — unconditional, a scene tap at index 4 indexes one past
`dev[4]` into `AppState`'s scalars and printf dereferences that as a `char*`,
which panics and reboots.

**The press flash is keyed by `pressKind` as well as index**, or a scene tap at
index 2 would also invert row 2's button over on the Devices page.

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
touch and rendering stop while one is outstanding. Hence `HTTP_CONNECT_MS` 1200
and `haBreakerOpen()`, which makes taps fail fast rather than each paying another
timeout. A single 4000 ms timeout on both phases once froze the screen for up to
8 s per tap.

**Polls and service calls are different workloads — don't unify their timeouts.**
`HTTP_READ_MS` 1500 covers a poll (a local template render, measured 14 ms).
Service calls get `HTTP_READ_SVC_MS` 2500, because `climate.*` goes out to the
Sensibo **cloud**: measured **1135–1643 ms** for a real change. The trap is that a
*no-op* write short-circuits in 24 ms, so benchmarking `set_temperature` with the
value it already has reports 24 ms and hides the problem entirely. That mistake
shipped a 1500 ms read timeout, and every genuine `T+`/`T-` tap then reported a
read timeout.

**A read timeout is not a failed command.** `haPostService()` treats
`HTTPC_ERROR_READ_TIMEOUT` as *delivered* and returns true. We connected and sent
the request; we only gave up waiting for the reply, and HA has almost certainly
executed it. Rolling back the optimistic state there would display the old
setpoint for a change that really happened. Only connect failures and HTTP error
codes roll back.

**Range-check every climate attribute.** The template's `|float(0)` default
conflates "attribute missing" with "value is zero" — a distinction the old
ArduinoJson `isNull()` check could make. Worse, the Sensibo emits nonsense
transiently during an `hvac_mode` change; an observed sample was
`cool,0,238,0,10,10` (target 0, min 0, **max 1.0**). Taking that at face value set
`tMax` to 1.0, which would make `T+`/`T-` clamp the setpoint to one degree.
`min`/`max` are therefore only accepted as a coherent pair spanning ≥5 °C, and a
rejected field keeps its last good value.

**Optimistic UI** (`doAction()` in `main.cpp`) is the load-bearing UX decision.
On tap it (1) applies the expected state locally and calls `screenRender()`,
(2) fires the blocking HTTP call, (3) either schedules a reconcile or **rolls
back from a saved copy** and sets `errMs`. IKEA Zigbee round-trips run 1–2s;
without step 1 every tap feels ignored. Preserve this ordering.

**Scenes** (`SCENE[]` in `screen.cpp`, `doScene()` in `main.cpp`) act on the
three bulbs only — the AC runs on a different comfort schedule, and folding it
in would make every scene tap a Sensibo *cloud* round trip.

- **The active scene is derived, never latched.** `sceneActive()` compares live
  device state against the table every render. That is what makes overriding one
  bulb on the Devices page deselect the scene, and a change made from the HA app
  select the matching one, with no "current scene" variable to fall out of sync.
  It shares `pctMatches()`/`kelvinMatches()` with `btnActive()` so the two pages
  can never disagree about what "30%" means.
- **An unknown kelvin is "cannot confirm", not "match".** Treating it as a match
  lights AWAKE and DAY simultaneously — they differ only there — and two active
  cards reads as a bug.
- **A scene is 1–2 HTTP calls, not 6.** `light.turn_on` accepts a *list* of
  `entity_id`, and brightness + colour temp go in one call. Three separate calls
  would be three sequential `HTTP_READ_SVC_MS` budgets (7.5 s of frozen `loop()`)
  and the bulbs would visibly step on one at a time. The call plan comes from
  `scenePlan()`, derived from the table rather than special-cased per scene — but
  it assumes every scene's ON set is **homogeneous**, which is what lets it
  collapse into one `turn_on`.
- **A partial failure still schedules the reconcile.** Unlike `doAction()`, where
  a failed call means nothing changed, RELAX's two calls can half-succeed and
  nothing local can tell which bulbs moved. Roll back to the last *observed*
  state rather than invent a mixed one, and let the poll settle it.

**The scene grid is built to scale, and `SCENE[]` is the only place the count
lives.** The table is declared unsized and `SCENE_N` is `sizeof`-derived, so
adding a scene is one line and nothing else. There is deliberately **no
`NUM_SCENES` in `config.h`** — a second count is the thing that goes stale.
`main.cpp` asks `sceneCount()` rather than assuming.

- **The snapshot is per visible TILE (9 bytes), not per scene.** That is what
  makes 100 scenes cost the same RAM as 5. The price is that `sceneSnap.row` must
  be part of the snapshot and a scroll must be treated as a **first draw**: the
  offset re-points every slot at a different scene, so not one cached `BtnVis` byte
  describes what is now meant to be there.
- **A scroll needs no body wipe, and that is load-bearing, not luck.** Tile rects
  are fixed, so the 10/9px gaps between them never change content — but only
  because a slot past the end of the table is blanked with a **square**
  `fillRect`. `fillRoundRect` leaves the four corner pixels of its bounding box
  untouched, so a rounded clear would strand the previous tile's corners.
- **`Hit::idx` is `int16_t` for this page.** `HIT_SCENE` carries an absolute index
  into the whole table, not the 0–11 on-screen slot, and an `int8_t` would wrap at
  128 — inside the range the grid was rebuilt to handle.
- **The arrows page, not step.** At three visible rows, a row-at-a-time arrow
  needs 32 taps to cross 100 scenes; a page needs 11. `sceneScrollBy()` clamps to
  `SCENE_MAX_ROW`, so the last page is full rather than mostly empty.
- **The whole scroll affordance compiles out at five scenes.** `SCENE_MAX_ROW` is
  a `constexpr` 0, so the gutter paints one `fillRect` and is dead to taps. Note
  the consequence: the arrow/thumb code is currently unexercised on hardware.
  `simulator.html`'s scene-table buttons (5 / 24 / 100) exist to drive it.
  Watch for `-Wdiv-by-zero` if you touch the thumb arithmetic — the early return
  does not stop GCC constant-folding a `constexpr 0` divisor, which is why
  `maxRow` substitutes 1 in the unreachable case.
- **The three pips on a tile replace the old caption, and are derived.** There is
  no room for "ALL BULBS, 30% 2202K", and the pips say it faster: position = which
  bulb, ring = off, colour = warm/cool, fill intensity = level. Being read off
  `SCENE[]` itself, they cannot describe a scene the tap won't send — which a
  hand-written caption could, and nearly did once already with `2200K` vs `2202K`.
- **Three columns, not four.** A 66px square tile could hold the pips and a name
  only in the fallback font; 88px holds it at full size, which is what a scene tile
  is for. Nine legible tiles per page beat twelve illegible ones, and the page
  still scales to 100+. This is also why the gaps differ per axis (12 across, 8
  down): the vertical budget is fixed at 208px, and `2*PITCH_Y + TILE_H == 208` has
  exactly one solution that keeps tiles above 60px.

**Rendering** (`src/ui/screen.cpp`) is **dirty-region based**. `screenRender()`
runs every loop pass, dispatches on `S.page`, and repaints only what changed:
- The **header** is three independent regions (connectivity glyph / tabs / clock)
  that **tile it exactly** — `static_assert`ed, because a gap leaves pixels
  nothing ever clears and an overlap is just as bad, since each region only clears
  its own rect. The clock repaints once a minute, which is invisible. It replaced a
  freshness readout that counted seconds since `haOkMs` — and since every poll
  resets that timestamp, the number oscillated `0↔1` and repainted the full bar
  several times a second. **Don't put any per-second value here**; that region is
  minute-rate by design. Connection health belongs to the glyph, and staleness to
  the card dimming, not to this readout. **The clock is exactly 5 characters** —
  "23:45" is 44px in `F_TITLE` against the 45px its region leaves, so anything
  wider spills into tab 2's cell, which only repaints on a page change, making the
  overflow permanent.
- **The current tab is an underline, not a filled pill.** A pill spent the
  solid-accent fill — the mark that means "selected" on a chip — on the one strip
  that navigates rather than acts, so the header read as a fourth row of buttons.
  `wTab()` now draws the label plus a 2px `C_ACCENT` bar under the selected one,
  and selection carries **two** signals (`C_TEXT` vs `C_TEXT3` *and* the bar), so
  it survives the night palette the same way the connectivity glyph does. Three
  things about it are load-bearing: the bar is flush with the **bottom of the rect
  `drawTab()` passes**, which is the cell's clear rect, so it lands on the last row
  above the 1px rule and the two read as one line — one row lower and it would
  overwrite a rule that is painted once and never restored, one row higher and a
  gap opens. It must stay **inside** that clear rect or a tab that stops being
  current keeps its bar forever. And it spans the **word, not the cell** (measured
  with `textW`, so a renamed tab needs no constant changed): at 81px against a
  46–65px label, a full-width bar is a box around the label, which is the button
  again. `TAB_UL_H` is 2 and not 3 because "Settings" descends to y 26 and the bar
  starts at 29; `simulator.html` asserts that clearance, the seating identity, and
  that every label still fits its cell.
- **The connectivity glyph replaced two labelled dots reading "WIFI" and "HA"** —
  permanent debug chrome spending 88px to tell a healthy system it was healthy.
  Now: both up → 3 bars in `C_TEXT3`; HA down → 3 bars plus an amber badge; Wi-Fi
  down → 0 bars, all red. **Colour AND a shape/badge change, never colour alone**,
  because the palette collapses to red at night and "is it working" has to survive
  that. The `HA` label's diagnostic value is not lost — it is in the serial log.
- Each **card's identity line** (icon + name + state) is **one** region compared by
  its rendered string *and* by the icon's resolved shape and colour. The icon has
  to be in that compare: the AC's state line does not mention its mode (the chips
  do), so a `cool`→`dry` change would otherwise dirty nothing and leave the wrong
  glyph on the glass.
- Each **control, tab, scene tile and settings toggle** is compared by its *visual*
  state (`BtnVis`), not by underlying values — so two brightness values mapping to
  the same highlight cost nothing, and a change repaints 2 chips instead of all 6.

**Every dirty rect inside a card starts at `CARD_IN_X0`, never at `CARD_X`.**
Filling `x 8..15` would paint the `R_LG` corner arcs with the surface colour and
square the card's corners off, one repaint at a time. Same reasoning as the
`fillRect`-not-`fillRoundRect` rule for blanking an empty scene slot.

**A card's line-1 dirty rect (`CARD_L1_H`) deliberately runs past the 13px ascent
box the datum centres on**, because it must cover descenders: clear only the ascent
box and renaming "Reading lamp" to "Lamp" leaves the g's tail on the card forever.
`CARD_L1_CY` is 9 and not 10 for the mirror-image reason — at 10 those descenders
reach into the control row. Device names are user data from `secrets.h`, so they
cannot be assumed to be the all-caps they happen to be today.

**A failed service call flashes the card's BORDER, not just its text.** The whole
surface carries the notification, and redrawing the outline alone is enough since
the fill and its contents are unchanged.

**The device card's icon is the highest-value pixel on the page.** It is derived
live: a bulb's real colour temperature (interpolated across the range, not
bucketed like the swatches) blended by its real brightness, so the icon column is
a scannable strip of what the room is actually doing. Two rules it obeys —
it dims with its own card when the poll goes stale (the same fail-soft rule the
text follows; dimming one and not the other left half the card claiming to be
current), and an **offline** device stays red rather than dimmed, because red
outranks stale.

**Every page snapshot must keep a per-item `BtnVis` byte**, and that is
load-bearing rather than an optimisation: the press flash expires by *time*, not
by any state change, so only a per-item vis compare notices `BV_PRESSED →
BV_ACTIVE` and repaints. A page that skips it leaves the tapped control inverted
forever. Each snapshot also needs its own `valid` flag on top, because
`BV_INACTIVE` is 0 and a `memset` alone reads as "already drawn as inactive".

**A page switch must wipe the body** (`bodyReset()`). Devices happens to
self-clear — each row's first draw `fillRect`s its whole band — but Scenes does
not: 88 × 64 tiles on a 100 × 72 pitch leave 12px and 8px gaps that would hold the
previous page's pixels permanently. (A *scroll* within Scenes is different and
needs no wipe — see the scene-grid notes above for why.) `screenInvalidate()` deliberately does **not**
`fillScreen()`, because `screenSplash()` draws the logo and then calls it; the
wipe is deferred to the next `screenRender()`.

**`statusSnap` is assigned field-by-field, not by aggregate init.**
`statusSnap = { ... }` compiles silently when a field is added (there is no
`-Wmissing-field-initializers` in this build) and zero-initialises it — which for
`tabVis` means "every tab inactive", while the computed value always has one
active, so the strip would repaint on every pass. A flicker bug with no warning.

GFX/GLCD fonts don't paint their own background, so each dirty region is
`fillRect`-cleared first. The row separator sits at `top - 2`, outside every
clear rect, so it is painted once.

**Night mode** recolours the whole UI in place, so `src/ui/theme.h`'s palette is
**runtime values, not `#define`s** — the `C_*` names are now array slots into
`THEME[]`. Consequence: **no `C_*` name may appear in a static or constexpr
initializer.** `THEME_LIST(X)` derives the slot enum, the day table and the night
table from one list so a colour cannot be half-added.

**Pure luminance is not enough for the night palette.** `redOnly()` maps `C_ERROR`
and `C_DIM` to the *identical* value, and a device card picks between them on the
same string (red = failed call / `OFFLINE`, dim = stale poll) — collapsing them
destroys the fail-soft rule below. `C_SUCCESS`/`C_ACCENT` collide too,
`C_WARM`/`C_COOL` land 2 of 31 steps apart, and the four text tiers derive into a
15..21 huddle that destroys the hierarchy they exist to express. Hence the third
column of `THEME_LIST`: structural darks derive, every semantic colour is pinned,
and the measured 17-step ladder is

```
BG 0 < SURFACE 2 < DIVIDER 3 < ELEVATED 4 < BORDER 6 < DIM 7 < SUCCESS 10
     < DISABLED 12 < NEUTRAL 13 < WARM 14 < TEXT3 16 < ACCENT 18 < TEXT2 20
     < WARNING 22 < TEXT 25 < COOL 27 < ERROR 31
```

All 17 are distinct. The `SURFACE`/`DIVIDER`/`ELEVATED` trio sits one step apart,
and that is the intended result rather than crowding: night mode exists to emit as
little light as possible, so the card, its rules and the controls on it all
collapse toward black and the page is carried by text and `ACCENT` alone. What must
not collapse is any pair a reader has to **tell apart**, and every such pair is ≥4
steps clear (measured: `DIM`/`ERROR` 24, `WARM`/`COOL` 13, `NEUTRAL`/`ELEVATED` 9,
`SUCCESS`/`ACCENT` 8, `NEUTRAL`/`ACCENT` 5, the text tiers 4–5).
`simulator.html` asserts both properties — no collisions, and a minimum separation
across an explicit `MUST_DIFFER` list.

`NEUTRAL`'s remaining one-step neighbours are `DISABLED` and `WARM`, and the trade
is argued on role separation in `theme.h`: the ladder ranks *values*, and those
three never appear as the same *kind* of mark (`NEUTRAL` is only ever a chip fill,
`DISABLED` only ever a label on a `SURFACE` fill, `WARM` only ever a swatch disc).
The low half of the ladder has no free level with two clear steps below `ACCENT`.

**A palette or rotation change repaints nothing by itself.** Every dirty-region
compare is on a *value*, and neither alters one — so `screenSetNight()` and
`screenSetFlip()` must `fillScreen()` + `screenInvalidate()`. Both are memoised
no-ops otherwise, which is why `loop()` can call them every pass.

**The schedule *writes* the Night mode toggle; it does not override it.**
`serviceNightSchedule()` is edge-triggered at 23:45 and 08:00, so between the
boundaries a manual toggle always wins and sticks. A level-triggered version
would re-assert itself on the next render and make the toggle physically
un-turn-off-able before 08:00. On the first valid clock reading it *adopts* the
window, which is what makes a 02:00 reboot come up already in night mode.
Scheduled transitions deliberately do **not** write NVS — only user taps do,
since boot-time adoption already restores the right state.

**Screen flip mirrors the tap, not the panel.** `tft.setRotation(3)` turns the
display 180, but touch here is hand-rolled bit-bang, so `readTouch()` still
returns coordinates in the rotation-1 frame its `TOUCH_*` constants were fitted
against. `handleTouch()` mirrors both axes — deliberately there and not in
`readTouch()`, which stays a pure raw→calibration-frame mapper, so the
calibration paths bypass it for free and the `touch dbg:` line logs the
coordinates actually hit-tested. **`CALIB_MODE` forces flip/night off and full
brightness**: calibration *defines* the reference frame, so the `TOUCH_*` block
it prints is meaningless in any other one, and a 1% red crosshair cannot be
aimed at.

**Clock.** NTP via `configTzTime(TZ_INFO, ...)` in `setupWifi()`, then the core's
SNTP client resyncs itself — there is no clock polling code, and none should be
added. `getLocalTime(&tm, 0)` is called with a **zero** timeout because it runs on
every render pass and must never block; it returns false until the first sync
lands, which is what shows `--:--`.

`TZ_INFO` is a POSIX TZ string with an **inverted sign**: `"ICT-7"` means UTC**+**7
(Asia/Bangkok, no DST so no rule half). Verified on hardware by logging local and
UTC together — printing only local time would look plausible while being hours
wrong, which is exactly the failure worth guarding against. Measured: local
23:00:57 / UTC 16:00:57, and ~0 s skew against the host's `TZ=Asia/Bangkok date`.

**Backlight ordering is a trap.** The LEDC setup in `setup()` **must** come after
`screenBegin()`: `TFT_eSPI::init()` does `pinMode(TFT_BL, OUTPUT); digitalWrite(
TFT_BL, TFT_BACKLIGHT_ON)` (`TFT_eSPI.cpp:786`), reclaiming GPIO 21 and detaching
any PWM. Configuring LEDC first leaves the duty silently ignored at 100%.

That same line is also a 100%-brightness flash on every boot, which a persisted
1% or night setting makes glaring in a dark bedroom — so `screenBegin()` drives
`PIN_BACKLIGHT` LOW immediately after `tft.init()` and the panel stays dark until
PWM comes up at the real duty a few ms later.

**Never re-write an unchanged LEDC duty.** `applyBacklight()` memoises it;
pushing the same value every frame visibly glitches CYD backlights. Effective
duty is computed in `applySettings()` on the render path, *not* in the tap
handler — that is what makes a brightness change picked while night mode is
dimming still land the moment night ends. `settingsLoad()` runs before
`screenBegin()` because `flip` decides the rotation of the one and only first
paint, and `briIdx` the first duty.

Two more rendering details worth keeping:
- `textFit()` tries the role, then a shorter form ("100" for "100%"), then
  `F_MICRO`. FreeSans is proportional, so don't replace this with a hand-measured
  width — it self-corrects when a label changes, which matters most for scene
  names, since a scene added to the table later cannot be checked against a
  measured width.
- Active controls use **dark text on the cyan fill**, not white. White-on-cyan
  measures ~1.9:1 contrast; dark-on-cyan is ~9:1.
- **Chip labels are uppercase and card titles are not**, and that is a fit
  decision rather than a stylistic one: caps have no descenders, so a 13px label
  centres cleanly in a 22px chip, where a lowercase 'y' would touch its edge.
  Titles and tab labels have the vertical room, so they get sentence case, which
  reads considerably calmer at this size.
- **The splash ticks.** `screenSplashProgress()` cycles three pips under the logo
  from the Wi-Fi wait loop, because a still logo for the length of
  `WIFI_CONNECT_MS` is indistinguishable from a hung board. Still wordless, so the
  two boot states remain visually identical — see the trade documented in
  `screen.h`.

**Layout** lives entirely in the LAYOUT block of `include/config.h`. Rows are
`ROWS_Y0 + i*ROW_H`; **32 + 4 × 52 = 240 exactly**, and the header tiles as
**26 + 3 × 81 + 51 = 320 exactly**. Both are `static_assert`ed in `screen.cpp`.
Change those `#define`s together, not the arithmetic in `screen.cpp`.

**The header grew 22 → 32px and the rows paid 2px each for it.** That buys the tab
targets a third more height in the *worst* band of the panel (below), and it buys
the header room for a real 9pt clock. `TAB_W` is 81 because the widest tab label
("Settings", 65px in `F_BODY`) has to fit with air around it — a 76px cell left
68px and looked like it was bursting. The label widths decided the cell width
here, not the reverse. (The 81 was originally sized for that label inside a
`SP_1`-inset **pill**; the pill is gone, so the budget is now the whole cell less
`SP_1` a side, 73px. `TAB_W` stays 81 regardless — it is fixed by the header
tiling above.)

**The tab strip is still the least accurate region of the panel.** `CAL_INSET` is
30, so the 4-point fit *interpolates* y=30..209 and **extrapolates** y=0..31 — and
resistive panels are worst near the bezel. `screenCalibVerifyScreen()` draws the
tab **cells** for exactly this reason; a verify pass that skipped them would never
reveal a miss there. It outlines the full `TAB_W × TAB_TAP_H` cell, which is what
`screenHitTest()` accepts — it used to outline the old pill, which was *smaller*
than the real target, so a tap the firmware would have taken could read as a miss. (Moving navigation to a bottom bar would fix the accuracy
outright, and was rejected: it costs ~36px of body, which drops the row bands to
44px, and a 44px band cannot hold a two-line card. The header keeps the tabs.)

**Each row band holds one card inset by `CARD_DY` (3px) top and bottom**, which is
what produces the uniform 6px gutter between cards and 3px against the header and
the bottom edge. A card is `CARD_H` 46px: `3 pad + 14 line1 + 4 gap + 22 controls
+ 3 pad`.

Devices and Settings share the row grid via `rowTop(slot)` / `cardTop(slot)`;
both take a **slot**, not a device index. **Every device row keeps SIX logical
slots with unchanged meanings**, even though the two kinds now lay those slots out
differently — 4 chips + 2 swatch cells for a bulb, 3 chips + a 3-cell stepper for
the AC. `doAction()`'s switch, the press-flash sub-index and `RowSnap::btnVis` all
key off the slot number, so **`btnRect()` is the only function that knows about the
difference**, and the renderer, the hit test and the calibration verify screen all
go through it. That is what stops the drawn rect and the tappable rect drifting
apart.

Note one deliberate inversion in there: **the AC stepper's slots run backwards
against x** — slot 5 (down) on the left, slot 3 (up) on the right — so the control
reads left-to-right as less-to-more. The slot numbers are fixed by `doAction()`, so
mapping them in `btnRect()` is what buys the natural order without touching the
action layer.

The chip pitch is shared: 4 across a device card and 5 across the Settings
brightness card are the same `CHIP_W`/`CHIP_PITCH`, so a control on one page is the
same size as a control on the other. The AC's mode chips are the one exception at
`ACM_W` 60 — "COOL" needs 51px of the 52 that leaves, the single place a label
decided a width.

Scenes ignores both grids and uses its own: `SCENE_COLS` × `SCENE_TILE_W/H` on a
`SCENE_PITCH_X` / `SCENE_PITCH_Y` pitch, with `sceneTileX()`/`sceneTileY()` taking
an on-screen **slot** rather than a scene index — which scene a slot holds depends
on `S.sceneRow`. It is `static_assert`ed twice, for the two distinct failures: it
must fill the body **exactly** in y (`32 + 2 × 72 + 64 = 240`), and it must stop at
or before `SCENE_SB_X0` in x (`8 + 2 × 100 + 88 = 296 ≤ 300`), because the grid and
the scroll gutter each clear only their own rect.

## Conventions

- **Comments explain why, not what** — especially for anything carried from
  btcticker-cyd or worked around on real hardware. Preserve those notes.
- **Fail soft**: never blank a value on a failed poll. Keep the last one and dim
  it (`C_DIM`) once `DEVICE_STALE_MS` passes. A failed *service call* is
  different — it flashes the state text red via `errMs`.
- **Never let an unreachable device read as a normal state.** HA reports
  `unavailable`/`unknown`, which naively collapses to `on == false` and renders as
  a plain `OFF` — indistinguishable from a healthy bulb that is off.
  `DeviceState::avail` exists for this: the card shows `OFFLINE` in red, its icon
  goes to a red outline, and every control greys out, because there is no current
  state to highlight. **The word is `OFFLINE` and not `UNAVAILABLE`** for a
  measured reason: at 75px against the latter's 122px it is what lets a
  14-character device name sit beside it without being truncated, and it is the
  plainer word besides.
- **When the identity line does not fit, the NAME is what loses.** The state is
  short, live, and the reason to look at the card at all, while a name is static
  and already known to whoever installed it. `textTrunc()` drops characters and
  appends ".."; the shipped default names fit in every state, verified in
  `simulator.html`. The AC's exceptional-mode line drops the word "Room" for the
  same reason — the number is unmistakably a temperature beside its degree ring.
- **There is no degree glyph in either font.** `U+00B0` is outside the GFX fonts'
  0x20..0x7E charset, and a trailing "C" reads as a third digit at a glance. Both
  the setpoint and the room reading draw a 2px ring instead, positioned off
  `fontAscent()` rather than off a measured pixel.
- **Touch targets are the full 52px row band vertically**, not the 22px control
  strip. This is used in a dark bedroom; generous targets are intentional. Scene
  tiles take their full 100 × 72 pitch, so the margins and gaps fold into the
  nearest tile rather than missing; a swatch's tap cell is 30px wide while its
  circle is 22px, because the cell is the target and the circle is the affordance;
  and a settings toggle row is the whole row at any x — the pill is an affordance,
  not the hit area. The 32px tab strip is the one exception, and it is the
  least-used control. The scene scroll gutter is 20 × 104 per arrow — thin, but
  unlike the tab strip it sits in the band the 4-point fit *interpolates*.
- **The AC setpoint is a stepper, not two buttons and a caption.** The row reads
  `[▼] 29° [▲]`, and the value sits in the grid slot *between* the two controls
  that change it — it used to be `set 29` on the row's top line beside `T+`/`T-`,
  which put the number and the arrows at opposite ends of the row. Three
  consequences: the setpoint is **gone from `stateText()`** (repeating it would be
  two numbers a step apart competing to be read); `AC_BTN_TEMP` is a **readout**,
  so it draws no button, takes no press flash, and `screenHitTest()` reports a tap
  there as a **miss** — that dead cell is also what stops a slightly-off tap from
  stepping the wrong way, which the old adjacent `T+`/`T-` pair could not; and it
  is a **text region compared by its rendered string**, like the row's top line,
  not a `BtnVis` byte. It compares on `stale`/`err` too, which is why
  `RowSnap::stale`/`err` are assigned at the *end* of `drawDeviceCard()` rather than
  inside the top line's block — updating them there would consume the transition
  before the setpoint region could see it.
- **The steps are relative** and must no-op (setting `errMs`) until a real
  setpoint is known. Never guess a starting temperature. The readout shows `--`
  in the same window, so the value and the control agree about what is known.
- Active-highlight comparisons use tolerances (`PCT_MID_MIN`/`MAX` etc.) because
  HA round-trips `brightness_pct` through a 0–255 byte. Exact compares will not
  match.
