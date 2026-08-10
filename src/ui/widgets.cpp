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
    // A bulb's brightness chip, selected — yellow rather than the shared
    // accent, scoped to this one control on request. See theme.h's C_BRI.
    case BV_ACTIVE_BRI: return { C_BRI,     C_BRI,     C_BG };
    // Recedes INTO the page rather than greying out on top of it: there is no
    // state to show, so the control should not look like it is showing one. The
    // devices card carries no fill, so the page background is what it sinks to.
    case BV_DISABLED: return { C_BG,       C_BG,       C_DISABLED };
    case BV_ERR:      return { C_BG,       C_ERROR,    C_ERROR };
    default:          return { C_ELEVATED, C_ELEVATED, C_TEXT2 };
  }
}

void wCard(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t fill,
           uint16_t edge) {
  tft.fillRect(x, y, w, h, fill);
  if (edge != fill) tft.drawRect(x, y, w, h, edge);
}

void wChip(int16_t x, int16_t y, int16_t w, int16_t h, const char* label,
           const char* alt, uint8_t vis) {
  const CtlColour c = ctlColour(vis);
  tft.fillRect(x, y, w, h, c.fill);
  if (c.edge != c.fill) tft.drawRect(x, y, w, h, c.edge);
  // SP_1 of padding each side — the label budget, not the chip width, is what
  // decides whether a caption fits.
  textFit(F_BODY, label, alt, x + w / 2, y + h / 2, w - 2 * SP_1, c.fg);
}

void wStepBtn(int16_t x, int16_t y, int16_t w, int16_t h, bool up, uint8_t vis) {
  const CtlColour c = ctlColour(vis == BV_ACTIVE ? BV_INACTIVE : vis);
  tft.fillRect(x, y, w, h, c.fill);
  if (c.edge != c.fill) tft.drawRect(x, y, w, h, c.edge);
  // Up is C_ERROR (red, reused as-is), down is C_DOWN (a vivid blue with its
  // own theme token — see theme.h's comment on it for why C_COOL couldn't be
  // reused the way C_ERROR was: it is deliberately pale, matching a bulb's
  // ~4000K appearance, which reads as muted rather than "popping" here).
  // Only at REST, though: BV_PRESSED and BV_DISABLED still take their colour
  // from ctlColour() above, so the press flash and the disabled recede read
  // exactly the way every other control's do — a disabled chevron staying
  // red or blue would read as "still selected" on a card that is trying to
  // say the opposite.
  const uint16_t chevFg = (vis == BV_PRESSED || vis == BV_DISABLED)
                               ? c.fg
                               : (up ? C_ERROR : C_DOWN);
  icoChevron(x + w / 2, y + h / 2, up, chevFg, 6, 4);
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
  // A square track with a square knob. The knob went from a disc to a block with
  // the corners: a circle sliding in a sharp-cornered slot is the one shape on
  // the panel that would still read as rounded, and the position of the knob —
  // not its outline — is what says on or off.
  const int16_t k = h - 2 * TGL_PAD;                 // knob side
  if (on) {
    tft.fillRect(x, y, w, h, C_ACCENT);
    tft.fillRect(x + w - TGL_PAD - k, y + TGL_PAD, k, k, C_BG);
  } else {
    tft.fillRect(x, y, w, h, C_ELEVATED);
    tft.fillRect(x + TGL_PAD, y + TGL_PAD, k, k, C_TEXT3);
  }
}

void wValue(int16_t x, int16_t y, int16_t w, int16_t h, const char* val,
            bool degree, uint16_t fg, uint16_t bg) {
  tft.fillRect(x, y, w, h, bg);

  // Font 4 has no U+00B0 (its 0x60 is a grave accent), and a "C" beside the
  // number reads as a third digit at a glance. A 2px ring is unambiguous and
  // costs nothing. Note Font 2 DOES carry a degree at 0x60 — TFT_ESPI_
  // GRAVE_IS_DEGREE is set in Font16.c — but the setpoint is F_NUM, and drawing
  // the room reading's degree from a glyph while this one stays a ring would put
  // two different degree marks on the same page.
  const int16_t vw   = textW(F_NUM, val);
  const int16_t degW = degree ? 9 : 0;          // 7px ring + SP_1/2 of air
  const int16_t cy   = y + h / 2;
  const int16_t bx   = x + (w - (vw + degW)) / 2;

  textAt(F_NUM, val, bx, cy, ML_DATUM, fg);

  if (degree) {
    // The ring rides the DIGIT TOPS, not the baseline, so it reads as part of
    // the number. fontInkTop() is where those tops are; positioning off the
    // metric rather than a measured pixel is what survived the move from
    // FreeSans to Font 4 with no change here beyond the metric's name.
    const int16_t ry = cy + fontInkTop(F_NUM) + 3;
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
  //
  // F_MICRO, not F_BODY — one step down, on request, so the label shrinks
  // enough that TAB_GAP (config.h) can open a real gap between tabs at the
  // same TAB_TAP_H/STATUS_H. This is the one caller that treats F_MICRO as a
  // first choice rather than textFit()'s last-resort fallback; see gfx.h.
  textFit(F_MICRO, label, nullptr, x + w / 2, y + h / 2, w - 2 * TAB_LBL_DX,
          sel ? C_TEXT : C_TEXT3);
  if (!sel) return;

  // Measured, not budgeted: the built-in face is proportional (well, GLCD is
  // monospace, but this still self-corrects if that ever changes) and textFit
  // centres on the same cx, so the bar tracks a renamed tab with no constant
  // to update.
  int16_t uw = textW(F_MICRO, label) + 2 * TAB_UL_PAD;
  if (uw > w) uw = w;

  // Pressed goes full-brightness like every other control, but as a brighter BAR
  // rather than a fill — the page switch is instant and local, so this only has
  // to confirm the tap, not stand in for a pending round trip.
  tft.fillRect(x + (w - uw) / 2, y, uw, TAB_UL_H,
               vis == BV_PRESSED ? C_TEXT : C_ACCENT);
}

void wPip(int16_t cx, int16_t cy, int16_t r, uint16_t c, bool filled) {
  if (filled) tft.fillCircle(cx, cy, r, c);
  else        tft.drawCircle(cx, cy, r, c);
}
