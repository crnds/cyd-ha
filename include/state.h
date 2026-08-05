#pragma once
#include <Arduino.h>
#include "config.h"

enum DevKind : uint8_t { DEV_LIGHT, DEV_CLIMATE };

// One controlled entity. Written by the poller and by optimistic tap updates;
// read by the renderer. Everything runs on the single Arduino loop task, so
// plain reads/writes are safe — no locking.
struct DeviceState {
  const char* entityId = nullptr;
  const char* name     = nullptr;
  DevKind     kind     = DEV_LIGHT;

  bool     known  = false;   // has any poll ever succeeded? gates T+/T-
  uint32_t okMs   = 0;       // millis() of last successful poll; 0 = never

  // HA reports "unavailable"/"unknown" for a device it can't reach. Without
  // this an unreachable bulb collapsed to on==false and rendered as a plain
  // OFF — indistinguishable from a healthy bulb that is genuinely off.
  bool     avail  = true;

  // ── light.* ──
  bool on         = false;
  int  pct        = -1;      // brightness 0-100, -1 = unknown
  int  kelvin     = -1;      // colour temp, -1 = unknown or unsupported
  bool supportsCT = true;    // false greys out the warm/cool swatches

  // ── climate.* ──
  // `mode` mirrors the entity's state string, which for climate IS the hvac
  // mode ("off" / "cool" / "dry" / "fan_only" / ...).
  char  mode[12] = "";
  float target   = NAN;      // setpoint, from attributes.temperature
  float room     = NAN;      // attributes.current_temperature
  float tMin     = AC_TEMP_MIN_DEF;
  float tMax     = AC_TEMP_MAX_DEF;
  float tStep    = AC_TEMP_STEP_DEF;

  // Set when a service call fails, so the row's state text flashes red for a
  // moment. Distinct from staleness (a failed *poll*), which dims instead.
  uint32_t errMs = 0;
};

struct AppState {
  DeviceState dev[NUM_DEVICES];

  uint8_t  netState = 0;     // 0 connecting, 1 live, 2 reconnecting
  bool     haOk     = false; // last HA request succeeded
  uint32_t haOkMs   = 0;     // millis() of last successful HA request
  uint32_t haFailMs = 0;     // millis() of last failed HA request (breaker)

  // Button being visually held down (inverted fill) for PRESS_FLASH_MS.
  int8_t   pressDev = -1;
  int8_t   pressBtn = -1;
  uint32_t pressMs  = 0;
};

extern AppState S;

// Number of buttons in a given device's row.
inline uint8_t btnCount(const DeviceState& d) {
  return d.kind == DEV_CLIMATE ? AC_BTNS : BULB_BTNS;
}
