#include "screen_int.h"

// The Settings page: 4 board-level knobs (backlight, night mode, night
// schedule, screen flip). Nothing here touches Home Assistant. See
// screen_int.h for the cross-file contract.

static const char* const BRI_LABEL[BRI_STEPS] = BRI_LABEL_LIST;

// Settings rows. Each caption says what the toggle will actually do, so the
// control is never asking about a value the user has to remember. Night
// mode has no caption of its own now — a chip row replaces it, one label per
// state, since there's no longer a single fixed effect to describe.
static const char* const SET_LABEL[SET_ROWS] = {
  "Brightness", "Night mode", "Night schedule", "Flip screen"
};
static const char* const SET_CAPTION[SET_ROWS] = {
  nullptr, nullptr, "23:45 - 08:00", "Rotate 180 degrees"
};
static const char* const NIGHT_LABEL[NIGHT_CHIPS] = { "OFF", "SHIFT", "RED" };

SettingSnap setSnap;

// Both Settings chip rows (5 brightness chips, 3 night-mode chips) share the
// device rows' chip pitch — one grid, so a control on the Settings page and a
// control on a device card are the same size and the pages read as one
// product — and differ only in which row they sit on.
Rect settingChipRect(uint8_t row, uint8_t i) {
  return { (int16_t)(CARD_IN_X0 + i * CHIP_PITCH), (int16_t)(cardTop(row) + CTL_DY),
           (int16_t)CHIP_W, (int16_t)CTL_H };
}

// Night mode is chip-driven now, not a toggle — it has no entry here.
static bool setToggleVal(uint8_t row) {
  switch (row) {
    case SET_ROW_SCHED: return S.set.nightSched;
    case SET_ROW_FLIP:  return S.set.flip;
    default:            return false;
  }
}

static void setIcon(uint8_t row, int16_t cx, int16_t cy) {
  switch (row) {
    case SET_ROW_BRI:   icoSun(cx, cy, C_TEXT2);              break;
    // The crescent's bite is painted in the CARD's fill, not C_BG.
    case SET_ROW_NIGHT: icoMoon(cx, cy, C_TEXT2, C_SURFACE);  break;
    case SET_ROW_SCHED: icoClock(cx, cy, C_TEXT2);            break;
    default:            icoRotate(cx, cy, C_TEXT2);           break;
  }
}

// Static parts of a settings card: the surface, its icon, title and caption. The
// controls repaint independently, so nothing here is redrawn once it is down.
static void drawSettingChrome(uint8_t row) {
  const int16_t top = cardTop(row);
  tft.fillRect(0, rowTop(row), SCR_W, ROW_H, C_BG);
  wCard(CARD_X, top, CARD_W, CARD_H, C_SURFACE, C_BORDER);

  if (row == SET_ROW_BRI || row == SET_ROW_NIGHT) {
    setIcon(row, CARD_ICO_CX, top + CARD_L1_CY);
    textAt(F_TITLE, SET_LABEL[row], CARD_TXT_X, top + CARD_L1_CY, ML_DATUM,
           C_TEXT);
  } else {
    // Title over caption, the pair vertically centred in the card. Two type
    // roles doing the work one size and two colours used to: weight separates
    // what the setting IS from what it will do.
    setIcon(row, CARD_ICO_CX, top + CARD_H / 2);
    textAt(F_TITLE, SET_LABEL[row],   CARD_TXT_X, top + SET_TITLE_CY, ML_DATUM,
           C_TEXT);
    textAt(F_BODY,  SET_CAPTION[row], CARD_TXT_X, top + SET_CAP_CY,   ML_DATUM,
           C_TEXT3);
  }
}

void drawSettings() {
  const bool first = !setSnap.valid;
  // SET_ROW_BRI is 0, so the brightness card shares band 0 with the connectivity
  // banner and is skipped whole while that is shown — chrome, level caption and
  // all five chips. Nothing else on this page is affected: the other three rows
  // still draw, and they are the ones that still WORK while HA is unreachable,
  // since no Settings row touches Home Assistant at all.
  const bool briHidden = noConnShown();
  if (first) {
    for (uint8_t r = 0; r < SET_ROWS; r++) {
      if (r == SET_ROW_BRI && briHidden) continue;
      drawSettingChrome(r);
    }
    setSnap.briShown = -1;
  }

  // The chosen level, named on the Brightness card's own identity line. A
  // segmented control shows WHICH of five is selected; it does not say what the
  // selection means, and "50%" spelled out is the difference between a row of
  // chips and a row of chips you can read.
  if (!briHidden && setSnap.briShown != (int8_t)S.set.briIdx) {
    setSnap.briShown = (int8_t)S.set.briIdx;
    const int16_t top = cardTop(SET_ROW_BRI);
    tft.fillRect(CARD_IN_X1 - 44, top + CARD_L1_Y, 45, CARD_L1_H, C_SURFACE);
    textAt(F_BODY, BRI_LABEL[S.set.briIdx], CARD_IN_X1, top + CARD_L1_CY,
           MR_DATUM, C_TEXT2);
  }

  for (uint8_t b = 0; b < BRI_STEPS && !briHidden; b++) {
    const uint8_t vis = pressedNow(HIT_SETTING, SET_ROW_BRI, (int8_t)b)
                            ? BV_PRESSED
                            : (b == S.set.briIdx ? BV_ACTIVE : BV_INACTIVE);
    if (!first && vis == setSnap.briVis[b]) continue;
    setSnap.briVis[b] = vis;

    const Rect r = settingChipRect(SET_ROW_BRI, b);
    wChip(r.x, r.y, r.w, r.h, BRI_LABEL[b], nullptr, vis);
  }

  // Night mode's 3-chip row, same shape as the brightness loop above.
  for (uint8_t c = 0; c < NIGHT_CHIPS; c++) {
    const uint8_t vis = pressedNow(HIT_SETTING, SET_ROW_NIGHT, (int8_t)c)
                            ? BV_PRESSED
                            : (NIGHT_CHIP_MODE[c] == S.set.nightMode ? BV_ACTIVE
                                                                     : BV_INACTIVE);
    if (!first && vis == setSnap.nightVis[c]) continue;
    setSnap.nightVis[c] = vis;

    const Rect r = settingChipRect(SET_ROW_NIGHT, c);
    wChip(r.x, r.y, r.w, r.h, NIGHT_LABEL[c], nullptr, vis);
  }

  // Toggles get no press flash: the flip IS the feedback, and it is immediate.
  for (uint8_t r = SET_ROW_NIGHT + 1; r < SET_ROWS; r++) {
    const bool on = setToggleVal(r);
    if (!first && on == setSnap.toggle[r]) continue;
    setSnap.toggle[r] = on;
    wToggle(TOGGLE_X, cardTop(r) + TOGGLE_DY, TOGGLE_W, TOGGLE_H, on);
  }

  setSnap.valid = true;
}
