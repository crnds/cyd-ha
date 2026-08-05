#include "screen.h"
#include "config.h"
#include "theme.h"
#include <TFT_eSPI.h>
#include <math.h>
#include <string.h>

// Layout (320x240, rotation 1 — the rotation the touch calibration in
// config.h was measured against; changing it will mirror every tap):
//
//   y   0..17    status bar   [WIFI dot][HA dot] ............ age
//   y  20..74    row 0        name .......... state / 6 buttons
//   y  75..129   row 1
//   y 130..184   row 2
//   y 185..239   row 3 (climate, 5 wider buttons)
//
// All geometry comes from the LAYOUT block in config.h — change those
// #defines together, not the arithmetic here.

static TFT_eSPI tft;

// Bulb buttons 4 and 5 are colour swatches (drawn as colour, not text), so
// their label slots are null. See drawSwatch().
static const char* BULB_LABEL[BULB_BTNS]     = {"OFF", "1%", "30%", "100%", nullptr, nullptr};
static const char* BULB_LABEL_ALT[BULB_BTNS] = {"OFF", "1",  "30",  "100",  nullptr, nullptr};
static const char* AC_LABEL[AC_BTNS]         = {"OFF", "AC", "DRY", "T+", "T-"};

// ── dirty-region snapshots ───────────────────────────────
// memcmp'd against freshly built values; a row only repaints when something it
// actually shows has changed. NAN bit patterns are stable, so memcmp is safe
// here even though NAN != NAN numerically.
struct RowSnap {
  bool     valid;
  bool     on;
  int      pct;
  int      kelvin;
  bool     supportsCT;
  char     mode[12];
  float    target;
  float    room;
  bool     stale;
  bool     err;
  uint8_t  activeMask;
  int8_t   pressBtn;
};
static RowSnap snap[NUM_DEVICES];

// Split into two independently-dirty halves (dots on the left, freshness text
// on the right) so a change in one never repaints the other. Repainting the
// full bar for a one-character change is what made it visibly flash.
struct StatusSnap {
  bool    valid;
  bool    wifiOk;
  bool    haOk;
  int32_t shown;   // -2 no data, -1 live, >=0 seconds stale
};
static StatusSnap statusSnap;

// ── geometry ─────────────────────────────────────────────

static inline int16_t rowTop(uint8_t dev) { return ROWS_Y0 + dev * ROW_H; }

static void btnRect(uint8_t dev, uint8_t b,
                    int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
  bool ac = (S.dev[dev].kind == DEV_CLIMATE);
  w = ac ? AC_BTN_W : BTN_W;
  h = BTN_H;
  x = BTN_X0 + b * (ac ? AC_BTN_PITCH : BTN_PITCH);
  y = rowTop(dev) + ROW_BTN_DY;
}

// ── active-state derivation ──────────────────────────────

static bool btnActive(const DeviceState& d, uint8_t b) {
  if (d.kind == DEV_CLIMATE) {
    switch (b) {
      case 0:  return strcmp(d.mode, "off") == 0;
      case 1:  return strcmp(d.mode, AC_MODE_COOL) == 0;
      case 2:  return strcmp(d.mode, AC_MODE_DRY) == 0;
      default: return false;   // T+/T- are momentary, never "current state"
    }
  }
  if (!d.on) return b == 0;    // bulb off: only OFF lit

  // A brightness button AND a colour swatch can both be active — they are
  // independent axes of an on-bulb, not mutually exclusive choices.
  switch (b) {
    case 1:  return d.pct >= 0 && d.pct <= PCT_LOW_MAX;
    case 2:  return d.pct >= PCT_MID_MIN && d.pct <= PCT_MID_MAX;
    case 3:  return d.pct >= PCT_HIGH_MIN;
    case 4:  return d.supportsCT && d.kelvin > 0 && d.kelvin <= KELVIN_WARM_MAX;
    case 5:  return d.supportsCT && d.kelvin >= KELVIN_COOL_MIN;
    default: return false;
  }
}

static uint8_t activeMask(const DeviceState& d) {
  uint8_t m = 0;
  for (uint8_t b = 0; b < btnCount(d); b++)
    if (btnActive(d, b)) m |= (1u << b);
  return m;
}

// ── drawing helpers ──────────────────────────────────────

// Button labels must never clip, and Font 2's width varies per glyph, so try
// progressively narrower renderings rather than trusting a hand-measured fit:
// Font 2 label -> Font 2 short form ("100" for "100%") -> Font 1.
static void drawFittedLabel(const char* primary, const char* fallback,
                            int16_t cx, int16_t cy, int16_t maxW, uint16_t fg) {
  tft.setTextColor(fg);
  tft.setTextDatum(MC_DATUM);

  tft.setTextFont(2);
  if (tft.textWidth(primary) <= maxW) { tft.drawString(primary, cx, cy); return; }
  if (fallback && tft.textWidth(fallback) <= maxW) { tft.drawString(fallback, cx, cy); return; }

  tft.setTextFont(1);
  tft.drawString(primary, cx, cy);
}

static void drawTextButton(int16_t x, int16_t y, int16_t w, int16_t h,
                           const char* label, const char* labelAlt,
                           bool active, bool pressed) {
  uint16_t fill, edge, fg;
  if (pressed) {
    // brief bright inversion so the tap is felt before HA has answered
    fill = C_TEXT;  edge = C_TEXT;  fg = C_BG;
  } else if (active) {
    // Dark label on the accent fill, not white: HA cyan is light enough that
    // white-on-cyan measures ~1.9:1 contrast while dark-on-cyan is ~9:1.
    fill = C_ACCENT; edge = C_ACCENT; fg = C_BG;
  } else {
    fill = C_SURFACE; edge = C_BORDER; fg = C_TEXT2;
  }
  tft.fillRoundRect(x, y, w, h, 4, fill);
  tft.drawRoundRect(x, y, w, h, 4, edge);
  drawFittedLabel(label, labelAlt, x + w / 2, y + h / 2, w - 6, fg);
}

// Colour-temperature swatch. The bulbs are white-spectrum (no RGB), so these
// two buttons are the ends of the colour-temp range rather than real hues —
// rendering them AS the colour is what makes that legible without a label.
static void drawSwatch(int16_t x, int16_t y, int16_t w, int16_t h,
                       uint16_t colour, const char* kLabel,
                       bool active, bool pressed, bool disabled) {
  uint16_t fill, edge, fg;
  if (disabled) {
    fill = C_BG;            edge = C_BORDER; fg = C_MUTED;
  } else if (pressed) {
    fill = C_TEXT;          edge = C_TEXT;   fg = C_BG;
  } else if (active) {
    fill = colour;          edge = colour;   fg = C_BG;
  } else {
    fill = tint565(colour); edge = C_BORDER; fg = colour;
  }
  tft.fillRoundRect(x, y, w, h, 4, fill);
  tft.drawRoundRect(x, y, w, h, 4, edge);

  tft.setTextFont(1);
  tft.setTextColor(fg);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(disabled ? "n/a" : kLabel, x + w / 2, y + h / 2);
}

// ── state text (right side of each row's top line) ───────

static const char* prettyMode(const char* m) {
  if (!strcmp(m, "off"))      return "OFF";
  if (!strcmp(m, "cool"))     return "COOL";
  if (!strcmp(m, "dry"))      return "DRY";
  if (!strcmp(m, "heat"))     return "HEAT";
  if (!strcmp(m, "fan_only")) return "FAN";
  if (!strcmp(m, "auto"))     return "AUTO";
  if (!strcmp(m, "unavailable")) return "UNAVAIL";
  return m[0] ? m : "?";
}

static void stateText(const DeviceState& d, char* out, size_t n) {
  if (!d.known) { snprintf(out, n, "--"); return; }

  if (d.kind == DEV_CLIMATE) {
    char t[16] = "--";
    if (!isnan(d.target)) snprintf(t, sizeof(t), "%.0f", d.target);
    if (!isnan(d.room))
      snprintf(out, n, "%s  set %s  room %.0f", prettyMode(d.mode), t, d.room);
    else
      snprintf(out, n, "%s  set %s", prettyMode(d.mode), t);
    return;
  }

  if (!d.on) { snprintf(out, n, "OFF"); return; }
  if (d.pct >= 0) {
    if (d.supportsCT && d.kelvin > 0) snprintf(out, n, "%d%%  %dK", d.pct, d.kelvin);
    else                              snprintf(out, n, "%d%%", d.pct);
  } else {
    snprintf(out, n, "ON");
  }
}

// ── rows ─────────────────────────────────────────────────

static void drawRow(uint8_t dev, bool force) {
  DeviceState& d = S.dev[dev];
  uint32_t now = millis();

  bool stale = d.known && (now - d.okMs > DEVICE_STALE_MS);
  bool err   = d.errMs && (now - d.errMs < 1500);

  int8_t press = (S.pressDev == (int8_t)dev && S.pressMs &&
                  now - S.pressMs < PRESS_FLASH_MS) ? S.pressBtn : -1;

  RowSnap cur;
  memset(&cur, 0, sizeof(cur));          // zero padding so memcmp is meaningful
  cur.valid      = true;
  cur.on         = d.on;
  cur.pct        = d.pct;
  cur.kelvin     = d.kelvin;
  cur.supportsCT = d.supportsCT;
  memcpy(cur.mode, d.mode, sizeof(cur.mode));
  cur.target     = d.target;
  cur.room       = d.room;
  cur.stale      = stale;
  cur.err        = err;
  cur.activeMask = activeMask(d);
  cur.pressBtn   = press;

  if (!force && memcmp(&cur, &snap[dev], sizeof(cur)) == 0) return;
  snap[dev] = cur;

  const int16_t top = rowTop(dev);

  // ── top line: name (left) + state (right) ──
  // GFX/GLCD fonts don't paint their own background, so clear first.
  tft.fillRect(0, top + ROW_LABEL_DY, SCR_W, 10, C_BG);
  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(stale ? C_DIM : C_TEXT);
  tft.drawString(d.name, BTN_X0, top + ROW_LABEL_DY);

  char st[48];
  stateText(d, st, sizeof(st));
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(err ? C_RED : (stale ? C_DIM : C_TEXT2));
  tft.drawString(st, SCR_W - BTN_X0, top + ROW_LABEL_DY);

  // ── button strip ──
  for (uint8_t b = 0; b < btnCount(d); b++) {
    int16_t x, y, w, h;
    btnRect(dev, b, x, y, w, h);
    bool active  = (cur.activeMask >> b) & 1u;
    bool pressed = (press == (int8_t)b);

    if (d.kind == DEV_CLIMATE) {
      drawTextButton(x, y, w, h, AC_LABEL[b], nullptr, active, pressed);
    } else if (b == 4) {
      drawSwatch(x, y, w, h, C_WARM, "2200K", active, pressed, !d.supportsCT);
    } else if (b == 5) {
      drawSwatch(x, y, w, h, C_COOL, "4000K", active, pressed, !d.supportsCT);
    } else {
      drawTextButton(x, y, w, h, BULB_LABEL[b], BULB_LABEL_ALT[b], active, pressed);
    }
  }

  // separator above every row but the first
  if (dev > 0) tft.drawFastHLine(0, top - 2, SCR_W, C_BORDER);
}

// ── status bar ───────────────────────────────────────────

static void drawDot(int16_t x, int16_t y, bool ok, const char* label) {
  tft.fillCircle(x, y, 3, ok ? C_GREEN : C_RED);
  tft.setTextFont(1);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(ok ? C_TEXT2 : C_RED);
  tft.drawString(label, x + 7, y);
}

#define STATUS_DOTS_W  108   // left region: the two connectivity dots
#define STATUS_AGE_W   108   // right region: the freshness readout

static void drawStatus(bool force) {
  uint32_t now  = millis();
  bool wifiOk   = (S.netState == 1);
  int32_t age   = S.haOkMs ? (int32_t)((now - S.haOkMs) / 1000) : -1;

  // Don't surface a live-counting age at all. Every successful poll resets
  // haOkMs, so while polling is healthy the number just oscillates 0<->1 and
  // tells you nothing you can't read from the HA dot. Only once data actually
  // goes stale does the elapsed time become worth showing — and by then it
  // increments at a sane 1 Hz instead of thrashing at the poll rate.
  int32_t shown = (age < 0) ? -2 : (age >= 5 ? age : -1);

  bool force_ = force || !statusSnap.valid;

  if (force_ || wifiOk != statusSnap.wifiOk || S.haOk != statusSnap.haOk) {
    tft.fillRect(0, 0, STATUS_DOTS_W, STATUS_H, C_BG);
    drawDot(10, STATUS_H / 2, wifiOk, "WIFI");
    drawDot(60, STATUS_H / 2, S.haOk, "HA");
  }

  if (force_ || shown != statusSnap.shown) {
    tft.fillRect(SCR_W - STATUS_AGE_W, 0, STATUS_AGE_W, STATUS_H, C_BG);
    char right[24];
    uint16_t fg;
    if (shown == -2)      { snprintf(right, sizeof(right), "no data");  fg = C_DIM; }
    else if (shown == -1) { snprintf(right, sizeof(right), "LIVE");     fg = C_TEXT2; }
    else if (shown < 100) { snprintf(right, sizeof(right), "%lds ago", (long)shown); fg = C_DIM; }
    else                  { snprintf(right, sizeof(right), "%ldm ago", (long)(shown / 60)); fg = C_DIM; }

    tft.setTextFont(1);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(fg);
    tft.drawString(right, SCR_W - BTN_X0, STATUS_H / 2);
  }

  // Sits at y == STATUS_H, outside both fillRects above, so it survives their
  // clears and only needs painting once.
  if (force_) tft.drawFastHLine(0, STATUS_H, SCR_W, C_BORDER);

  statusSnap = { true, wifiOk, S.haOk, shown };
}

// ── public API ───────────────────────────────────────────

void screenBegin() {
  tft.init();
  // rotation 1 == the orientation config.h's TOUCH_* calibration assumes
  tft.setRotation(1);
  tft.fillScreen(C_BG);
  screenInvalidate();
}

void screenInvalidate() {
  memset(snap, 0, sizeof(snap));
  memset(&statusSnap, 0, sizeof(statusSnap));
}

void screenRender() {
  drawStatus(false);
  for (uint8_t i = 0; i < NUM_DEVICES; i++) drawRow(i, false);
}

void screenMessage(const char* line1, const char* line2) {
  tft.fillScreen(C_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(4);
  tft.setTextColor(C_TEXT);
  tft.drawString(line1, SCR_W / 2, SCR_H / 2 - 14);
  if (line2) {
    tft.setTextFont(2);
    tft.setTextColor(C_TEXT2);
    tft.drawString(line2, SCR_W / 2, SCR_H / 2 + 14);
  }
  screenInvalidate();
}

void screenCalibTarget(int idx, int total, int16_t x, int16_t y) {
  tft.fillScreen(C_BG);

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(C_TEXT2);
  char msg[40];
  snprintf(msg, sizeof(msg), "TOUCH CALIBRATION  %d / %d", idx + 1, total);
  tft.drawString(msg, SCR_W / 2, SCR_H / 2 - 12);
  tft.setTextColor(C_MUTED);
  tft.drawString("tap the centre of the crosshair", SCR_W / 2, SCR_H / 2 + 8);

  // Crosshair drawn last so it sits over the text if they overlap.
  tft.drawFastHLine(x - 14, y, 29, C_ACCENT);
  tft.drawFastVLine(x, y - 14, 29, C_ACCENT);
  tft.drawCircle(x, y, 9, C_ACCENT);
  tft.fillCircle(x, y, 2, C_TEXT);
}

void screenCalibVerifyScreen() {
  tft.fillScreen(C_BG);
  // Draw the real button grid outlines so a tap can be judged against the
  // actual targets, not an abstract coordinate.
  for (uint8_t d = 0; d < NUM_DEVICES; d++) {
    bool ac = (d == NUM_DEVICES - 1);
    uint8_t n = ac ? AC_BTNS : BULB_BTNS;
    for (uint8_t b = 0; b < n; b++) {
      int16_t w = ac ? AC_BTN_W : BTN_W;
      int16_t x = BTN_X0 + b * (ac ? AC_BTN_PITCH : BTN_PITCH);
      tft.drawRoundRect(x, ROWS_Y0 + d * ROW_H + ROW_BTN_DY, w, BTN_H, 4, C_BORDER);
    }
  }
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  tft.setTextColor(C_MUTED);
  tft.drawString("VERIFY: tap boxes, dot should land inside", BTN_X0, 4);
}

void screenCalibDot(int16_t x, int16_t y) {
  tft.fillCircle(x, y, 3, C_ACCENT);
}

bool screenHitTest(int16_t px, int16_t py, int8_t& devIdx, int8_t& btnIdx) {
  if (py < ROWS_Y0) return false;
  int i = (py - ROWS_Y0) / ROW_H;
  if (i < 0 || i >= NUM_DEVICES) return false;

  const DeviceState& d = S.dev[i];
  for (uint8_t b = 0; b < btnCount(d); b++) {
    int16_t x, y, w, h;
    btnRect(i, b, x, y, w, h);
    // Vertically the whole row band counts as the button strip: this is a
    // bedroom device often used in the dark, so targets are 55px not 36px.
    if (px >= x && px < x + w && py >= rowTop(i) && py < rowTop(i) + ROW_H) {
      devIdx = (int8_t)i;
      btnIdx = (int8_t)b;
      return true;
    }
  }
  return false;
}
