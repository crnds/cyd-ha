// Host-compiled equivalence harness for the UI layer's pure functions — the
// ones that read DeviceState/THEME[] and return a value, touching no
// hardware: pctMatches, kelvinMatches, btnActive, iconVis, bulbHue,
// scenePlan, sceneActive, btnRect/settingChipRect/tabRect, screenHitTest —
// plus two call-counted checks of drawDeviceCard()/drawStatusRoom()'s
// dirty-region early-out.
//
// This is NOT a test of pixel output (simulator.html already owns visual
// fidelity to hardware). It exists so a refactor step can be diffed against a
// golden captured on the pre-refactor code: same stdout, byte for byte, means
// the logic is unchanged. See README.md in this directory for how to run it.
//
// Since screen.cpp was split into screen.cpp (core) + screen_devices.cpp +
// screen_scenes.cpp + screen_settings.cpp, some of this file's targets are
// `static` in screen.cpp (tabRect, drawStatusRoom) and some in
// screen_devices.cpp (btnActive, iconVis, bulbHue) — both are #included
// directly into this translation unit below, which is the only way to reach
// `static` (internal-linkage) symbols from outside their own file.
// screen_scenes.cpp/screen_settings.cpp are compiled and linked normally
// like gfx.cpp/icons.cpp/widgets.cpp/theme.cpp: this driver only ever calls
// their PUBLIC entry points (sceneActive, scenePlan, settingChipRect, ...),
// so an ordinary link is enough and there's no need to inline them too. Only
// TFT_eSPI.h and Arduino.h are swapped for host stubs (see stub/), by
// putting that directory first on the include path.

#include "state.h"

AppState S;

long hostDrawCalls = 0;

static unsigned long g_millis = 0;
unsigned long millis() { return g_millis; }
static void hostSetMillis(unsigned long ms) { g_millis = ms; }

#include "screen.cpp"
#include "screen_devices.cpp"

#include <cstdio>
#include <cstring>

// ── tiny deterministic hash, for the screenHitTest sweep ────────────────
// FNV-1a over the (kind, idx, sub) triple. A 230K-line golden for that sweep
// alone would dwarf everything else in the file for no readability gain; a
// hash catches any change while staying a few lines in the golden. A handful
// of named sample points are printed alongside it for a human to reason about
// if the hash ever does move.
static uint64_t g_hash = 1469598103934665603ULL;  // FNV-1a offset basis
static void hashByte(uint8_t b) {
  g_hash ^= b;
  g_hash *= 1099511628211ULL;
}
static void hashHit(const Hit& h) {
  hashByte((uint8_t)h.kind);
  hashByte((uint8_t)(h.idx >> 8));
  hashByte((uint8_t)h.idx);
  hashByte((uint8_t)h.sub);
}

static void resetDevices() {
  memset(S.dev, 0, sizeof(S.dev));
  for (int i = 0; i < NUM_DEVICES; i++) {
    S.dev[i].kind        = (i < NUM_BULBS) ? DEV_LIGHT : DEV_CLIMATE;
    S.dev[i].name        = (i < NUM_BULBS) ? "Bulb" : "AC";
    S.dev[i].entityId    = "entity";
    S.dev[i].avail       = true;
    S.dev[i].known       = true;
    S.dev[i].okMs        = 0;
    S.dev[i].pct         = -1;
    S.dev[i].kelvin      = -1;
    S.dev[i].supportsCT  = true;
    S.dev[i].target      = NAN;
    S.dev[i].room        = NAN;
    S.dev[i].tMin        = AC_TEMP_MIN_DEF;
    S.dev[i].tMax        = AC_TEMP_MAX_DEF;
    S.dev[i].tStep       = AC_TEMP_STEP_DEF;
    S.dev[i].humidity    = -1;
  }
}

// ── 1: pctMatches / kelvinMatches ────────────────────────────────────────
static void sweepMatches() {
  printf("== pctMatches ==\n");
  const int wants[] = { BRI_LOW, BRI_MID, BRI_HIGH };
  for (int want : wants)
    for (int got = -1; got <= 101; got++)
      printf("pctMatches want=%d got=%d -> %d\n", want, got, pctMatches(want, got));

  printf("== kelvinMatches ==\n");
  const int kwants[] = { KELVIN_WARM, KELVIN_COOL };
  for (int want : kwants)
    for (int got = -100; got <= 5000; got += 25)
      printf("kelvinMatches want=%d got=%d -> %d\n", want, got, kelvinMatches(want, got));
}

// ── 2: btnActive ──────────────────────────────────────────────────────────
static void sweepBtnActive() {
  printf("== btnActive (light) ==\n");
  const int pcts[]    = { -1, 0, 1, 4, 5, 6, 24, 25, 30, 35, 36, 94, 95, 100 };
  const int kelvins[] = { -1, 0, 2202, 2500, 2501, 3699, 3700, 4000 };
  resetDevices();
  DeviceState& d = S.dev[0];
  for (bool avail : { true, false })
    for (bool on : { true, false })
      for (int pct : pcts)
        for (int kelvin : kelvins)
          for (bool ct : { true, false }) {
            d.avail = avail; d.on = on; d.pct = pct; d.kelvin = kelvin; d.supportsCT = ct;
            for (uint8_t b = 0; b < BULB_BTNS; b++)
              printf("btnActive light avail=%d on=%d pct=%d kelvin=%d ct=%d b=%d -> %d\n",
                     avail, on, pct, kelvin, ct, b, btnActive(d, b));
          }

  printf("== btnActive (climate) ==\n");
  const char* modes[] = { "off", "cool", "dry", "fan_only", "heat", "auto", "" };
  DeviceState& ac = S.dev[NUM_BULBS];
  for (bool avail : { true, false })
    for (const char* m : modes) {
      ac.avail = avail;
      strncpy(ac.mode, m, sizeof(ac.mode) - 1);
      ac.mode[sizeof(ac.mode) - 1] = '\0';
      for (uint8_t b = 0; b < AC_BTNS; b++)
        printf("btnActive climate avail=%d mode=%s b=%d -> %d\n", avail, m, b, btnActive(ac, b));
    }
}

// ── 3: iconVis + bulbHue, across all 3 palettes ──────────────────────────
static void sweepIcon() {
  const int pcts[]    = { -1, 0, 50, 100 };
  const int kelvins[] = { -1, 0, 2202, 3000, 4000 };
  for (uint8_t mode = 0; mode < 3; mode++) {
    themeSetNightMode(mode);
    printf("== iconVis/bulbHue night=%d (light) ==\n", mode);
    resetDevices();
    DeviceState& d = S.dev[0];
    for (bool known : { true, false })
      for (bool avail : { true, false })
        for (bool on : { true, false })
          for (int pct : pcts)
            for (int kelvin : kelvins)
              for (bool ct : { true, false })
                for (bool stale : { true, false }) {
                  d.known = known; d.avail = avail; d.on = on;
                  d.pct = pct; d.kelvin = kelvin; d.supportsCT = ct;
                  uint8_t shape; uint16_t colour;
                  iconVis(d, stale, shape, colour);
                  printf("iconVis known=%d avail=%d on=%d pct=%d kelvin=%d ct=%d stale=%d "
                         "-> shape=%d colour=%04x  bulbHue=%04x\n",
                         known, avail, on, pct, kelvin, ct, stale, shape, colour, bulbHue(d));
                }

    printf("== iconVis night=%d (climate) ==\n", mode);
    const char* modes[] = { "off", "cool", "dry", "fan_only", "" };
    DeviceState& ac = S.dev[NUM_BULBS];
    for (bool known : { true, false })
      for (bool avail : { true, false })
        for (const char* m : modes)
          for (bool stale : { true, false }) {
            ac.known = known; ac.avail = avail;
            strncpy(ac.mode, m, sizeof(ac.mode) - 1);
            ac.mode[sizeof(ac.mode) - 1] = '\0';
            uint8_t shape; uint16_t colour;
            iconVis(ac, stale, shape, colour);
            printf("iconVis known=%d avail=%d mode=%s stale=%d -> shape=%d colour=%04x\n",
                   known, avail, m, stale, shape, colour);
          }
  }
  themeSetNightMode(0);
}

// ── 4/5: scenes ───────────────────────────────────────────────────────────
static void sweepScenes() {
  printf("== scenePlan / sceneName ==\n");
  printf("sceneCount=%u\n", sceneCount());
  for (uint16_t i = 0; i < sceneCount() + 1; i++) {
    uint8_t offMask, onMask; int pct, kelvin;
    scenePlan(i, offMask, onMask, pct, kelvin);
    printf("scene %u name=%s offMask=%u onMask=%u pct=%d kelvin=%d\n",
           i, sceneName(i), offMask, onMask, pct, kelvin);
  }

  printf("== sceneActive ==\n");
  struct BulbCase { bool known, avail, on; int pct, kelvin; bool ct; };
  // One curated case per interesting condition, applied uniformly to all
  // three bulbs — full cross-product across 3 independent bulbs would be
  // enormous for no extra coverage, since sceneActive treats each bulb
  // independently and stops at the first mismatch.
  const BulbCase cases[] = {
    { true,  true,  false, -1,   -1,   true  },   // OFF scene match
    { true,  true,  true,  30,   2202, true  },   // WORK scene match
    { true,  true,  true,  100,  2202, true  },   // AWAKE scene match
    { true,  true,  true,  100,  4000, true  },   // DAY scene match
    { true,  true,  true,  30,   4000, true  },   // on, wrong kelvin
    { true,  true,  true,  60,   2202, true  },   // on, wrong pct
    { false, true,  false, -1,   -1,   true  },   // unknown
    { true,  false, false, -1,   -1,   true  },   // unavailable
    { true,  true,  true,  100, -1,    false },   // no CT support, kelvin unknown
  };
  resetDevices();
  for (const auto& c : cases) {
    for (int i = 0; i < NUM_BULBS; i++) {
      DeviceState& d = S.dev[i];
      d.known = c.known; d.avail = c.avail; d.on = c.on;
      d.pct = c.pct; d.kelvin = c.kelvin; d.supportsCT = c.ct;
    }
    for (uint16_t idx = 0; idx < sceneCount(); idx++)
      printf("sceneActive known=%d avail=%d on=%d pct=%d kelvin=%d ct=%d scene=%u -> %d\n",
             c.known, c.avail, c.on, c.pct, c.kelvin, c.ct, idx, sceneActive(idx));
  }

  // One bulb offline, the other two matching. The offline bulb is skipped,
  // and where two scenes then tie (OFF vs RELAX with bulb 3 down) the earlier
  // one in the table is the only one lit.
  printf("== sceneActive, one bulb offline ==\n");
  const BulbCase live[] = { cases[0], cases[1], cases[2], cases[3] };
  for (int off = 0; off < NUM_BULBS; off++) {
    for (const auto& c : live) {
      for (int i = 0; i < NUM_BULBS; i++) {
        DeviceState& d = S.dev[i];
        d.known = true; d.avail = (i != off); d.on = c.on;
        d.pct = c.pct; d.kelvin = c.kelvin; d.supportsCT = c.ct;
      }
      printf("offline=%d on=%d pct=%d kelvin=%d ->", off, c.on, c.pct, c.kelvin);
      for (uint16_t idx = 0; idx < sceneCount(); idx++)
        printf(" %d", sceneActive(idx));
      printf("\n");
    }
  }
}

// ── 6: pure geometry ──────────────────────────────────────────────────────
static void dumpGeometry() {
  printf("== btnRect ==\n");
  resetDevices();
  for (uint8_t dev = 0; dev < NUM_DEVICES; dev++)
    for (uint8_t b = 0; b < btnCount(S.dev[dev]); b++) {
      const Rect r = btnRect(dev, b);
      printf("btnRect dev=%d b=%d -> x=%d y=%d w=%d h=%d\n", dev, b, r.x, r.y, r.w, r.h);
    }

  printf("== briRect ==\n");
  for (uint8_t b = 0; b < BRI_STEPS; b++) {
    const Rect r = settingChipRect(SET_ROW_BRI, b);
    printf("briRect b=%d -> x=%d y=%d w=%d h=%d\n", b, r.x, r.y, r.w, r.h);
  }

  printf("== nightRect ==\n");
  for (uint8_t c = 0; c < NIGHT_CHIPS; c++) {
    const Rect r = settingChipRect(SET_ROW_NIGHT, c);
    printf("nightRect c=%d -> x=%d y=%d w=%d h=%d\n", c, r.x, r.y, r.w, r.h);
  }

  printf("== volRect ==\n");
  for (uint8_t v = 0; v < VOL_STEPS; v++) {
    const Rect r = settingChipRect(SET_ROW_VOL, v);
    printf("volRect v=%d -> x=%d y=%d w=%d h=%d\n", v, r.x, r.y, r.w, r.h);
  }

  printf("== tabRect ==\n");
  for (uint8_t t = 0; t < TAB_COUNT; t++) {
    int16_t x, w;
    tabRect(t, x, w);
    printf("tabRect t=%d -> x=%d w=%d\n", t, x, w);
  }
}

// ── 7: screenHitTest, exhaustive over the panel, hashed per page ─────────
static void sweepHitTest() {
  printf("== screenHitTest ==\n");
  resetDevices();
  const PageId pages[] = { PAGE_DEVICES, PAGE_SCENES, PAGE_SETTINGS };
  const char* pageNames[] = { "devices", "scenes", "settings" };
  for (int p = 0; p < 3; p++) {
    S.page = pages[p];
    S.sceneRow = 0;
    g_hash = 1469598103934665603ULL;
    for (int16_t py = 0; py < SCR_H; py++)
      for (int16_t px = 0; px < SCR_W; px++)
        hashHit(screenHitTest(px, py));
    printf("hitTestHash page=%s -> %016llx\n", pageNames[p], (unsigned long long)g_hash);

    // A handful of named points for a human to reason about if the hash ever
    // moves: all four corners, the screen centre, and the first tab cell.
    //
    // The last four are the AC card's row band, which the Scenes page now
    // carries as well as Devices — the case a bare hash is least legible about.
    // In order: a mode chip, the setpoint's dead cell, and then the one that
    // pins the hit test's BRANCH ORDER — x 301 is inside both the scroll
    // gutter (from 300) and the card's up chevron (276..303), so on Scenes it
    // must read as the chevron, not as a scroll arrow. The last is the 20px
    // blank band between the last tile row and the card, which must be a miss
    // on Scenes and an ordinary row-2 tap on the other two pages.
    const int16_t samplesX[] = { 0, SCR_W - 1, 0, SCR_W - 1, SCR_W / 2, 10,
                                 128, 250, 301, SCR_W / 2 };
    const int16_t samplesY[] = { 0, 0, SCR_H - 1, SCR_H - 1, SCR_H / 2, STATUS_Y0 + 2,
                                 180, 180, 180, 145 };
    for (int s = 0; s < 10; s++) {
      Hit h = screenHitTest(samplesX[s], samplesY[s]);
      printf("hitTest page=%s (%d,%d) -> kind=%d idx=%d sub=%d\n",
             pageNames[p], samplesX[s], samplesY[s], h.kind, h.idx, h.sub);
    }
  }
}

// ── 8/9: dirty-region early-out, via the draw-call counter ───────────────
static long drawDeviceCardOnce(uint8_t dev, bool force) {
  long before = hostDrawCalls;
  drawDeviceCard(dev, force);
  return hostDrawCalls - before;
}
static long drawStatusRoomOnce(bool force) {
  long before = hostDrawCalls;
  drawStatusRoom(force);
  return hostDrawCalls - before;
}

static void sweepDirtyRegions() {
  printf("== drawDeviceCard dirty-region deltas ==\n");
  resetDevices();
  screenInvalidate();
  hostSetMillis(1000);

  printf("bulb first-draw delta=%ld\n", drawDeviceCardOnce(0, true));
  printf("bulb repeat-unchanged delta=%ld\n", drawDeviceCardOnce(0, false));
  S.dev[0].pct = 40;   // still off: pct is irrelevant to an off bulb's appearance
  printf("bulb pct-changed-while-off delta=%ld\n", drawDeviceCardOnce(0, false));
  S.dev[0].on = true; S.dev[0].pct = 30; S.dev[0].kelvin = 2202;
  printf("bulb turned-on delta=%ld\n", drawDeviceCardOnce(0, false));
  printf("bulb repeat-unchanged-2 delta=%ld\n", drawDeviceCardOnce(0, false));
  S.dev[0].pct = 100;
  printf("bulb pct-changed-while-on delta=%ld\n", drawDeviceCardOnce(0, false));
  printf("bulb repeat-unchanged-3 delta=%ld\n", drawDeviceCardOnce(0, false));
  S.dev[0].kelvin = 4000;
  printf("bulb kelvin-changed delta=%ld\n", drawDeviceCardOnce(0, false));
  S.dev[0].avail = false;
  printf("bulb went-unavailable delta=%ld\n", drawDeviceCardOnce(0, false));
  printf("bulb repeat-unavailable delta=%ld\n", drawDeviceCardOnce(0, false));
  hostSetMillis(1000 + DEVICE_STALE_MS + 1);
  printf("bulb went-stale (time only) delta=%ld\n", drawDeviceCardOnce(0, false));

  screenInvalidate();
  hostSetMillis(2000);
  printf("ac first-draw delta=%ld\n", drawDeviceCardOnce(NUM_BULBS, true));
  printf("ac repeat-unchanged delta=%ld\n", drawDeviceCardOnce(NUM_BULBS, false));
  S.dev[NUM_BULBS].target = 24.0f;
  printf("ac target-changed delta=%ld\n", drawDeviceCardOnce(NUM_BULBS, false));
  printf("ac repeat-unchanged-2 delta=%ld\n", drawDeviceCardOnce(NUM_BULBS, false));
  S.dev[NUM_BULBS].target = 22.0f;
  printf("ac target-changed-2 delta=%ld\n", drawDeviceCardOnce(NUM_BULBS, false));
  strncpy(S.dev[NUM_BULBS].mode, "cool", sizeof(S.dev[NUM_BULBS].mode) - 1);
  printf("ac mode-cool delta=%ld\n", drawDeviceCardOnce(NUM_BULBS, false));
  printf("ac repeat-mode-cool delta=%ld\n", drawDeviceCardOnce(NUM_BULBS, false));
  strncpy(S.dev[NUM_BULBS].mode, "off", sizeof(S.dev[NUM_BULBS].mode) - 1);
  printf("ac mode-off delta=%ld\n", drawDeviceCardOnce(NUM_BULBS, false));
  printf("ac repeat-mode-off delta=%ld\n", drawDeviceCardOnce(NUM_BULBS, false));

  printf("== drawDeviceCard press-flash deltas ==\n");
  // pressKind defaults to HIT_NONE, so nothing above this point ever
  // exercised pressedNow()'s true branch. That matters here specifically:
  // the press flash expires by TIME, not by any state change (CLAUDE.md),
  // which is exactly the class of bug a fingerprint/dirty-region refactor
  // could reintroduce by leaving a time-derived flag out of a compare.
  screenInvalidate();
  hostSetMillis(4000);
  S.dev[1].on = false;  // bulb 1, left in resetDevices()'s default state
  drawDeviceCardOnce(1, true);  // clean baseline snapshot, not itself asserted
  for (uint8_t b = 0; b < BULB_BTNS; b++) {
    S.pressKind = HIT_ROW; S.pressIdx = 1; S.pressSub = (int8_t)b; S.pressMs = g_millis;
    printf("bulb press b=%d start delta=%ld\n", b, drawDeviceCardOnce(1, false));
    printf("bulb press b=%d repeat delta=%ld\n", b, drawDeviceCardOnce(1, false));
    hostSetMillis(g_millis + PRESS_FLASH_MS + 1);
    printf("bulb press b=%d expired delta=%ld\n", b, drawDeviceCardOnce(1, false));
    S.pressKind = HIT_NONE; S.pressIdx = -1; S.pressSub = -1; S.pressMs = 0;
    printf("bulb press b=%d cleared-repeat delta=%ld\n", b, drawDeviceCardOnce(1, false));
  }
  // Page-safety: a press recorded against a DIFFERENT device row must not
  // repaint this one (pressKind/pressIdx are what stop a scene tap at index 2
  // from also inverting row 2 on the Devices page — see CLAUDE.md).
  S.pressKind = HIT_ROW; S.pressIdx = 2; S.pressSub = 0; S.pressMs = g_millis;
  printf("bulb press-on-other-row delta=%ld\n", drawDeviceCardOnce(1, false));
  S.pressKind = HIT_NONE; S.pressIdx = -1; S.pressSub = -1; S.pressMs = 0;

  printf("== drawStatusRoom dirty-region deltas ==\n");
  screenInvalidate();
  hostSetMillis(3000);
  printf("room first-draw delta=%ld\n", drawStatusRoomOnce(true));
  printf("room repeat-unchanged delta=%ld\n", drawStatusRoomOnce(false));
  S.dev[NUM_BULBS].room = 27.0f;
  printf("room temp-changed delta=%ld\n", drawStatusRoomOnce(false));
  printf("room repeat-unchanged-2 delta=%ld\n", drawStatusRoomOnce(false));
  S.dev[NUM_BULBS].room = 24.0f;
  printf("room temp-changed-2 delta=%ld\n", drawStatusRoomOnce(false));
  S.dev[NUM_BULBS].humidity = 55;
  printf("room humidity-changed delta=%ld\n", drawStatusRoomOnce(false));
  printf("room repeat-unchanged-3 delta=%ld\n", drawStatusRoomOnce(false));
}

int main() {
  sweepMatches();
  sweepBtnActive();
  sweepIcon();
  sweepScenes();
  dumpGeometry();
  sweepHitTest();
  sweepDirtyRegions();
  return 0;
}
