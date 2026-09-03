#include "icons.h"
#include "gfx.h"

// Every glyph below stays inside cx-7..cx+7 / cy-7..cy+7. That bound is the
// contract callers reserve space against — check it if you edit a radius.

void icoBulb(int16_t cx, int16_t cy, uint16_t c, bool filled) {
  // Globe sits 2px high so the screw base has room below it without pushing the
  // whole glyph off its centre line.
  const int16_t gy = cy - 2;
  if (filled) tft.fillCircle(cx, gy, 5, c);
  else        tft.drawCircle(cx, gy, 5, c);
  // Base: three tapering rows. Drawn solid in both states — an outlined base at
  // this size is two disconnected pixels.
  tft.drawFastHLine(cx - 3, cy + 4, 7, c);
  tft.drawFastHLine(cx - 3, cy + 5, 7, c);
  tft.drawFastHLine(cx - 2, cy + 6, 5, c);
}

void icoSnow(int16_t cx, int16_t cy, uint16_t c) {
  // Six-pointed asterisk: one vertical plus two diagonals at ±60°. Radius 6
  // rather than 7 so the diagonal endpoints land inside the box after rounding.
  tft.drawFastVLine(cx, cy - 6, 13, c);
  tft.drawLine(cx - 5, cy - 3, cx + 5, cy + 3, c);
  tft.drawLine(cx - 5, cy + 3, cx + 5, cy - 3, c);
  // Tips, which is what makes it read as a snowflake rather than an asterisk.
  tft.drawFastHLine(cx - 2, cy - 4, 5, c);
  tft.drawFastHLine(cx - 2, cy + 4, 5, c);
}

void icoDrop(int16_t cx, int16_t cy, uint16_t c) {
  // Solid by nature: a 1px outlined teardrop at 14px is unreadable.
  tft.fillTriangle(cx, cy - 7, cx - 5, cy + 1, cx + 5, cy + 1, c);
  tft.fillCircle(cx, cy + 1, 5, c);
}

void icoPower(int16_t cx, int16_t cy, uint16_t c) {
  // Three-quarter ring with the gap at the top, plus the stem through it.
  tft.drawCircleHelper(cx, cy + 1, 6, 4 | 8, c);   // lower half
  tft.drawFastVLine(cx - 6, cy - 2, 4, c);         // sides climbing back up
  tft.drawFastVLine(cx + 6, cy - 2, 4, c);
  tft.drawFastVLine(cx, cy - 7, 6, c);
}

void icoSun(int16_t cx, int16_t cy, uint16_t c) {
  tft.fillCircle(cx, cy, 3, c);
  tft.drawFastVLine(cx, cy - 7, 3, c);
  tft.drawFastVLine(cx, cy + 5, 3, c);
  tft.drawFastHLine(cx - 7, cy, 3, c);
  tft.drawFastHLine(cx + 5, cy, 3, c);
  // Diagonal rays as single pixels — a 2px diagonal at this size reads heavier
  // than the orthogonal ones and breaks the even stroke weight.
  tft.drawPixel(cx - 5, cy - 5, c);
  tft.drawPixel(cx + 5, cy - 5, c);
  tft.drawPixel(cx - 5, cy + 5, c);
  tft.drawPixel(cx + 5, cy + 5, c);
}

void icoMoon(int16_t cx, int16_t cy, uint16_t c, uint16_t bg) {
  // Disc minus an offset disc. RGB565 has no alpha, so the bite is painted in
  // the surface colour behind the icon — pass the CARD's fill, not C_BG, or the
  // crescent gets a dark notch.
  tft.fillCircle(cx - 1, cy, 7, c);
  tft.fillCircle(cx + 4, cy - 3, 6, bg);
}

void icoClock(int16_t cx, int16_t cy, uint16_t c) {
  tft.drawCircle(cx, cy, 7, c);
  tft.drawFastVLine(cx, cy - 4, 5, c);   // hour hand
  tft.drawFastHLine(cx, cy, 4, c);       // minute hand
}

void icoRotate(int16_t cx, int16_t cy, uint16_t c) {
  // Two half-arcs with opposed arrowheads — the standard rotate mark. Reads at
  // 14px where a "screen plus curved arrow" does not.
  tft.drawCircleHelper(cx, cy, 6, 1 | 2, c);   // upper half
  tft.drawCircleHelper(cx, cy, 6, 4 | 8, c);   // lower half
  tft.fillTriangle(cx + 3, cy - 1, cx + 8, cy - 1, cx + 6, cy + 4, c);
  tft.fillTriangle(cx - 3, cy + 1, cx - 8, cy + 1, cx - 6, cy - 4, c);
}

void icoChevron(int16_t cx, int16_t cy, ChevDir dir, uint16_t c, int16_t hw,
                int16_t hh) {
  switch (dir) {
    case CHEV_UP:
      tft.fillTriangle(cx - hw, cy + hh, cx + hw, cy + hh, cx, cy - hh, c);
      break;
    case CHEV_DOWN:
      tft.fillTriangle(cx - hw, cy - hh, cx + hw, cy - hh, cx, cy + hh, c);
      break;
    case CHEV_RIGHT:
      tft.fillTriangle(cx - hh, cy - hw, cx - hh, cy + hw, cx + hh, cy, c);
      break;
    case CHEV_LEFT:
      tft.fillTriangle(cx + hh, cy - hw, cx + hh, cy + hw, cx - hh, cy, c);
      break;
  }
}
