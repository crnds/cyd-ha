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

// True while the last call failed recently (see HA_BREAKER_MS). Callers that
// run on a tap should skip the network and fail fast instead of paying another
// blocking timeout with the UI frozen.
bool haBreakerOpen();

// light.* service calls
bool haLightOff(DeviceState& d);
bool haLightBrightness(DeviceState& d, int pct);
bool haLightKelvin(DeviceState& d, int kelvin);

// Group light.* calls for the Scenes page. `mask` is a bitmask over S.dev[],
// bit i = device i.
//
// light.turn_on / turn_off accept a LIST for entity_id, and that is the whole
// point of these: three separate calls would be three sequential
// HTTP_READ_SVC_MS budgets — up to 7.5 s of frozen loop() for one scene tap —
// and the bulbs would visibly step on one at a time instead of together.
// haLightsOn sends brightness and colour temp in ONE turn_on for the same
// reason; kelvin <= 0 omits it.
bool haLightsOff(uint8_t mask);
bool haLightsOn(uint8_t mask, int pct, int kelvin);

// climate.* service calls
bool haClimateMode(DeviceState& d, const char* hvacMode);
bool haClimateTemp(DeviceState& d, float celsius);
