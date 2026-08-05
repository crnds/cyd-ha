#pragma once
#include "state.h"

// Home Assistant REST client.
//
// HA is on the local LAN over plain HTTP, so this uses a bare WiFiClient — no
// TLS. That is a deliberate departure from ../btcticker-cyd, which talks to
// public HTTPS APIs and pays a WiFiClientSecure handshake per fetch. Dropping
// TLS is what makes a tap feel instant here.
//
// All functions are blocking, but each is a sub-100ms round trip on LAN and
// the caller runs at most one per loop pass, so the UI never stalls visibly.
// Every function updates S.haOk / S.haOkMs as a side effect.

// Refreshes ALL FOUR devices in one POST /api/template.
//
// Replaces the previous round-robin of 4x GET /api/states/<id>. Measured
// against the live instance (best of 7):
//     4x GET        64.0 ms   2403 bytes
//     1x template   14.2 ms     50 bytes    -> 48x less data, 4.5x faster
// Because all devices arrive together, per-device freshness improves from
// 4 * HA_POLL_MS (6 s) to HA_POLL_MS (1.5 s) — 4x fresher.
//
// The response is a flat delimited string, so this path uses no ArduinoJson at
// all, which removes the two heap allocations the old poll made every cycle.
bool haPollAll();

// GET /api/states/<entity_id> -> fills the light or climate fields of `d`.
// Retained for one-off diagnostics; the steady-state path is haPollAll().
bool haPollDevice(DeviceState& d);

// True while the last call failed recently (see HA_BREAKER_MS). Callers that
// run on a tap should skip the network and fail fast instead of paying another
// blocking timeout with the UI frozen.
bool haBreakerOpen();

// light.* service calls
bool haLightOff(DeviceState& d);
bool haLightBrightness(DeviceState& d, int pct);
bool haLightKelvin(DeviceState& d, int kelvin);

// climate.* service calls
bool haClimateMode(DeviceState& d, const char* hvacMode);
bool haClimateTemp(DeviceState& d, float celsius);
