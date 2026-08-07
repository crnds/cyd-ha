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
// every signature, AND so the GFX faces are NAMED in exactly one translation
// unit. TFT_eSPI.h declares all 48 bundled Free Fonts wherever it is included,
// with internal linkage, so the compiler emits only the ones a given .cpp
// actually references — reference a face from a second file and its glyph
// bitmaps land in flash twice. gfx.cpp is that one file; everyone else asks for
// a role through fontSet().
extern TFT_eSPI tft;

// ── typography scale ─────────────────────────────────────
// Four roles, no more. The build already carries LOAD_GFXFF, so this uses the
// proportional FreeSans faces TFT_eSPI ships rather than its built-in bitmap
// fonts: the old UI set device names in the 6x8 GLCD font, which made the most
// important string on each row the least legible thing on the panel.
//
// Roles, not sizes, so a component asks for the job the text is doing:
//
//   F_NUM    FreeSansBold12pt  the AC setpoint — the one number being adjusted,
//                              and deliberately the largest thing in the body
//   F_TITLE  FreeSansBold9pt   card titles, the clock: what a thing IS
//   F_BODY   FreeSans9pt       live state, chip labels, tab labels, captions
//   F_MICRO  GLCD 6x8          last-resort fit only, never a first choice
//
// Only two families/weights carry the whole UI. Hierarchy comes from weight and
// colour tier (see theme.h) rather than from more sizes, which is what keeps 320
// x 240 from turning into a ransom note.
enum FontRole : uint8_t { F_NUM = 0, F_TITLE, F_BODY, F_MICRO, F_ROLES };

// Selects a role. Also called internally by every helper below, so a caller that
// asks for a width gets that role left selected — convenient, but it means the
// font is never assumed to be whatever the last draw used.
void fontSet(FontRole r);

// Height of the glyph box an M*_DATUM vertically centres on, and how far
// descenders fall below it. TFT_eSPI centres free fonts on their ASCENT only, so
// `cy` positions caps and digits exactly and lets descenders hang — which is why
// every row height in config.h budgets fontDescent() below its text line.
int16_t fontAscent(FontRole r);
int16_t fontDescent(FontRole r);

// Pixel width in `r`. Leaves `r` selected.
int16_t textW(FontRole r, const char* s);

// Draws with `cy` as the glyph-box centre. `datum` must be one of ML/MC/MR —
// the whole UI positions text by a centre line so that a font change cannot
// silently shift a baseline.
void textAt(FontRole r, const char* s, int16_t x, int16_t cy, uint8_t datum,
            uint16_t fg);

// Centred in a box of `maxW`, degrading rather than ever painting outside it:
// `s` in `r`, then `alt` in `r` (a shorter form — "100" for "100%"), then `s` in
// F_MICRO. FreeSans is proportional, so this self-corrects when a label changes;
// a hand-measured width would not.
void textFit(FontRole r, const char* s, const char* alt, int16_t cx, int16_t cy,
             int16_t maxW, uint16_t fg);

// Left-aligned, clipped to `maxW` by dropping characters and appending "..".
// Device names come from secrets.h and are arbitrary user strings, so the layout
// has to survive one that is too long — truncating the NAME (and never the live
// state beside it) is the deliberate choice about which loses.
void textTrunc(FontRole r, const char* s, int16_t x, int16_t cy, int16_t maxW,
               uint16_t fg);
