#include "gfx.h"
#include <string.h>

// No font #include here on purpose: TFT_eSPI.h -> gfxfont.h already declares all
// 48 bundled Free Fonts in every translation unit that sees it, so including one
// explicitly is a redefinition error, not a convenience. Their tables have
// internal linkage and the compiler drops the ones nothing references — which is
// exactly why the three faces below must be REFERENCED from this file alone. A
// second .cpp that names one gets a second copy of its glyph bitmaps in flash.
TFT_eSPI tft;

static const GFXfont* const ROLE_FONT[F_ROLES] = {
  &FreeSansBold12pt7b,   // F_NUM
  &FreeSansBold9pt7b,    // F_TITLE
  &FreeSans9pt7b,        // F_BODY
  nullptr,               // F_MICRO -> built-in GLCD
};

// Measured off the font headers rather than guessed: ascent is max(-yOffset) and
// descent is max(height + yOffset) over the 0x20..0x7E charset, which is exactly
// what TFT_eSPI's glyph_ab / glyph_bb compute at setFreeFont() time. The layout
// in config.h is derived from these numbers, so if a face is ever swapped these
// must be re-measured with scripts/, not adjusted by eye.
static const int8_t ROLE_ASC[F_ROLES]  = { 17, 13, 13, 8 };
static const int8_t ROLE_DESC[F_ROLES] = {  6,  5,  5, 0 };

void fontSet(FontRole r) {
  if (r >= F_ROLES) r = F_BODY;
  // setTextFont(1) also clears gfxFont in a LOAD_GFXFF build, which is what
  // makes the GLCD fallback a clean switch rather than a font left half-set.
  if (ROLE_FONT[r]) tft.setFreeFont(ROLE_FONT[r]);
  else              tft.setTextFont(1);
}

int16_t fontAscent(FontRole r)  { return ROLE_ASC[r  < F_ROLES ? r : F_BODY]; }
int16_t fontDescent(FontRole r) { return ROLE_DESC[r < F_ROLES ? r : F_BODY]; }

int16_t textW(FontRole r, const char* s) {
  if (!s || !*s) return 0;
  fontSet(r);
  return (int16_t)tft.textWidth(s);
}

void textAt(FontRole r, const char* s, int16_t x, int16_t cy, uint8_t datum,
            uint16_t fg) {
  if (!s) return;
  fontSet(r);
  tft.setTextColor(fg);
  tft.setTextDatum(datum);
  tft.drawString(s, x, cy);
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
