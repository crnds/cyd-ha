#include "screen.h"
#include "config.h"
#include "theme.h"
#include "logo_ha.h"
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

// Swatch captions are derived from the constants they actually send, so the
// label can never drift from the value (it already had: the label read "2200K"
// while KELVIN_WARM was corrected to the bulbs' real 2202 K limit).
#define STRINGIFY_(x) #x
#define STRINGIFY(x)  STRINGIFY_(x)
#define KLABEL_WARM   STRINGIFY(KELVIN_WARM) "K"
#define KLABEL_COOL   STRINGIFY(KELVIN_COOL) "K"

// ── dirty-region snapshots ───────────────────────────────
// A row is tracked as two independent regions: the name/state text line, and
// each button separately. Previously any single change repainted the whole row
// including all 6 buttons; now a brightness change repaints only the 2 buttons
// whose appearance actually differs.
//
// Buttons are compared by their *visual* state rather than by the underlying
// values, which is both cheaper and exactly right — two different brightness
// values that map to the same highlight need no repaint.
enum BtnVis : uint8_t { BV_INACTIVE = 0, BV_ACTIVE, BV_PRESSED, BV_DISABLED };

struct RowSnap {
  bool    valid;
  char    stateStr[48];          // last rendered state text
  bool    stale;
  bool    err;
  uint8_t btnVis[BULB_BTNS];     // BULB_BTNS >= AC_BTNS, so this covers both
};
static RowSnap snap[NUM_DEVICES];

// Split into two independently-dirty halves (dots on the left, freshness text
// on the right) so a change in one never repaints the other. Repainting the
// full bar for a one-character change is what made it visibly flash.
struct StatusSnap {
  bool    valid;
  bool    wifiOk;
  bool    haOk;
  int16_t hhmm;    // local time as hour*60+min; -1 while NTP is unsynced
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
  // An unreachable device has no current state to highlight. Lighting OFF here
  // would claim the bulb is off when we simply cannot see it.
  if (!d.avail) return false;

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
                           bool active, bool pressed, bool disabled) {
  uint16_t fill, edge, fg;
  if (disabled) {
    fill = C_BG;    edge = C_BORDER; fg = C_MUTED;
  } else if (pressed) {
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
  // Say so explicitly rather than letting an unreachable device read as OFF.
  if (!d.avail) { snprintf(out, n, "UNAVAILABLE"); return; }

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

  RowSnap&      sn    = snap[dev];
  const bool    first = force || !sn.valid;
  const int16_t top   = rowTop(dev);

  // On a first/forced draw, clear the ENTIRE row band. The per-region clears
  // below only cover the 10px label strip and the button rectangles themselves,
  // which leaves the 4px bands above and below the button strip, the gaps
  // between buttons, and the side margins holding whatever was underneath.
  // screenMessage()'s boot text ("Connecting" / "Wi-Fi") sits at y~93-142, so
  // fragments of it survived in those slivers and stayed on screen for good.
  // Starts at top-1 to catch the row above ROWS_Y0; the separator at top-2 is
  // drawn after this, so it survives.
  if (first) tft.fillRect(0, top - 1, SCR_W, ROW_H + 1, C_BG);

  // ── region 1: name (left) + state (right) ──
  char st[48];
  stateText(d, st, sizeof(st));
  if (first || stale != sn.stale || err != sn.err || strcmp(st, sn.stateStr) != 0) {
    // GFX/GLCD fonts don't paint their own background, so clear first. This
    // rect spans y..y+10 only, well clear of the button strip at top+ROW_BTN_DY.
    tft.fillRect(0, top + ROW_LABEL_DY, SCR_W, 10, C_BG);
    tft.setTextFont(1);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(stale ? C_DIM : C_TEXT);
    tft.drawString(d.name, BTN_X0, top + ROW_LABEL_DY);

    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(err || !d.avail ? C_RED : (stale ? C_DIM : C_TEXT2));
    tft.drawString(st, SCR_W - BTN_X0, top + ROW_LABEL_DY);

    snprintf(sn.stateStr, sizeof(sn.stateStr), "%s", st);
    sn.stale = stale;
    sn.err   = err;
  }

  // ── region 2: each button, independently dirty ──
  const uint8_t n = btnCount(d);
  for (uint8_t b = 0; b < n; b++) {
    bool isSwatch = (d.kind == DEV_LIGHT && (b == 4 || b == 5));
    bool disabled = !d.avail || (isSwatch && !d.supportsCT);

    uint8_t vis = BV_INACTIVE;
    if (disabled)                 vis = BV_DISABLED;
    else if (press == (int8_t)b)  vis = BV_PRESSED;
    else if (btnActive(d, b))     vis = BV_ACTIVE;

    if (!first && vis == sn.btnVis[b]) continue;
    sn.btnVis[b] = vis;

    int16_t x, y, w, h;
    btnRect(dev, b, x, y, w, h);
    bool active  = (vis == BV_ACTIVE);
    bool pressed = (vis == BV_PRESSED);

    if (d.kind == DEV_CLIMATE) {
      drawTextButton(x, y, w, h, AC_LABEL[b], nullptr, active, pressed, disabled);
    } else if (b == 4) {
      drawSwatch(x, y, w, h, C_WARM, KLABEL_WARM, active, pressed, disabled);
    } else if (b == 5) {
      drawSwatch(x, y, w, h, C_COOL, KLABEL_COOL, active, pressed, disabled);
    } else {
      drawTextButton(x, y, w, h, BULB_LABEL[b], BULB_LABEL_ALT[b],
                     active, pressed, disabled);
    }
  }

  // separator above every row but the first; nothing else ever clears it
  if (first && dev > 0) tft.drawFastHLine(0, top - 2, SCR_W, C_BORDER);
  sn.valid = true;
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
#define STATUS_CLK_W   108   // right region: the clock

static void drawStatus(bool force) {
  bool wifiOk = (S.netState == 1);

  // Local wall clock, 24h. getLocalTime with a 0 ms timeout returns immediately
  // — it must never block, since this runs on every render pass. It reports
  // false until SNTP has landed, which is what drives the "--:--" placeholder.
  int16_t hhmm = -1;
  struct tm tmv;
  if (getLocalTime(&tmv, 0)) hhmm = (int16_t)(tmv.tm_hour * 60 + tmv.tm_min);

  bool force_ = force || !statusSnap.valid;

  // Same gap problem as the rows: the two half-width clears below leave
  // x=STATUS_DOTS_W..SCR_W-STATUS_CLK_W uncleared forever. Nothing draws there
  // today, but a leftover from screenSplash() would be permanent.
  if (force_) tft.fillRect(0, 0, SCR_W, STATUS_H, C_BG);

  if (force_ || wifiOk != statusSnap.wifiOk || S.haOk != statusSnap.haOk) {
    tft.fillRect(0, 0, STATUS_DOTS_W, STATUS_H, C_BG);
    drawDot(10, STATUS_H / 2, wifiOk, "WIFI");
    drawDot(60, STATUS_H / 2, S.haOk, "HA");
  }

  // Repaints once a minute. Nothing else lives in this region, so a minute-rate
  // repaint of 108x18 px is invisible — unlike the per-second freshness counter
  // that used to live here and made the whole bar flicker.
  if (force_ || hhmm != statusSnap.hhmm) {
    tft.fillRect(SCR_W - STATUS_CLK_W, 0, STATUS_CLK_W, STATUS_H, C_BG);
    char clk[8];
    if (hhmm < 0) snprintf(clk, sizeof(clk), "--:--");
    else          snprintf(clk, sizeof(clk), "%02d:%02d", hhmm / 60, hhmm % 60);

    tft.setTextFont(2);                    // readable across a dark room
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(hhmm < 0 ? C_DIM : C_TEXT);
    tft.drawString(clk, SCR_W - BTN_X0, STATUS_H / 2);
  }

  // Sits at y == STATUS_H, outside both fillRects above, so it survives their
  // clears and only needs painting once.
  if (force_) tft.drawFastHLine(0, STATUS_H, SCR_W, C_BORDER);

  statusSnap = { true, wifiOk, S.haOk, hhmm };
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

// Decodes the RLE logo a row at a time. A full 96x96 RGB565 buffer would be
// 18 KB, far past the task stack, so this keeps one 192-byte row and blits it.
// Runs may cross row boundaries, hence the run counter living outside the loop.
static void drawLogo(int16_t ox, int16_t oy) {
  uint16_t row[LOGO_HA_W];
  size_t  ri  = 0;
  uint8_t idx = 0, run = 0;

  for (int16_t y = 0; y < LOGO_HA_H; y++) {
    for (int16_t x = 0; x < LOGO_HA_W; x++) {
      if (run == 0 && ri + 1 < LOGO_HA_RLE_LEN) {
        idx = LOGO_HA_RLE[ri++];
        run = LOGO_HA_RLE[ri++];
      }
      row[x] = LOGO_HA_PAL[idx];
      if (run) run--;
    }
    tft.pushImage(ox, oy + y, LOGO_HA_W, 1, row);
  }
}

void screenSplash() {
  tft.fillScreen(C_BG);
  // Logo only, exactly centred. Both boot states (connecting and Wi-Fi portal)
  // render identically — see the note in screen.h about what that costs.
  drawLogo((SCR_W - LOGO_HA_W) / 2, (SCR_H - LOGO_HA_H) / 2);
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
