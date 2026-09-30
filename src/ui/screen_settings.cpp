#include "screen_int.h"

// The Settings page: 5 board-level knobs on 4 rows (backlight, night mode,
// beep volume, then night schedule and screen flip side by side). Nothing here
// touches Home Assistant. See screen_int.h for the cross-file contract.

static const char* const BRI_LABEL[BRI_STEPS] = BRI_LABEL_LIST;
static const char* const VOL_LABEL[VOL_STEPS] = VOL_LABEL_LIST;

// The three chip rows' titles, by row — sized SET_ROW_TGL because every row
// above it is a chip row. The toggle cards are titled separately: they share
// one row, so a per-row table cannot name them.
static const char* const SET_LABEL[SET_ROW_TGL] = {
  "Brightness", "Night mode", "Volume"
};
// Short on purpose. A half-width card leaves "Schedule" 61px (see the toggle
// block in config.h), and "Night schedule" / "Flip screen" are both over it.
// The captions that used to say what each toggle does went for the same
// reason — the pairing that made room for Volume costs exactly this.
static const char* const TGL_LABEL[SET_TGLS] = { "Schedule", "Flip" };
static const char* const NIGHT_LABEL[NIGHT_CHIPS] = { "OFF", "SHIFT", "RED" };

SettingSnap setSnap;

// All three Settings chip rows (5 brightness, 3 night-mode, 4 volume chips)
// share one chip pitch, so a control on any of them is the same size, and
// differ only in which row they sit on.
Rect settingChipRect(uint8_t row, uint8_t i) {
  return { (int16_t)(CARD_IN_X0 + i * CHIP_PITCH), (int16_t)(cardTop(row) + CTL_DY),
           (int16_t)CHIP_W, (int16_t)CTL_H };
}

static int16_t tglCardX(uint8_t t) { return (int16_t)(CARD_X + t * TGL_CARD_PITCH); }

static bool tglVal(uint8_t t) {
  return t == SET_TGL_SCHED ? S.set.nightSched : S.set.flip;
}

static void setIcon(uint8_t row, int16_t cx, int16_t cy) {
  switch (row) {
    case SET_ROW_BRI:   icoSun(cx, cy, C_TEXT2);              break;
    // The crescent's bite is painted in the CARD's fill, not C_BG.
    case SET_ROW_NIGHT: icoMoon(cx, cy, C_TEXT2, C_SURFACE);  break;
    // Volume's icon is NOT chrome: it shows the level, so drawSettings()
    // repaints it with the level label rather than drawing it once here.
    default:                                                  break;
  }
}

// Static parts of a settings row: the surface(s), icons and titles. The
// controls — and Volume's icon — repaint independently, so nothing here is
// redrawn once it is down.
static void drawSettingChrome(uint8_t row) {
  const int16_t top = cardTop(row);
  tft.fillRect(0, rowTop(row), SCR_W, ROW_H, C_BG);

  if (row == SET_ROW_TGL) {
    // Two half-width cards, each an icon, a one-line title and (drawn by
    // drawSettings()) a toggle. Offsets are the full card's CARD_ICO_CX /
    // CARD_TXT_X, taken from each half card's own left edge.
    for (uint8_t t = 0; t < SET_TGLS; t++) {
      const int16_t x = tglCardX(t);
      wCard(x, top, TGL_CARD_W, CARD_H, C_SURFACE, C_BORDER);
      const int16_t cy = top + CARD_H / 2;
      if (t == SET_TGL_SCHED) icoClock(x + (CARD_ICO_CX - CARD_X), cy, C_TEXT2);
      else                    icoRotate(x + (CARD_ICO_CX - CARD_X), cy, C_TEXT2);
      textAt(F_TITLE, TGL_LABEL[t], x + (CARD_TXT_X - CARD_X), cy, ML_DATUM,
             C_TEXT);
    }
    return;
  }

  wCard(CARD_X, top, CARD_W, CARD_H, C_SURFACE, C_BORDER);
  setIcon(row, CARD_ICO_CX, top + CARD_L1_CY);
  textAt(F_TITLE, SET_LABEL[row], CARD_TXT_X, top + CARD_L1_CY, ML_DATUM,
         C_TEXT);
}

// The chosen level named on a chip card's own identity line, right-aligned. A
// segmented control shows WHICH step is selected; it does not say what the
// selection means, and "50%" spelled out is the difference between a row of
// chips and a row of chips you can read. Brightness and Volume share this.
static void drawLevelLabel(uint8_t row, const char* label) {
  const int16_t top = cardTop(row);
  tft.fillRect(CARD_IN_X1 - 44, top + CARD_L1_Y, 45, CARD_L1_H, C_SURFACE);
  textAt(F_BODY, label, CARD_IN_X1, top + CARD_L1_CY, MR_DATUM, C_TEXT2);
}

// One segmented chip row. `sel` is the chip to highlight.
static void drawChipRow(uint8_t row, uint8_t n, const char* const* labels,
                        uint8_t sel, uint8_t* snap, bool first) {
  for (uint8_t c = 0; c < n; c++) {
    const uint8_t vis = pressedNow(HIT_SETTING, row, (int8_t)c)
                            ? BV_PRESSED
                            : (c == sel ? BV_ACTIVE : BV_INACTIVE);
    if (!first && vis == snap[c]) continue;
    snap[c] = vis;

    const Rect r = settingChipRect(row, c);
    wChip(r.x, r.y, r.w, r.h, labels[c], nullptr, vis);
  }
}

void drawSettings() {
  const bool first = !setSnap.valid;
  // SET_ROW_BRI is 0, so the brightness card shares band 0 with the connectivity
  // banner and is skipped whole while that is shown — chrome, level caption and
  // all five chips. Nothing else on this page is affected: the other rows still
  // draw, and they are the ones that still WORK while HA is unreachable, since
  // no Settings row touches Home Assistant at all.
  const bool briHidden = noConnShown();
  if (first) {
    for (uint8_t r = 0; r < SET_ROWS; r++) {
      if (r == SET_ROW_BRI && briHidden) continue;
      drawSettingChrome(r);
    }
    setSnap.briShown = -1;
    setSnap.volShown = -1;
  }

  if (!briHidden && setSnap.briShown != (int8_t)S.set.briIdx) {
    setSnap.briShown = (int8_t)S.set.briIdx;
    drawLevelLabel(SET_ROW_BRI, BRI_LABEL[S.set.briIdx]);
  }
  if (!briHidden)
    drawChipRow(SET_ROW_BRI, BRI_STEPS, BRI_LABEL, S.set.briIdx, setSnap.briVis,
                first);

  // Night mode's chips highlight by MODE, not by position — the chip order
  // (Off, Shift, Red) is not NightMode's storage order.
  uint8_t nightSel = 0;
  for (uint8_t c = 0; c < NIGHT_CHIPS; c++)
    if (NIGHT_CHIP_MODE[c] == S.set.nightMode) nightSel = c;
  drawChipRow(SET_ROW_NIGHT, NIGHT_CHIPS, NIGHT_LABEL, nightSel, setSnap.nightVis,
              first);

  // Volume: the label AND the icon both show the level, so both live in this
  // one region and repaint on this one compare. The icon's clear rect is the
  // 15px icon column (CARD_ICO_CX +-7) over the same line-1 band the label
  // uses, which stops before the title at CARD_TXT_X.
  if (setSnap.volShown != (int8_t)S.set.volIdx) {
    setSnap.volShown = (int8_t)S.set.volIdx;
    const int16_t top = cardTop(SET_ROW_VOL);
    tft.fillRect(CARD_ICO_CX - 7, top + CARD_L1_Y, 15, CARD_L1_H, C_SURFACE);
    icoSpeaker(CARD_ICO_CX, top + CARD_L1_CY, C_TEXT2, S.set.volIdx);
    drawLevelLabel(SET_ROW_VOL, VOL_LABEL[S.set.volIdx]);
  }
  drawChipRow(SET_ROW_VOL, VOL_STEPS, VOL_LABEL, S.set.volIdx, setSnap.volVis,
              first);

  // Toggles get no press flash: the flip IS the feedback, and it is immediate.
  for (uint8_t t = 0; t < SET_TGLS; t++) {
    const bool on = tglVal(t);
    if (!first && on == setSnap.toggle[t]) continue;
    setSnap.toggle[t] = on;
    wToggle(tglCardX(t) + TOGGLE_DX, cardTop(SET_ROW_TGL) + TOGGLE_DY, TOGGLE_W,
            TOGGLE_H, on);
  }

  setSnap.valid = true;
}
