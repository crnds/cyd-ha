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
   88×64 tiles, two rows of them, **plus the AC card copied down from Devices**
   in the row band below the grid. Five scenes are defined; the page is built
   for far more. No new entities: the card is device 3 drawn a second time, at
   the identical rect, so the one device a scene deliberately cannot reach is
   still one tap away from here.
3. **Settings** — 5 board-level knobs on 4 rows (backlight, night mode [off /
   shift / red], beep volume, then night schedule and screen flip sharing the
   last row as two half-width cards). Nothing here touches Home Assistant.

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
of *those* is the thing to push back on. Note the converse has now happened too,
also on request: Scenes gained a **fixed** row — the AC card in the bottom band —
so it is the one hybrid page, a scrolling grid above a pinned control. Its own
exact-tiling assert became a bound to make room for it.

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

- **It draws the panel's ACTUAL GLYPHS, not a stand-in.** The `GLYPHS` blob holds
  the real byte streams from `Font16.c` / `Font32rle.c` / `glcdfont.c`, and
  `gtext()` re-implements the same three decoders `TFT_eSPI::drawChar` uses (row
  bitmap / 8-bit RLE / 5 column bytes) at the same advances. So the simulator is
  now WYSIWYG rather than indicative, and `textW()` is byte-identical to
  `tft.textWidth()`.

  It used to size Helvetica to match FreeSans' cap height, which was close enough
  only because FreeSans essentially *is* Helvetica. That trick broke outright on
  the built-in faces: Font 2's `O` advances 8px at a 10px cap height where
  Helvetica needs ~11, so glyphs were drawn wider than their advances and visibly
  collided — the simulator reported a layout that was not the panel's. Since a
  bitmap font is fully determined, shipping the real pixels is the honest fix.

  Regenerate with `python3 scripts/gen_sim_fonts.py` (glyph blob) and
  `python3 scripts/font_metrics.py` (the `ADV`/`INK_*`/`BASE`/`BOXTOP` tables and
  `gfx.cpp`'s). If a role is ever repointed at a different face, **both** scripts
  must be re-run — the layout in `config.h` is derived from those numbers.
- **It re-checks every `static_assert` from `screen.cpp`**, plus things the
  compiler cannot see. On the **stacked Settings brightness card**: that line
  1's descenders stop before the control row, that its dirty rect covers those
  descenders, and that the line's ink does not start *above* that rect — the
  third is new with the shorter face, which rides higher in the line than a
  taller one could. On an **inline device row** (every device card now,
  bulbs and the AC alike, both drawing a single centred name line): that the
  icon and the name's ink both fit the control band they share, that the
  identity clear rect stops before the first chip, and that every chip label
  still fits its chip (a fit failure there is silent — `textFit()` would just
  drop to the alt label or `F_MICRO`). On the **header's room-reading region**
  (the AC's live temperature/humidity, relocated there from its card — see
  `STATUS_ROOM_W`): that the worst *realistic* string (`"27"` + ring +
  `"99"` + a small `"%"` — not `"100"`, which an indoor bedroom sensor doesn't
  read) still fits the region's reserved budget and still leaves the deliberate
  gap before the clock, the same kind of check the AC's identity column needed
  before the reading moved; that the clock fits its own region in `F_NUM`,
  **including the `"--:--"` placeholder the browser's live clock never
  produces**; and that `F_NUM`'s ink envelope stays inside the header band at
  all, which only became a question when these regions went up a size. On the
  **connectivity
  banner**: that it sits wholly inside the row band it clears (anything
  hanging out would be drawn over pixels nothing cleared — the fragment
  problem the band-clear exists to fix), that it stops short of the scene
  scroll gutter, and that `"No Connection"` still fits its 106px budget. It
  also asserts the night palette's red ladder is collision-free (see "Night
  mode").
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

**Every corner is square, and there is deliberately no radius token.** Cards,
scene tiles, chips, steppers, the scroll thumb and the settings toggle are all
plain `fillRect`/`drawRect`, so a surface and the controls on it share one corner
treatment and nothing has to decide which step it belongs to. This replaced an
`R_SM` 6 / `R_LG` 8 / pill-at-`h/2` set, on request — **don't reintroduce a radius
for a single component**, since one rounded control on an otherwise square page
reads as a rendering fault rather than as a style. The settings toggle's knob went
from a disc to a square block for that reason: a circle sliding in a sharp-cornered
slot was the last mark that still read as rounded, and the knob's *position*, not
its outline, is what says on or off. Two rules elsewhere in this file used to be
argued from corner arcs and are now argued from border pixels; both still hold.

**Colour tokens are semantic, and that is what fixed the old palette.** The
previous names described appearance (`C_MUTED`, `C_GREEN`, `C_RED`), so each ended
up serving several unrelated roles and none could be changed without changing all
of them. Now: `SURFACE`/`ELEVATED` (a card, then a control on it),
`BORDER`/`DIVIDER`, `TEXT`/`TEXT2`/`TEXT3`, `DISABLED` vs `DIM` (no state to show
vs a stale reading), `ACCENT`, and `SUCCESS`/`WARNING`/`ERROR`.
**`ACCENT` is the only saturated colour in a resting UI** — if everything is
highlighted, nothing is — and the three status colours appear only when there is
something to say. **`BRI` is the one deliberate exception**, on request: a
bulb card's `1%`/`30%`/`100%` chip turns yellow rather than cyan when selected,
scoped to exactly that control — the AC's mode chips, tabs, Settings chips,
the night-mode picker and a swatch's accent halo all stay on `ACCENT`. A
second saturated colour loose across the whole UI would undo the rule above;
confined to one control on one card kind, it doesn't. See `BV_ACTIVE_BRI`
below and `theme.h`'s `C_BRI` comment for why it needed its own night-mode
derivation rather than reusing `redOnly()`/`warmShift()` untouched — a plain
yellow collapses onto `WARM`'s amber under Night Shift's blanket scaling, so
`C_BRI`'s shift value is a hand-picked, green-boosted override instead.

**Type is four roles over TFT_eSPI's built-in BITMAP faces**, selected by number
in `gfx.cpp`: `F_NUM` (Font 4, 26px box, **live numeric readouts** — the AC
setpoint plus the header's room temperature/humidity and clock), `F_TITLE` and
`F_BODY` (both Font 2, 16px box), `F_MICRO` (Font 1 / GLCD 6×8, last-resort fit
plus the tab bar). These are drawn pixel by pixel at one fixed size, so every stem
lands on the grid and nothing is scaled or resampled at draw time — which is the
whole point, and why the FreeSans GFX faces this used to carry are gone.

**`F_NUM` was the AC setpoint ALONE until the header's numbers were bumped up to
it** on request. Two consequences worth keeping: the role now means "a number read
at a glance" rather than "the number being adjusted", so a label or caption still
must not ask for it; and the setpoint lost its claim to being the largest thing on
the panel. It still reads as the Devices page's emphasis, but because of where it
sits — on a card, between the two chevrons that change it — not because nothing
else is as big.

**This was a deliberate swap, on request, away from the proportional FreeSans
build.** Do not "restore" it as a legibility fix without re-reading the trades
below; three of them are worse than they were, and that was the accepted price.

- **`F_TITLE` and `F_BODY` ARE THE SAME FACE.** The built-in set has no bold, so
  the whole title-vs-state hierarchy now rests on **colour tier alone** (`C_TEXT`
  vs `C_TEXT2`/`C_TEXT3`); the weight signal that used to carry half of it does
  not exist. Do not "fix" this by promoting titles to Font 4 — at 18px caps it
  does not fit `CARD_L1_H`, and a card title as large as the AC setpoint inverts
  the emphasis of the row it sits on.
- **Body text is smaller than it was: 10px caps against FreeSans' 13px.** That is
  the cost of the pixel grid. It also squeezes the fallback ladder — `F_MICRO` is
  only 3px shorter than `F_BODY` now, so a `textFit()` fallback is far less
  visible than it used to be and correspondingly less of a warning.
- **The ladder has a hole, and it is expensive.** There is nothing between Font 2
  (10px caps) and Font 4 (18px caps) — TFT_eSPI has no Font 3 or Font 5 at all,
  their `fontdata[]` slots are null placeholders — so `F_NUM` is the only step
  above body text and a settings caption is necessarily the same size as its
  title. The consequence when something *is* bumped a size: it costs **1.8× the
  width**, not a nudge. `"23:45"` goes 35px → 63px and `%` goes 9px → 21px. That
  is why the header could only afford it for two regions after a glyph was deleted
  and `TAB_GAP` cut twice, and why its `%` had to stay behind at `F_TITLE`.
- **Mixing two roles on one line needs `fontBaseline()`.** An M\* datum centres
  each role on its *own* box, so the same `cy` does not put two roles on the same
  baseline — the difference of two `fontBaseline()` entries is the correction.
  `drawStatusRoom()` is the one caller (its small `%` after big digits). Don't
  reach for `fontInkBottom()` instead: that is an envelope including descenders,
  so on glyphs that have none it aligns the wrong row.
- **Font 2 is appreciably narrower**, which is the one thing that got easier:
  "Settings" went 65px → 49px, `"COOL"` 51px → 31px, and the default 14-character
  device names went ~151px → ~104px. (The clock went 44px → 35px too, and has
  since gone to 63px by moving up to `F_NUM` — that is a role change, not the face
  swap.) Several comments in `config.h` that read "X only just fits" now describe
  slack; they say so.
- **Ink is NOT centred in the box TFT_eSPI positions.** A built-in glyph sits
  inset by blank rows, by a different amount top and bottom, and `drawString`
  centres an M\* datum on the *full box* where it centred a free font on its
  *ascent*. Hence `ROLE_DY` in `gfx.cpp`, and hence `fontInkTop()`/
  `fontInkBottom()` replacing the old `fontAscent()`/`fontDescent()` — callers
  want "where does the ink start", which the old ascent/2 arithmetic only
  answered by accident of the free fonts being symmetric about their datum.
- **Flash cost is a wash, not a saving.** Font 2 + Font 4 + GLCD come to 8475 B
  against FreeSans' 6582 B + GLCD 1280 B = 7862 B, so **+613 B**. Font 4 carries a
  full 96-character set for a role that draws digits and `--`; that is where it
  goes. If flash ever matters, that is the thing to trim, not the face choice.
- **`ROLE_INK_TOP` / `ROLE_INK_BOT` / `ROLE_DY` are measured, not read off the
  font headers.** The headers give the nominal box (Font 2: 16 tall, baseline 13;
  Font 4: 26 tall, baseline 19) but Font 2's caps start 3 rows down and Font 4's
  1 row down, so the nominal numbers put the degree rings in the wrong place.
  `python3 scripts/font_metrics.py` decodes the glyph data and prints all three
  tables plus simulator.html's; re-run it if a role is ever repointed.

**Every component honours the full state set** — `BV_INACTIVE / BV_ACTIVE /
BV_ACTIVE_OFF / BV_PRESSED / BV_DISABLED / BV_ERR / BV_ACTIVE_BRI` — and resolves
it through one `ctlColour()` table rather than branching locally, so a pressed
chip and a pressed chevron cannot come out looking like different interactions.
The table's shape is the rule: **every selected state is a solid fill with dark
text, and only WHICH fill depends on what was selected.** Five decisions in there:

- **An inactive control has no border** (`edge == fill`). The old page outlined all
  23 controls at once; deleting those outlines, not changing any colour, is what
  made the page quiet.
- **`BV_ACTIVE_OFF` exists because OFF is not an accomplishment.** A cyan OFF chip
  made the quietest state on the Devices page the loudest mark on it, and with three
  bulbs off, three of the four cards lit up. `C_NEUTRAL` grey still reads as
  selected — 9 night-steps and ~2.75:1 day luminance clear of `C_ELEVATED` — without
  claiming anything is happening. It is a **vis state, not a colour the caller
  passes in**, which is what keeps the invariant above in this one table.
- **`BV_ACTIVE_BRI` exists because a bulb's brightness chip was asked to turn
  yellow, and only that chip.** `screen.cpp`'s region-2 loop is what does the
  scoping — `!ac && !isSwatch` picks it out from the same `btnActive()` branch
  that assigns `BV_ACTIVE`/`BV_ACTIVE_OFF` to every other chip and swatch — so
  the AC's `COOL`/`DRY` chips and a colour-temp swatch's halo are unaffected
  even though they share the exact same code path up to that point. Kept as
  its own vis rather than a colour parameter for the same reason
  `BV_ACTIVE_OFF` is: the "every selected state is a solid fill with dark
  text" invariant lives in `ctlColour()` alone, and a caller-supplied colour
  would let some future control quietly break it.
- **`BV_DISABLED` recedes into the background** rather than greying out on top of
  the card. There is no state to show, so the control must not look like it is
  showing one. The Devices card carries no fill, so `C_BG` is what it sinks to.
- **`BV_PRESSED` is a full-brightness fill**, deliberately the loudest thing the
  UI ever draws, because it has to land within `PRESS_FLASH_MS` and before HA has
  answered. It is the only such fill.

**`wStepBtn()`'s chevron colour is a second exception to the ctlColour() table,
alongside `wSwatch()`.** The AC setpoint's up/down chevrons are `C_ERROR`
(red) and `C_DOWN` (a vivid blue) respectively, on request, regardless of
`vis` — a direction identity, not a selection state, so it does not fit the
"one table, resolved by `BtnVis`" rule the rest of this section describes.
Up reuses `C_ERROR` as-is; down needed a genuinely new token, `C_DOWN` — the
obvious reuse candidate, `C_COOL`, is deliberately pale (it has to read as
"this bulb is ~4000K", not just "this is blue"), which read as muted rather
than the "pop" that was asked for, and `C_ACCENT` was ruled out on role
grounds (it means "selected"; this chevron is this colour at rest, so
reusing it would say the wrong thing regardless of hue). `C_DOWN` needed its
own pinned night(red) override, the same way `C_BRI`'s did: the day colour's
derived luminance landed exactly on `WARM`'s red-level, an exact collision
`simulator.html`'s `checkNightLadder()` catches globally rather than only
against the pair it was checked for — pinned to red-level 29 instead, clear
of `COOL` (27) and `ERROR` (31), the two things it can appear beside. Night
Shift needed no override: `C_DOWN`'s day red channel is already far from
`ERROR`'s, `WARM`'s and `COOL`'s pinned Shift red channels, so `THEME_DERIVE`
collides with nothing there. `BV_PRESSED` and `BV_DISABLED` are carved back
out of the override on both chevrons — both still take their colour from
`ctlColour()`, so the press flash and the disabled recede read exactly like
every other control's; a disabled chevron staying red or blue would read as
"still selected" on a card that is trying to say the opposite.

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
`climate.*` goes out to the Sensibo **cloud**: measured **1135–1643 ms** for a
real change. The trap is that a *no-op* write short-circuits in 24 ms, so
benchmarking `set_temperature` with the value it already has reports 24 ms and
hides the problem entirely. That mistake shipped a 1500 ms `HTTP_READ_SVC_MS`,
and every genuine `T+`/`T-` tap then reported a read timeout — which argued for
raising it to 2500, comfortably above the measured 1135–1643 ms range.

**`HTTP_READ_SVC_MS` is 1500 anyway, not 2500** — deliberately reverted once
the next fact below made the trade worth re-examining: a read timeout there
is not a failure, so the extra 1000 ms of 2500 bought only a cleaner log line
at the cost of a longer frozen screen. Still comfortably above the measured
1135–1643 ms real-change range, so a genuine climate call still reads back
clean far more often than it times out.

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

**Scenes** (`SCENE[]` in `src/ui/screen_scenes.cpp`, `doScene()` in `main.cpp`) act on the
three bulbs only — the AC runs on a different comfort schedule, and folding it
in would make every scene tap a Sensibo *cloud* round trip. **That is still
exactly true of the scene TABLE even though the AC card is now on the page**,
and the two must not be conflated: no scene touches the AC, and the card is not
a scene. It is `drawDeviceCard(SCENE_AC_SLOT)` — the Devices page's card 3,
drawn on a second page — so it takes no part in `sceneActive()`, `scenePlan()`
or the tiles' shared `BV_ERR`/`BV_DISABLED` derivation, and an AC fault greys
out nothing.

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
  would be three sequential `HTTP_READ_SVC_MS` budgets (4.5 s of frozen `loop()`)
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
  are fixed, so the 10/9px gaps between them never change content, and a slot past
  the end of the table is blanked with the same rect the tile occupies. This used
  to need saying: when tiles were rounded, the clear had to be a **square**
  `fillRect` on purpose, since `fillRoundRect` leaves the four corner pixels of its
  bounding box untouched and a rounded clear stranded the previous tile's corners.
  Corners are square now, so the drawn shape and the clear cannot drift apart.
- **`Hit::idx` is `int16_t` for this page.** `HIT_SCENE` carries an absolute index
  into the whole table, not the 0–11 on-screen slot, and an `int8_t` would wrap at
  128 — inside the range the grid was rebuilt to handle.
- **The arrows page, not step.** At two visible rows, a row-at-a-time arrow
  needs 33 taps to cross 100 scenes (34 grid rows); a page needs 17. The argument
  got *stronger* when the AC card took the bottom band and the grid went from
  three rows to two — fewer rows per page means more of them. `sceneScrollBy()`
  clamps to `SCENE_MAX_ROW`, so the last page is full rather than mostly empty.
- **The whole scroll affordance compiles out at five scenes.** `SCENE_MAX_ROW` is
  a `constexpr` 0, so the gutter paints one `fillRect` and is dead to taps. Note
  the consequence: the arrow/thumb code is currently unexercised on hardware.
  `simulator.html`'s scene-table buttons (5 / 24 / 100) exist to drive it. Note
  the threshold moved with `SCENE_VIS_ROWS`: scrolling now goes live at the 7th
  scene rather than the 10th.
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
  is for. Legible tiles beat a denser grid of illegible ones, and the page still
  scales to 100+. The gaps differ per axis (12 across, 8 down); the vertical
  budget used to *force* the 8, since `2*PITCH_Y + TILE_H == 208` had exactly one
  solution keeping tiles above 60px, and with two rows it no longer does — 8
  survives because the 88×64 tile was kept rather than grown, which is also what
  left rows 0 and 1 pixel-identical to where they were before.
- **The grid gives up its bottom row to the AC card, and with it the
  exact-tiling rule.** Two rows reach y 135, the card's band starts at
  `SCENE_AC_Y0` (156), and 20px of background sits between them. The y
  `static_assert` is `<=` rather than `==` as a result — a real, deliberate
  weakening, and the only place in this firmware where "a gap leaves pixels
  nothing ever clears" is suspended. It costs nothing *here* because that band
  has no content to go stale: across x 0..299 nothing writes it after
  `bodyReset()`'s one-time fill, and the gutter's column (x 300..319) is the one
  region that reaches in, which owns and clears its own rect there. What the
  assert still catches is the grid **growing** into the card, which would leave
  permanently wrong pixels — the same failure the x-side rule guards.
- **`SCENE_SB_MID` moved 104 → 78 with the grid, and had to.** The gutter's tap
  split halves the *gutter's* range, which is no longer the body's: the card
  runs to x 311 and so passes under the gutter's column, so
  `drawSceneScrollbar()` clears only down to `SCENE_AC_Y0 - 1`. Left at 104 the
  down arrow would have had 52px against the up arrow's 104. That clear bound is
  load-bearing, not tidiness: unbounded, it paints `C_BG` over x 300..319 of the
  card's band and erases the card's right border column at x 311 every time the
  gutter repaints — invisible today only because the gutter is dead at five
  scenes.
- **The hit test checks the card's band BEFORE the gutter.** They overlap in x
  (card 8..311, gutter 300..319) and y is the only axis separating them, so the
  y test has to resolve first — otherwise every tap on the card's up chevron
  (x 276..303) reads as a scroll. `hitDeviceRow()` in `screen.cpp` is the shared
  sweep both pages go through, factored out for the same reason `btnRect()` is
  the only function that knows a row's x layout: two hand-written copies of
  "skip the setpoint, walk `btnRect()`" would be free to drift, and the one
  thing that must be true of this card on both pages is that the same pixel does
  the same thing.

**The UI layer is four translation units now, not one.** `src/ui/screen.cpp`
used to hold all three pages plus lifecycle/hit-test/splash in a single
1732-line file, which meant every page's draw function inlined into one
`screenRender()` symbol regardless of what actually changed. It split along
page boundaries:

| file | owns | not shared with |
|---|---|---|
| `src/ui/screen.cpp` | lifecycle (`screenBegin`/`screenRender`/`screenInvalidate`/`screenSetFlip`/`screenSetNightMode`/`screenSetClock`), the header/tab strip, the connectivity overlay, `screenHitTest()` and its shared `hitDeviceRow()`, splash, calibration | — |
| `src/ui/screen_devices.cpp` | the Devices page (4 cards, 23 controls): `btnActive`, `iconVis`/`bulbHue`, `drawDeviceCard` | — |
| `src/ui/screen_scenes.cpp` | the Scenes page, including `SCENE[]` itself, and the one call to `drawDeviceCard()` from outside `screen.cpp` (the AC card in the bottom band) | `SCENE[]`'s storage — `sceneCount()`/`sceneMaxRow()` (`screen.h`) are the only way another file learns the count or the scroll range |
| `src/ui/screen_settings.cpp` | the Settings page | — |
| `src/ui/screen_int.h` | shared types (`Rect`, `CardFingerprint`/`RowSnap`, `SceneSnap`, `SettingSnap`), the three page snapshots as `extern` (defined without `static` in the .cpp that owns each, so `screen.cpp`'s `screenInvalidate()`/`bodyReset()` can still `memset()` them across the TU boundary), and small `static inline` predicates (`pressedNow`/`staleErr`/`alarmFg`/`noConnShown`/`pctMatches`/`kelvinMatches`/`rowTop`/`cardTop`) | not the public API — `screen.h` is |

Each page's own geometry function (`btnRect`/`settingChipRect`/`sceneTileX,Y`)
is defined in that page's `.cpp` (no longer `static`) and declared in
`screen_int.h`, since `screen.cpp`'s hit-test and calibration-verify screen
call across the boundary into all three — that is the one thing every page
used to keep fully private and now has to expose. `tabRect()` stayed fully
private to `screen.cpp`, since only cross-page code (the header) ever needed
it. The cost of the split is real and was accepted deliberately: without
LTO (see `platformio.ini` below), a call across a TU boundary can no longer
be inlined the way a same-file call could, so flash grew ~736 B for the split
alone. RAM is unchanged — the same total storage, just declared `extern`
instead of `static` in a different file. Measured end to end, this and the
two changes below (the pre-filter, ~260 B; the shared clock reading,
negligible) came out flat against dead-code removal earlier in the same
refactor: **+12 B total** — see `docs/OPTIMIZATION.md` for the step-by-step
numbers.

**`drawDeviceCard()`/`drawStatusRoom()` have a raw-input pre-filter ahead of
their per-region compares**, so "returns early unless a value changed" (see
Performance budget in `DESIGN.md`) is true of the whole function, not just
the drawing. `CardFingerprint`/`RoomFingerprint` (`screen_int.h`) capture
every raw field the function reads — `on`/`avail`/`known`/`supportsCT`/`pct`/
`kelvin`/`target`/`mode` plus the time-derived `stale`/`err`/`alarm`/`press`
— and `memcmp` the whole struct (zero-inited first, so padding can't cause a
false mismatch) against last pass. Bit-identical means the function returns
before doing any of `iconVis()`/`bulbHue()`'s colour maths or `tempText()`'s
formatting; the per-region/per-button compares are untouched and remain the
authority on what actually repaints. Floats are compared by BIT PATTERN via
that `memcmp`, never by `==` — `d.target`/`d.room` can be `NaN`, and
`NaN != NaN` would silently defeat the early-out exactly when a value is
genuinely unknown, which is the one case this exists to handle correctly.
**`ERR_FLASH_MS`** (`config.h`, 1500) replaced a bare `1500` duplicated at
three sites in `screen.cpp` and mirrored by hand in `simulator.html`.

**`loop()` reads the wall clock once per pass, not up to four times.**
`serviceNightSchedule()`/`serviceDailyRestart()` (`main.cpp`) take the shared
`hhmm` (hour×60+min, `-1` = NTP unsynced) as a parameter instead of each
calling `getLocalTime()` independently, and `screen.cpp`'s header clock gets
it through `screenSetClock()` (`screen.h`) — same "memoised, safe to call
every pass" contract as `screenSetNightMode`/`screenSetFlip`, for the same
layering reason: `tft`'s state stays file-static in `screen.cpp`, so
`main.cpp` reaches the header through a setter rather than touching it
directly. This matters because `screenRender()` itself runs up to 4 times in
one `loop()` pass — `doAction()`/`doScene()`/`doSetting()`'s immediate
optimistic repaint, plus `loop()`'s own — and the wall clock cannot have
changed within a pass. The two schedulers' separate edge trackers
(`lastMin`) are unrelated and untouched — only the redundant syscall was
removed, not the intentionally duplicated boundary logic.

**A host-compiled equivalence harness exists for this reason: none of the
above could be verified on hardware.** `tools/host_check/` `#include`s the
relevant `.cpp`(s) directly (their target functions are `static`) against
stub `Arduino.h`/`TFT_eSPI.h` headers and diffs stdout against a captured
golden — see `tools/host_check/README.md`. It is not a substitute for
`simulator.html`'s visual fidelity or an eventual hardware pass; it exists
specifically to catch a logic regression in the dirty-region/derivation code
that neither of those two checks reaches.

**Rendering** (`src/ui/screen.cpp`) is **dirty-region based**. `screenRender()`
runs every loop pass, dispatches on `S.page`, and repaints only what changed:
- The **header** is three independent regions (tabs / room reading / clock) that
  **tile it exactly** — the tiling is enforced by construction (`TAB_STRIP_W` is
  *derived* from the other two plus `TAB_X0`, so there is nothing left for a
  `static_assert` to catch), because a gap leaves pixels nothing ever clears and
  an overlap is just as bad, since each region only clears its own rect. There
  used to be a fourth, a 26px connectivity glyph at x 0..25, and `TAB_X0` began
  after it at 26; it is gone (see the connectivity banner below) and `TAB_X0` is
  0. The split is now **171 + 80 + 69 = 320**: the glyph's 26px did *not* stay
  with the tabs, it landed in `TAB_STRIP_W`'s derived width as slack and has since
  been spent — with another 8px from `TAB_GAP` — on making the two number regions
  a size bigger. The tabs are flush left and use 166px of their 171, leaving 5px
  before the room reading.

  **Both number regions are `F_NUM` now**, up a size on request. That is a
  1.8× width jump, not a nudge (see the ladder-hole note above), and it is the
  whole reason the budget is this tight. Three parts of it are load-bearing:
  - **The `%` stayed `F_TITLE`.** At `F_NUM` it is 21px against 9px — width this
    region does not have, and the pair would not have fitted at all. It is a unit
    marker rather than data, the same argument that used to keep the `/` quiet, and
    it is drawn **baseline-aligned** to the big digits via `fontBaseline()`, not at
    the same `cy`, or it would float mid-height and read as a smaller number.
  - **The degree ring stayed a 2px circle** with a 5px advance. `wValue()` already
    draws exactly that ring beside the AC setpoint's `F_NUM` digits, so scaling
    this one with the font would make the two disagree.
  - **The trailing `/` is gone.** At `F_MICRO` beside 18px digits it read as
    vestigial, and the 6px `STATUS_ROOM_W` leaves over now does the separating.
    That 6px is **deliberate**, which reverses the old "zero leftover" note — it is
    the gap before the clock, not slack, so don't spend it.

  The room region repaints on its own compare (room string, humidity string,
  resolved colour), independently of the tabs and the clock, the same "compare by
  rendered appearance" rule every other region follows. Its humidity budget is
  still 2 digits, not 3 (`"100%"`) — an indoor, air-conditioned bedroom sensor
  doesn't read 100% relative humidity, the same realistic-vs-possible judgement
  the temperature side (2 digits, not `ha.cpp`'s full -10..60C band) already made.
  A reading that *does* exceed it spills right, into the clock, and costs more
  than it used to: `"100"` is 14px wider at `F_NUM` than at `F_TITLE`. Still
  accepted, and still self-healing — `drawStatusRoom()` clears only its own rect,
  so a stray pixel sits there until the clock's next per-minute repaint clears
  that whole region. Worst case a 59-second-old artifact.

  The clock repaints once a minute, which is invisible. It replaced a
  freshness readout that counted seconds since `haOkMs` — and since every poll
  resets that timestamp, the number oscillated `0↔1` and repainted the full bar
  several times a second. **Don't put any per-second value here**; that region is
  minute-rate by design. Connection health belongs to the banner below, and
  staleness to the card dimming, not to this readout. **The clock is exactly 5
  characters** — `"23:45"` is 63px in `F_NUM` against the 69px this region now
  leaves (it was 35px of 41 at `F_TITLE`, and 51px before the room reading arrived
  beside it), so anything wider spills into the room region, which only repaints
  on its own compare, making *that* overflow permanent. The margins are what they
  always were: a 6px right margin and **zero px of left slack**. `"--:--"` is
  39px, comfortably narrower, as it has always been.

  **Vertical fit stopped being free when these went to `F_NUM`** and is worth
  re-checking if either moves again: its box is 26px against `F_TITLE`'s 16, in a
  31px band, and `ROLE_DY` seats it 4px low. The ink envelope lands at y 216..239
  inside the band's 209..239 — it fits, but the bottom is exactly the last screen
  row, and only because the glyphs actually drawn (digits, `:`, `-`, `%`) have no
  descenders. `simulator.html` checks the envelope; there is no `static_assert`
  for it, because the ink tables live in `gfx.cpp` as non-`constexpr` arrays.
- **The current tab is an underline, not a filled pill.** A pill spent the
  solid-accent fill — the mark that means "selected" on a chip — on the one strip
  that navigates rather than acts, so the header read as a fourth row of buttons.
  `wTab()` now draws the label plus a 2px `C_ACCENT` bar seated on the selected
  one, and selection carries **two** signals (`C_TEXT` vs `C_TEXT3` *and* the
  bar), so it survives the night palette the same way the connectivity banner
  does. Three things about it are load-bearing: the bar is flush with the **TOP
  of the rect `drawTab()` passes**, which is the cell's clear rect, so it lands
  on the first row below the 1px rule and the two read as one line — one row
  higher and it would overwrite a rule that is painted once and never restored,
  one row lower and a gap opens. (The header now sits at the bottom of the
  screen, so the rule is at the TOP of the band, not the bottom, and the bar
  flipped to match — it now reads as an "overline" sitting just above each
  label rather than below it.) It must stay **inside** that clear rect or a tab
  that stops being current keeps its bar forever. And it spans the **word, not
  the cell** (measured with `textW`, so a renamed tab needs no constant
  changed): at 81px against a 46–65px label, a full-width bar is a box around
  the label, which is the button again. `TAB_UL_H` is 2 and not 3 because
  "Settings"' ink starts well clear of the bar even at this tight a margin;
  `simulator.html` asserts that clearance, the seating identity, and that every
  label still fits its cell.
- **Connectivity is a BANNER IN THE BODY now, not a glyph in the header.**
  `drawNoConn()` draws `"No Connection"` over row band 0 while HA is
  unreachable, and nothing at all when it is reachable. This replaced a 26px
  wifi-bars glyph in the header's left corner (which had itself replaced two
  labelled dots reading `WIFI`/`HA`, 88px of permanent debug chrome telling a
  healthy system it was healthy). The glyph was removed on request once the
  header moved to the bottom edge and it became a bottom-left-corner ornament;
  `icoWifi`/`icoBadge` went with it, since nothing else used them. Five things
  about the replacement are load-bearing:
  - **It is gated on `!S.haOk` ALONE**, so it says *that* the link is down and
    not *which end*. The glyph distinguished Wi-Fi-down (0 bars, red) from
    HA-down (3 bars + amber badge); that is deliberately dropped.
    `updateNetState()` already forces `S.haOk` false whenever Wi-Fi drops, so
    one bit covers both causes, and it is the bit that matters to someone at
    the panel: nothing they tap will work. The finer diagnosis is in the serial
    log — the same trade that retired the `WIFI`/`HA` labels.
  - **It IS the body, so it must occlude a page.** The header has no room: three
    regions tile that bar exactly and each clears only its own rect. Occluding
    is the point, since the fault is not page-specific.
  - **It CLEARS ITS WHOLE ROW BAND FIRST, and that is what makes it read as an
    overlay rather than as a clipping bug.** Painted straight on top it leaves
    fragments of whatever it didn't quite cover — the first attempt left the
    bottom of a yellow brightness chip poking out under its border on Devices,
    and a 2px cyan sliver on Settings, whose STACKED card puts its controls 2px
    lower than an inline device card does. **The clear is the CURRENT PAGE's
    first row**, not one fixed rect: the two grids don't share a pitch, so a
    52px row band leaves 12px of a 64px scene tile showing. On Scenes it also
    **stops at `SCENE_SB_X0`** — the scroll gutter is its own region on its own
    compare, and clearing across it erased the up arrow with nothing to put it
    back.
  - **Every page SKIPS its first row while the banner shows** (`noConnShown()`).
    The banner draws *last*, so anything a page paints in that band on a later
    pass lands on top of it, and no page's compare would ever put it back —
    `drawNoConn()`'s own compare only tracks whether the banner should be shown,
    not whether something scribbled on it. Two real cases: a scene scroll
    repaints every visible tile, and a device card's stale/err transition
    repaints inside band 0 by itself. The skipped rows' snapshots go stale
    meanwhile, which is safe because the hide path is a full
    `screenInvalidate()`. Note the one thing this costs: Settings' brightness
    row is row 0, so it is hidden while HA is down even though it still works
    (no Settings row touches HA). The other three rows still draw.
  - **The hide path is `screenInvalidate()` with NO `fillScreen()`.** The banner
    sits on top of content already cached as clean, so clearing to `C_BG` would
    leave a banner-shaped hole; `fillScreen()` would fix that and flash the whole
    panel for a frame to remove a 114×26 box. `screenInvalidate()` writes no
    pixels — it drops every snapshot so the next `screenRender()` runs
    `bodyReset()` and redraws the body properly, the same one-frame (~20ms) lag
    night mode and screen flip already accept.
- Each **card's identity** (icon + name) is **one** region compared by its
  rendered string *and* by the icon's resolved shape and colour. The icon has
  to be in that compare, and on every card it is doing real work no string
  duplicates: nothing textual on the card mentions the AC's *mode* (the chips
  do — the room reading, now in the header, only ever shows a temperature and
  a humidity), so a `cool`→`dry` change would otherwise dirty nothing and
  leave the wrong glyph on the glass; a bulb has no mode string at all, so
  brightness and colour changes reach the screen *only* through the icon's
  resolved colour.
  **Every device card clears only its identity COLUMN** (`BULB_ID_W`, x
  16..59), not the card width — this used to be a bulb-only optimisation but
  now applies to the AC too, since it shares the same inline layout: the six
  controls beside it sit on the same row and repaint on their own compares, so
  a full-width clear would erase chips nothing is going to redraw.
- Each **control, tab, scene tile and settings toggle** is compared by its *visual*
  state (`BtnVis`), not by underlying values — so two brightness values mapping to
  the same highlight cost nothing, and a change repaints 2 chips instead of all 6.

**Every dirty rect inside a card starts at `CARD_IN_X0`, never at `CARD_X`.**
Filling `x 8..15` would paint over the card's own left border column and erase the
outline, one repaint at a time. (This used to be about the `R_LG` corner arcs;
the corners went square, the rule did not.)

**A STACKED card's line-1 dirty rect (`CARD_L1_H`) deliberately runs past the 13px
ascent box the datum centres on**, because it must cover descenders: clear only the
ascent box and renaming "Reading lamp" to "Lamp" leaves the g's tail on the card
forever. `CARD_L1_CY` is 8 and not 9 for the mirror-image reason — at 9 those
descenders reach into the control row. Device names are user data from
`secrets.h`, so they cannot be assumed to be the all-caps they happen to be today.
That is the **Settings brightness card only** now; **every device card is
inline** — the AC joined the three bulbs on request, so its buttons sit at the
same height and position theirs do rather than in a shorter strip under a
state line — and identity shares the control band, so the constraint there is
that the icon and the name's ink both fit inside `BULB_CTL_DY..BULB_CTL_H` —
`simulator.html` asserts that pair, which the stacked layout never needed. The
AC's identity column briefly stacked a room temperature and humidity line
under its name too, in place of the right-aligned state string a stacked AC
card used to draw beside a 26px control strip — that reading has since moved
again, to the header (`STATUS_ROOM_W`), so the AC's identity column is back to
the bulb's plain single line — see "Two card internals" below for the
geometry history.

**The card's BORDER is its alarm channel: a failed service call flashes it, and an
unreachable device HOLDS it.** The whole card carries the notification, and
redrawing the outline alone is enough since nothing behind it changes. The second use is not decoration — it is what replaced the word
`OFFLINE`, first on the bulb cards when their state line went away and later on
the AC too once it joined them inline, so the fail-loud rule survives a layout
with no room for text on either kind. It cannot misfire at boot:
`DeviceState::avail` starts `true` and only a poll that actually saw
`unavailable` clears it.

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
previous page's pixels permanently, and the 20px band between the last tile row
and the AC card is painted *here and nowhere else*. (Scenes' bottom band does
self-clear now, because the card in it is an ordinary device card doing the
ordinary thing — that changes nothing about the rest of the page.) (A *scroll*
within Scenes is different and needs no wipe — see the scene-grid notes above for
why.) **That `memset(snap, ...)` is also what lets ONE `RowSnap` serve the AC
card on two pages**: arriving on either drops the cached bytes, so the card
always does a first draw rather than trusting a snapshot taken while the other
page was on the glass. An optimisation that skipped the wipe would leave that
band empty on arrival, with the cached fingerprint suppressing the draw that
would have filled it. `screenInvalidate()` deliberately does **not**
`fillScreen()`, because `screenSplash()` draws the logo and then calls it; the
wipe is deferred to the next `screenRender()`.

**`statusSnap` is assigned field-by-field, not by aggregate init.**
`statusSnap = { ... }` compiles silently when a field is added (there is no
`-Wmissing-field-initializers` in this build) and zero-initialises it — which for
`tabVis` means "every tab inactive", while the computed value always has one
active, so the strip would repaint on every pass. A flicker bug with no warning.

**All text is drawn transparent, so each dirty region is `fillRect`-cleared
first.** `textAt()` uses the one-argument `setTextColor()`, which sets the
background to the same colour and thereby selects TFT_eSPI's transparent glyph
path for both the Font 2 bitmap and the Font 4 RLE decoder. That is not merely
inherited from the old GFX behaviour — it is now required: with `ROLE_DY` applied,
Font 4's 26px box is seated 4px low, so it **overhangs whatever band it sits in**
and an opaque draw would overdraw the neighbours. Two live cases: the AC setpoint,
whose box busts its 26px `CTL_H` row by 4px top and bottom, and the header's clock
and room reading, whose box reaches y 240 — one row past the last screen line.
Both are harmless *only* because the draw is transparent and Font 4's deepest
possible ink is row 239. The row separator sits at `top - 2`, outside every clear
rect, so it is painted once.

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
and the measured 19-step ladder is

```
BG 0 < SURFACE 2 < DIVIDER 3 < ELEVATED 4 < BORDER 6 < DIM 7 < SUCCESS 10
     < DISABLED 12 < NEUTRAL 13 < WARM 14 < TEXT3 16 < ACCENT 18 < TEXT2 20
     < WARNING 22 < BRI 24 < TEXT 25 < COOL 27 < DOWN 29 < ERROR 31
```

`BRI` (the bulb brightness chip's yellow) and `DOWN` (the AC stepper's blue
down chevron) are the two newest entries — later, scoped exceptions to
"`ACCENT` is the only saturated colour", added on request for one control
each. Both needed their own pinned night(red) level rather than deriving:
`BRI`'s day luminance would otherwise collide with nothing directly, but its
Shift value collapses onto `WARM`'s amber (see `C_BRI`'s comment); `DOWN`'s
derived night(red) level lands exactly on `WARM`'s (14), a real collision
`simulator.html` catches, so it is pinned to 29 instead. All 19 are distinct.
The `SURFACE`/`DIVIDER`/`ELEVATED` trio sits one step apart,
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
`DISABLED` only ever a label on an unfilled control, `WARM` only ever a swatch disc).
The low half of the ladder has no free level with two clear steps below `ACCENT`.

**A palette or rotation change repaints nothing by itself.** Every dirty-region
compare is on a *value*, and neither alters one — so `screenSetNightMode()` and
`screenSetFlip()` must `fillScreen()` + `screenInvalidate()`. Both are memoised
no-ops otherwise, which is why `loop()` can call them every pass.

**The schedule *writes* the Night mode picker; it does not override it.**
`serviceNightSchedule()` is edge-triggered at 23:45 and 08:00, so between the
boundaries a manual pick always wins and sticks. A level-triggered version
would re-assert itself on the next render and make the picker physically
un-turn-off-able before 08:00. On the first valid clock reading it *adopts* the
window, which is what makes a 02:00 reboot come up already in night mode.
Scheduled transitions deliberately do **not** write NVS — only user taps do,
since boot-time adoption already restores the right state.

**Night mode is 3-way — Off / Shift / Red — and the Settings row that used to
be a toggle is now a 3-chip segmented control**, the same pattern the
Brightness row's 5 chips already use. This was a deliberate reuse rather than
a new row: the Settings page is fixed at exactly 4 rows that tile the body
exactly (see below), so a 5th row was explicitly ruled out and the existing
Night mode row absorbed the new state instead. (Volume later faced the same
wall and was handled the same way — by pairing the two toggles into one row,
not by adding a fifth.) `NightMode`
(`include/state.h`) is `{ NIGHT_OFF = 0, NIGHT_RED = 1, NIGHT_SHIFT = 2 }` —
**RED deliberately kept ordinal 1**, matching the legacy bool's "true", so a
device already persisting `s.nit = 1` in NVS reads back as full Red with zero
migration code; `NIGHT_SHIFT` (2) is the only genuinely new value. The 3
chips' on-screen order (Off, Shift, Red) is *not* `NightMode`'s storage
order, so `NIGHT_CHIP_MODE[]` is the one place that maps chip position to
mode, used by both the renderer and `doSetting()`.

**Shift is manual only — the schedule never selects it.**
`serviceNightSchedule()` still writes only `NIGHT_RED`/`NIGHT_OFF` at its two
boundaries, exactly as it wrote `true`/`false` before Shift existed. A
manually-picked Shift rides through both boundary checks untouched — same
"manual wins between boundaries" rule as always — and loses to whichever the
boundary sets the moment one actually fires. There is no schedule state that
means "engage Shift."

**Shift does not touch the backlight — only Red does.** `applySettings()`
forces `BRI_DUTY[BRI_NIGHT]` (the dimmest step) only when
`S.set.nightMode == NIGHT_RED`; Shift leaves `briIdx` alone. This is a
deliberate scope limit: there is no second "night brightness" concept in this
codebase to reuse (`BRI_NIGHT` is just index 0 of the ordinary 5-step
`BRI_DUTY_LIST`), and inventing a dedicated Shift duty was out of scope for
what is meant to be a palette-only effect.

**Shift's palette is a milder, warmer alternative to Red — green and blue
scaled down, not zeroed.** `warmShift()` (`theme.cpp`) mirrors `redOnly()`'s
shape but keeps green at `NIGHT_SHIFT_GREEN_PCT` and blue at
`NIGHT_SHIFT_BLUE_PCT` of their day values, leaving red untouched —
`THEME_LIST` gained a fourth column for it, with the same `THEME_DERIVE`
sentinel meaning "compute from day" that the Red column uses. **Both
constants were cut twice on real-hardware feedback, and the two rounds are
worth telling apart because they were different KINDS of fix.** Round one
(60/35 → 60/15, blue only): `ACCENT` specifically — the fill on selected
chips and the tab underline — still read as a saturated blue popping out of
an otherwise warm/amber screen, so `ACCENT` got its own cut-harder override
(see below) and the blanket blue dropped with it. Round two (60/15 → 45/10,
both channels): the palette as a whole still carried too much green/blue,
not just one token, so this time the fix was tightening the blanket knob
itself — `NIGHT_SHIFT_GREEN_PCT`/`NIGHT_SHIFT_BLUE_PCT` in `theme.cpp` — 
rather than adding more per-token patches. **A single luminance ladder
cannot certify this palette the way it certifies Red's**: `redOnly()`'s
output is genuinely monochrome, so ranking by red level alone proves
distinctness; `warmShift()`'s output has all three channels live, so two
colours can share near-identical luminance while reading as visually
distinct by hue (`WARM`/`COOL` are separated mainly by the red channel,
which passes through unscaled, despite landing under 18 luminance points
apart), or vice versa. The values were computed by a throwaway script, never
hand-typed, and verified per pair by both luminance gap and per-channel
delta — see the Shift ladder in `theme.h`'s comment block for the numbers
and the pair (`DIM`/`ERROR`) that needed the most care. **`ACCENT` is the
only token still pinned to a custom ratio rather than the blanket**: at the
blanket rate alone it collapses toward `SUCCESS` (both read as a dim green),
which is the same collision `redOnly()`'s own comment already documents for
the red column — cutting the blanket further doesn't remove that need, since
it's a hue collision, not a brightness one. `ACCENT`'s override (green 15%,
blue 22%) lands a dark navy-teal roughly 40 luminance points clear of
`SUCCESS`'s now-blanket dark green, with a real green/blue channel gap on
top. Every OTHER semantic token needed no custom ratio once the blanket
itself moved low enough — they're still pinned explicitly rather than left
as `THEME_DERIVE`, so a future retune of the blanket constants can't
silently drift them without a conscious recompute-and-reverify pass.
`simulator.html`'s `checkShiftDistinct()` asserts pairwise uniqueness plus a
per-channel delta on the same `MUST_DIFFER` pairs, rather than porting the
Red ladder's single-axis metric.

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

**The backlight PWM is 25 kHz because 5 kHz was AUDIBLE on this unit** — a
constant high-pitched whine from the panel's backlight drive circuit, measured
by ear rather than derived. `BL_PWM_HZ`/`BL_PWM_BITS` (`config.h`) replaced the
bare `5000, 8` literals at the `ledcSetup()` call, and a `static_assert` in
`main.cpp` now enforces `BL_PWM_HZ << BL_PWM_BITS <= 80 MHz`, since exceeding
the LEDC source clock fails at *runtime* with a dead backlight. Four things
worth keeping:

- **What identified it was that 100% is SILENT and the other four steps are
  not.** `esp32-hal-ledc.c:90-93` promotes a duty of exactly `(1 << bits) - 1`
  to `(1 << bits)`, so `BRI_DUTY`'s 255 becomes a constant DC high with no edges
  at all, while 3/64/128/191 switch 5000 times a second. The top step never
  drove the circuit, so it never sang. That asymmetry is the diagnostic — a
  whine at every step *including* 100% is not this.
- **It was not a regression, and nothing on the branch caused it.** `git log
  -S'ledcSetup' --all` returns only the baseline commit; the frequency never
  moved. It became audible when brightness became a setting: the fixed
  `BL_DUTY 230` it replaced sat near DC where switching energy is small, and
  `BRI_DEFAULT`'s 128 is 50% duty — the loudest point on the curve. Don't go
  looking in the render or refactor history for this one.
- **Frequency and resolution move against each other**, so "just add
  resolution" is not free: at 8 bits the ceiling is 80 MHz / 256 = 312.5 kHz,
  but at 12 bits it is 19.5 kHz, i.e. back inside the audible band. 25 kHz is
  deliberately above the ~20 kHz some people still hear. **Do not lower it.**
- **The bottom step is the fragile one.** Duty 3/256 at 25 kHz is a ~0.47 us
  pulse against 2.3 us at 5 kHz. If the 1% step ever comes up dark or unstable,
  raise `BRI_DUTY_LIST`'s first entry — not the frequency.

`ledcSetup()` returns the frequency it actually *achieved*, not the one it was
asked for, so `setup()` logs both (`backlight pwm: asked .. got ..`). A clamped
value shows up in the serial log instead of being re-diagnosed by ear.

There is also a whine while *flashing*, and that one is not fixable here: in
download mode the chip is in the bootloader, LEDC is not running and GPIO 21 is
high-impedance, so no firmware constant governs the pin.

**Every accepted tap plays a "tock"** through the speaker connector (GPIO 26 →
the board's SC8002B amp), from `beep()` at the top of `dispatchHit()`. It is a
damped **sine** at 420 Hz plus a faster-dying overtone at 2.76× (a wood-block
partial), synthesised sample by sample into **GPIO 26's DAC** (DAC2) at 16 kHz.
It took three tries, and the two failures are the thing to keep:
- **A flat 2 kHz square wave was shrill** — that band is where the ear is most
  sensitive, and a constant pitch with a hard stop is what reads as a beep.
- **An octave falling 520 → 260 Hz sounded like a duck quack.** The downward
  glide is most of a quack, and doing volume by LEDC duty meant a narrow pulse,
  rich in harmonics, which is nasal. Don't reintroduce a pitch sweep or go back
  to LEDC for this: a square wave cannot sound like a knock, and the DAC's real
  amplitude is also what makes volume honest.

Four things about the implementation:
- It **blocks for `TOCK_MS`** (35 ms) on purpose, since a start-here/finish-in-
  `loop()` sound would stall mid-note for the whole blocking HTTP call behind a
  device tap, and a timer ISR or I2S DMA would be a second thread of execution.
- Samples go out through **`dac_output_voltage()`, not `dacWrite()`** — the
  Arduino wrapper re-runs pad/RTC-GPIO init on every call. Timing is a busy-wait
  on an absolute per-sample deadline, and each tock logs `tock: amp=.. 560
  samples in N us`; N should be ~35000 (measured 35007), and much more means
  the per-sample maths is overrunning and the pitch has gone flat.
- The DAC **idles at `DAC_MID` (128)** and is **ramped there over ~200 ms at
  boot** rather than jumped, since the amp input is AC coupled and a 0 → 1.65 V
  step would pop the speaker on every boot.
- A **miss is silent**, so the AC setpoint readout's dead cell still says
  "nothing happened".

**Volume is a Settings row** (`SET_ROW_VOL`, 4 chips: 0% mute / 33% / 67% /
100%), persisted as `s.vol`. Three things about it:
- **Volume is the tock's peak AMPLITUDE** in DAC steps either side of
  `DAC_MID`: `VOL_AMP_LIST` `{0, 16, 50, 127}`, picked for roughly even
  *loudness* (~-18 / -8 / 0 dB) rather than even steps. 127 is the full swing,
  and a `static_assert` stops it clipping.
- **A Volume chip beeps AFTER acting, every other tap before.** `dispatchHit()`
  skips it and `doSetting()` beeps once the new level is set, so the beep is a
  preview of the level just picked — and tapping the already-selected chip still
  beeps, since that is how you hear it. Mute skips the tone *and* the 30 ms stall.
- **The speaker icon carries the level** (a cross at mute, then 1–3 waves), so
  it is not chrome: it repaints with the level label on `setSnap.volShown`.

**It cost the toggles their captions.** The page is still exactly 4 rows —
chosen on request over re-cutting it into five shorter ones, which would have
shrunk every target on it — so Night schedule and Flip screen now share row 3
as two 150px cards titled "Schedule" / "Flip", and "23:45 - 08:00" /
"Rotate 180 degrees" are gone. `Hit::sub` on `SET_ROW_TGL` says which toggle.

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

**That memo's sentinel must be a value no duty can equal, hence `int16_t last =
-1` and not a `uint8_t`.** It was seeded `0xFF`, which is also `BRI_DUTY`'s 100%
step: booting with 4/4 saved in NVS compared `255 == 255` on the very first call,
returned early, and left the channel at the 0 duty `ledcAttachPin()` starts with —
while `screenBegin()` had already driven the pin LOW. The result is a **totally
black panel with a perfectly healthy `loop()` behind it**, which reads as dead
hardware: the serial log showed Wi-Fi up, HA polling and touch sampling normally.
The other four brightness steps wrote fine, so it only appeared at 100%. If the
screen is ever black, check `ledcWrite` is actually being reached before
suspecting the panel.

Two more rendering details worth keeping:
- `textFit()` tries the role, then a shorter form ("100" for "100%"), then
  `F_MICRO`. The built-in faces are proportional too, so don't replace this with a
  hand-measured width — it self-corrects when a label changes, which matters most
  for scene names, since a scene added to the table later cannot be checked
  against a measured width. Note the fallback is *quieter* than it was: `F_MICRO`
  is 3px shorter than `F_BODY` now rather than 5, so it no longer announces itself.
- Active controls use **dark text on the cyan fill**, not white. White-on-cyan
  measures ~1.9:1 contrast; dark-on-cyan is ~9:1.
- **Chip labels are uppercase and card titles are not**, and that is a fit
  decision rather than a stylistic one: caps have no descenders, so a 13px label
  centres cleanly in a 26px chip, where a lowercase 'y' would touch its edge.
  Titles and tab labels have the vertical room, so they get sentence case, which
  reads considerably calmer at this size.
- **The splash ticks.** `screenSplashProgress()` cycles three pips under the logo
  from the Wi-Fi wait loop, because a still logo for the length of
  `WIFI_CONNECT_MS` is indistinguishable from a hung board. Still wordless, so the
  two boot states remain visually identical — see the trade documented in
  `screen.h`.

**Layout** lives entirely in the LAYOUT block of `include/config.h`. Rows are
`ROWS_Y0 + i*ROW_H`; **0 + 4 × 52 = 208 exactly = `STATUS_Y0`**, and that one IS
`static_assert`ed in `screen.cpp`. The header tiles as **219 + 60 + 41 = 320
exactly** in x, but has no assert and needs none: `TAB_STRIP_W` is *derived* as
`SCR_W - STATUS_ROOM_W - STATUS_CLK_W - TAB_X0`, so it cannot fail to sum.
Change those `#define`s together, not the arithmetic in `screen.cpp`.

**The header is anchored to the BOTTOM edge (y 208..239), not the top — moved
there on request, not discovered there.** `STATUS_Y0` (`SCR_H - STATUS_H`) is
the top edge of the band and where the divider now sits; the body occupies
`ROWS_Y0..STATUS_Y0-1` (0..207) instead of `STATUS_H..SCR_H-1`. The header's
own internal layout is mirrored top-to-bottom around its own centre rather than
just translated: the divider sits at the TOP of the band (the seam with the
body above it, not the bottom), and the tab underline seats on it from below
(`TAB_UL_Y == STATUS_DIV_Y + 1`) rather than from above — see `wTab()` in
`widgets.cpp`, which flipped which edge of its rect the bar hugs. No other
constant changed: `STATUS_H` is still 32, `TAB_W` is still 81, the three
header regions still tile the bar exactly in x.

**The header grew 22 → 32px and the rows paid 2px each for it — before it ever
moved.** That buys the tab targets a third more height in the *worst* band of
the panel (below), and it buys the header room for a full-size clock. `TAB_W`
became 81 because the widest tab label ("Settings") was 65px in FreeSans and
had to fit with air around it — a 76px cell left 68px and looked like it was
bursting. The label widths decided the cell width then, not the reverse. Two
things have since retired that reasoning without changing the number: the pill
became an underline, so the budget is the whole cell less `SP_1` a side (73px)
rather than 65px; and "Settings" is 49px in Font 2. `TAB_W` stays 81 because it
is fixed by the header tiling above.

**The tab strip is still the least accurate region of the panel — now at the
bottom bezel instead of the top one.** `CAL_INSET` is 30, applied to *every*
corner of the 4-point fit, so it interpolates y=30..209 and **extrapolates**
y=0..29 **and** y=210..239 symmetrically — resistive panels are worst near
*any* bezel, not specifically the top one, which is what makes relocating the
header a wash for accuracy rather than a regression: it needed no recalibration
and no target resize. `screenCalibVerifyScreen()` draws the tab **cells** at
their new position (`STATUS_Y0`, not y=0) for the same reason it always did — a
verify pass that skipped them would never reveal a miss there. It outlines the
full `TAB_W × TAB_TAP_H` cell, which is what `screenHitTest()` accepts — it
used to outline the old pill, which was *smaller* than the real target, so a
tap the firmware would have taken could read as a miss. (A previously-rejected
alternative was **adding** a *second*, separate bottom bar while keeping this
one at the top: that would have cost ~36px of *additional* body, dropping the
row bands to 44px, which cannot hold a two-line card. Relocating the existing
32px header costs nothing in body space, which is a different trade and is why
this move was accepted where that one wasn't.)

**Each row band holds one card inset by `CARD_DY` (2px) top and bottom**, which is
what produces the uniform 4px gutter between cards and 2px against the header and
the bottom edge.

**There are two card internals, not one — but only ONE is a device card now.**
A STACKED card is `CARD_H` 48px as `1 pad + 18 line1 + 1 gap + 26 controls +
2 pad`, and that is the Settings brightness card only. **Every device card is
INLINE** — bulbs and the AC alike — `4 pad + 40 controls + 4 pad`, with the icon
and identity text sharing that same 40px band as a fixed column on the left.
The AC joined the bulbs here on request: it used to be the other STACKED card,
with its controls in their own 26px strip under a right-aligned state string,
visibly out of step with the bulb rows above and below it. Going inline is what
let bulb controls grow from 26px to 40px, and it is why the bulb card's state
line is gone (see "The bulb card has no state text" in `config.h`) — the AC
paid the same price for the same reason, moving its room reading off that state
line. It first landed as two extra lines stacked under the AC's name
(`AC_ID_TEMP_CY`/`AC_ID_HUM_CY`), then moved again — on request — to the header
(`STATUS_ROOM_W`), so it reads on every page rather than only Devices; the
AC's identity column is back to a bulb's plain single line as a result.

Devices and Settings share the row grid via `rowTop(slot)` / `cardTop(slot)`;
both take a **slot**, not a device index. **Every device row keeps SIX logical
slots with unchanged meanings**, even though the two kinds now lay those slots
out differently **in X only** — 4 chips + 2 swatch cells across a bulb's row, 3
chips + a 3-cell stepper across the AC's, both starting at `BULB_CTL_X0` and
sharing the same `BULB_CTL_DY..BULB_CTL_H` band vertically (they used to differ
in Y too, before the AC went inline). `doAction()`'s switch, the press-flash
sub-index and `RowSnap::btnVis` all key off the slot number, so **`btnRect()`
is the only function that knows about the (now X-only) difference**, and the
renderer, the hit test and the calibration verify screen all go through it.
That is what stops the drawn rect and the tappable rect drifting apart.

**The AC stepper reads up-left, down-right** — slot 3 (up) on the left, slot 5
(down) on the right — on request. This used to be the other way round,
deliberately, so the control read left-to-right as less-to-more; that
reasoning is retired along with the layout, not overlooked. The slot numbers
are fixed by `doAction()`, so mapping them in `btnRect()` is what buys
whichever order is wanted without touching the action layer.

**The chip pitch is no longer shared, and that is a real cost of the inline row.**
It used to be one `CHIP_W`/`CHIP_PITCH` across a device card (4) and the Settings
brightness card (5), so a control on one page was the same size as a control on the
other. There are now three widths: `BULB_CHIP_W` 43 on a bulb, `CHIP_W` 54 on the
brightness card, `ACM_W` 43 on the AC. The AC's number moved twice for two
different reasons: first 54 → 60 (pre-inline) because "COOL" needed 51px of the
52 a 54px chip left; then 60 → 43 when the AC went inline and had to fit its 3
chips + stepper into the narrower 244px left after adopting the bulb's 44px
identity column, same as `BULB_CHIP_W` had to. Landing on the *same* 43 as the
bulb chip is coincidence, not a restored shared pitch — "COOL" only needs 31px
of Font 2's, so there was room to spare either way. Each width is still fixed by
its own row tiling `CARD_IN_W` (or, for the AC and a bulb now, `CARD_IN_W -
BULB_ID_W`) exactly, so they cannot be reconciled without re-cutting a row —
don't "restore" one in isolation.

The bulb chip is the one target that got **smaller** in the axis that matters:
43px against 54px horizontally, where the drawn height went 26 → 40 but the
tappable height was always the 52px row band. The AC's mode chips paid the same
cost when it went inline. `simulator.html` checks every chip's labels still fit
(`"100%"` is 33px of the bulb chip's 35px budget; `"COOL"` is 31px of the AC
chip's 35px), but the touch cost is real and `pio run -e calib -t upload` is
the thing to reach for if taps start missing.

Scenes ignores both grids and uses its own: `SCENE_COLS` × `SCENE_TILE_W/H` on a
`SCENE_PITCH_X` / `SCENE_PITCH_Y` pitch, with `sceneTileX()`/`sceneTileY()` taking
an on-screen **slot** rather than a scene index — which scene a slot holds depends
on `S.sceneRow`. It is `static_assert`ed twice, for the two distinct failures: it
must stop at or before the AC card's row band in y (`0 + 1 × 72 + 64 = 136 ≤ 156`
= `SCENE_AC_Y0`), and at or before `SCENE_SB_X0` in x (`8 + 2 × 100 + 88 = 296 ≤
300`), because the grid, the gutter and the card each clear only their own rect.
The y one used to be an equality against `STATUS_Y0` — see the Scenes notes above
for why it is a bound now. (Two more assert that `SCENE_AC_Y0` really is the
body's last row band and that `SCENE_SB_MID` lands inside the gutter's own
range.)

## Conventions

- **Comments explain why, not what** — especially for anything carried from
  btcticker-cyd or worked around on real hardware. Preserve those notes.
- **Fail soft**: never blank a value on a failed poll. Keep the last one and dim
  it (`C_DIM`) once `DEVICE_STALE_MS` passes. A failed *service call* is
  different — it flashes the state text red via `errMs`.
- **Never let an unreachable device read as a normal state.** HA reports
  `unavailable`/`unknown`, which naively collapses to `on == false` and renders as
  a plain `OFF` — indistinguishable from a healthy bulb that is off.
  `DeviceState::avail` exists for this. The treatment is **the card's border in
  `C_ERROR`**, a red icon outline, every control greyed out because there is no
  current state to highlight, and the name in red too, being the only text left
  on the card. That last part is now true of **every** device card, AC included:
  the AC used to additionally show the word **`OFFLINE`** (not `UNAVAILABLE` —
  at 75px against the latter's 122px it let a long device name sit beside it
  without truncating, and it was the plainer word besides), but that state
  string is gone along with the AC's stacked layout, the same way the bulb
  cards lost theirs earlier — border + icon + red name now carries the alarm
  on both kinds.
- **The NAME is what loses when a card's identity does not fit.** Every device
  card now budgets it the same way: `BULB_NAME_W` is 20px, **two Font 2
  characters**, which fits the shipped ordinal bulb names (`1`/`2`/`3`) and the
  AC's default (`AC`), truncating anything longer hard. `textTrunc()` drops
  characters and appends "..", and `simulator.html` warns by name whenever it
  fires, so this cannot happen quietly (`?name=Bedside+reading+lamp`). This
  used to be a **bulb-only** rule — the AC's name lost to its live state
  instead, being static and already known to whoever installed the device,
  while the state was short, live, and the reason to look at the card at all —
  but there is no longer a competing state string on that line to lose to, so
  the AC's name now follows the same budget for the same reason (its room
  temperature and humidity moved to the header instead, and get no comparable
  truncation protection there — they are short by construction; see
  `STATUS_ROOM_W`'s budget in `config.h`, sized for the worst *realistic* case
  rather than the worst possible one). An exceptional mode (heat/fan_only/auto)
  now loses its NAME entirely rather than being squeezed into that budget — the
  icon's fallback to a plain power glyph is the only signal that mode still
  gets.
- **There is no degree glyph in either font.** `U+00B0` is outside the GFX fonts'
  0x20..0x7E charset, and a trailing "C" reads as a third digit at a glance. Both
  the setpoint and the room reading draw a 2px ring instead, positioned off
  `fontAscent()` rather than off a measured pixel.
- **Touch targets are the full 52px row band vertically**, not the drawn control
  strip, **on both pages that carry a device row** — Devices' four, and the AC
  card at the bottom of Scenes, which goes through the same `hitDeviceRow()`.
  This is used in a dark bedroom; generous targets are intentional. Scene
  tiles take their full 100 × 72 pitch, so the margins and gaps fold into the
  nearest tile rather than missing — which on Scenes means the blank band's top
  8px (y 136..143) fold up into the last tile row and only y 144..155 is
  genuinely dead, the pitch rule working rather than a bound needing tightening; a swatch's tap cell is 30px wide while its
  circle is 26px, because the cell is the target and the circle is the affordance;
  and a settings toggle is its whole HALF of the row at any x, split at the gap
  between the two cards (`TGL_SPLIT_X`) — the track is an affordance, not the
  hit area. The 32px tab strip is the one exception, and it is the
  least-used control. The scene scroll gutter is 20 × 78 per arrow (it was 104,
  before the AC card shortened the gutter to the grid's own range) — thin, but
  mostly sits in the band the 4-point fit *interpolates*, unlike the tab strip,
  which sits entirely inside whichever bezel band it extrapolates (the bottom
  one now that the header has moved there).
- **The AC setpoint is a stepper, not two buttons and a caption.** The row reads
  `[▼] 29° [▲]`, and the value sits in the grid slot *between* the two controls
  that change it — it used to be `set 29` on the row's top line beside `T+`/`T-`,
  which put the number and the arrows at opposite ends of the row. Three
  consequences: the setpoint is **gone from the identity column** (it briefly
  shared that column with the room reading `roomTempText()` drew there, which
  argued against repeating the setpoint alongside it — two numbers a step
  apart competing to be read; the room reading has since moved to the header,
  but the setpoint never moved back, since a stepper's value belongs between
  its own two controls regardless of what else is or isn't on the identity
  line); `AC_BTN_TEMP` is a **readout**,
  so it draws no button, takes no press flash, and `screenHitTest()` reports a tap
  there as a **miss** — that dead cell is also what stops a slightly-off tap from
  stepping the wrong way, which the old adjacent `T+`/`T-` pair could not; and it
  is a **text region compared by its rendered string**, like the identity column,
  not a `BtnVis` byte. It compares on `stale`/`err` too, which is why
  `RowSnap::stale`/`err` are assigned at the *end* of `drawDeviceCard()` rather than
  inside region 1's block — updating them there would consume the transition
  before the setpoint region could see it.
- **The steps are relative** and must no-op (setting `errMs`) until a real
  setpoint is known. Never guess a starting temperature. The readout shows `--`
  in the same window, so the value and the control agree about what is known.
- Active-highlight comparisons use tolerances (`PCT_MID_MIN`/`MAX` etc.) because
  HA round-trips `brightness_pct` through a 0–255 byte. Exact compares will not
  match.
