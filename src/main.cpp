// Home Assistant bedroom controller for the CYD (ESP32-2432S028R).
//
// Everything runs on the single Arduino loop() task — no RTOS tasks, no
// locking. loop() calls, in order: updateNetState -> servicePoll ->
// serviceNightSchedule -> serviceDailyRestart -> applySettings ->
// handleTouch -> serviceIdleHome -> screenRender, then delay(20).
//
// The touch stack (xptWrite/xptRead/readTouch) is carried from the sibling
// ../btcticker-cyd firmware, which was calibrated against this same physical
// unit. Two hard-won constraints live in there — see readTouch().

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <XPT2046_Bitbang.h>
#include <driver/dac.h>
#include <math.h>

#include "config.h"
#include "secrets.h"
#include "state.h"
#include "net/ha.h"
#include "ui/screen.h"

AppState S;

static const uint8_t BRI_DUTY[BRI_STEPS] = BRI_DUTY_LIST;
static const uint8_t VOL_AMP[VOL_STEPS] = VOL_AMP_LIST;
// The LEDC timer divides the 80 MHz APB clock, so frequency and duty
// resolution trade against each other. Exceeding this makes ledc_timer_config
// fail at RUNTIME (a log line and a dead backlight), which is a poor way to
// find out; BL_PWM_HZ's comment explains why the frequency must not drop back
// under ~20 kHz to buy resolution.
static_assert((uint64_t)BL_PWM_HZ << BL_PWM_BITS <= 80000000ULL,
              "BL_PWM_HZ * 2^BL_PWM_BITS exceeds the 80 MHz LEDC source clock");
// A duty wider than the resolution would silently wrap to a dim value.
static_assert(BL_PWM_BITS == 8,
              "BRI_DUTY_LIST is expressed out of 255; rescale it if the "
              "resolution changes");


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

// Five keys, all uint8_t, clamped on load. Small enough that a RowMeta table
// (as ../btc-cyd-v2 uses for its thirteen) would cost more than it saves.
// K_NIGHT's stored range widened from 0/1 (bool) to 0/1/2 (NightMode) — same
// key, no migration needed: see NightMode's ordinal comment in state.h.
#define K_BRI   "s.bri"
#define K_NIGHT "s.nit"
#define K_SCHED "s.nsch"
#define K_FLIP  "s.flip"
// "s.vol6", not "s.vol": the 4-step version stored its index under "s.vol",
// and read back against the 6-step table those indices mean different levels
// (its 100% would come back as 60%). A new key just starts at VOL_DEFAULT.
#define K_VOL   "s.vol6"

static void settingsLoad() {
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    // Never hang on this — the device is still fully usable on defaults, and
    // putUChar() no-ops when the namespace failed to open, so saves fail soft.
    Serial.println("nvs: open failed, running on defaults");
    return;
  }
  uint8_t bri = prefs.getUChar(K_BRI, BRI_DEFAULT);
  S.set.briIdx     = (bri < BRI_STEPS) ? bri : BRI_DEFAULT;   // clamp corrupt
  uint8_t nm = prefs.getUChar(K_NIGHT, NIGHT_OFF);
  S.set.nightMode  = (nm < NIGHT_MODE_COUNT) ? nm : NIGHT_OFF;   // clamp corrupt
  S.set.nightSched = prefs.getUChar(K_SCHED, 1) != 0;
  S.set.flip       = prefs.getUChar(K_FLIP,  0) != 0;
  uint8_t vol = prefs.getUChar(K_VOL, VOL_DEFAULT);
  S.set.volIdx     = (vol < VOL_STEPS) ? vol : VOL_DEFAULT;   // clamp corrupt
  Serial.printf("settings: bri=%u/%u (duty %u) nightMode=%d sched=%d flip=%d "
                "vol=%u/%u (amp %u)\n",
                S.set.briIdx, BRI_STEPS - 1, BRI_DUTY[S.set.briIdx],
                S.set.nightMode, S.set.nightSched, S.set.flip,
                S.set.volIdx, VOL_STEPS - 1, VOL_AMP[S.set.volIdx]);
}

// Only a user tap persists. Scheduled night transitions deliberately do not:
// the boot-time window adoption in serviceNightSchedule() already restores the
// right state, so writing twice a day forever would buy nothing.
static void settingsSave(const char* key, uint8_t val) {
  prefs.putUChar(key, val);
}

// Rewriting the LEDC duty every frame visibly glitches CYD backlights, so only
// push it when it actually changes. The first call must ALWAYS write, since the
// boot seed goes through here and screenBegin() has left the pin LOW — skip it
// and the panel never lights at all.
//
// Hence the wider type: `last` must hold a value no duty can equal, and a
// uint8_t has none. It was seeded 0xFF, which is also BRI_DUTY's 100% step, so
// booting at 4/4 compared 255 == 255 on the very first call, returned early, and
// left the backlight at the 0 duty ledcAttachPin() starts with — a black screen
// with a healthy loop() behind it. The other four steps wrote fine, which is
// what made it look like dead hardware rather than a brightness bug.
static void applyBacklight(uint8_t duty) {
  static int16_t last = -1;
  if (duty == last) return;
  last = duty;
  ledcWrite(BL_CHANNEL, duty);
}

// Palette, rotation and backlight, all memoised no-ops when nothing changed —
// so this is ~3 compares at 50 Hz. Effective duty is computed HERE rather than
// in the tap handler, which is what makes a brightness change picked while
// night mode is dimming still land the moment night ends.
static void applySettings() {
  screenSetNightMode(S.set.nightMode);
  screenSetFlip(S.set.flip);
  applyBacklight(BRI_DUTY[S.set.nightMode == NIGHT_RED ? BRI_NIGHT : S.set.briIdx]);
}

// The schedule WRITES the Night mode picker at its two boundaries rather than
// overriding it. Edge-triggered, so between the boundaries a manual pick
// always wins and sticks — a level-triggered version would re-assert itself on
// the next render and make the picker physically un-turn-off-able before
// 08:00. This only ever writes NIGHT_RED/NIGHT_OFF, exactly as it wrote
// true/false before Night Shift existed — a manually-picked NIGHT_SHIFT rides
// through both boundary checks below untouched unless one of them actually
// fires, at which point it loses to whichever the boundary sets. The schedule
// never selects Shift itself.
// `hhmm` is read once per loop() pass (hour*60+min, -1 = NTP still unsynced)
// rather than by this function calling getLocalTime() itself — see loop()'s
// comment. An unsynced clock must never be read as midnight, so a negative
// value is treated exactly like the old "getLocalTime() returned false".
static void serviceNightSchedule(int16_t hhmm) {
  static int16_t lastMin = -1;

  // Reset so re-enabling the schedule re-adopts the current window rather than
  // waiting up to a day for the next boundary.
  if (!S.set.nightSched) { lastMin = -1; return; }
  if (hhmm < 0) return;

  const int16_t m = hhmm;
  if (m == lastMin) return;

  if (lastMin < 0) {
    // First valid clock reading, or the schedule was just re-enabled: adopt the
    // window. This is what makes a 02:00 reboot come up already in night mode.
    S.set.nightMode = (m >= NIGHT_ON_MIN || m < NIGHT_OFF_MIN) ? NIGHT_RED : NIGHT_OFF;
    Serial.printf("night: adopting window at %02d:%02d -> %d\n",
                  m / 60, m % 60, S.set.nightMode);
  } else {
    // The `lastMin < B && m > B` half catches a boundary missed because a
    // blocking HTTP call held loop() across the minute. Neither clause misfires
    // at the midnight wrap, where lastMin is 1439 and m is 0.
    if (m == NIGHT_ON_MIN  || (lastMin < NIGHT_ON_MIN  && m > NIGHT_ON_MIN)) {
      S.set.nightMode = NIGHT_RED;
      Serial.println("night: schedule on");
    } else if (m == NIGHT_OFF_MIN || (lastMin < NIGHT_OFF_MIN && m > NIGHT_OFF_MIN)) {
      S.set.nightMode = NIGHT_OFF;
      Serial.println("night: schedule off");
    }
  }
  lastMin = m;
}

// A daily restart at a quiet hour, so slow heap drift over a 24/7 uptime is a
// non-issue rather than something that has to be proven absent by a soak test
// that hasn't run long enough. Edge-triggered on the wall clock, the same
// pattern serviceNightSchedule() uses for its two boundaries — deliberately
// NOT the same function, because unlike night mode this has exactly one
// boundary and nothing to adopt at boot (a fresh boot is itself a restart).
// `hhmm`: see serviceNightSchedule()'s comment — same shared reading, same
// "negative means unsynced, treat like getLocalTime() returning false" rule.
static void serviceDailyRestart(int16_t hhmm) {
  static int16_t lastMin = -1;
  if (hhmm < 0) return;

  const int16_t m = hhmm;
  if (m == lastMin) return;

  // The first valid reading only adopts the clock; it must NOT restart, or a
  // boot that happens to land inside the same minute as RESTART_MIN would
  // trigger a second restart immediately. Every reading after that checks the
  // edge, including the `lastMin < X && m > X` clause that catches a boundary
  // missed because a blocking HA call held loop() across the minute — the
  // same trap serviceNightSchedule() guards against. Neither clause misfires
  // at the midnight wrap, where lastMin is 1439 and m is 0.
  if (lastMin >= 0 &&
      (m == RESTART_MIN || (lastMin < RESTART_MIN && m > RESTART_MIN))) {
    Serial.println("restart: scheduled daily restart");
    Serial.flush();
    ESP.restart();
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

// Decodes a button tap into what doAction() should apply and fire. This is
// NOT table-driven: several branches carry real per-button side effects
// (errMs stamps, distinct log lines, the fabsf at-limit check) that a table
// would need an escape hatch for per row, moving complexity rather than
// removing it. Returns false when there is nothing to do — already handled
// (logged, errMs stamped) right here — true with kind/iArg/fArg/sArg
// populated otherwise.
static bool decodeAction(DeviceState& d, int8_t btn, ActKind& kind,
                         int& iArg, float& fArg, const char*& sArg) {
  if (d.kind == DEV_CLIMATE) {
    switch (btn) {
      case 0: kind = ACT_C_MODE; sArg = "off";        return true;
      case 1: kind = ACT_C_MODE; sArg = AC_MODE_COOL; return true;
      case 2: kind = ACT_C_MODE; sArg = AC_MODE_DRY;  return true;
      // AC_BTN_TEMP (the slot between these two) is the setpoint READOUT and
      // falls through to `default: return false`. screenHitTest() already
      // reports a tap there as a miss, so this is belt and braces.
      case AC_BTN_TUP:
      case AC_BTN_TDN: {
        // Mirrors the chevrons' greyed-out BV_DISABLED state on screen: with
        // the AC off there is no active setpoint to step, so a tap here is
        // ignored the same way a swatch tap is on a bulb with !supportsCT.
        if (strcmp(d.mode, "off") == 0) return false;
        // The steps are relative, so they cannot act until a real setpoint is
        // known — the same rule that makes the readout show "--" until then.
        if (!d.known || isnan(d.target)) {
          d.errMs = millis();
          Serial.println("ac: no setpoint known yet, ignoring step");
          return false;
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
          return false;
        }
        kind = ACT_C_TEMP; fArg = t;
        return true;
      }
      default: return false;
    }
  }

  switch (btn) {
    case 0: kind = ACT_L_OFF;                        return true;
    case 1: kind = ACT_L_BRI;    iArg = BRI_LOW;      return true;
    case 2: kind = ACT_L_BRI;    iArg = BRI_MID;      return true;
    case 3: kind = ACT_L_BRI;    iArg = BRI_HIGH;     return true;
    case 4: if (!d.supportsCT) return false;
            kind = ACT_L_KELVIN; iArg = KELVIN_WARM;  return true;
    case 5: if (!d.supportsCT) return false;
            kind = ACT_L_KELVIN; iArg = KELVIN_COOL;  return true;
    default: return false;
  }
}

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
  if (!decodeAction(d, btn, kind, iArg, fArg, sArg)) return;

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
  // must restore the exact pre-tap state of every bulb. sizeof(DeviceState)
  // is 72 B on this target (measured), so 216 B of loop stack.
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

// Audible tap confirmation. Deliberately BLOCKING, for TOCK_MS: the obvious
// non-blocking version (start the sound here, finish it from loop()) cannot
// work, because doAction()/doScene() block loop() on HTTP for up to
// HTTP_READ_SVC_MS — the sound would stall mid-note for 1.5 s on every device
// tap. Driving the DAC from a timer ISR or an I2S DMA buffer would avoid the
// wait, but either is a second thread of execution, which this firmware has
// none of on purpose. 60 ms before the optimistic repaint is imperceptible.
//
// At the Volume setting's amplitude, and not at all when muted — returning
// before the loop means a muted panel doesn't pay the stall either.
//
// Samples go out through dac_output_voltage(), not dacWrite(): the Arduino
// wrapper re-runs dac_output_enable() — pad and RTC-GPIO init — on every call,
// which is set-up work, not a sample write, and far too heavy to repeat 16000
// times a second. beep() enables the DAC once per tock instead.
//
// Sample timing is a busy-wait on an absolute deadline per sample rather than a
// delayMicroseconds() per sample, so the maths and DAC write cost don't
// accumulate into a slower, flatter-pitched sound, and an interrupt landing
// mid-sound (Wi-Fi) only delays one sample instead of shifting every one after.
// Between tocks the speaker pin is a plain GPIO driven LOW with the DAC off —
// NOT a DAC held at some level. See the IDLE IS GROUND note in config.h: a DAC
// parked at mid-scale fed supply noise to the amp and crackled constantly.
// pinMode() goes through gpio_config(), which releases the pad from the RTC
// mux the DAC put it on, so the digital driver really does own it again.
static void speakerIdle() {
  dac_output_disable(SPEAKER_DAC);
  pinMode(PIN_SPEAKER, OUTPUT);
  digitalWrite(PIN_SPEAKER, LOW);
}

// The raw two-partial waveform at sample i, before any scaling. Unipolar:
// each partial is (1 - cos), so it starts at exactly 0 with zero slope and
// never goes below it — the tock rises out of the grounded idle and needs no
// bias to swing around.
static float tockSample(uint32_t i) {
  const float w1 = 2.0f * (float)M_PI * TOCK_HZ;
  const float t  = (float)i / TOCK_RATE_HZ;                   // seconds
  const float us = t * 1e6f;
  return expf(-us / TOCK_TAU1_US) * (1.0f - cosf(w1 * t))
       + TOCK_P2 * expf(-us / TOCK_TAU2_US) * (1.0f - cosf(w1 * TOCK_F2_X * t));
}

// Plays n samples of sample(i) — already scaled to DAC steps, 0..amp — out of
// the speaker DAC at TOCK_RATE_HZ, then hands the pin back to grounded idle.
// Shared by the tock and the boot chime so both get the same start-from-ground,
// raised-cosine fade-out and absolute-deadline timing. Returns elapsed us, which
// should be ~n / TOCK_RATE_HZ seconds: much more means sample() is overrunning
// its 62.5 us slot and the sound is playing slow and flat.
template <typename F>
static uint32_t dacPlay(uint32_t n, uint32_t fade, F sample) {
  // First sample is 0, the level the GPIO was holding, so enabling the DAC
  // under it is not a step.
  dac_output_voltage(SPEAKER_DAC, 0);
  dac_output_enable(SPEAKER_DAC);
  const uint32_t t0 = micros();
  for (uint32_t i = 0; i < n; i++) {
    float v = sample(i);
    // Raised-cosine fade over the last `fade` samples, so the tail lands on
    // exactly 0 rather than stepping there from a DAC step or two.
    if (i + fade >= n)
      v *= 0.5f * (1.0f + cosf((float)M_PI * (i + fade - n + 1) / fade));
    dac_output_voltage(SPEAKER_DAC, (uint8_t)lroundf(v));
    const uint32_t due = (uint32_t)(((uint64_t)(i + 1) * 1000000ULL) / TOCK_RATE_HZ);
    while (micros() - t0 < due) {}
  }
  speakerIdle();
  return micros() - t0;
}

static void beep() {
  const uint8_t amp = VOL_AMP[S.set.volIdx];
  if (amp == 0) return;
  const uint32_t n = (uint32_t)TOCK_RATE_HZ * TOCK_MS / 1000;
  const uint32_t fade = (uint32_t)TOCK_RATE_HZ * TOCK_FADE_MS / 1000;
  // The waveform's MEASURED peak, not the worst case of both partials peaking
  // on the same sample — that never happens, and scaling for it is what once
  // left the tock at 87 of 127 DAC steps. Deterministic, so it is measured
  // once, on the first tap, outside the timed loop.
  static float peak = 0.0f;
  if (peak == 0.0f)
    for (uint32_t i = 0; i < n; i++) peak = fmaxf(peak, tockSample(i));
  // tanh soft clip (see TOCK_DRIVE in config.h): the attack's peaks flatten
  // for loudness, the quiet tail stays clean. Dividing by tanh(TOCK_DRIVE) maps
  // the peak back to exactly amp, and tanh keeps 0 at 0, so the output stays
  // inside 0..amp and the tock still starts from ground.
  const float k = amp / tanhf(TOCK_DRIVE);
  const float g = TOCK_DRIVE / peak;
  const uint32_t us =
      dacPlay(n, fade, [&](uint32_t i) { return k * tanhf(g * tockSample(i)); });
  // Should read ~TOCK_MS * 1000.
  Serial.printf("tock: amp=%u %u samples in %u us\n", (unsigned)amp,
                (unsigned)n, (unsigned)us);
}

// ── boot chime ───────────────────────────────────────────

struct ChimeNote { uint16_t hz, atMs, tauMs; };
static const ChimeNote CHIME[] = CHIME_NOTE_LIST;
static constexpr uint8_t CHIME_N = sizeof(CHIME) / sizeof(CHIME[0]);

// The chime's waveform, one sample per next(): each note's two partials as the
// same unipolar env * (1 - cos) the tock uses, summed — with the envelope
// multiplied by an attack (1 - exp(-t / CHIME_ATTACK_MS)) so each note swells
// in rather than striking. That attack starts at 0 too, so a note entering
// while others ring is still not a step. Unlike tockSample() it
// is RECURSIVE — a rotating phasor and a multiplicative envelope per partial,
// a handful of multiplies each — rather than an expf() + cosf() per partial per
// sample. With ten partials sounding at once (5 notes x 2), the closed form
// would be 30 transcendentals with the attack in a 62.5 us slot that also has
// to fit a tanhf().
// Deterministic, so running it twice from reset() gives the same samples:
// once to measure the peak, once to play.
class ChimeSynth {
  struct Partial { uint32_t at; float c, s, cw, sw, e, d, a; };
  float ra_ = 0.0f;   // per-sample decay of the attack's remaining gap
  Partial p_[CHIME_N * 2];
  uint32_t i_ = 0;

 public:
  void reset() {
    i_ = 0;
    ra_ = expf(-1000.0f / ((float)CHIME_ATTACK_MS * TOCK_RATE_HZ));
    for (uint8_t n = 0; n < CHIME_N; n++) {
      for (uint8_t k = 0; k < 2; k++) {
        Partial& q = p_[n * 2 + k];
        const float hz  = CHIME[n].hz * (k ? CHIME_F2_X : 1.0f);
        const float tau = (float)CHIME[n].tauMs / (k ? CHIME_TAU2_DIV : 1); // ms
        const float w   = 2.0f * (float)M_PI * hz / TOCK_RATE_HZ;
        q.at = (uint32_t)CHIME[n].atMs * TOCK_RATE_HZ / 1000;
        q.c = 1.0f; q.s = 0.0f;              // phase 0: (1 - c) starts at 0
        q.cw = cosf(w); q.sw = sinf(w);
        q.e = k ? CHIME_P2 : 1.0f;
        q.d = expf(-1000.0f / (tau * TOCK_RATE_HZ));
        q.a = 1.0f;                          // attack gap: (1 - a) starts at 0
      }
    }
  }
  float next() {
    float v = 0.0f;
    for (Partial& q : p_) {
      if (i_ < q.at) continue;
      v += q.e * (1.0f - q.a) * (1.0f - q.c);
      const float c = q.c * q.cw - q.s * q.sw;
      q.s = q.s * q.cw + q.c * q.sw;
      q.c = c;
      q.e *= q.d;
      q.a *= ra_;
    }
    i_++;
    return v;
  }
};

// Once per POWER-ON, at the Volume setting, silent when muted. Deliberately
// not on every boot: serviceDailyRestart() reboots the panel at 05:30 in a
// bedroom, and a crash loop would chime on every lap — so a software restart,
// a panic, a watchdog or a brownout boots quietly. ESP_RST_POWERON is the
// plug going in (and the EN-pin reset esptool does after a flash); ESP_RST_EXT
// is the reset button.
static void bootChime() {
  const esp_reset_reason_t why = esp_reset_reason();
  if (why != ESP_RST_POWERON && why != ESP_RST_EXT) {
    Serial.printf("chime: skipped, reset reason %d\n", (int)why);
    return;
  }
  const uint8_t vol = VOL_AMP[S.set.volIdx];
  if (vol == 0) return;
  const float amp = vol * (CHIME_GAIN_PCT / 100.0f);
  const uint32_t n = (uint32_t)TOCK_RATE_HZ * CHIME_MS / 1000;
  const uint32_t fade = (uint32_t)TOCK_RATE_HZ * CHIME_FADE_MS / 1000;
  // Measured peak, same reason as beep(): the notes overlap, and summing every
  // partial's worst case would leave the chime far quieter than amp.
  ChimeSynth syn;
  syn.reset();
  float peak = 0.0f;
  for (uint32_t i = 0; i < n; i++) peak = fmaxf(peak, syn.next());
  const float k = amp / tanhf(CHIME_DRIVE);
  const float g = CHIME_DRIVE / peak;
  syn.reset();
  const uint32_t us =
      dacPlay(n, fade, [&](uint32_t) { return k * tanhf(g * syn.next()); });
  // Should read ~CHIME_MS * 1000.
  Serial.printf("chime: amp=%u %u samples in %u us\n", (unsigned)lroundf(amp),
                (unsigned)n, (unsigned)us);
}

// Not table-driven: the rows are two genuinely different shapes, not uniform
// arms. SET_ROW_BRI/SET_ROW_NIGHT/SET_ROW_VOL set an index-derived value from
// `sub` with its own bounds check; SET_ROW_TGL's `sub` only says WHICH of two
// plain toggles was hit. A single table spanning both would need a variant
// per row to cover the difference — that moves the complexity rather than
// removing it. The two toggles ARE identical in shape, so those collapse
// into toggleSetting() below.
static void toggleSetting(bool& field, const char* key) {
  field = !field;
  settingsSave(key, field);
}

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
    case SET_ROW_NIGHT: {
      if (sub < 0 || sub >= NIGHT_CHIPS) return;
      const uint8_t mode = NIGHT_CHIP_MODE[sub];
      if (S.set.nightMode == mode) return;
      S.set.nightMode = mode;
      settingsSave(K_NIGHT, S.set.nightMode);
      break;
    }
    case SET_ROW_VOL:
      if (sub < 0 || sub >= VOL_STEPS) return;
      if (S.set.volIdx != (uint8_t)sub) {
        S.set.volIdx = (uint8_t)sub;
        settingsSave(K_VOL, S.set.volIdx);
      }
      // The one tap that beeps AFTER acting rather than before (dispatchHit()
      // skips it): the beep is a preview of the level just picked, so it must
      // use the new duty. Also why a tap on the already-selected chip still
      // beeps rather than returning early — it is how you hear the level.
      beep();
      break;
    case SET_ROW_TGL:
      if (sub == SET_TGL_SCHED)     toggleSetting(S.set.nightSched, K_SCHED);
      else if (sub == SET_TGL_FLIP) toggleSetting(S.set.flip,       K_FLIP);
      else return;
      break;
    default: return;
  }
  applySettings();
  screenRender();
}

// ── touch ────────────────────────────────────────────────

// Gate, read, flip-mirror, debug-log and debounce — everything about getting
// ONE new tap's coordinates out of the digitiser. Returns false whenever
// there is nothing new to dispatch this pass (nothing touching, a rejected
// sample, a held finger, or too soon after the last tap); outX/outY are only
// meaningful when it returns true.
static bool sampleTouch(int16_t& outX, int16_t& outY) {
  static uint32_t lastTap = 0, lastDbg = 0, lastPoll = 0;
  static bool     wasDown = false;

  // A finger tap holds contact far longer than TOUCH_POLL_MS, so sampling at
  // 20 Hz instead of every pass halves the bit-bang cost for free.
  uint32_t pollNow = millis();
  if (pollNow - lastPoll < TOUCH_POLL_MS) return false;
  lastPoll = pollNow;

  // Cheaper still: skip the read entirely when idle. PENIRQ already has to
  // read LOW for a tap to be accepted (TOUCH_REQUIRE_IRQ below), so a HIGH
  // reading here means nothing is touching the panel and the 3-sample
  // bit-bang inside readTouch() (~1.5 ms) has nothing to find — one
  // digitalRead replaces it. Except every ~5 s, when a full read still runs
  // so the "touch idle: rawZ=.." log further down keeps the noise floor
  // visible instead of going dark the instant nothing is touching the glass.
  // This is deliberately just a gate on whether to READ, not a substitute for
  // the post-read IRQ check below — that one re-reads the pin after the
  // bit-bang and is what actually decides whether a tap is accepted.
  if (digitalRead(PIN_TOUCH_IRQ) == HIGH && pollNow - lastDbg < 5000) {
    wasDown = false;
    return false;
  }

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
  if (!ok) { wasDown = false; return false; }
  if (wasDown) return false;
  if (now - lastTap < TOUCH_TAP_MS) return false;
  wasDown = true;
  lastTap = now;

  outX = x;
  outY = y;
  return true;
}

// The 5-case dispatch, plus the press-flash bookkeeping every case shares.
static void dispatchHit(Hit h) {
  // A miss stays silent: a beep there would claim something happened. That
  // matters most on the AC setpoint readout, which is a miss precisely so an
  // off-target tap between the chevrons does nothing.
  if (h.kind == HIT_NONE) return;
  // A Volume chip beeps from doSetting() instead, at the level it just set.
  if (!(h.kind == HIT_SETTING && h.idx == SET_ROW_VOL)) beep();

  S.pressKind = h.kind;
  S.pressIdx  = h.idx;
  S.pressSub  = h.sub;
  S.pressMs   = millis();

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
      // A page per tap, not a row: at two visible rows a row-at-a-time arrow
      // would need 33 taps to cross a 100-scene list (34 grid rows) where paging
      // needs 17. The argument got STRONGER when the AC card took the bottom band
      // and the grid dropped from three rows to two — fewer rows per page means
      // more of them. sceneScrollBy() clamps, so the last page shows the tail
      // rather than a screen of empty slots. No repaint call needed —
      // drawScenes() notices the offset moved.
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

// millis() of the last touch, misses included. Starts at boot, so a panel
// nobody touches after power-on also ends up on Scenes.
static uint32_t lastTouchMs = 0;

static void handleTouch() {
  int16_t x, y;
  if (!sampleTouch(x, y)) return;
  lastTouchMs = millis();
  dispatchHit(screenHitTest(x, y));
}

// Back to Scenes after IDLE_HOME_MS without a touch (see config.h). Runs AFTER
// handleTouch(): a tap on the pass the timeout would fire has just reset
// lastTouchMs, so it is never hit-tested against a page that isn't on the
// glass yet. Before screenRender(), so the switch paints this same pass, the
// same way a tab tap does. Unsigned subtraction keeps it right across the
// 49-day millis() wrap.
static void serviceIdleHome() {
  if (S.page == PAGE_SCENES || millis() - lastTouchMs < IDLE_HOME_MS) return;
  Serial.printf("idle: %lus without a touch -> Scenes\n",
                (unsigned long)(IDLE_HOME_MS / 1000));
  S.page = PAGE_SCENES;
}

// ── networking ───────────────────────────────────────────

static void updateNetState() {
  static uint32_t lastTry = 0;
  bool up = (WiFi.status() == WL_CONNECTED);
  S.netState = up ? 1 : 2;

  // servicePoll() is skipped while Wi-Fi is down, so nothing else would ever
  // clear haOk. This one line is what lets the whole UI express connectivity as
  // a single bit: drawNoConn()'s "No Connection" banner gates on !S.haOk alone
  // and is correct for a Wi-Fi drop as well as an HA outage only because of it.
  // Originally it fixed the same fault in the old two-dot bar, which showed WIFI
  // red beside HA green and so hid the failure it was there to report.
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

  const bool creds = strlen(WIFI_SSID) > 0;
  uint32_t t0 = 0;
  if (creds) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    t0 = millis();
  }
  // With the logo, before either Wi-Fi path. With creds in secrets.h,
  // association runs on the Wi-Fi task while this blocks and the loop below
  // would only be waiting for it, so the chime is free (t0 is taken first, so it
  // does not eat into WIFI_CONNECT_MS). The WiFiManager path can't overlap it —
  // autoConnect() does its own begin() and blocking it would mean driving
  // WiFiManager's internals — so there it adds CHIME_MS to boot.
  bootChime();

  if (creds) {
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
// assumed correct. `timeValid`/`lt`: loop()'s single getLocalTime() reading —
// see its comment — rather than a second call here.
static void logClockOnce(bool timeValid, const struct tm& lt) {
  static bool done = false;
  if (done || !timeValid) return;
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
  S.set.flip = false; S.set.nightMode = NIGHT_OFF; S.set.nightSched = false;
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
  // ledcSetup() returns the frequency it actually achieved, not what it was
  // asked for, so log both: a clamped or rejected value shows up here rather
  // than being re-diagnosed by ear. See BL_PWM_HZ in config.h for why 25 kHz.
  const uint32_t blHz = ledcSetup(BL_CHANNEL, BL_PWM_HZ, BL_PWM_BITS);
  Serial.printf("backlight pwm: asked %u Hz, got %u Hz\n",
                (unsigned)BL_PWM_HZ, (unsigned)blHz);
  ledcAttachPin(PIN_BACKLIGHT, BL_CHANNEL);

  // Tap sound: park the speaker pin at ground. Driven LOW rather than left
  // floating into the amp, which would hum, and rather than a DAC level, which
  // crackled (see speakerIdle()).
  speakerIdle();
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

  // Read once, use everywhere this pass needs it. serviceNightSchedule(),
  // serviceDailyRestart(), the header's clock (screenSetClock()) and
  // logClockOnce() each used to call getLocalTime() independently — up to 4
  // calls a pass for a value that cannot change within one. Zero timeout:
  // must never block, and reports false until SNTP lands.
  struct tm localTm;
  const bool timeValid = getLocalTime(&localTm, 0);
  const int16_t hhmm = timeValid
      ? (int16_t)(localTm.tm_hour * 60 + localTm.tm_min) : -1;
  screenSetClock(hhmm);

  // Before handleTouch() so a scheduled flip lands before the tap that follows
  // it is mapped. All memoised no-ops when nothing changed.
  serviceNightSchedule(hhmm);
  serviceDailyRestart(hhmm);
  applySettings();
  handleTouch();
  serviceIdleHome();
  screenRender();
  logClockOnce(timeValid, localTm);
  logHeap();
  delay(20);
}
