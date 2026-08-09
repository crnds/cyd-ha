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
| **Three built-in bitmap faces, no bold, no in-between size** | Font 4 (18px caps), Font 2 (10px caps), GLCD (7px), and nothing between them. There is no bold at all, so hierarchy is carried by **colour tier alone** — not by weight, and not by more sizes. |
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
| `C_SURFACE` | `#16181D` | 2 *(derived)* | A filled card (Scenes, Settings) |
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
ever a chip fill, `DISABLED` is only ever a *label* on an unfilled control (so a
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

`src/ui/gfx.h`. **Four roles over TFT_eSPI's built-in BITMAP faces**, selected by
number. These are drawn pixel by pixel at one fixed size, so every stem lands on
the grid and nothing is scaled or resampled at draw time.

This replaced a build on the bundled proportional **FreeSans** GFX faces, which
were 1-bit outlines rasterised at 9pt/12pt and stair-stepped on every diagonal.
The swap was made on request, and it is a genuine trade rather than a pure win —
see 4.2.

Roles, not sizes — a component asks for the job the text is doing:

| Role | Face | Ink (rel. `cy`) | Baseline | Cap | Used for |
|---|---|---|---|---|---|
| `F_NUM` | Font 4 (26px box) | −8 … +15 | +10 | 18 | The AC setpoint, alone. The one number being adjusted, and deliberately the largest thing in the body |
| `F_TITLE` | Font 2 (16px box) | −5 … +7 | +5 | 10 | Card titles, scene names, the clock |
| `F_BODY` | Font 2 (16px box) | −5 … +7 | +5 | 10 | Live state, chip labels, tab labels, captions |
| `F_MICRO` | Font 1, GLCD 6×8 | −4 … +3 | +3 | 7 | Last-resort fit only, never a first choice |

**Ink extents are measured, not read off the font headers.** The headers give the
nominal box (Font 2: 16 tall, baseline 13; Font 4: 26 tall, baseline 19), but
every built-in glyph sits *inset* inside that box — Font 2's caps start 3 rows
down, Font 4's 1 row down — so the nominal numbers put the degree rings in the
wrong place. `python3 scripts/font_metrics.py` decodes the glyph data and prints
`gfx.cpp`'s three tables and the simulator's. Every row height in `config.h` is
derived from these; re-run the script rather than adjusting by eye.

### 4.1 Rules

- **TFT_eSPI centres a built-in font on its FULL BOX** with an `M*` datum, where
  it centred a free font on its *ascent*. The box carries blank rows the ascent
  did not, so `gfx.cpp` applies a per-role `ROLE_DY` before drawing. `F_NUM`'s
  `+4` lands Font 4's digits on the exact pixels FreeSansBold 12pt used, so the
  setpoint and its degree ring did not move at all.
- **Ask for ink, not for ascent.** `fontInkTop()`/`fontInkBottom()` replaced
  `fontAscent()`/`fontDescent()` because a built-in face's ink is not symmetric
  about its datum, so the old `cy - fontAscent()/2` only found the ink top by
  accident of FreeSans being symmetric. Both degree rings ride `fontInkTop()`.
- **Hierarchy is colour tier alone now.** The built-in set has **no bold**, so
  `F_TITLE` and `F_BODY` are literally the same face and the weight signal is
  gone. A settings caption is the same size *and weight* as its title, separated
  only by `C_TEXT` vs `C_TEXT3`. There is also nothing between Font 2 and Font 4,
  so a third size remains unavailable — same constraint as before, new numbers.
- **Chip labels are uppercase; titles and tabs are sentence case.** A fit
  decision: caps have no descenders, so the label centres cleanly in the chip
  where a lowercase 'y' would touch the edge. Titles and tabs have the vertical
  room, and sentence case reads considerably calmer at this size.
- **Text degrades, it never overflows.** `textFit()` tries the role, then a
  shorter form ("100" for "100%"), then `F_MICRO`. `textTrunc()` drops characters
  and appends "..". The built-in faces are proportional too, so neither may be
  replaced with a hand-measured width — they self-correct when a label changes,
  which matters most for scene names, since a scene added later cannot be checked
  against a measured width.
- **The faces are numbers, so nothing can duplicate them.** The old rule ("name a
  face in `gfx.cpp` and nowhere else", because `gfxfont.h` declared all 48 free
  fonts with internal linkage in every TU) is moot: `setTextFont(2)` carries no
  glyph table with it. The build keeps `LOAD_GFXFF` anyway, because
  `setTextFont()` clears `gfxFont` only under that flag — which is what guarantees
  font 1 means GLCD.

### 4.2 What the swap cost and bought

| | FreeSans (before) | Built-in bitmap (now) |
|---|---|---|
| Body cap height | 13px | **10px** — smaller |
| Bold available | yes (`F_TITLE`) | **no** — title/body differ by colour only |
| `F_MICRO` gap below body | 5px | **3px** — a fallback is less visible |
| Font data in flash | 6582 B + 1280 B GLCD | 8475 B — **+613 B** |
| Widths | wider | narrower, so every fit budget gained slack |
| Edges | stair-stepped outlines | on the pixel grid by construction |

Font 4 carries a full 96-character set for a role that only ever draws two digits
and `--`; that is where the extra flash goes, and it is the thing to trim first if
flash ever matters — not the face choice.

### 4.3 Measured widths that decided a layout

Every number here is smaller than the FreeSans value it replaced, so several of
these constraints are now slack. They are kept because they are the reason the
constant has the value it has, and because a future face could tighten them again.

| String | Role | Was | Now | Consequence |
|---|---|---|---|---|
| `"Settings"` | `F_BODY` | 65 | 49 | Set `TAB_W` to 81. Now fixed by header tiling instead |
| `"23:45"` | `F_TITLE` | 44 | 35 | `STATUS_CLK_W` 51 with a 6px right margin — was 1px of slack, now 10 |
| `"COOL"` | `F_BODY` | 51 | 31 | Set `ACM_W` to 60. Now fixed by the card's 288px tiling instead |
| `"OFFLINE"` vs `"UNAVAILABLE"` | `F_BODY` | 75 / 122 | 51 / 82 | Chose the shorter word so a 14-char name is not truncated |
| `"TRADFRI BULB 1"` | `F_TITLE` | 151 | 103 | Fits every default state with room; longer names truncate |
| `"100%  4000K"` | `F_BODY` | 108 | 85 | The tightest bulb state: leaves 183px of 268 for the name |

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

## 6. Corners

**Square, everywhere. There is no radius token.** Cards, scene tiles, chips,
steppers, the scroll thumb and the settings toggle are all plain rects, so every
surface and every control on it share one corner treatment and nothing has to
decide which step it belongs to.

This replaced a two-step-plus-pill set (`R_SM` 6 for controls, `R_LG` 8 for
containers, `h/2` for the toggle). Don't reintroduce a radius for one component:
a single rounded control on an otherwise square page reads as a rendering fault
rather than as a style.

Two consequences, both of which used to need care and no longer do:

- A partial repaint **cannot strand a corner arc**. `fillRoundRect` leaves the
  four corner pixels of its bounding box untouched, so anything that blanked a
  rounded shape had to be square on purpose — see the empty scene slot.
- A clear that reaches a card's edge column now eats a **border pixel** rather
  than an arc. Still wrong, still prevented by starting every in-card dirty rect
  at `CARD_IN_X0`; just for a simpler reason.

The one shape argued separately is the **settings toggle knob**, which went from
a disc to a square block. A circle sliding in a sharp-cornered slot is the only
mark left that would still read as rounded, and the knob's *position* — not its
outline — is what says on or off.

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
| `BV_DISABLED` | `C_BG` | *= fill* | `C_DISABLED` |
| `BV_ERR` | `C_BG` | `C_ERROR` | `C_ERROR` |

Note the shape of it: **every selected state is a solid fill with dark text, and
only *which* fill depends on what was selected.** That invariant is why
`BV_ACTIVE_OFF` is a state rather than a colour parameter.

Four decisions in that table:

- **An inactive control has no border** (`edge == fill`). The old page outlined all
  23 controls at once. Deleting those outlines — not changing any colour — is the
  single largest reduction in visual noise in this redesign. An unselected control
  is legible from its fill against the card and does not need a box.
- **`BV_DISABLED` recedes into the background** rather than greying out on top of
  the card. There is no state to show, so the control must not look like it is
  showing one. The Devices card carries no fill, so `C_BG` is what it sinks to.
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
| `wToggle` | A settings toggle: a square knob at one end of a square track. **No press flash** — the flip *is* the feedback, and it is immediate because nothing here touches the network. |
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
groups are separate chips with `SP_1` gaps rather than a divided track — and with
square corners the bleed is gone outright, so only the "one control, repainted
per cell" half of the argument still applies.

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
height in the *worst* band of the panel and buys the header room for a full-size
clock rather than a cramped one.

**A bottom tab bar was considered and rejected.** It would fix the accuracy
problem outright (the accurate, *interpolated* band), but costs ~36px of body,
dropping the row bands to 44px — and 44px cannot hold a two-line card. The header
keeps the tabs.

### 9.2 Body — y 32..239

```
32 + 4 × 52 = 240        four row bands, tiling exactly
```

Each band holds one **card** inset by `CARD_DY` (2) top and bottom, which produces
the uniform 4px gutter between cards and 2px against the header and the bottom
edge. The inset was 3 until the controls grew: those pixels moved into the
control row, so the drawn button is nearer the 52px band the hit test accepts.

There are now **two card internals**, not one. The AC card and the Settings
brightness card still stack an identity line over a control row; the three bulb
cards are **inline** — identity and controls on a single line — which is what
gives their controls the card's full height.

```
card    x   8..311  (304 wide), h 48, square corners
content x  16..303  (288 wide)          ← CARD_IN_X0 .. CARD_IN_X1

STACKED (AC, settings), from the card's top:
  1  pad
  1..19   line 1 dirty rect   (identity + live state, datum at cy = 8)
  20..45  control row         (CTL_DY 20, CTL_H 26)
  2  pad

INLINE (bulbs), from the card's top:
  4  pad
  4..43   identity column AND controls, one band  (BULB_CTL_DY 4, BULB_CTL_H 40)
  4  pad
```

Two subtle rules, both load-bearing:

- **Every dirty rect inside a card starts at `CARD_IN_X0`, never `CARD_X`.**
  Filling x 8..15 would paint over the card's own left border column and erase
  the outline, one repaint at a time. (When cards were rounded this was about the
  `R_LG` corner arcs instead; the corners went square, the rule did not.)
- **Line 1's dirty rect runs to y+19, past the 13px ascent box its datum
  centres on**, because it must cover descenders. Clear only the ascent box and
  renaming "Reading lamp" to "Lamp" leaves the g's tail on the card forever.
  `CARD_L1_CY` is 8 and not 9 for the mirror-image reason — at 9 those
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
bulb   ( i ) 1   [OFF][ 1% ][30%][100%]    ( ◉ )( ○ )
       identity  slots 0..3 chips           slots 4,5 swatch cells
       44        4 × 43 + 3 × 4 = 184       2 × 30 = 60
       x 16..59  x 60..243                  x 244..303       → 288 exactly

AC     [  OFF  ][ COOL  ][  DRY  ]  [▼]  24°  [▲]
       slots 0..2, 3 × 60 + 2 × 4 = 188   slots 5, 4, 3
       x 16..203                          28 + 44 + 28 = 100, x 204..303

settings brightness — no longer the same pitch as a bulb chip
       5 × 54 + 4 × 4 = 286, x 16..301
```

The bulb row's identity column is paid for out of the chips' width, so the
swatches did not move: `SW_X0` is 244 under either layout. A bulb chip is
**43 × 40 against the 54 × 26 it was** — +22% of drawn area, but 11px *narrower*,
and horizontal is the axis that matters for touch (the hit test already accepts
the full 52px row band vertically). That is the price of the inline row, and
there is no arrangement that keeps a 15px icon, a name, four chips and two
swatches inside 288px without paying it.

One deliberate inversion: **the AC stepper's slots run backwards against x** —
slot 5 (down) on the left, slot 3 (up) on the right — so the control reads
left-to-right as less-to-more. The slot numbers are fixed by `doAction()`, so
mapping them in `btnRect()` buys the natural order without touching the action
layer.

The chip pitch **used to be** shared between a device card (4) and the brightness
card (5), so a control on one page was the same size as a control on the other.
The inline bulb row broke that: three chip widths now coexist — 43 on a bulb, 54
on the brightness card, 60 on the AC (where `"COOL"` needed 51px of the 52 that
leaves). Worth knowing before "restoring" one of them: each is fixed by its own
row tiling the card's 288px exactly, so they cannot be reconciled without
re-cutting a row.

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

**After.** Four cards — a border on the bare background, no fill. A bulb card is
**one inline line**: status icon + name
(`F_TITLE`), then four borderless chips and two circular swatches at the card's
full height. The AC card still stacks — line 1 is icon + name + live state
(`F_BODY`, right-aligned), line 2 is three mode chips plus a stepper.

**The status icon is the highest-value pixel on the page**, and it is derived, not
a label:

- A bulb renders **filled in its actual colour temperature** — interpolated across
  the range, not bucketed like the swatches, because the icon is a *readout* and a
  bulb sitting at 3000 K from the phone should look like 3000 K — **blended by its
  actual brightness** toward the background (1% still lands at 70/255, visible).
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

**The bulb cards' state line was REMOVED, on request, when they went inline.**
It read "30%  2700K" / "Off" / "OFFLINE" and there is no width for it beside a
full row of controls. Two things it was doing had to go somewhere:

- **An off-preset value.** The chips cover three levels, so a bulb set to 47% from
  the phone lights *no* chip — and this is not hypothetical, the live device
  reported `on,76,2202` on the first boot after flashing. The icon still carries
  it (real colour temperature, blended by real brightness), but **the exact number
  is gone from the screen**: 47% and 55% now look the same, and neither lights a
  chip. That is the accepted cost, and it is the one thing to revisit first if the
  page ever feels like it is hiding something.
- **`OFFLINE`.** This was not a readout but a safety rule (§Conventions: never let
  an unreachable device read as a normal state), so it could not simply be
  dropped. It moved to **the card's border**, which already flashed `C_ERROR` for a
  failed service call and now *holds* it for an unreachable device. An offline
  bulb is a red border around a red icon and a red name with all six controls
  greyed — louder than the word was, and it costs no width.

The AC card keeps its line, and keeps the word.

**Why the AC's state line shows the room, not the mode.** The chips select the mode
and the icon shows it; repeating it would waste the line. What nothing else on the
card can show is the room reading. A mode *outside* the three chips (heat,
fan_only, auto — set from the HA app) **is** named there, because otherwise the
card would show no active chip and no explanation.

**When the AC's line 1 does not fit, the NAME loses.** The state is short, live,
and the reason to look at the card at all; a name is static and already known to
whoever installed it. `textTrunc()` handles it. Two width decisions follow from
this: `OFFLINE` over `UNAVAILABLE` (75px vs 122px), and dropping the word `Room`
in the exceptional-mode case only — the number is unmistakably a temperature
beside its degree ring.

**A bulb's name budget is fixed, not residual.** With nothing to its right to
compete with, the inline identity column simply *is* the budget: 20px after the
icon, or two Font 2 digits. That fits the shipped ordinal names (`1`, `2`, `3`)
with air and truncates anything longer to a character plus `..`. It is the second
real cost of going inline, and `simulator.html` reports every truncation by name
so it cannot happen quietly — `?name=Bedside+reading+lamp` shows what a descriptive
name now looks like.

**The AC setpoint is a stepper**, `[▼] 24° [▲]`, with the value in the grid cell
*between* the two controls that change it. That cell is a **readout**: it draws no
button, takes no press flash, and `screenHitTest()` reports a tap there as a miss.
The dead cell is deliberate — it is what stops a slightly-off tap from stepping the
wrong way, which the old adjacent `T+`/`T-` pair could not. The steps are relative,
so they no-op until a real setpoint is known, and the readout shows `--` in the
same window so the value and the control agree about what is known.

**The card's border is its alarm channel.** A failed command flashes it and an
unreachable device holds it — one signal, two durations. The whole card carries
the notification rather than one word of text, and redrawing the outline alone is
enough since nothing behind it changes.

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
| Bulb chip | 43 × 52 | 43 × 40 |
| Colour swatch | 30 × 52 | 26 × 26 circle |
| AC mode chip | 60 × 52 | 60 × 26 |
| Brightness chip | 54 × 52 | 54 × 26 |
| Stepper chevron | 28 × 52 | 28 × 26 |
| Scene tile | 100 × 72 (full pitch) | 88 × 64 |
| Settings toggle | whole row, any x | 44 × 24 track, 18 × 18 knob |
| Tab | 81 × 32 | label + 2px underline |
| Scroll arrow | 20 × 104 | 12 × 10 chevron |
| AC setpoint cell | **nothing** | 44 × 26 readout |

The full row band counts vertically, and the full scene pitch counts in both axes,
so margins and gaps fold into the nearest control rather than missing. A settings
toggle's track is an affordance, not the hit area. The tab strip is the one place a
target is *not* generous relative to its neighbours, and it is the least-used
control — that is the trade.

**The inline bulb row narrowed its chips from 54 to 43**, and that is the one
place this table went backwards. Vertically nothing changed (the row band was
always the target) and the drawn control grew from 26 to 40, so a chip is easier
to *see* and easier to land on by eye — but 11px of horizontal slack is gone, and
horizontal is the axis a resistive panel misses on. Worth re-checking with
`pio run -e calib -t upload` if taps on the Devices page start feeling less
reliable.

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
| One bulb chip repaint | `fillRect` 43×40 + label | ~1.7k px |
| Bulb identity repaint | `fillRect` 44×40 + icon + 1 string | ~1.8k px |
| AC line 1 repaint | `fillRect` 288×19 + icon + 2 strings | ~5.5k px |
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
- **Start a card's dirty rect at `CARD_X`.** It eats the border column.
- **Reintroduce a corner radius for one component.** Corners are square
  everywhere; a lone rounded control reads as a rendering fault.
- **Add a per-second value to the clock region.** It is minute-rate by design.
- **Let a page branch on `vis` itself.** Go through `ctlColour()`.
- **Give Devices or Settings a scrollbar.** They `static_assert` that their rows
  tile the body exactly, and that is the point.

---

## 17. Verification

**`simulator.html` is the primary tool**, and three things about it are
load-bearing rather than conveniences:

1. **It draws the panel's actual glyphs.** The `GLYPHS` blob is the real byte
   data from `Font16.c` / `Font32rle.c` / `glcdfont.c`, and `gtext()` re-implements
   the same three decoders `TFT_eSPI::drawChar` uses, at the same advances — so the
   simulator is WYSIWYG and `textW()` is byte-identical to `tft.textWidth()`.
   Sizing an outline face to match cap height (what it did for FreeSans) broke on
   the built-in faces: Font 2's `O` advances 8px at a 10px cap height where
   Helvetica needs ~11, so glyphs overlapped and the reported layout was not the
   panel's. Regenerate with `scripts/gen_sim_fonts.py` + `scripts/font_metrics.py`.
2. **It re-checks every `static_assert` from `screen.cpp`**, plus three things the
   compiler cannot see — that a card's line-1 descenders stop before the control
   row, that its dirty rect covers those descenders, and that the line's ink does
   not start above that rect — plus the night ladder.
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

**Not verified, and worth a look on the panel:** whether Font 2's 10px caps are
big enough at arm's length for the device names and the settings captions — this
is the one real risk of the move off FreeSans, and the simulator can no longer
help decide it, since it now draws these exact pixels and says they are legible at
3× zoom; whether the title/caption pairing on Settings still separates now that
they share a face *and* a size and differ only in colour; and repaint smoothness
on a tab switch, the one heavy operation.
