#pragma once
#include <stdint.h>

// COLOUR TOKENS — the palette half of the design system. The other half is
// geometry (spacing scale, radii, control sizes), which lives in the LAYOUT
// block of include/config.h because simulator.html mirrors that file and a
// second geometry source is the thing that goes stale. Colour + type live here;
// nothing else defines either.
//
// Tokens are SEMANTIC, not literal: the name says what a colour is for, so a
// component never picks a hex value and two components asking for the same role
// can never disagree. Ranked by the role they play:
//
//   ground        BG                      the screen behind everything
//   surfaces      SURFACE ELEVATED        card fill, then controls on a card
//   lines         BORDER DIVIDER          card edge, then rules inside one
//   text          TEXT TEXT2 TEXT3        primary / secondary / tertiary
//   inert         DISABLED DIM            unavailable control / stale value
//   interactive   ACCENT                  selected, and only selected
//                 NEUTRAL                 selected, but nothing is on
//   status        SUCCESS WARNING ERROR    online / degraded / failed
//   physical      WARM COOL                what 2202K and 4000K look like
//
// ACCENT is deliberately the only saturated colour in a resting UI: if
// everything is highlighted, nothing is. SUCCESS/WARNING/ERROR appear only when
// there is something to say.
//
// NEUTRAL is the one exception to "selected == ACCENT", and it exists because
// OFF is not an accomplishment. A cyan OFF chip made the quietest state on the
// Devices page the loudest mark on it, and with three bulbs off, three of the
// four cards lit up. Grey still reads as selected — it is well clear of
// ELEVATED — without claiming anything is happening. It is a fill role, not a
// text one, which is why it is its own token rather than TEXT3 borrowed: the
// two are free to move independently, exactly as SURFACE and ELEVATED are.
//
// These are RUNTIME values, not #defines, because night mode recolours the
// whole UI in place. Consequence: NO C_* NAME MAY APPEAR IN A STATIC OR
// CONSTEXPR INITIALIZER. (Nothing does — logo_ha.h's palette is raw hex.)

// RGB888 -> RGB565, compile-time
#define RGB565(r, g, b) \
  (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

// One list, four derivations (slot enum, day table, night table, shift
// table), so a colour cannot be half-added.
//
// Third column is the night override. THEME_DERIVE means "compute it from the
// day colour by luminance" (see redOnly() in theme.cpp); anything else is a
// hand-picked red, because pure luminance collapses pairs that MUST stay
// distinguishable:
//
//   ERROR and DIM both derive to 0x7000 — and drawCard() picks between them on
//   the very same string (red = failed service call / UNAVAILABLE, dim = stale
//   poll). Collapsing them destroys the "fail soft" rule in CLAUDE.md.
//   SUCCESS and ACCENT both derive to 0x9000, which would make the connectivity
//   glyph read as the selected-tab fill. WARM and COOL land 2/31 steps apart,
//   losing the "instantly distinguishable across a dark bedroom" property their
//   own comment below claims. The four text tiers derive into a 15..21 huddle
//   that destroys the hierarchy they exist to express.
//
// The overrides are ordered by 5-bit red level so nothing confusable sits
// adjacent. Structural darks are left to derive (they are near-black either
// way); every semantic colour is pinned. Measured ladder, all 19 distinct:
//
//   BG 0 < SURFACE 2 < DIVIDER 3 < ELEVATED 4 < BORDER 6 < DIM 7 < SUCCESS 10
//   < DISABLED 12 < NEUTRAL 13 < WARM 14 < TEXT3 16 < ACCENT 18 < TEXT2 20
//   < WARNING 22 < BRI 24 < TEXT 25 < COOL 27 < DOWN 29 < ERROR 31
//
// The SURFACE/DIVIDER/ELEVATED trio sits one step apart, and that is the
// intended result rather than a crowding failure: night mode exists to emit as
// little light as possible, so the card, its rules and the controls on it all
// collapse toward black and the page is carried by text and ACCENT alone. What
// must not collapse is any pair a reader has to TELL APART — and every such pair
// (DIM/ERROR, SUCCESS/ACCENT, WARM/COOL, NEUTRAL/ELEVATED, NEUTRAL/ACCENT, BRI/
// NEUTRAL, BRI/ACCENT, BRI/WARM, BRI/COOL, DOWN/ERROR, DOWN/COOL, DOWN/ACCENT,
// the text tiers) is ≥2 steps clear.
//
// NEUTRAL's one-step neighbours are DISABLED and WARM, and BRI's is TEXT
// (24 vs 25) — none of those three are a pair: the ladder ranks values, not
// roles, and each of those two never appears as the same KIND of mark as its
// neighbour. NEUTRAL is only ever a chip fill; DISABLED is only ever a label
// on a SURFACE fill (a disabled chip differs from a selected one by its whole
// fill, not by 1/31 of red); WARM is a swatch disc; TEXT is a label colour,
// never a fill a selected BRI chip needs to be told apart from — its dark
// BV_ACTIVE-style label sits ON that fill, not beside it. There is no free
// level with two clear steps below ACCENT — the low half of the ladder is
// full — so this is the trade, and it is made on role separation rather than
// on hoping nobody looks.
//
// simulator.html asserts the ladder is collision-free, which is the only cheap
// way to check it without standing in a dark bedroom.
//
// Fourth column is Night Shift — a milder alternative computed by
// warmShift() (theme.cpp): green scaled to 45% and blue to 10% of their day
// values, red left untouched. Both channels were cut TWICE on real-hardware
// feedback: green+blue started at 60/35, blue alone dropped to 60/15 when
// ACCENT still read as a saturated blue against an otherwise warm/amber
// screen, and both channels were cut again to 45/10 when the palette still
// read as carrying too much green/blue overall — not a single-token problem,
// a global one, hence tightening the blanket knob itself rather than patching
// more individual overrides. Unlike the red column, this output is NOT
// monochrome, so a single luminance ladder cannot certify it: two colours
// can share near-identical luminance while reading as visually distinct by
// hue, or vice versa. Values below were computed by a throwaway script
// (never hand-typed — see CLAUDE.md's Night mode section), and verified per
// pair by BOTH luminance gap and per-channel delta:
//
//   DIM/ERROR land only 7.5 luminance apart, but 14/31 red steps and 8/63
//   green steps apart — DIM stays a muted brown, ERROR a near-saturated red,
//   told apart by hue rather than brightness. WARM/COOL are 14.7 apart in
//   luminance but 11/31 red steps apart, for the same reason (red is passed
//   through unscaled, and the two already differed there in day mode).
//
//   ACCENT is the only token still pinned to a custom ratio rather than the
//   blanket 45/10: at the blanket rate alone it collapses toward SUCCESS
//   (both read as a dim green — the fix at 60/15 was ACCENT-specific hue
//   separation, and cutting the blanket further does not remove that need).
//   ACCENT's override (green 15%, blue 22%) lands a dark navy-teal (lum 30,
//   versus the day colour's lum 132) that keeps just enough blue to read as
//   a distinct hue from SUCCESS's now-blanket dark green (lum 70) — 40
//   luminance points clear, plus a real green/blue channel gap. Every other
//   semantic token needed NO custom ratio once the blanket itself moved —
//   they are still pinned explicitly (not THEME_DERIVE) so a future retune of
//   the blanket constants cannot silently drift them without a recompute.
//
// All 18 Shift values are pairwise-unique RGB565 words; simulator.html's
// checkShiftDistinct() asserts uniqueness plus a per-channel delta on the
// same MUST_DIFFER pairs above, rather than porting the red ladder's
// single-axis metric.
//
// TH_BG IS PINNED TO 0x0041 AND MUST NOT MOVE: logo_ha.h's generated palette
// bakes that exact value as the splash mark's background, which is what lets
// the logo blit as an opaque rectangle with no transparency handling. Change it
// and the boot splash grows a visible 96x96 box. Regenerate the logo first.
#define THEME_DERIVE 0xFFFF
// Fourth column is Night Shift: a warmer, milder alternative to the red-only
// night column above, computed by warmShift() (theme.cpp) instead of
// redOnly() — green/blue scaled down, not zeroed. Structural darks derive
// fine (near-black either way); every semantic token is pinned explicitly,
// same reasoning as the night column, computed by a throwaway script rather
// than by hand (see CLAUDE.md's Night mode section for the method and the
// two pairs — SUCCESS/ACCENT, DIM/ERROR — that needed the most care).
#define THEME_LIST(X)                                                          \
  /*     slot          day                          night          shift    */ \
  X(TH_BG,        RGB565(0x07, 0x09, 0x0D), THEME_DERIVE, THEME_DERIVE) /* ground        */ \
  X(TH_SURFACE,   RGB565(0x16, 0x18, 0x1D), THEME_DERIVE, THEME_DERIVE) /* card fill     */ \
  X(TH_ELEVATED,  RGB565(0x23, 0x27, 0x2F), THEME_DERIVE, THEME_DERIVE) /* control on it */ \
  X(TH_BORDER,    RGB565(0x2F, 0x34, 0x3E), THEME_DERIVE, THEME_DERIVE) /* card edge     */ \
  X(TH_DIVIDER,   RGB565(0x1C, 0x1F, 0x26), THEME_DERIVE, THEME_DERIVE) /* rule / track  */ \
  X(TH_TEXT,      RGB565(0xF4, 0xF6, 0xFA), 0xC800,       0xF363)       /* primary       */ \
  X(TH_TEXT2,     RGB565(0xA6, 0xB0, 0xC2), 0xA000,       0xA282)       /* secondary     */ \
  X(TH_TEXT3,     RGB565(0x70, 0x7A, 0x8C), 0x8000,       0x71A1)       /* tertiary      */ \
  X(TH_DISABLED,  RGB565(0x44, 0x4C, 0x59), 0x6000,       0x4101)       /* unavailable   */ \
  X(TH_DIM,       RGB565(0x8A, 0x94, 0xA6), 0x3800,       0x8A02)       /* stale value   */ \
  X(TH_ACCENT,    RGB565(0x18, 0xBC, 0xF2), THEME_DERIVE, 0x18E6)       /* selected      */ \
  X(TH_NEUTRAL,   RGB565(0x7A, 0x82, 0x8E), 0x6800,       0x79C1)       /* selected: off */ \
  X(TH_SUCCESS,   RGB565(0x2F, 0xD9, 0x7C), 0x5000,       0x2B01)       /* online        */ \
  X(TH_WARNING,   RGB565(0xF5, 0xA5, 0x24), 0xB000,       0xF240)       /* degraded      */ \
  X(TH_ERROR,     RGB565(0xFF, 0x4D, 0x6A), 0xF800,       0xF901)       /* failed        */ \
  X(TH_WARM,      RGB565(0xFF, 0xB0, 0x5C), 0x7000,       0xFA81)       /* ~2202K amber  */ \
  X(TH_COOL,      RGB565(0xA6, 0xCD, 0xFF), 0xD800,       0xA2E3)       /* ~4000K blue   */ \
  X(TH_BRI,       RGB565(0xFF, 0xD1, 0x00), THEME_DERIVE, 0xFC40)       /* bulb % active */ \
  X(TH_DOWN,      RGB565(0x29, 0x79, 0xFF), 0xE800,       THEME_DERIVE) /* AC step: down */

#define TH_ENUM_(slot, day, night, shift) slot,
enum ThemeSlot : uint8_t { THEME_LIST(TH_ENUM_) TH_COUNT };
#undef TH_ENUM_

// Live palette. Written ONLY by themeSetNightMode().
extern uint16_t THEME[TH_COUNT];

// Cast to an rvalue so `C_BG = x;` cannot compile by accident.
#define C_BG       ((uint16_t)THEME[TH_BG])
#define C_SURFACE  ((uint16_t)THEME[TH_SURFACE])
#define C_ELEVATED ((uint16_t)THEME[TH_ELEVATED])
#define C_BORDER   ((uint16_t)THEME[TH_BORDER])
#define C_DIVIDER  ((uint16_t)THEME[TH_DIVIDER])
#define C_TEXT     ((uint16_t)THEME[TH_TEXT])
#define C_TEXT2    ((uint16_t)THEME[TH_TEXT2])
#define C_TEXT3    ((uint16_t)THEME[TH_TEXT3])
#define C_DISABLED ((uint16_t)THEME[TH_DISABLED])
#define C_DIM      ((uint16_t)THEME[TH_DIM])
#define C_ACCENT   ((uint16_t)THEME[TH_ACCENT])
#define C_NEUTRAL  ((uint16_t)THEME[TH_NEUTRAL])
#define C_SUCCESS  ((uint16_t)THEME[TH_SUCCESS])
#define C_WARNING  ((uint16_t)THEME[TH_WARNING])
#define C_ERROR    ((uint16_t)THEME[TH_ERROR])

// Colour-temperature swatches. These are what KELVIN_WARM / KELVIN_COOL look
// like to the eye, not a physical blackbody conversion — the point is that the
// two controls are instantly distinguishable across a dark bedroom.
#define C_WARM     ((uint16_t)THEME[TH_WARM])
#define C_COOL     ((uint16_t)THEME[TH_COOL])

// A second saturated colour, deliberately — the one exception alongside
// NEUTRAL to "ACCENT is the only saturated colour in a resting UI". Scoped to
// exactly one control: a bulb card's 1%/30%/100% chip when selected, on
// request, so it reads distinctly from the cyan used for everything else
// selected on the same card (OFF's neutral grey, a colour-temp swatch's
// accent halo). THEME_DERIVE is safe for its night(red) column — it lands at
// red-level 24, >=3 clear of NEUTRAL/ACCENT/WARM/COOL, the fills it can appear
// beside on one card — but Shift needed a hand-picked override: the blanket
// 45/10 scaling collapses any saturated yellow onto WARM's amber (both
// red-saturated already, and 45% compresses the one channel that could still
// tell them apart), so BRI's shift is boosted in green instead (chan
// (31,34,0) vs WARM's (31,20,1), ~32 luminance points and 14 green steps
// clear) rather than derived.
#define C_BRI      ((uint16_t)THEME[TH_BRI])

// A vivid blue, on request, for the AC setpoint's DOWN chevron — the up
// chevron pairs it with C_ERROR (see wStepBtn(), widgets.cpp), reused as-is
// rather than given a token of its own, since red already existed and needed
// no new derivation. Blue had no equally vivid existing token: C_COOL is
// deliberately pale (it has to read as "this bulb is ~4000K", not just "this
// is blue"), and C_ACCENT is reserved for "selected" — this chevron is drawn
// this colour at REST, not when selected, so reusing ACCENT there would say
// the wrong thing regardless of hue. Needed its own pinned night(red)
// override rather than THEME_DERIVE: the day colour's derived luminance
// lands exactly on WARM's red-level (14), an exact collision simulator.html's
// checkNightLadder() catches globally, not just against WARM specifically.
// Pinned to red-level 29 instead — clear of COOL (27) and ERROR (31), the
// two things it can appear beside — using the same "step<<11" construction
// ERROR/WARM/COOL's own overrides use. THEME_DERIVE is safe for Shift: the
// day colour's R5 (5) is far from ERROR's pinned shift R5 (31), WARM's (31)
// and COOL's (20), so no collision needed working around there.
#define C_DOWN     ((uint16_t)THEME[TH_DOWN])

// per-channel blend a -> b, t = 0..255. RGB565 has no alpha, so every "tinted"
// or "translucent" fill in this UI is a precomputed blend against what is
// behind it. That is also why there are no shadows or glass effects: they would
// each cost a second read-modify-write of the panel.
inline uint16_t lerp565(uint16_t a, uint16_t b, uint8_t t) {
  int32_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int32_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int32_t r  = ar + ((br - ar) * t) / 255;
  int32_t g  = ag + ((bg - ag) * t) / 255;
  int32_t bl = ab + ((bb - ab) * t) / 255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// A selected surface: ~22% of the accent over the card, not the accent itself.
// Used wherever a whole tile is selected rather than a small chip — a solid
// accent slab at that size is the loudest thing on the panel, and it keeps the
// solid fill meaningful as the press flash. Safe with the runtime palette: the
// macro expands inside a function body, so C_* is read at call time.
inline uint16_t tint565(uint16_t c) { return lerp565(C_BG, c, 56); }

// Switches the whole palette to mode 0=off / 1=red / 2=shift, mirroring
// state.h's NightMode — kept as a plain uint8_t rather than the enum so this
// file stays the dependency-free leaf module the design-system table
// describes (no include of state.h). Returns true only when it actually
// changed, so callers can skip the repaint — it is cheap enough to call
// every pass.
bool themeSetNightMode(uint8_t mode);
uint8_t themeNightMode();

// Maps one arbitrary colour the way the palette is currently mapped. Only the
// splash logo needs this: its 9-entry palette is baked hex, so without it the
// Home Assistant mark renders full-colour over a red-only UI.
uint16_t themeMap(uint16_t c);
