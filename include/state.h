#pragma once
#include <Arduino.h>
#include "config.h"

enum DevKind : uint8_t { DEV_LIGHT, DEV_CLIMATE };

// The three pages the tab bar switches between. Scope is still fixed — three
// pages, no scrolling, no sub-pages — it is just no longer exactly one.
enum PageId : uint8_t { PAGE_DEVICES = 0, PAGE_SCENES, PAGE_SETTINGS, PAGE_COUNT };
static_assert(PAGE_COUNT == TAB_COUNT, "tab bar and page enum disagree");

// What a touch landed on. Lives here rather than in screen.h because AppState
// needs it for the press flash, and state.h -> screen.h -> state.h would be
// circular.
//
//   HIT_TAB      idx = PageId,        sub = -1
//   HIT_ROW      idx = device 0..3,   sub = button
//   HIT_SCENE    idx = ABSOLUTE scene index (not the on-screen slot), sub = -1
//   HIT_SCROLL   idx = -1 up / +1 down, sub = -1
//   HIT_SETTING  idx = settings row,  sub = control within the row (-1 = toggle)
//
// HIT_NONE is 0 so that a memset/zero-init of the press fields means "nothing
// held", matching how every other snapshot in the renderer is cleared.
enum HitKind : uint8_t { HIT_NONE = 0, HIT_TAB, HIT_ROW, HIT_SCENE, HIT_SCROLL,
                         HIT_SETTING };

// idx is int16_t, not int8_t, purely for HIT_SCENE: the Scenes grid scrolls, so
// the index is into the whole table rather than the 12 tiles on screen, and an
// int8_t would silently wrap at 128 scenes — inside the range this page was
// rebuilt to handle.
struct Hit { HitKind kind; int16_t idx; int8_t sub; };

// Night mode is 3-way. RED keeps ordinal 1 to match the legacy bool's
// "true" — a device already persisting s.nit=1 in NVS needs zero migration
// code; SHIFT (2) is the only genuinely new value.
enum NightMode : uint8_t { NIGHT_OFF = 0, NIGHT_RED = 1, NIGHT_SHIFT = 2,
                           NIGHT_MODE_COUNT };

// The 3-chip row's on-screen order (Off, Shift, Red) is NOT NightMode's
// storage order, so this is the one place mapping chip position <-> mode —
// used both by the renderer (mode -> highlighted chip) and the tap handler
// (chip -> mode).
static const uint8_t NIGHT_CHIP_MODE[3] = { NIGHT_OFF, NIGHT_SHIFT, NIGHT_RED };

// Board-level settings, persisted to NVS (namespace NVS_NAMESPACE). These are
// the only mutable state that outlives a reboot.
struct Settings {
  uint8_t briIdx     = BRI_DEFAULT;  // index into BRI_DUTY_LIST
  uint8_t nightMode  = NIGHT_OFF;    // NightMode: Off / Red (red-only + 1% backlight) / Shift (warm palette only)
  bool    nightSched = true;         // schedule writes nightMode Off<->Red at the boundaries
  bool    flip       = false;        // display rotated 180
};

// One controlled entity. Written by the poller and by optimistic tap updates;
// read by the renderer. Everything runs on the single Arduino loop task, so
// plain reads/writes are safe — no locking.
struct DeviceState {
  const char* entityId = nullptr;
  const char* name     = nullptr;
  DevKind     kind     = DEV_LIGHT;

  bool     known  = false;   // has any poll ever succeeded? gates temp steps
  uint32_t okMs   = 0;       // millis() of last successful poll; 0 = never

  // HA reports "unavailable"/"unknown" for a device it can't reach. Without
  // this an unreachable bulb collapsed to on==false and rendered as a plain
  // OFF — indistinguishable from a healthy bulb that is genuinely off.
  bool     avail  = true;

  // ── light.* ──
  bool on         = false;
  int  pct        = -1;      // brightness 0-100, -1 = unknown
  int  kelvin     = -1;      // colour temp, -1 = unknown or unsupported
  // false greys out the warm/cool swatches. Only ever written by the
  // single-entity poll path, which has no caller in the shipped firmware, so
  // this is currently always true and the greying-out branch is unreachable.
  bool supportsCT = true;

  // ── climate.* ──
  // `mode` mirrors the entity's state string, which for climate IS the hvac
  // mode ("off" / "cool" / "dry" / "fan_only" / ...).
  char  mode[12] = "";
  float target   = NAN;      // setpoint, from attributes.temperature
  float room     = NAN;      // attributes.current_temperature
  float tMin     = AC_TEMP_MIN_DEF;
  float tMax     = AC_TEMP_MAX_DEF;
  float tStep    = AC_TEMP_STEP_DEF;
  int   humidity = -1;       // current_humidity 0-100, -1 = unknown/unsupported

  // Set when a service call fails, so the row's state text flashes red for a
  // moment. Distinct from staleness (a failed *poll*), which dims instead.
  uint32_t errMs = 0;
};

struct AppState {
  DeviceState dev[NUM_DEVICES];

  PageId   page = PAGE_DEVICES;
  Settings set;

  // Scenes page scroll offset, in GRID ROWS of SCENE_COLS tiles. Deliberately
  // survives a page switch, so coming back to Scenes lands where you left it.
  // Not persisted: a reboot starting at the top is the right default.
  uint16_t sceneRow = 0;

  uint8_t  netState = 0;     // 0 connecting, 1 live, 2 reconnecting
  bool     haOk     = false; // last HA request succeeded
  uint32_t haOkMs   = 0;     // millis() of last successful HA request
  uint32_t haFailMs = 0;     // millis() of last failed HA request (breaker)

  // Control being visually held down (inverted fill) for PRESS_FLASH_MS.
  // pressKind is what makes this page-safe: without it a scene tap at index 2
  // would also invert row 2's button over on the Devices page.
  HitKind  pressKind = HIT_NONE;
  int16_t  pressIdx  = -1;   // matches Hit::idx; see the note there
  int8_t   pressSub  = -1;
  uint32_t pressMs   = 0;
};

extern AppState S;

// Number of buttons in a given device's row.
inline uint8_t btnCount(const DeviceState& d) {
  return d.kind == DEV_CLIMATE ? AC_BTNS : BULB_BTNS;
}
