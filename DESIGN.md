# DESIGN.md — the cyd-ha design system

The specification for how this firmware looks and behaves, and why. Read this
before changing anything visual.

**How it relates to the other docs.** `README.md` is for someone using the
device. `CLAUDE.md` is the engineering memory — hard-won hardware constraints and
the traps that cost real debugging time. **This file is the design system**: the
tokens, the components, the grids, the rules, and the reasoning behind each. Where
the three overlap, the token values in `include/config.h`, `src/ui/theme.h` and
`src/ui/gfx.h` are the source of truth and this document describes them.

---

## 1. The medium comes first

Nothing here is a style preference. Every rule below traces to a property of a
2.8" 320×240 ILI9341 panel with a resistive digitiser, driven by one blocking
Arduino task on an ESP32. The constraints, and what each one forced:

| Constraint | What it forces |
|---|---|
| **320×240 total, 208px of body** | Four row bands of 52px. There is no room for a third type size or a fourth page. |
| **RGB565, no alpha channel** | Every "translucent" fill is a precomputed blend against what is behind it (`lerp565`). This is why there are no shadows, no gradients and no glass: each would be a second read-modify-write of the panel for a purely decorative result. |
| **Every HA call blocks `loop()`** | A timeout is a UI-freeze budget. Rendering must be cheap enough that it is never the bottleneck, which is what makes dirty-region rendering structural rather than an optimisation. |
| **Dirty-region rendering** | Components are driven by a single `vis` byte, not by underlying values — so two brightness values that map to the same highlight cost nothing to "change" between. Every visual state must be *comparable*. |
| **Resistive touch, 4-point linear fit** | Targets are generous and rectangular. Accuracy degrades toward the bezel (`CAL_INSET` is 30, so y=0..31 is *extrapolated*), which is why the header is the one place we spend height on a target. |
| **GFX fonts scale by integer `textsize` only** | 9pt is the smallest proportional size available. There is no face between FreeSans 9pt and the 6×8 GLCD bitmap, so hierarchy is carried by **weight and colour tier**, not by more sizes. |
| **Used in a dark bedroom** | A night palette that emits red only, at 1% backlight. Nothing may depend on hue alone, because at night there is only one hue. |
| **A phantom tap changes a real light** | Feedback must be immediate and unambiguous; a control that looks tappable must be tappable, and one that isn't must not. |

---

## 2. Principles

Seven, in priority order. When they conflict, the earlier one wins.

1. **Never let an unreachable device read as a normal state.** The worst failure
   this UI can have is looking calm while lying. An offline bulb must not render
   as a healthy off bulb.
2. **Fail soft, never blank.** A failed poll keeps the last value and dims it. A
   failed *command* is a distinct signal (red border) from stale data (dimming).
3. **Derive, don't store.** Active scene, active chip, icon colour and the pips
   are all computed from live state every render. Nothing latches, so nothing can
   fall out of sync, and no caption can describe something a tap won't send.
4. **One tap acts, everywhere.** No drill-down, no confirmation, no mode. The
   optimistic-UI ordering (apply locally → repaint → fire the call → reconcile or
   roll back) is what makes that feel instant against 1–2 s Zigbee round trips.
5. **Colour is information, not decoration.** `ACCENT` means *selected*.
   `SUCCESS`/`WARNING`/`ERROR` appear only when there is something to say. If
   everything is highlighted, nothing is — and by extension, **selecting nothing
   is not an achievement**: see `C_NEUTRAL` below.
6. **Never signal with colour alone.** The palette collapses to a single hue at
   night, so every state that matters also changes shape, weight, fill or badge.
7. **Every element earns its pixels.** No icon exists because a row looked bare;
   no border exists that a fill could imply.

---

## 3. Colour tokens

`src/ui/theme.h`. Seventeen semantic tokens. **A page never picks a hex value** —
it asks for a role, so two components requesting the same role cannot disagree,
and a role can be retuned in one place.

The previous palette named colours after their appearance (`C_MUTED`, `C_GREEN`,
`C_RED`). Each ended up serving several unrelated jobs, so none could be changed
without changing all of them. That is the problem this table fixes.

| Token | Day | Night (5-bit red) | Role |
|---|---|---|---|
| `C_BG` | `#07090D` | 0 *(derived)* | The ground behind everything |
| `C_SURFACE` | `#16181D` | 2 *(derived)* | A card |
| `C_ELEVATED` | `#23272F` | 4 *(derived)* | A control sitting **on** a card |
| `C_BORDER` | `#2F343E` | 6 *(derived)* | A card's edge |
| `C_DIVIDER` | `#1C1F26` | 3 *(derived)* | Rules and tracks — felt, not read |
| `C_TEXT` | `#F4F6FA` | 25 | Primary text: what a thing **is** |
| `C_TEXT2` | `#A6B0C2` | 20 | Secondary: live values, chip labels |
| `C_TEXT3` | `#707A8C` | 16 | Tertiary: captions, unselected tabs |
| `C_DISABLED` | `#444C59` | 12 | No state to show |
| `C_DIM` | `#8A94A6` | 7 | A **stale** reading — distinct from disabled |
| `C_ACCENT` | `#18BCF2` | 18 *(derived)* | Selected. The only saturated colour at rest |
| `C_NEUTRAL` | `#7A828E` | 13 | Selected, but what it selected is **off** |
| `C_SUCCESS` | `#2FD97C` | 10 | Online |
| `C_WARNING` | `#F5A524` | 22 | Degraded (HA unreachable, Wi-Fi up) |
| `C_ERROR` | `#FF4D6A` | 31 | Command failed / device offline |
| `C_WARM` | `#FFB05C` | 14 | What 2202 K looks like |
| `C_COOL` | `#A6CDFF` | 27 | What 4000 K looks like |

### 3.1 `C_NEUTRAL`, and why "selected" is not one colour

The one exception to "selected means `ACCENT`", and the clearest example of a token
earning its slot. Filling the **OFF** chip with cyan made the *quietest* state on
the Devices page the *loudest* mark on it — and with three bulbs off, three of the
four cards lit up like something was happening. The accent was announcing an
absence.

`C_NEUTRAL` is the same treatment in grey: still a solid fill with dark text, so it
unambiguously reads as selected, but it claims nothing. Verified separation —
against `C_ELEVATED` (the unselected chip fill) it is 9 night-steps and ~2.75:1
day luminance clear, and against `C_ACCENT` 5 night-steps.

Two things about it are deliberate:

- **It is a `vis` state, not a colour the caller passes in.** `BV_ACTIVE_OFF` goes
  through `ctlColour()` like everything else, so the rule stays in one table:
  *every selected control in this UI is a solid fill with dark text, and only which
  fill depends on what was selected.* A caller that could pass a colour would be a
  caller that could break that.
- **It is a fill role, not a text one** — which is why it is its own token rather
  than `C_TEXT3` borrowed. The two are then free to move independently, exactly as
  `SURFACE` and `ELEVATED` are.

Its night level (13) sits one step from `DISABLED` (12) and `WARM` (14), and the low
half of the ladder has no free level with two clear steps below `ACCENT`. That trade
is made on **role separation** rather than on hoping nobody looks: `NEUTRAL` is only
ever a chip fill, `DISABLED` is only ever a *label* on a `SURFACE` fill (so a
disabled chip differs from a selected one by its entire fill, not by 1/31 of red),
and `WARM` is only ever a swatch disc. The ladder ranks values; those three never
appear as the same *kind* of mark.

### 3.2 Pinned background

**`C_BG` is pinned to `0x0041` and must not move.** `src/ui/logo_ha.h` is
generated and bakes that exact value as the splash mark's background, which is
what lets the logo blit as an opaque rectangle with no transparency handling.
Change it and the boot splash grows a visible 96×96 box. Regenerate the logo
first (see `CLAUDE.md`).

### 3.3 Night mode

`themeSetNight()` maps the whole palette to red only — luminance pushed into the
red channel, green and blue zeroed. Red at 1% backlight does not wake anyone or
wreck dark adaptation.

Pure luminance is **not** sufficient, which is why the third column above exists.
Left to derive, `C_ERROR` and `C_DIM` land on the *identical* value — and a device
card picks between them on the very same string (red = failed call / `OFFLINE`,
dim = stale poll). `C_SUCCESS`/`C_ACCENT` collide too, `C_WARM`/`C_COOL` land 2 of
31 steps apart, and the four text tiers derive into a 15..21 huddle that destroys
the hierarchy they exist to express.

So: structural darks derive, every semantic colour is pinned. Measured ladder,
all seventeen distinct:

```
BG 0 < SURFACE 2 < DIVIDER 3 < ELEVATED 4 < BORDER 6 < DIM 7 < SUCCESS 10
     < DISABLED 12 < NEUTRAL 13 < WARM 14 < TEXT3 16 < ACCENT 18 < TEXT2 20
     < WARNING 22 < TEXT 25 < COOL 27 < ERROR 31
```

The `SURFACE`/`DIVIDER`/`ELEVATED` trio sits one step apart, and that is intended
rather than crowded: night mode exists to emit as little light as possible, so the
card, its rules and the controls on it all collapse toward black and the page is
carried by text and `ACCENT` alone. What must not collapse is any pair a reader has
to **tell apart**, and every such pair clears it comfortably — measured:

| Pair | Steps |
|---|---|
| `DIM` / `ERROR` | 24 |
| `WARM` / `COOL` | 13 |
| `NEUTRAL` / `SURFACE` | 11 |
| `NEUTRAL` / `ELEVATED` | 9 |
| `SUCCESS` / `ACCENT` | 8 |
| `TEXT` / `TEXT2` | 5 |
| `NEUTRAL` / `ACCENT` | 5 |
| `TEXT2` / `TEXT3`, `TEXT3` / `DISABLED` | 4 |

`simulator.html` asserts both properties — no collisions, plus a minimum separation
across an explicit `MUST_DIFFER` list. The one-step neighbours that remain
(`DISABLED`/`NEUTRAL`/`WARM`) are justified on role separation in §3.1.

A palette swap changes no *value* that a dirty-region compare looks at, so
`screenSetNight()` must `fillScreen()` + `screenInvalidate()` explicitly.

---

## 4. Typography

`src/ui/gfx.h`. **Four roles over two faces.** The build already carried
`LOAD_GFXFF`, so this uses TFT_eSPI's bundled proportional FreeSans rather than
its bitmap fonts. The old UI set device names in the 6×8 GLCD font, which made the
most important string on each row the least legible thing on the panel.

Roles, not sizes — a component asks for the job the text is doing:

| Role | Face | Ascent | Descent | Cap | Used for |
|---|---|---|---|---|---|
| `F_NUM` | FreeSansBold 12pt | 17 | 6 | 18 | The AC setpoint, alone. The one number being adjusted, and deliberately the largest thing in the body |
| `F_TITLE` | FreeSansBold 9pt | 13 | 5 | 13 | Card titles, scene names, the clock |
| `F_BODY` | FreeSans 9pt | 13 | 5 | 13 | Live state, chip labels, tab labels, captions |
| `F_MICRO` | GLCD 6×8 | 8 | 0 | 8 | Last-resort fit only, never a first choice |

**Ascent and descent are measured, not guessed** — `max(-yOffset)` and
`max(height + yOffset)` over the 0x20..0x7E charset, which is exactly what
TFT_eSPI's `glyph_ab`/`glyph_bb` compute at `setFreeFont()` time. Every row height
in `config.h` is derived from these numbers. If a face is swapped, re-extract them
(and the simulator's advance tables) rather than adjusting by eye.

### 4.1 Rules

- **TFT_eSPI centres a free font on its ASCENT** with an `M*` datum. So `cy`
  positions caps and digits exactly and lets descenders hang below — which is why
  every text line budgets `fontDescent()` beneath it.
- **Hierarchy is weight and colour tier, never a third size.** At 320×240 a third
  size reads as a ransom note. A settings caption is therefore the same *size* as
  its title, separated by weight (`F_TITLE` vs `F_BODY`) and tier (`C_TEXT` vs
  `C_TEXT3`). That is a consequence of the integer-`textsize` constraint, not an
  oversight.
- **Chip labels are uppercase; titles and tabs are sentence case.** A fit
  decision: caps have no descenders, so a 13px label centres cleanly in a 22px
  chip where a lowercase 'y' would touch the edge. Titles and tabs have the
  vertical room, and sentence case reads considerably calmer at this size.
- **Text degrades, it never overflows.** `textFit()` tries the role, then a
  shorter form ("100" for "100%"), then `F_MICRO`. `textTrunc()` drops characters
  and appends "..". FreeSans is proportional, so neither may be replaced with a
  hand-measured width — they self-correct when a label changes, which matters most
  for scene names, since a scene added later cannot be checked against a measured
  width.
- **Name the faces in `gfx.cpp` and nowhere else.** `TFT_eSPI.h` pulls in
  `gfxfont.h`, which declares all 48 bundled fonts *with internal linkage* in every
  translation unit that sees it. An explicit `#include <Fonts/GFXFF/...>` is
  therefore a **redefinition error**, and merely referencing a face from a second
  `.cpp` puts a second copy of its glyph bitmaps in flash. Verified: three faces,
  one copy each, **7.85 KB** total (`nm --print-size | grep FreeSans` → all `_ZL…`).

### 4.2 Measured widths that decided a layout

| String | Role | Width | Consequence |
|---|---|---|---|
| `"Settings"` | `F_BODY` | 65 | Set `TAB_W` to 81 — a 76px cell leaves 68px and the pill looks like it is bursting |
| `"23:45"` | `F_TITLE` | 44 | `STATUS_CLK_W` 51 with a 6px right margin |
| `"COOL"` | `F_BODY` | 51 | Set `ACM_W` to 60. The one place a label decided a width |
| `"OFFLINE"` vs `"UNAVAILABLE"` | `F_BODY` | 75 vs 122 | Chose the shorter word so a 14-char name is not truncated |
| `"TRADFRI BULB 1"` | `F_TITLE` | 151 | Fits every default state; longer names truncate |
| `"100%  4000K"` | `F_BODY` | 108 | The tightest bulb state: leaves exactly 151px, the full name |

---

## 5. Spacing

Five values. Every margin, gap and pad in the UI is one of them, which is what
makes the layout read as deliberate rather than nudged. **Something needing a
sixth value is a sign the layout is wrong, not that the scale is.**

```
SP_1  4     control gaps, card inset
SP_2  8     screen margins, card padding, label padding
SP_3  12    scene grid columns
SP_4  16    splash pip spacing
SP_6  24    scrollbar track inset
```

## 6. Corner radii

Two steps and a pill, applied by role and never mixed within one:

```
R_SM  6      chips, small controls sitting on a surface
R_LG  8      cards, scene tiles — anything that is a container
pill  h/2    toggles, tab chips — anything whose height defines its shape
```

## 7. Iconography

`src/ui/icons.cpp`. **Drawn from primitives, never stored.** Three reasons, and
the first is not thrift:

1. **They recolour for free.** Night mode swaps the palette at runtime, so a baked
   RGB565 sprite would render in day colours over a red-only UI — the exact
   problem `themeMap()` exists to paper over for the one bitmap that *is* baked
   (the splash logo).
2. **No generator.** `logo_ha.h` needs a browser in the loop to regenerate; an
   icon that is nine `drawFastVLine` calls needs nothing.
3. **Consistency by construction.** Every glyph is built on the same grid with the
   same stroke, so optical size and weight cannot drift.

**The grid:** every icon centres on `(cx, cy)`, fits a 15×15 box, and never draws
outside it. Callers reserve one size for all of them (`CARD_ICO_R` = 7). Stroke is
1px throughout, except where a shape is solid by nature (the droplet, the
chevrons).

**Every icon is used, and every one carries state.**

| Icon | Carries |
|---|---|
| `icoBulb` | A bulb's live state — filled in its real colour temperature, or a hollow outline when off |
| `icoSnow` / `icoDrop` / `icoPower` | The AC's live mode: cooling, drying, off-or-unusual |
| `icoSun` / `icoMoon` / `icoClock` / `icoRotate` | The four settings, which are otherwise four identical rows of text and a toggle |
| `icoWifi` | The entire connectivity readout, 0 or 3 bars |
| `icoChevron` | Direction, in the setpoint stepper and the scroll gutter |
| `icoBadge` | An overlay dot on another glyph — the HA-down badge |

`icoMoon` takes the colour **behind** it: a crescent is a disc minus a disc, and
with no alpha the bite must be painted in the card's own fill.

---

## 8. Components

`src/ui/widgets.cpp`. One `vis` byte in, pixels out. The state set is closed and
**every component honours all of it**:

| State | Meaning |
|---|---|
| `BV_INACTIVE` | Available, not selected |
| `BV_ACTIVE` | Selected / current state |
| `BV_ACTIVE_OFF` | Selected, and what it selected is **off** — see §3.1 |
| `BV_PRESSED` | Held — a tactility flash, not a state |
| `BV_DISABLED` | Unavailable: there is no state, so none is implied |
| `BV_ERR` | The last command on this surface failed |

`BV_INACTIVE` is 0 so a `memset` of a snapshot means "inactive" — which is why
every snapshot also carries a `valid` flag, or a cleared one would read as
"already drawn".

**New states go on the END of the enum.** The raw ordinal is what every page's
snapshot stores and compares against next frame's, so renumbering is safe within a
build but the value is what a dirty-region compare sees. Inserting in the middle
would silently change what "unchanged" means across a partial rebuild.

### 8.1 The state → colour table

Resolved once, in `ctlColour()`. **Never branch on `vis` locally** — a component
that rolled its own is how a pressed chip and a pressed chevron end up looking
like different interactions.

| State | Fill | Edge | Foreground |
|---|---|---|---|
| `BV_INACTIVE` | `C_ELEVATED` | *= fill* | `C_TEXT2` |
| `BV_ACTIVE` | `C_ACCENT` | *= fill* | `C_BG` |
| `BV_ACTIVE_OFF` | `C_NEUTRAL` | *= fill* | `C_BG` |
| `BV_PRESSED` | `C_TEXT` | *= fill* | `C_BG` |
| `BV_DISABLED` | `C_SURFACE` | *= fill* | `C_DISABLED` |
| `BV_ERR` | `C_SURFACE` | `C_ERROR` | `C_ERROR` |

Note the shape of it: **every selected state is a solid fill with dark text, and
only *which* fill depends on what was selected.** That invariant is why
`BV_ACTIVE_OFF` is a state rather than a colour parameter.

Four decisions in that table:

- **An inactive control has no border** (`edge == fill`). The old page outlined all
  23 controls at once. Deleting those outlines — not changing any colour — is the
  single largest reduction in visual noise in this redesign. An unselected control
  is legible from its fill against the card and does not need a box.
- **`BV_DISABLED` recedes *into* the card** rather than greying out on top of it.
  There is no state to show, so the control must not look like it is showing one.
- **`BV_ACTIVE` is dark text on the accent, never white.** White-on-cyan measures
  ~1.9:1 contrast; dark-on-cyan is ~9:1.
- **`BV_PRESSED` is a full-brightness fill**, deliberately the loudest thing the
  UI ever draws, because it must land within `PRESS_FLASH_MS` (120 ms) and before
  HA has answered. It is the only such fill in the system.

### 8.2 The components

| Component | Notes |
|---|---|
| `wCard` | The container everything sits in. Replaced hairline-separated rows: a surface groups its contents without drawing a line, and gives text something to be legible against. |
| `wChip` | The workhorse control. `SP_1` padding each side; the *label budget*, not the chip width, decides whether a caption fits. |
| `wStepBtn` | One end of a stepper. **No `BV_ACTIVE` case on purpose** — a step is momentary, so it is never "the current state". |
| `wSwatch` | A colour-temperature control. Carries no label because the control **is** its value. Selection is a jump from a tinted disc to a saturated one **plus a halo** — a luminance change, not a hue change, so it survives night mode. Disabled is hollow: this bulb has no colour axis at all, so showing a colour would claim an option that does not exist. |
| `wToggle` | A settings toggle. **No press flash** — the flip *is* the feedback, and it is immediate because nothing here touches the network. |
| `wValue` | A large numeric readout. **Not a button and not a tap target.** |
| `wPill` | A navigation chip. Only the selected one draws a pill. |
| `wPip` | One scene pip. An off bulb is a **ring, not a dim disc**: a disc dark enough to read as "off" is also dark enough to be invisible across a dark room. |

**There is no degree glyph in either font.** `U+00B0` is outside the GFX fonts'
0x20..0x7E charset, and a trailing "C" reads as a third digit at a glance. Both the
setpoint and the room reading draw a 2px ring instead, positioned off
`fontAscent()` rather than off a measured pixel.

**Why `wPill` has no track behind it.** An iOS-style segmented control wants a
continuous rounded track, and it cannot be repainted per-cell: `fillRoundRect`
omits its corner pixels, so a first-or-last cell's rounded end would bleed into
the neighbouring cell's fill on a partial repaint. Selected-pill-only reads as one
control and is safe under dirty-region rendering. The same reasoning is why chip
groups are separate rounded chips with `SP_1` gaps rather than a divided track.

---

## 9. Layout

All geometry lives in the LAYOUT block of `include/config.h`, and
`simulator.html` mirrors that file. **Geometry is deliberately not in `theme.h`
with the colours**: the simulator mirrors `config.h` and nothing else, so putting
sizes next to colours would give the layout two sources of truth — the exact
failure that file exists to catch.

Nothing in `screen.cpp` computes a position from a literal. Change the `#define`s
together, never the arithmetic.

### 9.1 Header — y 0..31

Three regions that **tile the bar exactly**. A gap leaves pixels nothing ever
clears; an overlap is just as bad, since each region only clears its own rect.

```
26 + 3 × 81 + 51 = 320
│    │          └── clock,    x 269..319
│    └───────────── tabs,     x  26..268
└────────────────── wifi,     x   0..25
divider at y=31; every region clears 31px tall, so the rule survives and is painted once
```

32px, up from 22. The rows paid 2px each, which buys the tab targets a third more
height in the *worst* band of the panel and buys the header room for a real 9pt
clock. Tab pill: inset `SP_1`, 24px tall, `r = 12`.

**A bottom tab bar was considered and rejected.** It would fix the accuracy
problem outright (the accurate, *interpolated* band), but costs ~36px of body,
dropping the row bands to 44px — and 44px cannot hold a two-line card. The header
keeps the tabs.

### 9.2 Body — y 32..239

```
32 + 4 × 52 = 240        four row bands, tiling exactly
```

Each band holds one **card** inset by `CARD_DY` (3) top and bottom, which produces
the uniform 6px gutter between cards and 3px against the header and the bottom
edge.

```
card    x   8..311  (304 wide), h 46, r = R_LG
content x  16..303  (288 wide)          ← CARD_IN_X0 .. CARD_IN_X1

card internals, from the card's top:
  3  pad
  2..20   line 1 dirty rect   (identity + live state, datum at cy = 9)
  21..42  control row         (CTL_DY 21, CTL_H 22)
  3  pad
```

Two subtle rules, both load-bearing:

- **Every dirty rect inside a card starts at `CARD_IN_X0`, never `CARD_X`.**
  Filling x 8..15 would paint the `R_LG` corner arcs with the surface colour and
  square the card's corners off, one repaint at a time.
- **Line 1's dirty rect runs to y+20, past the 13px ascent box its datum
  centres on**, because it must cover descenders. Clear only the ascent box and
  renaming "Reading lamp" to "Lamp" leaves the g's tail on the card forever.
  `CARD_L1_CY` is 9 and not 10 for the mirror-image reason — at 10 those
  descenders reach into the control row. Device names come from `secrets.h` and
  cannot be assumed to be the all-caps they happen to be today.

### 9.3 Control grids

**Both device kinds keep six logical slots with unchanged meanings**, even though
they lay those slots out differently. `doAction()`'s switch, the press-flash
sub-index and `RowSnap::btnVis` all key off the slot number, so **`btnRect()` is
the only function that knows about the difference** — and the renderer, the hit
test and the calibration verify screen all go through it. That is what stops the
drawn rect and the tappable rect drifting apart.

```
bulb   [ OFF ][ 1% ][ 30% ][ 100% ]    ( ◉ )( ○ )
       slots 0..3 chips                 slots 4,5 swatch cells
       4 × 54 + 3 × 4 = 228             2 × 30 = 60
       x 16..243                        x 244..303        → 288 exactly

AC     [  OFF  ][ COOL  ][  DRY  ]  [▼]  24°  [▲]
       slots 0..2, 3 × 60 + 2 × 4 = 188   slots 5, 4, 3
       x 16..203                          28 + 44 + 28 = 100, x 204..303

settings brightness — the SAME chip pitch as a device card
       5 × 54 + 4 × 4 = 286, x 16..301
```

One deliberate inversion: **the AC stepper's slots run backwards against x** —
slot 5 (down) on the left, slot 3 (up) on the right — so the control reads
left-to-right as less-to-more. The slot numbers are fixed by `doAction()`, so
mapping them in `btnRect()` buys the natural order without touching the action
layer.

The chip pitch is shared between a device card (4) and the brightness card (5), so
a control on one page is the same size as a control on the other. The AC's mode
chips are the one exception at 60px, because `"COOL"` needs 51px of the 52 that
leaves.

### 9.4 Scenes grid

```
3 columns × 3 rows of 88 × 64 tiles
x:  8 + 2 × 100 + 88 = 296  ≤ 300 (gutter)      gaps 12
y: 32 + 2 ×  72 + 64 = 240  exactly              gaps 8
gutter x 300..319, split at y 136
```

`static_assert`ed **twice**, for two distinct failures: exact in y, because a
leftover band below the last row would hold whatever the previous page left there;
and stopping before the gutter in x, because the grid and the gutter each clear
only their own rect, so an overlap is a permanently wrong pixel.

**Three columns, not the four it used to be.** A 66px square tile could hold the
pips and a name only in the fallback font. 88px holds the name at full size, which
is what a scene tile is *for* — nine legible tiles per page beat twelve illegible
ones, and the page still scales past a hundred. This is also why the gaps differ
per axis: the vertical budget is fixed at 208px, and `2 × PITCH_Y + TILE_H == 208`
has exactly one solution keeping tiles above 60px, and it is 8.

Tile contents are vertically centred: pips span y+15..23, the name's ascent box
spans y+36..49, so content runs 15..49 — centre 32, exactly half of 64.

---

## 10. Information hierarchy

Applied consistently on every surface, in this order:

1. **What is this?** — card title, `F_TITLE`, `C_TEXT`
2. **What is it doing right now?** — the status icon (fastest to read) and the
   live state value, `F_BODY`, `C_TEXT2`
3. **What can I do?** — the control row, `C_ELEVATED` chips with one `C_ACCENT`
4. **Supporting detail** — captions, `C_TEXT3`
5. **Chrome** — connectivity, clock, navigation; quiet unless something is wrong

The icon is placed first in reading order deliberately: it is the only element
that can be read without reading, so it carries the answer to (2) before the eye
reaches any text.

---

## 11. Screen-by-screen review

### 11.1 Header (was: status bar)

**Before.** 22px. `● WIFI ● HA` in 88px on the left, three ALL-CAPS tab labels in
the 6×8 GLCD font, a clock on the right.

**Problems.**
- 88px — 27% of the bar's width — was permanent debug chrome telling a healthy
  system it was healthy. Two labelled dots is a diagnostic readout, not product
  chrome.
- The tab band was 24px, the thinnest target in the firmware, in the *least
  accurate* region of the panel.
- ALL-CAPS 6px labels: shouty and hard to read at a glance.

**After.** 32px. One 26px connectivity glyph; three sentence-case tab labels in
`F_BODY` in 81px pills; a `F_TITLE` clock.

**The connectivity glyph** is the whole readout in 26px:

| State | Rendering |
|---|---|
| Everything reachable | 3 bars in `C_TEXT3` — present, unobtrusive, reassuring |
| HA not answering | 3 bars **plus an amber badge** |
| Wi-Fi down | **0 bars**, all in `C_ERROR` |

Colour *and* a shape/badge change every time, never colour alone. The `HA` label's
diagnostic value is not lost — it is in the serial log.

**The clock is exactly 5 characters, and that is a hard rule.** `"23:45"` is 44px
in `F_TITLE` against the 45px its region leaves after a 6px margin, so anything
wider paints into tab 2's cell — which only repaints on a page change, making the
overflow permanent. The region is also **minute-rate by design**: it replaced a
freshness readout counting seconds since the last poll, and since every poll resets
that timestamp the number oscillated 0↔1 and repainted the bar several times a
second. Connection health belongs to the glyph and staleness to the card dimming;
**do not put a per-second value here.**

### 11.2 Devices

**Before.** Four rows separated by hairlines. Each: a 6×8-font name and state on a
10px line, then six 48×36 outlined buttons.

**Problems.**
- **Hierarchy inverted.** The device name — the most important string in the row —
  was set in the smallest, blockiest font on the panel.
- **23 outlined boxes at once.** Every control carried a border and a fill, so
  nothing had visual priority and the page read as an engineering console.
- **No grouping.** A hairline separates; it does not group. Nothing said "these six
  controls belong to that name".
- **No at-a-glance state.** Reading whether a bulb was on required reading text.
- Colour swatches labelled `2202K`/`4000K` in 6px type — unreadable, and the label
  duplicated what the colour already said.
- The AC's `set 24` sat on the row's top line while `T+`/`T-` sat at the far end of
  the button strip: the number and the controls that change it were at opposite
  ends of the row.

**After.** Four cards. Line 1: status icon + name (`F_TITLE`) + live state
(`F_BODY`, right-aligned). Line 2: borderless chips plus two circular swatches, or
three mode chips plus a stepper.

**The status icon is the highest-value pixel on the page**, and it is derived, not
a label:

- A bulb renders **filled in its actual colour temperature** — interpolated across
  the range, not bucketed like the swatches, because the icon is a *readout* and a
  bulb sitting at 3000 K from the phone should look like 3000 K — **blended by its
  actual brightness** toward the card fill (1% still lands at 70/255, visible).
- Off is a hollow outline; offline is a hollow outline in `C_ERROR`.
- The AC shows a snowflake, a droplet, or a power symbol.

So the icon column is a scannable strip of what the room is doing. Two rules it
obeys: it **dims with its own card** when the poll goes stale (the same fail-soft
rule the text follows — dimming one and not the other left half the card claiming
to be current), and an **offline device stays red rather than dimmed**, because red
outranks stale.

**The OFF chip is grey, not cyan** (`BV_ACTIVE_OFF` / `C_NEUTRAL`, §3.1). This was
caught only once the page was on real hardware with real state: three of four cards
had an off device, so three cyan OFF chips lit the page up as though something were
happening. Selecting *nothing* had become the most emphatic mark on the screen.
Grey still reads as selected without making that claim.

**Why the state line stays even though the chips show the presets.** The chips
cover three levels. A bulb set to 47% from the phone lights *no* chip, and the
state line is the only thing that stops that looking like a fault. This is not
hypothetical — the live device reported `on,76,2202` on the first boot after
flashing, so the very first render exercised it.

**Why the AC's state line shows the room, not the mode.** The chips select the mode
and the icon shows it; repeating it would waste the line. What nothing else on the
card can show is the room reading. A mode *outside* the three chips (heat,
fan_only, auto — set from the HA app) **is** named there, because otherwise the
card would show no active chip and no explanation.

**When line 1 does not fit, the NAME loses.** The state is short, live, and the
reason to look at the card at all; a name is static and already known to whoever
installed it. `textTrunc()` handles it. Two width decisions follow from this:
`OFFLINE` over `UNAVAILABLE` (75px vs 122px), and dropping the word `Room` in the
exceptional-mode case only — the number is unmistakably a temperature beside its
degree ring.

**The AC setpoint is a stepper**, `[▼] 24° [▲]`, with the value in the grid cell
*between* the two controls that change it. That cell is a **readout**: it draws no
button, takes no press flash, and `screenHitTest()` reports a tap there as a miss.
The dead cell is deliberate — it is what stops a slightly-off tap from stepping the
wrong way, which the old adjacent `T+`/`T-` pair could not. The steps are relative,
so they no-op until a real setpoint is known, and the readout shows `--` in the
same window so the value and the control agree about what is known.

**A failed command flashes the card's border**, not one word of text. The whole
surface carries the notification, and redrawing the outline alone is enough since
the fill and its contents are unchanged.

### 11.3 Scenes

**Before.** A 4-column grid of 66px square tiles, three pips and a name.

**Problems.**
- 66px minus padding left ~59px for the name. `"AWAKE"` is 68px, so real scene
  names fell back to the 6×8 font — the *design* was to show a name and the
  *result* was often not to.
- Twelve tiles at that density read as a dense grid of chips rather than a set of
  cards.

**After.** A 3-column grid of 88×64 tiles. Names fit at full `F_TITLE` size with
room to spare; every default scene name and the stress-test names clear the budget.

**The three pips are kept, refined, and remain derived.** Position = which bulb,
ring = off, colour = warm/cool, fill intensity = level. Being read off `SCENE[]`
itself they cannot describe a scene the tap won't send — which a hand-written
caption could, and nearly did, reading `2200K` while `KELVIN_WARM` was corrected to
the bulbs' real 2202 K limit.

**The active scene is derived, never latched.** `sceneActive()` compares live state
against the table every render, which is what makes overriding one bulb on the
Devices page deselect the scene and a change from the HA app select the matching
one, with no "current scene" variable to fall out of sync. An **unknown kelvin is
"cannot confirm", not "match"** — treating it as a match lights AWAKE and DAY
simultaneously, since they differ only there, and two active tiles reads as a bug.

**The page is built to scale and `SCENE[]` is the only place the count lives.** The
snapshot is per visible **tile** (9 bytes), so 100 scenes cost the same RAM as 5.
The scroll affordance compiles out entirely at five scenes. The arrows **page**
rather than step: at three visible rows, stepping needs 32 taps to cross 100
scenes where paging needs 11.

### 11.4 Settings

**Before.** Four hairline-separated rows: a micro-label above five 58px segments,
then three rows of title + caption + toggle.

**Problems.**
- Four rows of near-identical text with no way to tell them apart at a glance.
- The brightness row's chosen level was shown only by which segment was
  highlighted — the page never said "50%" anywhere.
- Same grouping problem as Devices.

**After.** Four cards on the same row grid. Each carries an icon
(sun/moon/clock/rotate), so the four are distinguishable without reading. The
brightness card **names the chosen level on its own identity line**: a segmented
control shows *which* of five is selected but not what the selection *means*, and
"50%" spelled out is the difference between a row of chips and a row of chips you
can read.

Titles are `F_TITLE`/`C_TEXT`, captions `F_BODY`/`C_TEXT3`, and each caption says
what the toggle will actually do so the control is never asking about a value the
user has to remember.

### 11.5 Splash

**Before.** The Home Assistant logo, centred, and nothing else — frozen for as
long as `WIFI_CONNECT_MS` (20 s).

**Problem.** A still logo for 20 seconds is indistinguishable from a hung board.
"Is it working?" is the one question a boot screen has to answer.

**After.** Three pips under the logo, cycling from the Wi-Fi wait loop. Costs three
`fillCircle`s at 5 Hz. **Still wordless**, so it does not undo the deliberate
decision that both boot states (connecting, and captive portal) look identical —
that trade, and its cost, is documented in `screen.h`.

---

## 12. Touch and feedback

**Targets are generous by design** — this is used in a dark bedroom, often
half-asleep. The visual control is frequently smaller than the thing you can hit:

| Target | Tappable | Drawn |
|---|---|---|
| Device chip | 54 × 52 | 54 × 22 |
| Colour swatch | 30 × 52 | 22 × 22 circle |
| Stepper chevron | 28 × 52 | 28 × 22 |
| Scene tile | 100 × 72 (full pitch) | 88 × 64 |
| Settings toggle | whole row, any x | 44 × 24 pill |
| Tab | 81 × 32 | 73 × 24 pill |
| Scroll arrow | 20 × 104 | 12 × 10 chevron |
| AC setpoint cell | **nothing** | 44 × 22 readout |

The full row band counts vertically, and the full scene pitch counts in both axes,
so margins and gaps fold into the nearest control rather than missing. A settings
toggle's pill is an affordance, not the hit area. The tab strip is the one place a
target is *not* generous relative to its neighbours, and it is the least-used
control — that is the trade.

**Feedback ordering is fixed** (`doAction()`): apply the expected state locally →
repaint → fire the blocking call → reconcile, or roll back from a saved copy and
set `errMs`. IKEA Zigbee round trips run 1–2 s; without the optimistic step every
tap would feel ignored. The 120 ms press flash covers the gap before even the
optimistic repaint is visible.

**The press flash is keyed by kind as well as index**, or a scene tap at index 2
would also invert row 2's chip on the Devices page. And because the flash expires
by *time* rather than by any state change, **every snapshot needs a per-item vis
byte** — a page that skips it leaves the tapped control inverted forever.

---

## 13. State semantics

The full matrix. Every distinction here is one a user must be able to make.

| Condition | Icon | Title | Value | Controls | Card |
|---|---|---|---|---|---|
| Normal, on | live colour | `C_TEXT` | `C_TEXT2` | one `C_ACCENT` | `C_BORDER` |
| Normal, off | `C_DISABLED` outline | `C_TEXT` | `Off`, `C_TEXT2` | OFF chip `C_NEUTRAL` | `C_BORDER` |
| No preset matches (e.g. 47%) | live colour | `C_TEXT` | `C_TEXT2` | **none active** | `C_BORDER` |
| Never polled | `C_DISABLED` | `C_TEXT` | `--` | inactive | `C_BORDER` |
| Stale poll (>20 s) | dimmed toward card | `C_DIM` | `C_DIM` | unchanged | `C_BORDER` |
| Command failed | live colour | `C_TEXT` | `C_ERROR` | unchanged | **`C_ERROR`** |
| Offline / unavailable | `C_ERROR` outline | `C_TEXT` | `OFFLINE`, `C_ERROR` | all `BV_DISABLED` | `C_BORDER` |
| No colour-temp support | live colour | `C_TEXT` | no `K` shown | swatches hollow | `C_BORDER` |

Note the deliberate separation of the three failure kinds: **stale** dims (the data
is old but probably right), **failed command** reddens the border (the data is right
but your tap did not land), **offline** reddens the value and disables the controls
(there is no data). Collapsing any two of these is what the pinned night-mode reds
exist to prevent.

---

## 14. Performance budget

| Operation | Cost | Notes |
|---|---|---|
| Steady-state render | a handful of compares | Every region returns early unless a *value* changed |
| One chip repaint | `fillRoundRect` 54×22 + label | ~1.2k px |
| Card line 1 repaint | `fillRect` 288×19 + icon + 2 strings | ~5.5k px |
| Page switch | body wipe 320×208 + 4 cards or 9 tiles | The heaviest operation, and only on a tap |
| Scene scroll | 9 tile repaints, **no body wipe** | Tile rects are fixed, so the gaps never change content |
| Palette / rotation change | full `fillScreen` + invalidate | Neither alters a compared value, so both must force it |

**Deliberately absent, and why:** shadows, gradients, glassmorphism and any
alpha-composited effect. RGB565 has no alpha, so each would be a second
read-modify-write of the panel for a decorative result — and they would break the
dirty-region model, because a soft edge means a region can no longer be cleared by
filling its own rect.

**Measured on the device after this redesign:** RAM 50,128 B (15.3%), flash
1,010,489 B (32.1%), of which the three font faces are 7.85 KB. Heap free 239,604 B
with a 235,188 B minimum, flat across a reboot and sustained running.

---

## 15. Deliberately not built

The brief's component list included several things this firmware has no use for.
Adding them would contradict both "every element serves a purpose" and this
repo's standing rule against becoming a general HA dashboard:

- **Dialogs and menus.** Every action here is one tap and immediately reversible.
  A confirmation step would make the product worse, and a menu implies navigation
  this UI does not have.
- **Charts.** There is no history in this firmware — the poller keeps one current
  value per device. A chart would need a ring buffer, a time axis and a data model
  that does not exist, to display four numbers.
- **Forms and text entry.** Nothing here is typed. Wi-Fi credentials go through
  WiFiManager's own captive portal, off-device.
- **A fifth device or a fourth page.** Out of scope by standing decision. If a
  request needs one, say so explicitly rather than quietly adding it.

The listed components that *do* have a job are all implemented: buttons, cards,
status card, device tiles, navigation, lists, progress (the discrete brightness
slider, the scroll thumb, the splash pips), sliders, toggles, badges, notifications
(the transient error border) and loading indicators.

---

## 16. How to extend it

**Do:**

- **Add a scene** — one line in `SCENE[]` in `screen.cpp`. Nothing else. No count
  to update, no snapshot to resize.
- **Retune a colour** — one row of `THEME_LIST`. Then check the night ladder in
  `simulator.html`; if the new value collides, pin an override in the third column.
- **Add a control** — reach for an existing widget. If you need a new one, put it
  in `widgets.cpp` and drive it from `ctlColour()`.
- **Add a visual state** — append to `BtnVis` (end only, see §8) and give it a row
  in `ctlColour()`. Never hand a component a colour to bypass the table.
- **Change a size** — the LAYOUT block of `config.h`, then mirror it in
  `simulator.html` and let its assertions tell you what you broke.

**Don't:**

- **Put a `C_*` name in a static or constexpr initializer.** The palette is runtime
  values because night mode recolours in place.
- **Include a GFX font header, or name a face outside `gfx.cpp`.** Redefinition
  error, or a duplicate copy of the glyph bitmaps in flash.
- **Compute a position from a literal in `screen.cpp`.** It belongs in `config.h`.
- **Start a card's dirty rect at `CARD_X`.** It squares off the corners.
- **Add a per-second value to the clock region.** It is minute-rate by design.
- **Let a page branch on `vis` itself.** Go through `ctlColour()`.
- **Give Devices or Settings a scrollbar.** They `static_assert` that their rows
  tile the body exactly, and that is the point.

---

## 17. Verification

**`simulator.html` is the primary tool**, and three things about it are
load-bearing rather than conveniences:

1. **It carries the real font metrics.** The `ADV_*` tables are the exact
   `xAdvance` values from TFT_eSPI's font headers, and `gtext()` draws character by
   character at those advances, so `textW()` is byte-identical to
   `tft.textWidth()`. That is the only reason its clipping warnings can be trusted;
   the previous version approximated with `13px system-ui`.
2. **It re-checks every `static_assert` from `screen.cpp`**, plus two things the
   compiler cannot see — that a card's line-1 descenders stop before the control
   row, and that its dirty rect covers those descenders — plus the night ladder.
3. **State is deep-linkable**: `?page=1&night=1&scenes=100&stale=1&ha=0&mode=heat`.

A headless sweep (`eval` the `<script>` against a Proxy-stubbed canvas, then drive
`page`/`set`/`dev`/`sceneStress()` and read `log.innerHTML`) covers all three pages
in both palettes, all 32 scroll offsets at 100 scenes, and every fault state in
about a second. **Current status: clean**, with one expected warning — a
deliberately 26-character device name truncating, which is the mechanism proving it
works.

**Confirmed on hardware** after flashing: settings load before `screenBegin()` (no
100% backlight flash), Wi-Fi reconnect, NTP sync at exactly UTC+7
(`local 22:17:13 / utc 15:17:13`), first HA poll succeeded, heap flat, touch noise
floor z = 53–65 against a 400 threshold, and tab taps at y = 8–10 landing in the
correct cells.

**Not verified, and worth a look on the panel:** the appearance of 1-bit FreeSans
rendering at arm's length (the simulator substitutes Helvetica — widths are exact,
glyph shapes are approximate); whether the same-size title/caption pairing on
Settings holds up on glass, given 9pt is the floor; and repaint smoothness on a tab
switch, the one heavy operation.
