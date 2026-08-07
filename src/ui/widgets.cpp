#include "widgets.h"
#include "config.h"
#include "gfx.h"
#include "icons.h"
#include "theme.h"

CtlColour ctlColour(uint8_t vis) {
  switch (vis) {
    // A 120ms flash has to be unmistakable before HA has answered, so this one
    // state is deliberately the loudest thing the UI ever draws. It is the only
    // place a full-brightness fill appears.
    case BV_PRESSED:  return { C_TEXT,     C_TEXT,     C_BG };
    case BV_ACTIVE:   return { C_ACCENT,   C_ACCENT,   C_BG };
    // Selected, but nothing is on. Reads as selected against C_ELEVATED without
    // spending the accent on the one state that is not an accomplishment.
    case BV_ACTIVE_OFF: return { C_NEUTRAL, C_NEUTRAL, C_BG };
    // Recedes INTO the card rather than greying out on top of it: there is no
    // state to show, so the control should not look like it is showing one.
    case BV_DISABLED: return { C_SURFACE,  C_SURFACE,  C_DISABLED };
    case BV_ERR:      return { C_SURFACE,  C_ERROR,    C_ERROR };
    default:          return { C_ELEVATED, C_ELEVATED, C_TEXT2 };
  }
}

void wCard(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t fill,
           uint16_t edge) {
  tft.fillRoundRect(x, y, w, h, R_LG, fill);
  if (edge != fill) tft.drawRoundRect(x, y, w, h, R_LG, edge);
}

void wChip(int16_t x, int16_t y, int16_t w, int16_t h, const char* label,
           const char* alt, uint8_t vis) {
  const CtlColour c = ctlColour(vis);
  tft.fillRoundRect(x, y, w, h, R_SM, c.fill);
  if (c.edge != c.fill) tft.drawRoundRect(x, y, w, h, R_SM, c.edge);
  // SP_1 of padding each side — the label budget, not the chip width, is what
  // decides whether a caption fits.
  textFit(F_BODY, label, alt, x + w / 2, y + h / 2, w - 2 * SP_1, c.fg);
}

void wStepBtn(int16_t x, int16_t y, int16_t w, int16_t h, bool up, uint8_t vis) {
  const CtlColour c = ctlColour(vis == BV_ACTIVE ? BV_INACTIVE : vis);
  tft.fillRoundRect(x, y, w, h, R_SM, c.fill);
  if (c.edge != c.fill) tft.drawRoundRect(x, y, w, h, R_SM, c.edge);
  icoChevron(x + w / 2, y + h / 2, up, c.fg, 6, 4);
}

void wSwatch(int16_t cx, int16_t cy, int16_t r, uint16_t colour, uint8_t vis) {
  switch (vis) {
    case BV_ACTIVE:
      // Saturated disc plus a halo at the full radius. The selection signal is
      // the luminance jump from the tinted state below, not the hue — which is
      // what keeps it legible once the palette collapses to red at night.
      tft.fillCircle(cx, cy, r - 2, colour);
      tft.drawCircle(cx, cy, r, colour);
      break;
    case BV_PRESSED:
      tft.fillCircle(cx, cy, r - 2, C_TEXT);
      break;
    case BV_DISABLED:
      // Hollow: this bulb has no colour-temperature axis at all, so showing a
      // colour would claim an option that does not exist.
      tft.drawCircle(cx, cy, r - 2, C_DISABLED);
      break;
    default:
      tft.fillCircle(cx, cy, r - 2, tint565(colour));
      tft.drawCircle(cx, cy, r - 2, lerp565(C_BG, colour, 150));
      break;
  }
}

void wToggle(int16_t x, int16_t y, int16_t w, int16_t h, bool on) {
  const int16_t r = h / 2;
  if (on) {
    tft.fillRoundRect(x, y, w, h, r, C_ACCENT);
    tft.fillCircle(x + w - r, y + r, r - 3, C_BG);
  } else {
    tft.fillRoundRect(x, y, w, h, r, C_ELEVATED);
    tft.fillCircle(x + r, y + r, r - 3, C_TEXT3);
  }
}

void wValue(int16_t x, int16_t y, int16_t w, int16_t h, const char* val,
            bool degree, uint16_t fg, uint16_t bg) {
  tft.fillRect(x, y, w, h, bg);

  // Neither built-in nor FreeSans covers U+00B0, and a "C" beside the number
  // reads as a third digit at a glance. A 2px ring is both unambiguous and the
  // one glyph in the UI that no font could supply.
  const int16_t vw   = textW(F_NUM, val);
  const int16_t degW = degree ? 9 : 0;          // 7px ring + SP_1/2 of air
  const int16_t cy   = y + h / 2;
  const int16_t bx   = x + (w - (vw + degW)) / 2;

  textAt(F_NUM, val, bx, cy, ML_DATUM, fg);

  if (degree) {
    // TFT_eSPI centres a free font on its ASCENT, so with an M* datum the digit
    // tops sit at cy - fontAscent/2 and the ring rides them rather than the
    // baseline. Positioned off the metric, not off a measured pixel.
    const int16_t ry = cy - fontAscent(F_NUM) / 2 + 3;
    const int16_t rx = bx + vw + 5;
    tft.drawCircle(rx, ry, 3, fg);
    tft.drawCircle(rx, ry, 2, fg);
  }
}

void wTab(int16_t x, int16_t y, int16_t w, int16_t h, const char* label,
          uint8_t vis) {
  const bool sel = (vis == BV_ACTIVE || vis == BV_PRESSED);

  // Text tier is the first of the two selection signals: C_TEXT for the page you
  // are on, C_TEXT3 for the ones you are not. Nine night-steps apart, so the
  // strip still says where you are with the bar ignored entirely.
  textFit(F_BODY, label, nullptr, x + w / 2, y + h / 2, w - 2 * TAB_LBL_DX,
          sel ? C_TEXT : C_TEXT3);
  if (!sel) return;

  // Measured, not budgeted: FreeSans is proportional and textFit centres on the
  // same cx, so the bar tracks a renamed tab with no constant to update.
  int16_t uw = textW(F_BODY, label) + 2 * TAB_UL_PAD;
  if (uw > w) uw = w;

  // Pressed goes full-brightness like every other control, but as a brighter BAR
  // rather than a fill — the page switch is instant and local, so this only has
  // to confirm the tap, not stand in for a pending round trip.
  tft.fillRect(x + (w - uw) / 2, y + h - TAB_UL_H, uw, TAB_UL_H,
               vis == BV_PRESSED ? C_TEXT : C_ACCENT);
}

void wPip(int16_t cx, int16_t cy, int16_t r, uint16_t c, bool filled) {
  if (filled) tft.fillCircle(cx, cy, r, c);
  else        tft.drawCircle(cx, cy, r, c);
}
