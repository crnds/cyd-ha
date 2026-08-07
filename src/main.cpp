// Home Assistant bedroom controller for the CYD (ESP32-2432S028R).
//
// Everything runs on the single Arduino loop() task — no RTOS tasks, no
// locking. loop() calls, in order: updateNetState -> servicePoll ->
// handleTouch -> screenRender, then delay(20).
//
// The touch stack (xptWrite/xptRead/readTouch) is carried from the sibling
// ../btcticker-cyd firmware, which was calibrated against this same physical
// unit. Two hard-won constraints live in there — see readTouch().

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <XPT2046_Bitbang.h>
#include <math.h>

#include "config.h"
#include "secrets.h"
#include "state.h"
#include "net/ha.h"
#include "ui/screen.h"

AppState S;

static const uint8_t BRI_DUTY[BRI_STEPS] = BRI_DUTY_LIST;

// Software-SPI XPT2046 on the dedicated CYD touch pins. Used only for its
// begin()/setCalibration() pin setup — reads go through xptRead() below.
// Hardware VSPI + TFT_eSPI's TOUCH_CS fought over the same CS pin and left
// touch dead on many CYD units.
static XPT2046_Bitbang ts(PIN_TOUCH_MOSI, PIN_TOUCH_MISO, PIN_TOUCH_CLK,
                          PIN_TOUCH_CS, SCR_W, SCR_H);

// ── low-level bit-bang (same protocol as the lib, but lets us log pressure
//    even when it's below the library's internal touch threshold) ───────────

static void xptWrite(uint8_t cmd) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_TOUCH_MOSI, (cmd >> i) & 1);
    digitalWrite(PIN_TOUCH_CLK, LOW);
    delayMicroseconds(5);
    digitalWrite(PIN_TOUCH_CLK, HIGH);
    delayMicroseconds(5);
  }
  digitalWrite(PIN_TOUCH_MOSI, LOW);
  digitalWrite(PIN_TOUCH_CLK, LOW);
}

static uint16_t xptRead(uint8_t cmd) {
  xptWrite(cmd);
  uint16_t result = 0;
  for (int i = 15; i >= 0; i--) {
    digitalWrite(PIN_TOUCH_CLK, HIGH);
    delayMicroseconds(5);
    digitalWrite(PIN_TOUCH_CLK, LOW);
    delayMicroseconds(5);
    result |= (uint16_t)digitalRead(PIN_TOUCH_MISO) << i;
  }
  return result >> 4;
}

// Multi-sample read. Mirrors the Paul Stoffregen XPT2046 sample order: discard
// the first noisy X, average the closest pair of X/Y reads.
//
// AXIS MAPPING — read this before touching anything below. bestSx is built from
// the 0xD1 (Y-command) samples and bestSy from the 0x91 (X-command) samples,
// which is btcticker-cyd's convention. On THIS unit that is backwards, so
// TOUCH_SWAP_XY is 1 and the effective mapping is:
//     screen X  <-  bestSy  <-  0x91 (X-command) samples
//     screen Y  <-  bestSx  <-  0xD1 (Y-command) samples
// Getting this wrong is what collapsed every tap into the left third of the
// screen. Verified correct against all 23 buttons (residuals < 0.6 px).
static bool readTouch(int16_t& sx, int16_t& sy,
                      uint16_t* rawX = nullptr, uint16_t* rawY = nullptr,
                      uint16_t* rawZ = nullptr) {
  uint16_t bestZ = 0, bestSx = 0, bestSy = 0;

  for (int sample = 0; sample < 3; sample++) {
    digitalWrite(PIN_TOUCH_CS, LOW);
    uint16_t z1 = xptRead(0xB1);
    uint16_t z  = z1 + 4095;
    uint16_t z2 = xptRead(0xC1);
    z -= z2;

    uint16_t d0 = 0, d1 = 0, d2 = 0, d3 = 0, d4 = 0, d5 = 0;
    if (z >= TOUCH_Z_MIN) {
      xptRead(0x91);          // dummy X
      d0 = xptRead(0xD1);     // Y
      d1 = xptRead(0x91);     // X
      d2 = xptRead(0xD1);     // Y
      d3 = xptRead(0x91);     // X
      d4 = xptRead(0xD0);     // Y + power down
      d5 = xptRead(0x00);
    } else {
      xptRead(0xD0);
    }
    digitalWrite(PIN_TOUCH_CS, HIGH);

    if (z > bestZ) {
      bestZ = z;
      auto avg2 = [](uint16_t a, uint16_t b, uint16_t c) -> uint16_t {
        uint16_t ab = a > b ? a - b : b - a;
        uint16_t ac = a > c ? a - c : c - a;
        uint16_t bc = b > c ? b - c : c - b;
        if (ab <= ac && ab <= bc) return (a + b) / 2;
        if (ac <= ab && ac <= bc) return (a + c) / 2;
        return (b + c) / 2;
      };
      bestSx = avg2(d0, d2, d4);
      bestSy = avg2(d1, d3, d5);
    }
    delayMicroseconds(100);
  }

  if (rawX) *rawX = bestSx;
  if (rawY) *rawY = bestSy;
  if (rawZ) *rawZ = bestZ;
  if (bestZ < TOUCH_Z_MIN || (bestSx == 0 && bestSy == 0)) return false;

  uint16_t ax = bestSx, ay = bestSy;
#if TOUCH_SWAP_XY
  uint16_t tmp = ax; ax = ay; ay = tmp;
#endif
  int16_t x = map((int)ax, TOUCH_X_MIN, TOUCH_X_MAX, 0, SCR_W - 1);
  int16_t y = map((int)ay, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, SCR_H - 1);
#if TOUCH_INVERT_X
  x = (SCR_W - 1) - x;
#endif
#if TOUCH_INVERT_Y
  y = (SCR_H - 1) - y;
#endif
  if (x < 0) x = 0; else if (x > SCR_W - 1) x = SCR_W - 1;
  if (y < 0) y = 0; else if (y > SCR_H - 1) y = SCR_H - 1;
  sx = x;
  sy = y;
  return true;
}

// ── settings / backlight / night mode ────────────────────

static Preferences prefs;

// Four keys, all uint8_t, clamped on load. Small enough that a RowMeta table
// (as ../btc-cyd-v2 uses for its thirteen) would cost more than it saves.
#define K_BRI   "s.bri"
#define K_NIGHT "s.nit"
#define K_SCHED "s.nsch"
#define K_FLIP  "s.flip"

static void settingsLoad() {
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    // Never hang on this — the device is still fully usable on defaults, and
    // putUChar() no-ops when the namespace failed to open, so saves fail soft.
    Serial.println("nvs: open failed, running on defaults");
    return;
  }
  uint8_t bri = prefs.getUChar(K_BRI, BRI_DEFAULT);
  S.set.briIdx     = (bri < BRI_STEPS) ? bri : BRI_DEFAULT;   // clamp corrupt
  S.set.night      = prefs.getUChar(K_NIGHT, 0) != 0;
  S.set.nightSched = prefs.getUChar(K_SCHED, 1) != 0;
  S.set.flip       = prefs.getUChar(K_FLIP,  0) != 0;
  Serial.printf("settings: bri=%u/%u (duty %u) night=%d sched=%d flip=%d\n",
                S.set.briIdx, BRI_STEPS - 1, BRI_DUTY[S.set.briIdx],
                S.set.night, S.set.nightSched, S.set.flip);
}

// Only a user tap persists. Scheduled night transitions deliberately do not:
// the boot-time window adoption in serviceNightSchedule() already restores the
// right state, so writing twice a day forever would buy nothing.
static void settingsSave(const char* key, uint8_t val) {
  prefs.putUChar(key, val);
}

// Rewriting the LEDC duty every frame visibly glitches CYD backlights, so only
// push it when it actually changes. Seeded 0xFF so the first call always
// writes — including the boot seed, which must go through here or the memo
// desyncs from the panel.
static void applyBacklight(uint8_t duty) {
  static uint8_t last = 0xFF;
  if (duty == last) return;
  last = duty;
  ledcWrite(BL_CHANNEL, duty);
}

// Palette, rotation and backlight, all memoised no-ops when nothing changed —
// so this is ~3 compares at 50 Hz. Effective duty is computed HERE rather than
// in the tap handler, which is what makes a brightness change picked while
// night mode is dimming still land the moment night ends.
static void applySettings() {
  screenSetNight(S.set.night);
  screenSetFlip(S.set.flip);
  applyBacklight(BRI_DUTY[S.set.night ? BRI_NIGHT : S.set.briIdx]);
}

// The schedule WRITES the Night mode toggle at its two boundaries rather than
// overriding it. Edge-triggered, so between the boundaries a manual toggle
// always wins and sticks — a level-triggered version would re-assert itself on
// the next render and make the toggle physically un-turn-off-able before 08:00.
static void serviceNightSchedule() {
  static int16_t lastMin = -1;

  // Reset so re-enabling the schedule re-adopts the current window rather than
  // waiting up to a day for the next boundary.
  if (!S.set.nightSched) { lastMin = -1; return; }

  struct tm t;
  // Zero timeout: this runs every loop pass and must never block. Returns false
  // until SNTP lands, and an unsynced clock must never be read as midnight.
  if (!getLocalTime(&t, 0)) return;

  int16_t m = (int16_t)(t.tm_hour * 60 + t.tm_min);
  if (m == lastMin) return;

  if (lastMin < 0) {
    // First valid clock reading, or the schedule was just re-enabled: adopt the
    // window. This is what makes a 02:00 reboot come up already in night mode.
    S.set.night = (m >= NIGHT_ON_MIN || m < NIGHT_OFF_MIN);
    Serial.printf("night: adopting window at %02d:%02d -> %d\n",
                  t.tm_hour, t.tm_min, S.set.night);
  } else {
    // The `lastMin < B && m > B` half catches a boundary missed because a
    // blocking HTTP call held loop() across the minute. Neither clause misfires
    // at the midnight wrap, where lastMin is 1439 and m is 0.
    if (m == NIGHT_ON_MIN  || (lastMin < NIGHT_ON_MIN  && m > NIGHT_ON_MIN)) {
      S.set.night = true;
      Serial.println("night: schedule on");
    } else if (m == NIGHT_OFF_MIN || (lastMin < NIGHT_OFF_MIN && m > NIGHT_OFF_MIN)) {
      S.set.night = false;
      Serial.println("night: schedule off");
    }
  }
  lastMin = m;
}

// ── HA poll scheduling ───────────────────────────────────
// All four devices refresh together in one templated request every HA_POLL_MS,
// so every device is at most HA_POLL_MS stale. The previous round-robin left
// each device up to 4 * HA_POLL_MS behind. A tap schedules an early refresh to
// reconcile the optimistic guess against real state.

static uint32_t nextPollMs    = 0;
static uint32_t backoffMs     = 0;
static uint32_t reconcileAtMs = 0;
static bool     reconcilePend = false;

static void servicePoll() {
  uint32_t now = millis();
  bool due = (int32_t)(now - nextPollMs) >= 0;

  if (reconcilePend && (int32_t)(now - reconcileAtMs) >= 0) {
    reconcilePend = false;
    due = true;                     // pull the next refresh forward
  } else if (!due) {
    return;
  }

  bool ok = haPollAll();

  if (ok) {
    backoffMs  = 0;
    nextPollMs = now + HA_POLL_MS;
  } else {
    backoffMs  = backoffMs ? (backoffMs * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : backoffMs * 2)
                           : RETRY_BASE_MS;
    nextPollMs = now + backoffMs;
  }
}

// Heap watch — the poll path no longer allocates (the template response is
// parsed in fixed buffers), so this should sit flat. Logged so a soak test can
// prove it rather than assume it.
static void logHeap() {
  static uint32_t next = 0;
  if ((int32_t)(millis() - next) < 0) return;
  next = millis() + 30000UL;
  Serial.printf("heap: free=%u min=%u largest=%u  up=%lus\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap(),
                (unsigned long)(millis() / 1000));
}

// ── actions ──────────────────────────────────────────────

enum ActKind : uint8_t { ACT_NONE, ACT_L_OFF, ACT_L_BRI, ACT_L_KELVIN,
                         ACT_C_MODE, ACT_C_TEMP };

// Applies the expected result locally and repaints BEFORE the HTTP call, then
// reverts if the call failed. IKEA Zigbee round-trips run 1-2s; without this
// optimistic step every tap would feel like the screen had ignored it.
static void doAction(int16_t devIdx, int8_t btn) {
  DeviceState& d = S.dev[devIdx];

  // Fail fast while the breaker is open. Every HA call blocks the loop, so
  // without this each tap against an unreachable HA would cost another full
  // connect timeout with the screen frozen.
  if (haBreakerOpen()) {
    d.errMs = millis();
    Serial.println("ha: breaker open, skipping call");
    return;
  }

  const DeviceState before = d;

  ActKind     kind = ACT_NONE;
  int         iArg = 0;
  float       fArg = 0;
  const char* sArg = nullptr;

  if (d.kind == DEV_CLIMATE) {
    switch (btn) {
      case 0: kind = ACT_C_MODE; sArg = "off";        break;
      case 1: kind = ACT_C_MODE; sArg = AC_MODE_COOL; break;
      case 2: kind = ACT_C_MODE; sArg = AC_MODE_DRY;  break;
      // AC_BTN_TEMP (the slot between these two) is the setpoint READOUT and
      // falls through to `default: return`. screenHitTest() already reports a
      // tap there as a miss, so this is belt and braces.
      case AC_BTN_TUP:
      case AC_BTN_TDN: {
        // The steps are relative, so they cannot act until a real setpoint is
        // known — the same rule that makes the readout show "--" until then.
        if (!d.known || isnan(d.target)) {
          d.errMs = millis();
          Serial.println("ac: no setpoint known yet, ignoring step");
          return;
        }
        float t = d.target + (btn == AC_BTN_TUP ? d.tStep : -d.tStep);
        if (t < d.tMin) t = d.tMin;
        if (t > d.tMax) t = d.tMax;
        // Already at the limit. Flash the row instead of returning silently:
        // six consecutive up-taps at max once produced no feedback whatsoever,
        // which reads as "the tap missed" rather than "you are at 31". Compared
        // with an epsilon because these are floats off a /10 division.
        if (fabsf(t - d.target) < 0.01f) {
          d.errMs = millis();
          Serial.printf("ac: already at limit %.1f (min %.1f max %.1f)\n",
                        d.target, d.tMin, d.tMax);
          return;
        }
        kind = ACT_C_TEMP; fArg = t;
        break;
      }
      default: return;
    }
  } else {
    switch (btn) {
      case 0: kind = ACT_L_OFF;                        break;
      case 1: kind = ACT_L_BRI;    iArg = BRI_LOW;     break;
      case 2: kind = ACT_L_BRI;    iArg = BRI_MID;     break;
      case 3: kind = ACT_L_BRI;    iArg = BRI_HIGH;    break;
      case 4: if (!d.supportsCT) return;
              kind = ACT_L_KELVIN; iArg = KELVIN_WARM; break;
      case 5: if (!d.supportsCT) return;
              kind = ACT_L_KELVIN; iArg = KELVIN_COOL; break;
      default: return;
    }
  }

  // 1. optimistic local state
  switch (kind) {
    case ACT_L_OFF:    d.on = false; break;
    case ACT_L_BRI:    d.on = true; d.pct = iArg; break;
    case ACT_L_KELVIN: d.on = true; d.kelvin = iArg; break;
    case ACT_C_MODE:   strncpy(d.mode, sArg, sizeof(d.mode) - 1);
                       d.mode[sizeof(d.mode) - 1] = '\0'; break;
    case ACT_C_TEMP:   d.target = fArg; break;
    default: return;
  }
  screenRender();

  // 2. fire it
  bool ok = false;
  switch (kind) {
    case ACT_L_OFF:    ok = haLightOff(d);                break;
    case ACT_L_BRI:    ok = haLightBrightness(d, iArg);   break;
    case ACT_L_KELVIN: ok = haLightKelvin(d, iArg);       break;
    case ACT_C_MODE:   ok = haClimateMode(d, sArg);       break;
    case ACT_C_TEMP:   ok = haClimateTemp(d, fArg);       break;
    default: break;
  }

  // 3. reconcile or roll back. The refresh covers all four devices in one
  // request now, so there is no per-device queue to target.
  if (ok) {
    reconcilePend = true;
    reconcileAtMs = millis() + RECONCILE_MS;
  } else {
    d = before;
    d.errMs = millis();
  }
}

// ── scenes ───────────────────────────────────────────────
// Same optimistic ordering as doAction(), but over three devices at once. The
// call set is DERIVED from the scene table rather than special-cased per scene,
// so editing the table cannot desync it: one turn_off for the bulbs the scene
// wants dark, one turn_on for the rest.
static void doScene(int16_t idx) {
  // The table in screen.cpp owns the count, so ask rather than assume one.
  if (idx < 0 || (uint16_t)idx >= sceneCount()) return;

  if (haBreakerOpen()) {
    // No single row owns a scene, so flash all three — that is what the card's
    // error state reads off.
    for (uint8_t i = 0; i < NUM_BULBS; i++) S.dev[i].errMs = millis();
    Serial.println("ha: breaker open, skipping scene");
    return;
  }

  // doAction() saves ONE `before`; a scene mutates three, and a partial failure
  // must restore the exact pre-tap state of every bulb. ~204 B of loop stack.
  DeviceState before[NUM_BULBS];
  for (uint8_t i = 0; i < NUM_BULBS; i++) before[i] = S.dev[i];

  uint8_t offMask, onMask;
  int onPct, onK;
  scenePlan((uint16_t)idx, offMask, onMask, onPct, onK);

  // 1. optimistic, all three at once
  for (uint8_t i = 0; i < NUM_BULBS; i++) {
    DeviceState& d = S.dev[i];
    if (onMask & (1u << i)) {
      d.on  = true;
      d.pct = onPct;
      if (onK > 0 && d.supportsCT) d.kelvin = onK;
    } else {
      d.on = false;
    }
  }
  screenRender();

  // 2. fire, dark bulbs first so a lights-up transition never flashes every
  //    bulb on and then back off. Each call blocks loop() for up to
  //    HTTP_READ_SVC_MS, so RELAX's two are the longest freeze in the firmware.
  //    Skip the second once the first has hard-failed: haPostService() does not
  //    consult the breaker, so we would pay another full timeout for a scene we
  //    are about to roll back wholesale.
  bool okOff = true, okOn = true;
  if (offMask) okOff = haLightsOff(offMask);
  if (onMask)  okOn  = okOff && haLightsOn(onMask, onPct, onK);

  // 3. RELAX can half-succeed and nothing here can tell which bulbs moved, so
  //    roll back to the last OBSERVED state rather than invent a mixed one —
  //    and schedule the reconcile ANYWAY. Unlike doAction(), where a failed
  //    call means nothing changed, a partial scene failure means HA's truth now
  //    differs from both the guess and the rollback; only a poll can settle it.
  if (!(okOff && okOn)) {
    for (uint8_t i = 0; i < NUM_BULBS; i++) {
      S.dev[i] = before[i];
      S.dev[i].errMs = millis();
    }
  }
  reconcilePend = true;
  reconcileAtMs = millis() + RECONCILE_MS;
}

// ── settings ─────────────────────────────────────────────
// Local only — no HA call, so no optimistic/rollback dance. applySettings()
// must run BEFORE the repaint: a flip or night change alters the rotation and
// palette the frame is drawn in, and doing it after would paint one frame in
// the old one and immediately wipe it.
static void doSetting(int16_t row, int8_t sub) {
  switch (row) {
    case SET_ROW_BRI:
      if (sub < 0 || sub >= BRI_STEPS) return;
      if (S.set.briIdx == (uint8_t)sub) return;
      S.set.briIdx = (uint8_t)sub;
      settingsSave(K_BRI, S.set.briIdx);
      break;
    case SET_ROW_NIGHT:
      S.set.night = !S.set.night;
      settingsSave(K_NIGHT, S.set.night);
      break;
    case SET_ROW_SCHED:
      S.set.nightSched = !S.set.nightSched;
      settingsSave(K_SCHED, S.set.nightSched);
      break;
    case SET_ROW_FLIP:
      S.set.flip = !S.set.flip;
      settingsSave(K_FLIP, S.set.flip);
      break;
    default: return;
  }
  applySettings();
  screenRender();
}

// ── touch ────────────────────────────────────────────────

static void handleTouch() {
  static uint32_t lastTap = 0, lastDbg = 0, lastPoll = 0;
  static bool     wasDown = false;

  // A finger tap holds contact far longer than TOUCH_POLL_MS, so sampling at
  // 20 Hz instead of every pass halves the bit-bang cost for free.
  uint32_t pollNow = millis();
  if (pollNow - lastPoll < TOUCH_POLL_MS) return;
  lastPoll = pollNow;

  int16_t  x = -1, y = -1;
  uint16_t rx = 0, ry = 0, rz = 0;
  bool ok = readTouch(x, y, &rx, &ry, &rz);

#if !CALIB_MODE
  // The 180 flip never reaches the XPT2046 — the digitiser is physically fixed,
  // so readTouch() still returns coordinates in the rotation-1 frame its
  // TOUCH_* constants were fitted against. Mirror both axes to reach the frame
  // that is actually on screen. Exactly equivalent to toggling TOUCH_INVERT_X
  // and TOUCH_INVERT_Y together, which is what 180 means for a
  // landscape-to-landscape rotation (no axis swap; both are 320x240).
  //
  // Deliberately here and not in readTouch(): that stays a pure
  // raw-to-calibration-frame mapper, the calibration paths bypass this for
  // free, and the `touch dbg:` line below then logs the coordinates that were
  // actually hit-tested — the only in-situ diagnostic for this.
  if (ok && S.set.flip) { x = (SCR_W - 1) - x; y = (SCR_H - 1) - y; }
#endif

  bool irqLow = digitalRead(PIN_TOUCH_IRQ) == LOW;
  uint32_t now = millis();

#if TOUCH_REQUIRE_IRQ
  // Both signals must agree. Pressure alone let noise at z~100 through, and a
  // phantom tap here doesn't just misdraw — it fires a real service call and
  // changes a light. See TOUCH_REQUIRE_IRQ in config.h.
  if (ok && !irqLow) {
    if (now - lastDbg > 4000) {
      lastDbg = now;
      Serial.printf("touch reject: z=%u raw=%u,%u irq=high (noise)\n",
                    (unsigned)rz, (unsigned)rx, (unsigned)ry);
    }
    ok = false;
  }
#endif

  if (ok && now - lastDbg > 800) {
    lastDbg = now;
    Serial.printf("touch dbg: z=%u raw=%u,%u -> %d,%d irq=%d\n",
                  (unsigned)rz, (unsigned)rx, (unsigned)ry,
                  (int)x, (int)y, irqLow ? 1 : 0);
  } else if (!ok && now - lastDbg > 5000) {
    // Keeps the noise floor visible so TOUCH_Z_MIN can be retuned from data
    // rather than guessed — this is exactly what was missing when the phantom
    // taps first appeared.
    lastDbg = now;
    Serial.printf("touch idle: rawZ=%u irq=%d\n", (unsigned)rz, irqLow ? 1 : 0);
  }

  // edge-trigger on press so a held finger doesn't spam HA service calls
  if (!ok) { wasDown = false; return; }
  if (wasDown) return;
  if (now - lastTap < TOUCH_TAP_MS) return;
  wasDown = true;
  lastTap = now;

  Hit h = screenHitTest(x, y);
  if (h.kind == HIT_NONE) return;

  S.pressKind = h.kind;
  S.pressIdx  = h.idx;
  S.pressSub  = h.sub;
  S.pressMs   = now;

  switch (h.kind) {
    case HIT_TAB:
      Serial.printf("tap: tab %d\n", (int)h.idx);
      // handleTouch() runs before screenRender() in loop(), so the new page
      // paints on this same pass — no extra render call needed.
      S.page = (PageId)h.idx;
      break;

    case HIT_ROW:
      // MUST stay inside this branch. Unconditional, as it used to be, a scene
      // tap at index 4 would evaluate S.dev[4].name — one past a 4-element
      // array, landing in AppState's scalars, which printf then dereferences as
      // a char*. That is a LoadProhibited panic and a reboot, not a wrong name.
      Serial.printf("tap: %s btn %d\n", S.dev[h.idx].name, (int)h.sub);
      doAction(h.idx, h.sub);
      break;

    case HIT_SCENE:
      Serial.printf("tap: scene %d %s\n", (int)h.idx, sceneName((uint16_t)h.idx));
      doScene(h.idx);
      break;

    case HIT_SCROLL:
      // A page per tap, not a row: at three visible rows a row-at-a-time arrow
      // would need 23 taps to cross a 100-scene list. sceneScrollBy() clamps, so
      // the last page shows the tail rather than a screen of empty slots. No
      // repaint call needed — drawScenes() notices the offset moved.
      Serial.printf("tap: scene scroll %+d\n", (int)h.idx);
      sceneScrollBy((int16_t)(h.idx * SCENE_VIS_ROWS));
      break;

    case HIT_SETTING:
      Serial.printf("tap: setting %d/%d\n", (int)h.idx, (int)h.sub);
      doSetting(h.idx, h.sub);
      break;

    default:
      break;
  }
}

// ── networking ───────────────────────────────────────────

static void updateNetState() {
  static uint32_t lastTry = 0;
  bool up = (WiFi.status() == WL_CONNECTED);
  S.netState = up ? 1 : 2;

  // servicePoll() is skipped while Wi-Fi is down, so nothing else would ever
  // clear haOk — the bar showed WIFI red beside HA green, which hid the fault.
  if (!up) S.haOk = false;

  if (!up && millis() - lastTry > 5000) {
    lastTry = millis();
    WiFi.reconnect();
  }
  // the case LED is bright in a dark bedroom — only use it as an offline
  // beacon (active LOW)
  digitalWrite(PIN_LED_R, up ? HIGH : LOW);
}

static void setupWifi() {
  screenSplash();          // logo only

  if (strlen(WIFI_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t0 = millis();
    // Tick the splash's progress pips while we block here. Three fillCircles at
    // 5 Hz, and it is the difference between "connecting" and "hung".
    for (uint8_t ph = 0;
         WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_MS; ph++) {
      screenSplashProgress(ph);
      delay(200);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    // secrets.h carries no credentials, so hand off to WiFiManager. It usually
    // connects straight away from its OWN NVS store (that is the normal path
    // here) and only opens the hotspot when that fails — so this is NOT
    // necessarily a "needs setup" state, and the message must not claim it is.
    //
    // The splash carries no text, so if the portal DOES open, the hotspot name
    // appears nowhere on screen. Serial is the only in-situ hint; see README.
    screenSplash();
    Serial.printf("wifi: no creds in secrets.h -> WiFiManager; if it cannot "
                  "reconnect it opens the hotspot \"%s\" for %ds\n",
                  AP_PORTAL_NAME, 180);
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    wm.autoConnect(AP_PORTAL_NAME);
  }

  S.netState = (WiFi.status() == WL_CONNECTED) ? 1 : 2;
  Serial.printf("wifi: %s  ip=%s\n",
                S.netState == 1 ? "connected" : "FAILED",
                WiFi.localIP().toString().c_str());

  if (S.netState == 1) {
    // Kicks off the core's SNTP client, which then resyncs on its own — there is
    // no polling code for the clock anywhere. Deliberately does NOT block on the
    // first sync: the status bar shows "--:--" until it lands, which is a second
    // or two, and blocking here would just delay the UI appearing.
    configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
    Serial.printf("ntp: %s / %s  tz=%s\n", NTP_SERVER_1, NTP_SERVER_2, TZ_INFO);
  }
}

// Logs the wall clock once, the first time SNTP produces a valid time. Exists so
// the timezone can actually be verified against a known-good source rather than
// assumed correct.
static void logClockOnce() {
  static bool done = false;
  if (done) return;
  struct tm lt;
  if (!getLocalTime(&lt, 0)) return;
  done = true;

  // Log local AND UTC so the offset is self-evident: Asia/Bangkok must read
  // exactly UTC+7. Printing only local time would look plausible while being
  // hours wrong, which is the whole failure mode worth guarding against.
  time_t nowSec = time(nullptr);
  struct tm gt;
  gmtime_r(&nowSec, &gt);
  char lbuf[32], gbuf[32];
  strftime(lbuf, sizeof(lbuf), "%Y-%m-%d %H:%M:%S", &lt);
  strftime(gbuf, sizeof(gbuf), "%Y-%m-%d %H:%M:%S", &gt);
  Serial.printf("ntp: synced  local %s  utc %s  tz=%s\n", lbuf, gbuf, TZ_INFO);
}

#if CALIB_MODE
// ── touch calibration (env:calib) ────────────────────────
// Records the raw ADC pair at 4 known screen points and solves for the TOUCH_*
// constants. Written because the values inherited from btcticker-cyd mapped
// every real tap into the left third of the screen, and guessing replacements
// would have cost a 66-second flash cycle per attempt.

#define CAL_INSET 30
static const int16_t CAL_PT[4][2] = {
  {CAL_INSET,              CAL_INSET},              // 0 top-left
  {SCR_W - 1 - CAL_INSET,  CAL_INSET},              // 1 top-right
  {SCR_W - 1 - CAL_INSET,  SCR_H - 1 - CAL_INSET},  // 2 bottom-right
  {CAL_INSET,              SCR_H - 1 - CAL_INSET},  // 3 bottom-left
};
static uint16_t calA[4], calB[4];   // A = raw channel 1, B = raw channel 2

static inline bool calDown(uint16_t& a, uint16_t& b, uint16_t& z) {
  int16_t x, y;
  readTouch(x, y, &a, &b, &z);     // return value ignored; z is the gate
  return z >= TOUCH_Z_MIN && digitalRead(PIN_TOUCH_IRQ) == LOW;
}

// Blocks until a full press-and-release, returning the raw pair sampled at peak
// pressure (first contact is noisy and reads low).
static void calWaitTap(uint16_t& outA, uint16_t& outB) {
  uint16_t a, b, z;
  while (calDown(a, b, z)) delay(20);   // wait for release so one press != two points
  delay(150);

  uint16_t bestZ = 0;
  outA = outB = 0;
  while (bestZ == 0 || calDown(a, b, z)) {
    if (calDown(a, b, z) && z > bestZ) { bestZ = z; outA = a; outB = b; }
    delay(15);
  }
  Serial.printf("calib: raw=%u,%u  (peak z=%u)\n", outA, outB, bestZ);
  delay(150);
}

static void calibRun() {
  Serial.println("\n===== TOUCH CALIBRATION =====");
  Serial.println("tap the centre of each crosshair");

  for (int i = 0; i < 4; i++) {
    screenCalibTarget(i, 4, CAL_PT[i][0], CAL_PT[i][1]);
    Serial.printf("point %d: screen (%d,%d) -- waiting\n",
                  i + 1, CAL_PT[i][0], CAL_PT[i][1]);
    calWaitTap(calA[i], calB[i]);
  }

  // Points 0->1 differ ONLY in screen X, so whichever raw channel moves more
  // between them is the one tracking screen X.
  int dA = abs((int)calA[1] - (int)calA[0]);
  int dB = abs((int)calB[1] - (int)calB[0]);
  bool swapXY = (dB > dA);
  uint16_t* cx = swapXY ? calB : calA;
  uint16_t* cy = swapXY ? calA : calB;
  Serial.printf("\nchannel deltas across screen-X: A=%d B=%d -> swap_xy=%d\n",
                dA, dB, swapXY ? 1 : 0);

  // Linear fit per axis, then extrapolate from the inset points out to the
  // true screen edges (0 and SCR_*-1), which is what map() expects.
  const float lo = CAL_INSET;
  float hiX = SCR_W - 1 - CAL_INSET, hiY = SCR_H - 1 - CAL_INSET;

  float vx0 = (cx[0] + cx[3]) / 2.0f, vx1 = (cx[1] + cx[2]) / 2.0f;
  float slopeX = (vx1 - vx0) / (hiX - lo);
  float xAt0 = vx0 - slopeX * lo, xAtMax = vx0 + slopeX * ((SCR_W - 1) - lo);

  float vy0 = (cy[0] + cy[1]) / 2.0f, vy1 = (cy[2] + cy[3]) / 2.0f;
  float slopeY = (vy1 - vy0) / (hiY - lo);
  float yAt0 = vy0 - slopeY * lo, yAtMax = vy0 + slopeY * ((SCR_H - 1) - lo);

  // map() needs MIN<MAX, so a negative slope becomes an invert flag instead.
  int xMin, xMax, invX, yMin, yMax, invY;
  if (xAt0 <= xAtMax) { xMin = lroundf(xAt0);   xMax = lroundf(xAtMax); invX = 0; }
  else                { xMin = lroundf(xAtMax); xMax = lroundf(xAt0);   invX = 1; }
  if (yAt0 <= yAtMax) { yMin = lroundf(yAt0);   yMax = lroundf(yAtMax); invY = 0; }
  else                { yMin = lroundf(yAtMax); yMax = lroundf(yAt0);   invY = 1; }

  Serial.println("\n----- paste into include/config.h -----");
  Serial.printf("#define TOUCH_X_MIN    %d\n", xMin);
  Serial.printf("#define TOUCH_X_MAX    %d\n", xMax);
  Serial.printf("#define TOUCH_Y_MIN    %d\n", yMin);
  Serial.printf("#define TOUCH_Y_MAX    %d\n", yMax);
  Serial.printf("#define TOUCH_SWAP_XY  %d\n", swapXY ? 1 : 0);
  Serial.printf("#define TOUCH_INVERT_X %d\n", invX);
  Serial.printf("#define TOUCH_INVERT_Y %d\n", invY);
  Serial.println("---------------------------------------");

  // Live verify using the freshly computed numbers, so accuracy is confirmed
  // BEFORE spending a flash cycle on the real firmware.
  Serial.println("\nVERIFY: tap the outlined boxes; dot should land inside.");
  screenCalibVerifyScreen();
  while (true) {
    uint16_t a, b, z;
    if (calDown(a, b, z)) {
      uint16_t ax = swapXY ? b : a, ay = swapXY ? a : b;
      int16_t sx = map((int)ax, xMin, xMax, 0, SCR_W - 1);
      int16_t sy = map((int)ay, yMin, yMax, 0, SCR_H - 1);
      if (invX) sx = (SCR_W - 1) - sx;
      if (invY) sy = (SCR_H - 1) - sy;
      sx = constrain(sx, 0, SCR_W - 1);
      sy = constrain(sy, 0, SCR_H - 1);

      Hit h = screenHitTest(sx, sy);
      Serial.printf("verify: raw=%u,%u -> %d,%d  %s\n", ax, ay, sx, sy,
                    h.kind == HIT_ROW ? (String("row ") + h.idx + " btn " + h.sub).c_str()
                  : h.kind == HIT_TAB ? (String("tab ") + h.idx).c_str()
                  : "(no button)");
      screenCalibDot(sx, sy);
      while (calDown(a, b, z)) delay(20);
    }
    delay(20);
  }
}
#endif  // CALIB_MODE

// ── setup / loop ─────────────────────────────────────────

static void initDevices() {
  S.dev[0].entityId = ENT_BULB1; S.dev[0].name = NAME_BULB1; S.dev[0].kind = DEV_LIGHT;
  S.dev[1].entityId = ENT_BULB2; S.dev[1].name = NAME_BULB2; S.dev[1].kind = DEV_LIGHT;
  S.dev[2].entityId = ENT_BULB3; S.dev[2].name = NAME_BULB3; S.dev[2].kind = DEV_LIGHT;
  S.dev[3].entityId = ENT_AC;    S.dev[3].name = NAME_AC;    S.dev[3].kind = DEV_CLIMATE;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\ncyd-ha: Home Assistant bedroom controller");

  pinMode(PIN_LED_R, OUTPUT); digitalWrite(PIN_LED_R, HIGH);
  pinMode(PIN_LED_G, OUTPUT); digitalWrite(PIN_LED_G, HIGH);
  pinMode(PIN_LED_B, OUTPUT); digitalWrite(PIN_LED_B, HIGH);

  // Retained only for its pin setup. Deliberately NOT calling setCalibration():
  // nothing reads the library's mapping (every sample goes through xptRead()),
  // and post-TOUCH_SWAP_XY the TOUCH_X_* constants belong to the other channel,
  // so passing them in would be wrong if anyone ever switched to ts.get*().
  ts.begin();
  // explicit modes for the hand-rolled read path (ts.begin() sets these too,
  // but xptRead() must not depend on that staying true across lib versions)
  pinMode(PIN_TOUCH_MOSI, OUTPUT);
  pinMode(PIN_TOUCH_CLK,  OUTPUT);
  pinMode(PIN_TOUCH_CS,   OUTPUT);
  pinMode(PIN_TOUCH_MISO, INPUT);
  pinMode(PIN_TOUCH_IRQ,  INPUT);
  digitalWrite(PIN_TOUCH_CS, HIGH);
  Serial.printf("touch: bitbang mosi=%d miso=%d clk=%d cs=%d irq=%d\n",
                PIN_TOUCH_MOSI, PIN_TOUCH_MISO, PIN_TOUCH_CLK,
                PIN_TOUCH_CS, PIN_TOUCH_IRQ);

  // Before screenBegin(): `flip` decides the rotation of the one and only
  // first paint, and briIdx the first backlight duty. Costs ~15ms and touches
  // no network, so it is safe this early.
  settingsLoad();

#if CALIB_MODE
  // Calibration DEFINES the reference frame, so it must run at rotation 1, full
  // brightness and day colours — the TOUCH_* block it prints is meaningless
  // otherwise, and a 1% red crosshair cannot be aimed at. Forcing the values
  // here rather than adding conditionals downstream keeps every consumer right.
  S.set.flip = false; S.set.night = false; S.set.nightSched = false;
  S.set.briIdx = BRI_STEPS - 1;
#endif

  // initDevices() first: the calibration verify pass calls screenHitTest(),
  // which reads each row's kind to pick the right button widths.
  initDevices();
  screenBegin(S.set.flip);

  // MUST come after screenBegin(): TFT_eSPI::init() does
  //   pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  // (TFT_eSPI.cpp:786) which reclaims GPIO 21 as a plain output and detaches
  // any PWM already attached to it. Configuring LEDC first — as this used to —
  // left the duty silently ignored and the backlight pinned at 100%.
  ledcSetup(BL_CHANNEL, 5000, 8);
  ledcAttachPin(PIN_BACKLIGHT, BL_CHANNEL);
  // Seeds all three memoised effects through the same path that maintains
  // them, so nothing can desync from the panel.
  applySettings();

#if CALIB_MODE
  calibRun();          // never returns; no Wi-Fi or HA in calibration mode
#endif

  setupWifi();
  screenInvalidate();
}

void loop() {
  updateNetState();
  if (S.netState == 1) servicePoll();
  // Before handleTouch() so a scheduled flip lands before the tap that follows
  // it is mapped. All four calls are memoised no-ops when nothing changed.
  serviceNightSchedule();
  applySettings();
  handleTouch();
  screenRender();
  logClockOnce();
  logHeap();
  delay(20);
}
