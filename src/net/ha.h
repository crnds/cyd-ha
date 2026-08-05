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

// GET /api/states/<entity_id> -> fills the light or climate fields of `d`.
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
