#include "ha.h"
#include "config.h"
#include "secrets.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <math.h>

// One shared plain-HTTP client. Requests are strictly sequential (one per loop
// pass), so a single socket is enough and avoids re-allocating per call.
static WiFiClient net;

static void markResult(bool ok) {
  S.haOk = ok;
  if (ok) S.haOkMs = millis();
}

// Opens `http` against HA and applies auth. Caller must http.end().
// useHTTP10 keeps responses unchunked so they can be stream-parsed.
static bool haBegin(HTTPClient& http, const char* path) {
  char url[192];
  snprintf(url, sizeof(url), "http://%s:%d%s", HA_HOST, HA_PORT, path);
  if (!http.begin(net, url)) return false;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.useHTTP10(true);
  http.addHeader("Authorization", "Bearer " HA_TOKEN);
  return true;
}

// POST a JSON body to /api/services/<domain>/<service>. The response body is
// the list of changed states, which we ignore — the reconcile poll re-reads
// authoritative state a moment later anyway.
static bool haPostService(const char* domain, const char* service, const char* body) {
  char path[96];
  snprintf(path, sizeof(path), "/api/services/%s/%s", domain, service);

  HTTPClient http;
  if (!haBegin(http, path)) { markResult(false); return false; }
  http.addHeader("Content-Type", "application/json");

  int code = http.POST((uint8_t*)body, strlen(body));
  http.end();

  bool ok = (code >= 200 && code < 300);
  if (!ok) Serial.printf("ha: POST %s/%s -> %d  %s\n", domain, service, code, body);
  markResult(ok);
  return ok;
}

// ── state polling ────────────────────────────────────────

static void parseLight(DeviceState& d, JsonDocument& doc) {
  const char* st = doc["state"] | "";
  d.on = (strcmp(st, "on") == 0);

  JsonObject at = doc["attributes"];

  // brightness is absent while the bulb is off — keep the last known level so
  // returning to "on" still shows a sensible highlight.
  if (!at["brightness"].isNull()) {
    int b = at["brightness"].as<int>();
    d.pct = (int)lroundf(b * 100.0f / 255.0f);
  }
  if (!at["color_temp_kelvin"].isNull()) {
    d.kelvin = at["color_temp_kelvin"].as<int>();
  }

  // supported_color_modes tells us whether the warm/cool swatches can do
  // anything at all. IKEA ships both "white spectrum" (color_temp) and plain
  // dimmable-white TRADFRI bulbs, and they look identical in the app.
  JsonArray modes = at["supported_color_modes"];
  if (!modes.isNull()) {
    bool ct = false;
    for (JsonVariant m : modes) {
      const char* s = m.as<const char*>();
      if (s && strcmp(s, "color_temp") == 0) { ct = true; break; }
    }
    d.supportsCT = ct;
  }
}

static void parseClimate(DeviceState& d, JsonDocument& doc) {
  // For a climate entity the top-level state IS the hvac mode.
  const char* st = doc["state"] | "";
  strncpy(d.mode, st, sizeof(d.mode) - 1);
  d.mode[sizeof(d.mode) - 1] = '\0';

  JsonObject at = doc["attributes"];
  if (!at["temperature"].isNull())         d.target = at["temperature"].as<float>();
  if (!at["current_temperature"].isNull()) d.room   = at["current_temperature"].as<float>();
  if (!at["min_temp"].isNull())            d.tMin   = at["min_temp"].as<float>();
  if (!at["max_temp"].isNull())            d.tMax   = at["max_temp"].as<float>();
  // Sensibo reports 1.0 on most units but 0.5 on some — always prefer the
  // entity's own value so T+/T- step by exactly what HA will accept.
  if (!at["target_temp_step"].isNull())    d.tStep  = at["target_temp_step"].as<float>();
}

bool haPollDevice(DeviceState& d) {
  char path[128];
  snprintf(path, sizeof(path), "/api/states/%s", d.entityId);

  HTTPClient http;
  if (!haBegin(http, path)) { markResult(false); return false; }

  int code = http.GET();
  if (code != 200) {
    Serial.printf("ha: GET %s -> %d\n", d.entityId, code);
    http.end();
    markResult(false);
    return false;
  }

  // Filter so only the handful of fields we render is ever materialised —
  // a light's full attribute blob (effect lists, icons) is far larger.
  JsonDocument filter;
  filter["state"] = true;
  JsonObject fa = filter["attributes"].to<JsonObject>();
  if (d.kind == DEV_CLIMATE) {
    fa["temperature"]         = true;
    fa["current_temperature"] = true;
    fa["min_temp"]            = true;
    fa["max_temp"]            = true;
    fa["target_temp_step"]    = true;
  } else {
    fa["brightness"]            = true;
    fa["color_temp_kelvin"]     = true;
    fa["supported_color_modes"] = true;
  }

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    Serial.printf("ha: json %s -> %s\n", d.entityId, err.c_str());
    markResult(false);
    return false;
  }

  if (d.kind == DEV_CLIMATE) parseClimate(d, doc);
  else                       parseLight(d, doc);

  d.known = true;
  d.okMs  = millis();
  markResult(true);
  return true;
}

// ── service calls ────────────────────────────────────────

bool haLightOff(DeviceState& d) {
  char body[128];
  snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", d.entityId);
  return haPostService("light", "turn_off", body);
}

bool haLightBrightness(DeviceState& d, int pct) {
  char body[160];
  snprintf(body, sizeof(body),
           "{\"entity_id\":\"%s\",\"brightness_pct\":%d}", d.entityId, pct);
  return haPostService("light", "turn_on", body);
}

bool haLightKelvin(DeviceState& d, int kelvin) {
  char body[160];
  snprintf(body, sizeof(body),
           "{\"entity_id\":\"%s\",\"color_temp_kelvin\":%d}", d.entityId, kelvin);
  return haPostService("light", "turn_on", body);
}

bool haClimateMode(DeviceState& d, const char* hvacMode) {
  char body[160];
  snprintf(body, sizeof(body),
           "{\"entity_id\":\"%s\",\"hvac_mode\":\"%s\"}", d.entityId, hvacMode);
  return haPostService("climate", "set_hvac_mode", body);
}

bool haClimateTemp(DeviceState& d, float celsius) {
  char body[160];
  snprintf(body, sizeof(body),
           "{\"entity_id\":\"%s\",\"temperature\":%.1f}", d.entityId, celsius);
  return haPostService("climate", "set_temperature", body);
}
