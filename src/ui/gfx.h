#pragma once
#include <TFT_eSPI.h>
#include <stdint.h>

// TYPE TOKENS + the one and only panel handle.
//
// LAYERING RULE: only src/ui/* may include this. main.cpp goes through
// screen.h and must never poke the panel directly — that rule is what keeps the
// rotation memo, the palette memo and the dirty-region snapshots owned by one
// layer each.
//
// The handle lives here rather than file-static in screen.cpp so that
// widgets.cpp and icons.cpp can draw without threading a TFT_eSPI& through
// every signature, AND so the faces are NAMED in exactly one translation unit.
// That mattered acutely with the GFX free fonts (TFT_eSPI.h declares all 48 with
// internal linkage, so naming one from a second .cpp duplicated its glyph
// bitmaps in flash). It costs nothing now that the roles are built-in fonts
// selected by NUMBER, but the rule stays: everyone asks for a role via fontSet().
extern TFT_eSPI tft;

// ── typography scale ─────────────────────────────────────
// Four roles, no more, over TFT_eSPI's BUILT-IN bitmap faces. These are drawn
// pixel by pixel at one fixed size, so every stem and bowl lands on the grid and
// nothing is scaled or resampled at draw time — which is the whole reason for
// preferring them to the FreeSans GFX faces this used to carry.
//
// Roles, not sizes, so a component asks for the job the text is doing:
//
//   F_NUM    Font 4 (26px box)  LIVE NUMERIC READOUTS: the AC setpoint, and the
//                               header's room temperature/humidity and clock
//   F_TITLE  Font 2 (16px box)  card titles: what a thing IS. Also a unit marker
//                               beside F_NUM digits (the header's "%")
//   F_BODY   Font 2 (16px box)  live state, chip labels, captions
//   F_MICRO  Font 1 (GLCD 6x8)  last-resort fit; ALSO the tab bar, on request
//
// F_NUM WAS THE AC SETPOINT ALONE until the header's two number regions were
// bumped up to it on request. That widened the role from "the one number being
// adjusted" to "numbers read at a glance", and it cost the setpoint its old claim
// to being the largest thing on the panel. The setpoint still reads as the
// emphasis of the Devices page, but now because of where it sits — on a card,
// between the two chevrons that change it — rather than because nothing else is
// as big. Anything NEW asking for F_NUM should be a live number of that kind; a
// label or a caption still must not have it (see the CARD_L1_H note below).
//
// F_MICRO's tab-bar use is a deliberate, scoped exception to "last-resort,
// never a first choice" below — chosen there specifically, on request, to
// shrink "Devices"/"Scenes"/"Settings" by one step so TAB_GAP (config.h)
// could open a real gap between tabs without widening the header. It is
// still the fallback for every OTHER role's textFit(); only wTab() picks it
// as a first choice.
//
// TWO CONSEQUENCES OF THE BUILT-IN SET, both real constraints rather than
// oversights:
//
//   * There is no bold, so F_TITLE and F_BODY are THE SAME FACE. The whole
//     title-vs-state hierarchy therefore rests on colour tier alone (C_TEXT vs
//     C_TEXT2/C_TEXT3, see theme.h) — the weight signal that used to carry half
//     of it is simply not available. Do not "fix" this by promoting titles to
//     Font 4: at an 18px cap height it does not fit CARD_L1_H, and a card title
//     as large as the AC setpoint inverts the emphasis of the row it sits on.
//   * There is nothing between Font 2 and Font 4 (10px caps and 18px caps), so
//     the ladder has a hole in the middle and F_NUM is the only step above body.
//     Bumping anything "one size" therefore costs 1.8x the WIDTH, not a nudge —
//     which is why the header could only afford it for two regions after a glyph
//     was deleted and TAB_GAP cut twice, and why its "%" had to stay behind at
//     F_TITLE (21px against 9px). Mixing two roles on one line needs
//     fontBaseline(); see below.
//
// Body text is accordingly SMALLER than the FreeSans build it replaced: 10px
// caps against 13px. That is the cost of the pixel grid, and it is why F_MICRO
// must stay a genuine last resort — the gap between F_BODY and F_MICRO is now
// only 3px, so a fallback is far less visible than it used to be.
enum FontRole : uint8_t { F_NUM = 0, F_TITLE, F_BODY, F_MICRO, F_ROLES };

// Selects a role. Also called internally by every helper below, so a caller that
// asks for a width gets that role left selected — convenient, but it means the
// font is never assumed to be whatever the last draw used.
void fontSet(FontRole r);

// Ink extent of `r` measured from the `cy` handed to textAt(): the topmost and
// bottommost rows a glyph of that role can paint. Top is negative (above cy),
// bottom positive.
//
// These are DATUM-RELATIVE rather than the usual baseline-relative ascent and
// descent, because a built-in face's ink is not centred in the box TFT_eSPI
// positions — it sits inset by a few blank rows, and by a different amount top
// and bottom. Callers want "where does the ink start", which the old
// fontAscent()/2 arithmetic only answered by accident of the free fonts being
// symmetric about their datum. Both degree rings ride fontInkTop(), and
// config.h budgets fontInkBottom() below every text line.
//
// Measured, not read off the font header: scripts/font_metrics.py decodes the
// glyph data. Re-run it if a role is ever pointed at a different face.
int16_t fontInkTop(FontRole r);
int16_t fontInkBottom(FontRole r);

// Baseline row of `r`, also relative to textAt()'s `cy`. For MIXING TWO ROLES ON
// ONE LINE: an M* datum centres each role on its own box, so the same cy does NOT
// put two roles on the same baseline, and the difference of two of these is the
// correction. The header's room reading needs it for the small "%" that follows
// its big digits. Don't reach for fontInkBottom() instead — that is an envelope
// including descenders, so on glyphs that have none it aligns the wrong row.
int16_t fontBaseline(FontRole r);

// Pixel width in `r`. Leaves `r` selected.
int16_t textW(FontRole r, const char* s);

// Draws with `cy` as the glyph-box centre. `datum` must be one of ML/MC/MR —
// the whole UI positions text by a centre line so that a font change cannot
// silently shift a baseline.
void textAt(FontRole r, const char* s, int16_t x, int16_t cy, uint8_t datum,
            uint16_t fg);

// Centred in a box of `maxW`, degrading rather than ever painting outside it:
// `s` in `r`, then `alt` in `r` (a shorter form — "100" for "100%"), then `s` in
// F_MICRO. The built-in faces are proportional too, so this still self-corrects
// when a label changes where a hand-measured width would not.
void textFit(FontRole r, const char* s, const char* alt, int16_t cx, int16_t cy,
             int16_t maxW, uint16_t fg);

// Left-aligned, clipped to `maxW` by dropping characters and appending "..".
// Device names come from secrets.h and are arbitrary user strings, so the layout
// has to survive one that is too long — truncating the NAME (and never the live
// state beside it) is the deliberate choice about which loses. Font 2 is
// appreciably narrower than the FreeSans it replaced, so this fires less often
// than it did; it is not thereby less necessary.
void textTrunc(FontRole r, const char* s, int16_t x, int16_t cy, int16_t maxW,
               uint16_t fg);
