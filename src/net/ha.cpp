#include "ha.h"
#include "config.h"
#include "secrets.h"
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <math.h>

// One shared plain-HTTP client. Requests are strictly sequential (one per loop
// pass), so a single socket is enough and avoids re-allocating per call.
static WiFiClient net;

static void markResult(bool ok) {
  S.haOk = ok;
  if (ok) S.haOkMs = millis();
  else    S.haFailMs = millis();
}

// True while the circuit breaker is open — the last call failed recently, so
// skip the network entirely rather than making the caller pay another timeout.
bool haBreakerOpen() {
  return !S.haOk && S.haFailMs && (millis() - S.haFailMs < HA_BREAKER_MS);
}

// Opens `http` against HA and applies auth. Caller must http.end().
// useHTTP10 keeps responses unchunked so they can be stream-parsed.
static bool haBegin(HTTPClient& http, const char* path, int readMs = HTTP_READ_MS) {
  char url[192];
  snprintf(url, sizeof(url), "http://%s:%d%s", HA_HOST, HA_PORT, path);
  if (!http.begin(net, url)) return false;
  http.setTimeout(readMs);
  http.setConnectTimeout(HTTP_CONNECT_MS);
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
  if (!haBegin(http, path, HTTP_READ_SVC_MS)) { markResult(false); return false; }
  http.addHeader("Content-Type", "application/json");

  int code = http.POST((uint8_t*)body, strlen(body));
  http.end();

  if (code >= 200 && code < 300) { markResult(true); return true; }

  // A read timeout is NOT a failed command. We connected and sent the request;
  // we just gave up waiting for the reply. HA has almost certainly executed it —
  // observed repeatedly with climate.* calls, which wait on the Sensibo cloud.
  // Rolling back the optimistic state here would show the OLD setpoint for a
  // change that did in fact happen. Treat it as delivered and let the reconcile
  // refresh establish the truth a moment later.
  if (code == HTTPC_ERROR_READ_TIMEOUT) {
    Serial.printf("ha: POST %s/%s read-timeout (assuming delivered)  %s\n",
                  domain, service, body);
    markResult(true);
    return true;
  }

  Serial.printf("ha: POST %s/%s -> %d  %s\n", domain, service, code, body);
  markResult(false);
  return false;
}

// HA's sentinel states for a device it currently cannot reach. Declared here
// because both the bulk and single-entity parse paths below need it.
static inline bool isUnavail(const char* st) {
  return strcmp(st, "unavailable") == 0 || strcmp(st, "unknown") == 0;
}

static inline bool inRange(float v, float lo, float hi) {
  return v >= lo && v <= hi;
}

// ── bulk polling via the template API ────────────────────

// Per-entity fragments of the Jinja template. Deliberately written WITHOUT
// {%- ... -%} statement tags: those contain '%', which snprintf would consume
// as a format specifier. Loop-free means the entity id repeats, costing a few
// hundred bytes of request — irrelevant next to a 4x latency win.
//
// Climate temperatures are emitted in TENTHS so a 0.5 target_temp_step or a
// 24.5 setpoint survives; |int would truncate them.
#define TPL_LIGHT                                                    \
  "{{ states('%s') }},{{ state_attr('%s','brightness')|int(0) }},"    \
  "{{ state_attr('%s','color_temp_kelvin')|int(0) }};"

// current_humidity defaults to -1 (not 0) so a sensor that genuinely has no
// humidity reading is distinguishable from one reporting 0% — the range check
// below only accepts 0..100, so a missing attribute just keeps the last value.
#define TPL_CLIMATE                                                             \
  "{{ states('%s') }}"                                                          \
  ",{{ (state_attr('%s','temperature')|float(0)*10)|round(0)|int }}"            \
  ",{{ (state_attr('%s','current_temperature')|float(0)*10)|round(0)|int }}"    \
  ",{{ (state_attr('%s','min_temp')|float(0)*10)|round(0)|int }}"               \
  ",{{ (state_attr('%s','max_temp')|float(0)*10)|round(0)|int }}"               \
  ",{{ (state_attr('%s','target_temp_step')|float(1)*10)|round(0)|int }}"       \
  ",{{ state_attr('%s','current_humidity')|int(-1) }}"

// Applies one "state,brightness,kelvin" group to a light.
static void applyLight(DeviceState& d, const char* st, int bri, int kelvin) {
  d.avail = !isUnavail(st);
  d.on    = (strcmp(st, "on") == 0);
  // A bulb that is off reports 0 for both; keep the last known values so
  // turning it back on still shows a sensible highlight.
  if (bri > 0)    d.pct    = (int)lroundf(bri * 100.0f / 255.0f);
  if (kelvin > 0) d.kelvin = kelvin;
}

bool haPollAll() {
  // One buffer holds the whole request body. 879-char template measured for
  // these entity ids; 1600 leaves ample room for longer names.
  static char body[1600];
  int n = snprintf(body, sizeof(body),
                   "{\"template\":\"" TPL_LIGHT TPL_LIGHT TPL_LIGHT TPL_CLIMATE "\"}",
                   ENT_BULB1, ENT_BULB1, ENT_BULB1,
                   ENT_BULB2, ENT_BULB2, ENT_BULB2,
                   ENT_BULB3, ENT_BULB3, ENT_BULB3,
                   ENT_AC, ENT_AC, ENT_AC, ENT_AC, ENT_AC, ENT_AC, ENT_AC);
  if (n <= 0 || n >= (int)sizeof(body)) {
    Serial.printf("ha: template body overflow (%d)\n", n);
    markResult(false);
    return false;
  }

  HTTPClient http;
  if (!haBegin(http, "/api/template")) { markResult(false); return false; }
  http.addHeader("Content-Type", "application/json");

  int code = http.POST((uint8_t*)body, n);
  if (code != 200) {
    Serial.printf("ha: POST /api/template -> %d\n", code);
    http.end();
    markResult(false);
    return false;
  }
  // "off,0,0;off,0,0;on,76,2202;cool,290,237,180,310,10,55"
  char buf[192];

  // Read straight off the stream into the fixed buffer — no String allocation.
  // useHTTP10(true) (see haBegin) keeps the response unchunked specifically so
  // Content-Length is always present and getSize() is trustworthy; that is what
  // makes a bounded readBytes() safe instead of blocking for the rest of
  // HTTP_READ_MS waiting for bytes that were never coming.
  int len = http.getSize();
  if (len <= 0 || (size_t)len >= sizeof(buf)) {
    Serial.printf("ha: template response size implausible (%d)\n", len);
    http.end();
    markResult(false);
    return false;
  }
  size_t got = http.getStreamPtr()->readBytes(buf, (size_t)len);
  http.end();
  buf[got] = '\0';
  if (got != (size_t)len) {
    Serial.printf("ha: template short read (%u of %d)\n", (unsigned)got, len);
    markResult(false);
    return false;
  }

  // Log only on change: silent in steady state, but every externally-made
  // change (phone app, automation) shows up with a timestamp — which is how
  // the poll latency and the parse get verified without watching the screen.
  static char prev[192] = "";
  if (strcmp(buf, prev) != 0) {
    Serial.printf("ha state: %s\n", buf);
    snprintf(prev, sizeof(prev), "%s", buf);
  }

  char* saveptr = nullptr;
  char* tok = strtok_r(buf, ";", &saveptr);
  int   idx = 0;
  bool  okAll = true;

  while (tok && idx < NUM_DEVICES) {
    DeviceState& d = S.dev[idx];
    char st[20]  = "";
    bool thisOk  = false;

    if (d.kind == DEV_CLIMATE) {
      int tg = 0, rm = 0, mn = 0, mx = 0, sp = 0, hum = -1;
      if (sscanf(tok, "%19[^,],%d,%d,%d,%d,%d,%d", st, &tg, &rm, &mn, &mx, &sp, &hum) == 7) {
        d.avail = !isUnavail(st);
        strncpy(d.mode, st, sizeof(d.mode) - 1);
        d.mode[sizeof(d.mode) - 1] = '\0';

        // Every climate field is range-checked, and a rejected field keeps its
        // last good value. Two reasons this is necessary:
        //  1. The template's |float(0) default conflates "attribute missing"
        //     with "value is zero"; the old ArduinoJson isNull() check could
        //     tell them apart, and this flattened that distinction.
        //  2. The Sensibo reports nonsense transiently during an hvac_mode
        //     change. An actual observed sample was "cool,0,238,0,10,10" —
        //     target 0, min 0, max 1.0. Taking max=1.0 at face value would make
        //     the chevrons clamp the setpoint to one degree.
        float ftg = tg / 10.0f, frm = rm / 10.0f;
        float fmn = mn / 10.0f, fmx = mx / 10.0f, fsp = sp / 10.0f;

        if (inRange(ftg, 5.0f, 40.0f))   d.target = ftg;
        if (inRange(frm, -10.0f, 60.0f)) d.room   = frm;
        // min/max are only taken as a coherent pair spanning a usable band
        if (inRange(fmn, 5.0f, 30.0f) && inRange(fmx, 10.0f, 40.0f) &&
            (fmx - fmn) >= 5.0f) {
          d.tMin = fmn;
          d.tMax = fmx;
        } else if (mn || mx) {
          Serial.printf("ha: rejected implausible climate limits %.1f/%.1f "
                        "(keeping %.1f/%.1f)\n", fmn, fmx, d.tMin, d.tMax);
        }
        if (inRange(fsp, 0.1f, 5.0f)) d.tStep = fsp;
        // -1 is the template's own "attribute missing" sentinel (see
        // TPL_CLIMATE), distinct from a genuine 0%, so it is rejected the same
        // way an out-of-range value is: keep the last known reading.
        if (hum >= 0 && hum <= 100) d.humidity = hum;

        thisOk = true;
      }
    } else {
      int bri = 0, k = 0;
      if (sscanf(tok, "%19[^,],%d,%d", st, &bri, &k) == 3) {
        applyLight(d, st, bri, k);
        thisOk = true;
      }
    }

    // Per-device, not a shared flag: one malformed group must not stop the
    // other three from being marked fresh.
    if (thisOk) { d.known = true; d.okMs = millis(); }
    else        { okAll = false; }

    tok = strtok_r(nullptr, ";", &saveptr);
    idx++;
  }

  if (idx != NUM_DEVICES || !okAll) {
    // buf itself is unusable here: strtok_r() has punched '\0's into it at
    // every ';', so printing it now would only show the first token. `prev`
    // was set to the pre-tokenized string above (unconditionally equal to
    // buf, whether or not this pass logged a change), so it is what to print.
    Serial.printf("ha: template parse failed (%d/%d fields): %s\n",
                  idx, NUM_DEVICES, prev);
    markResult(false);
    return false;
  }
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

// Builds  "a","b","c"  for the devices whose bit is set. Returns bytes written,
// or -1 on overflow.
static int idList(char* out, size_t n, uint8_t mask) {
  int w = 0;
  for (uint8_t i = 0; i < NUM_DEVICES; i++) {
    if (!(mask & (1u << i))) continue;
    int k = snprintf(out + w, n - (size_t)w, "%s\"%s\"", w ? "," : "",
                     S.dev[i].entityId);
    if (k < 0 || w + k >= (int)n) return -1;
    w += k;
  }
  return w;
}

// A formatting overflow is a bug in here, not an HA outage, so it logs and
// returns false WITHOUT markResult(false) — opening the breaker would make the
// next unrelated tap fail fast for no reason. The 3-bulb body runs ~114 bytes
// with the current entity IDs; 256 is sized so a rename cannot silently
// truncate it into malformed JSON that HA answers with a 400.
static bool sceneBodyOverflow(const char* what, int n, size_t cap) {
  if (n > 0 && n < (int)cap) return false;
  Serial.printf("ha: %s body overflow (%d of %u)\n", what, n, (unsigned)cap);
  return true;
}

bool haLightsOff(uint8_t mask) {
  char ids[192], body[256];
  if (idList(ids, sizeof(ids), mask) <= 0) return false;
  int n = snprintf(body, sizeof(body), "{\"entity_id\":[%s]}", ids);
  if (sceneBodyOverflow("turn_off", n, sizeof(body))) return false;
  return haPostService("light", "turn_off", body);
}

bool haLightsOn(uint8_t mask, int pct, int kelvin) {
  char ids[192], body[256];
  if (idList(ids, sizeof(ids), mask) <= 0) return false;
  int n = (kelvin > 0)
        ? snprintf(body, sizeof(body),
                   "{\"entity_id\":[%s],\"brightness_pct\":%d,"
                   "\"color_temp_kelvin\":%d}", ids, pct, kelvin)
        : snprintf(body, sizeof(body),
                   "{\"entity_id\":[%s],\"brightness_pct\":%d}", ids, pct);
  if (sceneBodyOverflow("turn_on", n, sizeof(body))) return false;
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
