#include "gfx.h"
#include <string.h>

// No font #include here. The built-in faces are compiled in by the LOAD_FONT2 /
// LOAD_FONT4 / LOAD_GLCD flags in platformio.ini and selected by NUMBER, so
// there is nothing to include and no way to duplicate a glyph table by naming a
// face twice — the hazard the old FreeSans mapping had to be careful about.
// LOAD_FONT4 also implies LOAD_RLE (TFT_eSPI.h), which is what decodes Font 4.
TFT_eSPI tft;

// Role -> built-in font number. Font 1 is GLCD 6x8; 2 and 4 are the proportional
// bitmap faces. See gfx.h for why F_TITLE and F_BODY have to share one.
static const uint8_t ROLE_FONT[F_ROLES] = { 4, 2, 2, 1 };

// MEASURED ink extents relative to the datum cy, not read off the font headers.
// The headers give the NOMINAL box (Font 2: 16 tall, baseline 13; Font 4: 26
// tall, baseline 19) but every glyph sits inset inside it — Font 2's caps start
// 3 rows down, Font 4's 1 row down — so the nominal numbers put an ornament in
// the wrong place. scripts/font_metrics.py decodes the glyph data and prints
// these tables; re-run it, don't nudge them by eye.
//
//   role     face     ink        baseline  cap height
//   F_NUM    Font 4   cy-8..+15  cy+10     18px
//   F_TITLE  Font 2   cy-5..+7   cy+5      10px
//   F_BODY   Font 2   cy-5..+7   cy+5      10px
//   F_MICRO  Font 1   cy-4..+3   cy+3       7px
static const int8_t ROLE_INK_TOP[F_ROLES] = { -8, -5, -5, -4 };
static const int8_t ROLE_INK_BOT[F_ROLES] = { 15,  7,  7,  3 };

// The baseline column of the same table, exposed because MIXING TWO ROLES ON ONE
// LINE needs it: an M* datum centres each role on its own box, so two roles drawn
// at the same cy do not share a baseline. The header's room reading draws a small
// F_TITLE "%" after big F_NUM digits and has to sit them on one line
// (drawStatusRoom()); the difference of two entries here is that offset. Ink
// extents cannot answer it — those are envelopes, and neither digits nor "%" have
// descenders, so aligning ink bottoms would align the wrong rows.
static const int8_t ROLE_BASE[F_ROLES] = { 10,  5,  5,  3 };

// Datum compensation, added to cy inside textAt(). TFT_eSPI centres a GFX free
// font on its ASCENT but a built-in font on its FULL BOX (drawString: `cheight =
// glyph_ab` versus `cheight = fontHeight(font)`), and that box carries blank
// rows the ascent did not.
//
// F_NUM's +4 is what makes the switch invisible where it would show most: it
// lands Font 4's digits on the exact pixels FreeSansBold12pt used, so the AC
// setpoint and the degree ring beside it are unmoved. Uncorrected the number
// would jump 4px up its control row.
//
// The Font 2 roles take 0 — TFT_eSPI's own centring is already right for them —
// and are deliberately RE-CENTRED rather than top-aligned with the old face:
// their cap box shrank 13px -> 10px, and holding the old top would leave every
// line riding high in a row height budgeted for the taller font.
static const int8_t ROLE_DY[F_ROLES] = { 4, 0, 0, 0 };

static inline FontRole roleOf(FontRole r) { return r < F_ROLES ? r : F_BODY; }

void fontSet(FontRole r) {
  // setTextFont() also clears gfxFont in a LOAD_GFXFF build, so no stale free
  // font can survive here — which matters because font 1 means GLCD only while
  // gfxFont is null. Nothing calls setFreeFont() any more, but the build still
  // carries LOAD_GFXFF for that null-safety and for gfxfont.h's declarations.
  tft.setTextFont(ROLE_FONT[roleOf(r)]);
}

int16_t fontInkTop(FontRole r)    { return ROLE_INK_TOP[roleOf(r)]; }
int16_t fontInkBottom(FontRole r) { return ROLE_INK_BOT[roleOf(r)]; }
int16_t fontBaseline(FontRole r)  { return ROLE_BASE[roleOf(r)]; }

int16_t textW(FontRole r, const char* s) {
  if (!s || !*s) return 0;
  fontSet(r);
  return (int16_t)tft.textWidth(s);
}

void textAt(FontRole r, const char* s, int16_t x, int16_t cy, uint8_t datum,
            uint16_t fg) {
  if (!s) return;
  fontSet(r);
  // One-argument setTextColor sets the background to the same colour, which is
  // what selects TFT_eSPI's transparent glyph path for both the Font 2 bitmap
  // and the Font 4 RLE decoder. Every dirty region is fillRect-cleared first, so
  // painting a background here would only risk overdrawing a neighbour: Font 4's
  // 26px box is taller than the 26px control row it sits in once the datum
  // offset is applied, and on the AC row it reaches the last screen line.
  tft.setTextColor(fg);
  tft.setTextDatum(datum);
  tft.drawString(s, x, cy + ROLE_DY[roleOf(r)]);
}

void textFit(FontRole r, const char* s, const char* alt, int16_t cx, int16_t cy,
             int16_t maxW, uint16_t fg) {
  if (!s) return;
  if (textW(r, s) <= maxW) { textAt(r, s, cx, cy, MC_DATUM, fg); return; }
  if (alt && textW(r, alt) <= maxW) { textAt(r, alt, cx, cy, MC_DATUM, fg); return; }
  textAt(F_MICRO, s, cx, cy, MC_DATUM, fg);
}

void textTrunc(FontRole r, const char* s, int16_t x, int16_t cy, int16_t maxW,
               uint16_t fg) {
  if (!s || !*s || maxW <= 0) return;
  if (textW(r, s) <= maxW) { textAt(r, s, x, cy, ML_DATUM, fg); return; }

  // Drop one character at a time and re-measure, because the face is
  // proportional: cutting by a fixed character count would over- or under-trim
  // depending on the letters. n is bounded by the buffer, so a pathologically
  // long name costs at most 46 textWidth() calls on a path that only runs when a
  // name genuinely does not fit.
  char buf[48];
  size_t n = strlen(s);
  if (n > sizeof(buf) - 3) n = sizeof(buf) - 3;
  while (n > 0) {
    memcpy(buf, s, n);
    buf[n]     = '.';
    buf[n + 1] = '.';
    buf[n + 2] = '\0';
    if (textW(r, buf) <= maxW) { textAt(r, buf, x, cy, ML_DATUM, fg); return; }
    n--;
  }
  // Not even ".." fits: draw nothing rather than a stray glyph. The state text
  // beside it is the thing worth keeping in this case.
}
