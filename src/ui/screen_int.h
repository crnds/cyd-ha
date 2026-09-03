#pragma once
#include "screen.h"
#include "config.h"
#include "gfx.h"
#include "icons.h"
#include "theme.h"
#include "widgets.h"

// Internal split of screen.cpp: screen.cpp itself keeps lifecycle
// (screenBegin/screenRender/screenInvalidate), the header/tab strip, the
// connectivity overlay, hit-testing, the splash and calibration screens —
// everything cross-page. Each page's drawing lives in its own translation
// unit (screen_devices.cpp / screen_scenes.cpp / screen_settings.cpp) and
// OWNS that page's geometry function (btnRect/settingChipRect/sceneTileX,Y);
// screen.cpp calls across the boundary for hit-testing and the calibration
// verify screen, which is why those are declared here rather than staying
// `static` the way they were in the one-file version.
//
// This header is NOT the public API — screen.h is. It exists so the four
// .cpp files can share types, dirty-region snapshot storage, and small
// derivation helpers without duplicating any of them.

// ── geometry ─────────────────────────────────────────────
struct Rect { int16_t x, y, w, h; };

// Takes a row SLOT, not a device index — the Settings page reuses this grid.
static inline int16_t rowTop(uint8_t slot)  { return ROWS_Y0 + slot * ROW_H; }
static inline int16_t cardTop(uint8_t slot) { return rowTop(slot) + CARD_DY; }

// Defined in the .cpp that owns the page each describes; declared here so
// screen.cpp's hit-test and calibration-verify screen can reach them too.
Rect    btnRect(uint8_t dev, uint8_t b);          // screen_devices.cpp
Rect    settingChipRect(uint8_t row, uint8_t i);  // screen_settings.cpp
int16_t sceneTileX(uint8_t slot);                 // screen_scenes.cpp
int16_t sceneTileY(uint8_t slot);                 // screen_scenes.cpp

// ── time-derived / shared predicates ────────────────────
// One place that knows the press-flash timing rule. pressKind is what keeps it
// page-safe — matching on the index alone would light row 2 on the Devices page
// when scene 2 was tapped.
static inline bool pressedNow(HitKind k, int16_t idx, int8_t sub) {
  return S.pressKind == k && S.pressIdx == idx && S.pressSub == sub &&
         S.pressMs && (millis() - S.pressMs) < PRESS_FLASH_MS;
}

// One place that knows the other two time-derived flags: a poll gone quiet
// (see DEVICE_STALE_MS) and a service call's red flash (see ERR_FLASH_MS).
static inline void staleErr(const DeviceState& d, uint32_t now, bool& stale, bool& err) {
  stale = d.known && (now - d.okMs > DEVICE_STALE_MS);
  err   = d.errMs && (now - d.errMs < ERR_FLASH_MS);
}

// The alarm/stale/normal text-colour ladder, shared by the identity name and
// (the two-argument shape of) the AC setpoint: red outranks everything, a
// stale-but-otherwise-fine reading dims, anything else gets its caller's
// normal colour.
static inline uint16_t alarmFg(bool alarm, bool stale, uint16_t base) {
  return alarm ? C_ERROR : (stale ? C_DIM : base);
}

// True while the connectivity banner OWNS row band 0, so every page's draw
// function skips its first row (see drawNoConn() in screen.cpp).
static inline bool noConnShown() { return !S.haOk; }

// Round-trip tolerances, factored out so a scene tile and the row chip it
// corresponds to can never disagree about what "30%" means. HA rounds
// brightness_pct through a 0-255 byte, so exact compares would never match.
static inline bool pctMatches(int want, int got) {
  if (want >= BRI_HIGH) return got >= PCT_HIGH_MIN;
  if (want >= BRI_MID)  return got >= PCT_MID_MIN && got <= PCT_MID_MAX;
  return got >= 0 && got <= PCT_LOW_MAX;
}
static inline bool kelvinMatches(int want, int got) {
  return (want <= KELVIN_WARM_MAX) ? (got > 0 && got <= KELVIN_WARM_MAX)
                                   : (got >= KELVIN_COOL_MIN);
}

// ── cross-file dirty-region snapshots ───────────────────
// Each page keeps its own snapshot, defined (not `static`) in that page's
// .cpp so screen.cpp's screenInvalidate()/bodyReset() can memset it directly
// — the same "one shared reset path" the one-file version had, just across a
// TU boundary now. drawStatus()/drawStatusRoom()/drawNoConn() are cross-page
// (part of the header/overlay, not any one page), so StatusSnap and
// NoConnSnap stay private to screen.cpp — nothing here needs them.
//
// EVERY PAGE SNAPSHOT MUST KEEP A PER-ITEM BtnVis BYTE, and that is
// load-bearing rather than an optimisation: the press flash expires by TIME,
// not by any state change, so only a per-item vis compare notices
// BV_PRESSED -> BV_ACTIVE and repaints. A page that skips it leaves the
// tapped control inverted forever. Each snapshot also needs its own `valid`
// flag on top, because BV_INACTIVE is 0 and a memset alone reads as "already
// drawn as inactive".

// Raw-input fingerprint for drawDeviceCard()'s pre-filter (see there): every
// field the function's drawing decisions read, exactly as read, before any
// derivation. If this is bit-identical to last pass the render inputs cannot
// have changed, so the whole function can return before doing any work at
// all. Floats are compared by BIT PATTERN via memcmp over the whole struct,
// not by == (d.target can be NaN, and NaN != NaN) — see drawDeviceCard for
// the full rationale.
struct CardFingerprint {
  bool   on, avail, known, supportsCT;
  int    pct, kelvin;
  float  target;
  char   mode[sizeof(DeviceState::mode)];
  bool   stale, err, alarm;
  int8_t press;
};

struct RowSnap {
  bool     valid;
  CardFingerprint fp;            // see above; compared before anything else
  char     tempStr[8];          // AC row only: last rendered setpoint
  bool     stale;
  bool     err;
  bool     acOff;                // AC row only: mode == "off", greys the setpoint
  bool     alarm;               // err OR offline — what the card's border shows
  uint16_t icoColour;
  uint8_t  icoShape;
  uint8_t  btnVis[BULB_BTNS];   // BULB_BTNS == AC_BTNS, so this covers both
};
extern RowSnap snap[NUM_DEVICES];   // screen_devices.cpp

// Sized per visible TILE rather than per scene, which is the whole reason
// Scenes can scale: 9 bytes whether the table holds 5 scenes or 500.
struct SceneSnap {
  bool     valid;
  uint16_t row;                     // scroll offset these slots were drawn at
  uint8_t  vis[SCENE_PER_PAGE];
  uint8_t  sbVis;                   // gutter: 0 idle, 1 up held, 2 down held
};
extern SceneSnap sceneSnap;         // screen_scenes.cpp

struct SettingSnap {
  bool    valid;
  uint8_t briVis[BRI_STEPS];
  int8_t  briShown;             // level named on the Brightness card's own line
  uint8_t nightVis[NIGHT_CHIPS];
  bool    toggle[SET_ROWS];     // indices SET_ROW_BRI, SET_ROW_NIGHT unused
};
extern SettingSnap setSnap;         // screen_settings.cpp

// ── per-page draw entry points ──────────────────────────
// Called from screen.cpp's screenRender(); defined in that page's own file.
void drawDeviceCard(uint8_t dev, bool force);   // screen_devices.cpp
void drawScenes();                              // screen_scenes.cpp
void drawSettings();                            // screen_settings.cpp
