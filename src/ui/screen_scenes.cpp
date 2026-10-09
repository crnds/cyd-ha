#include "screen_int.h"
#include <string.h>

// The Scenes page: macros over the three bulbs, in a scrolling 3-column grid
// of 88x64 tiles, TWO rows of them. The bottom row band of the body is not
// this file's — the AC card is copied there from the Devices page, drawn by
// screen.cpp's screenRender() calling straight into drawDeviceCard(), so
// nothing here has to know about it beyond stopping at SCENE_AC_Y0. See
// screen_int.h for the cross-file contract, config.h's Scenes block for why
// the grid no longer fills the body, and CLAUDE.md's "Scenes" / "The scene
// grid is built to scale" sections for the rest of the design rationale.

// Macros over the three bulbs. The AC is deliberately untouched: it runs on a
// different comfort schedule than the lighting, and folding it in would make
// every scene tap a Sensibo cloud round trip.
//
// THIS TABLE IS THE ONLY DECLARATION OF HOW MANY SCENES THERE ARE. It is
// unsized on purpose and SCENE_N below is derived from it, so adding a scene is
// one line here and nothing else — no count in config.h to forget, and no
// snapshot to resize (the renderer's snapshot is per visible TILE, not per
// scene). That is what makes the page scale to a list far longer than the
// screen. Kept private to this file — sceneCount()/sceneMaxRow() (screen.h)
// are the only way another file learns how many there are or how far the grid
// scrolls, so the table stays the single source of truth for both.
struct SceneBulb { bool on; int16_t pct; int16_t kelvin; };
struct Scene { const char* name; SceneBulb b[NUM_BULBS]; };

#define SB_OFF   {false, 0, 0}
#define SB_MIDW  {true, BRI_MID,  KELVIN_WARM}
#define SB_HIW   {true, BRI_HIGH, KELVIN_WARM}
#define SB_HIC   {true, BRI_HIGH, KELVIN_COOL}

static const Scene SCENE[] = {
  {"OFF",   {SB_OFF,  SB_OFF,  SB_OFF }},
  {"RELAX", {SB_OFF,  SB_OFF,  SB_MIDW}},
  {"WORK",  {SB_MIDW, SB_MIDW, SB_MIDW}},
  {"AWAKE", {SB_HIW,  SB_HIW,  SB_HIW }},
  {"DAY",   {SB_HIC,  SB_HIC,  SB_HIC }},
};

static constexpr uint16_t SCENE_N = sizeof(SCENE) / sizeof(SCENE[0]);
// Grid rows the table needs, and the largest scroll offset that still fills the
// screen. SCENE_MAX_ROW is 0 today, which is what makes the whole scroll
// affordance compile out and the page look exactly like a fixed one. Note the
// threshold moved when the AC card took a band and SCENE_VIS_ROWS went 3 -> 2:
// scrolling now goes live at the 7th scene rather than the 10th.
static constexpr uint16_t SCENE_ROWS_N = (SCENE_N + SCENE_COLS - 1) / SCENE_COLS;
static constexpr uint16_t SCENE_MAX_ROW =
    SCENE_ROWS_N > SCENE_VIS_ROWS ? (uint16_t)(SCENE_ROWS_N - SCENE_VIS_ROWS) : 0;

SceneSnap sceneSnap;

// Take an on-screen SLOT (0..SCENE_PER_PAGE-1), not a scene index — which scene
// a slot shows depends on S.sceneRow.
int16_t sceneTileX(uint8_t slot) {
  return SCENE_X0 + (slot % SCENE_COLS) * SCENE_PITCH_X;
}
int16_t sceneTileY(uint8_t slot) {
  return ROWS_Y0 + (slot / SCENE_COLS) * SCENE_PITCH_Y;
}

// Does the LIVE state of every REACHABLE bulb match this scene's definition?
// An unavailable bulb is skipped rather than failing the match: it has no
// current state to compare, and letting it veto every scene blanked the whole
// grid the moment one bulb dropped off the network — the tile drawing a red
// pip for it is what says that bulb is not being vouched for.
static bool sceneMatches(uint16_t idx) {
  const Scene& sc = SCENE[idx];
  bool compared = false;

  for (uint8_t i = 0; i < NUM_BULBS; i++) {
    const DeviceState& d = S.dev[i];
    const SceneBulb&   w = sc.b[i];

    // Never polled is different from offline: that is boot, nothing at all is
    // known yet, and a lit tile would claim a room state we have not seen.
    if (!d.known) return false;
    if (!d.avail) continue;
    compared = true;

    if (!w.on) { if (d.on) return false; continue; }   // want off
    if (!d.on) return false;                           // want on, is off

    // pct/kelvin only mean anything on a bulb that is ON — an off bulb keeps
    // its last known level rather than blanking it (the fail-soft rule), so
    // these would compare stale values. The two guards above make that
    // unreachable, which is why they must stay above this and not below.
    if (d.pct < 0 || !pctMatches(w.pct, d.pct)) return false;

    if (w.kelvin > 0 && d.supportsCT) {
      // An unknown kelvin is "cannot confirm", not "match". Treating it as a
      // match would light AWAKE and DAY simultaneously — they differ only
      // here — and two active tiles reads as a bug. Skipped entirely on a
      // non-CT bulb, where requiring it would make every scene permanently
      // inactive instead.
      if (d.kelvin <= 0 || !kelvinMatches(w.kelvin, d.kelvin)) return false;
    }
  }
  // With every bulb offline there is nothing left to match on, and "matches
  // vacuously" would light every tile at once.
  return compared;
}

// A scene is ACTIVE when the LIVE state of the bulbs matches its definition.
// Derived every render, never latched — that is exactly what makes overriding
// one bulb on the Devices page deselect the scene, and what makes a change from
// the HA app select the matching one, with no "current scene" variable to fall
// out of sync.
//
// With a bulb offline, two scenes that differ ONLY in that bulb both match
// (OFF and RELAX, with bulb 3 down). Two lit tiles reads as a bug, so the
// earlier one in the table wins. With every bulb reachable the table cannot
// produce a tie, so the scan is skipped and the common path costs what it did.
bool sceneActive(uint16_t idx) {
  if (idx >= SCENE_N || !sceneMatches(idx)) return false;

  bool anyOffline = false;
  for (uint8_t i = 0; i < NUM_BULBS; i++)
    if (!S.dev[i].avail) anyOffline = true;
  if (!anyOffline) return true;

  for (uint16_t j = 0; j < idx; j++)
    if (sceneMatches(j)) return false;
  return true;
}

const char* sceneName(uint16_t idx) {
  return idx < SCENE_N ? SCENE[idx].name : "?";
}

uint16_t sceneCount()  { return SCENE_N; }
uint16_t sceneMaxRow() { return SCENE_MAX_ROW; }

bool sceneScrollBy(int16_t rows) {
  if (SCENE_MAX_ROW == 0) return false;
  int32_t r = (int32_t)S.sceneRow + rows;
  if (r < 0)               r = 0;
  if (r > SCENE_MAX_ROW)   r = SCENE_MAX_ROW;
  if ((uint16_t)r == S.sceneRow) return false;
  S.sceneRow = (uint16_t)r;
  return true;
}

void scenePlan(uint16_t idx, uint8_t& offMask, uint8_t& onMask,
               int& pct, int& kelvin) {
  offMask = onMask = 0;
  pct = kelvin = 0;
  if (idx >= SCENE_N) return;

  for (uint8_t i = 0; i < NUM_BULBS; i++) {
    const SceneBulb& w = SCENE[idx].b[i];
    if (w.on) {
      onMask |= (uint8_t)(1u << i);
      // Collapsing the ON set into one call is only valid while every ON bulb
      // in a scene shares a level and colour. That holds for all five today; a
      // future scene with mixed levels would silently get the last bulb's.
      pct = w.pct; kelvin = w.kelvin;
    } else {
      offMask |= (uint8_t)(1u << i);
    }
  }
}

// One tile. `slot` is where it sits on screen, `idx` which scene it shows.
// `offline` is a bitmask of bulbs HA reports unavailable; their pips are drawn
// red whatever the scene would set them to.
static void drawSceneTile(uint8_t slot, uint16_t idx, uint8_t vis,
                          uint8_t offline) {
  const int16_t x = sceneTileX(slot), y = sceneTileY(slot);

  // A slot past the end of the table is blanked with the same rect the tile
  // occupies, which is why a scroll needs no body wipe: every tile either
  // repaints its own rect or blanks it, and the gaps never change content.
  if (idx >= SCENE_N) {
    tft.fillRect(x, y, SCENE_TILE_W, SCENE_TILE_H, C_BG);
    return;
  }

  // `mono` means "the fill has taken over the tile's colour" — the pips must
  // then be drawn in the foreground colour rather than in warm/cool, which
  // against a C_TEXT press fill or a C_ERROR error border would read as noise.
  uint16_t fill, edge, fg;
  bool     mono = true;
  switch (vis) {
    case BV_PRESSED:
      fill = C_TEXT;            edge = C_TEXT;   fg = C_BG;                  break;
    case BV_ACTIVE:
      // Tinted rather than a solid accent slab: at 88x64 a saturated fill is the
      // loudest thing on the panel, and it keeps the solid fill meaningful as
      // the press flash. Colour pips stay legible on a tint, so not mono.
      fill = tint565(C_ACCENT); edge = C_ACCENT; fg = C_ACCENT; mono = false; break;
    case BV_ERR:
      fill = C_SURFACE;         edge = C_ERROR;  fg = C_ERROR;               break;
    case BV_DISABLED:
      fill = C_SURFACE;         edge = C_SURFACE; fg = C_DISABLED;           break;
    default:
      fill = C_SURFACE;         edge = C_BORDER; fg = C_TEXT2;  mono = false; break;
  }

  wCard(x, y, SCENE_TILE_W, SCENE_TILE_H, fill, edge);

  // Three pips, one per bulb, read straight off this scene's own definition:
  // position says WHICH bulb, colour warm-or-cool, and how bright the pip is
  // says what level it will be set to. Derived, so it cannot describe something
  // the tap won't send.
  const Scene&  sc = SCENE[idx];
  const int16_t cx = x + SCENE_TILE_W / 2;
  const int16_t py = y + SCENE_PIP_DY;
  for (uint8_t i = 0; i < NUM_BULBS; i++) {
    const int16_t px = cx + ((int16_t)i - 1) * SCENE_PIP_GAP;
    const SceneBulb& b = sc.b[i];
    // An unreachable bulb is a solid red pip, the same C_ERROR its own card's
    // border and icon carry on Devices. Solid, not a ring: a ring already
    // means "this scene turns it off". The press flash keeps its mono pips —
    // it lasts PRESS_FLASH_MS and is about the tap, not the bulbs.
    if ((offline & (1u << i)) && vis != BV_PRESSED) {
      wPip(px, py, SCENE_PIP_R, C_ERROR, true);
      continue;
    }
    if (!b.on) {
      wPip(px, py, SCENE_PIP_R, mono ? fg : C_DISABLED, false);
      continue;
    }
    uint16_t c;
    if (mono) {
      c = fg;
    } else {
      c = (b.kelvin <= 0)               ? C_TEXT2
        : (b.kelvin <= KELVIN_WARM_MAX) ? C_WARM : C_COOL;
      // Level as a blend toward the tile, matching how a device card's icon
      // blends toward its own surface. 1% still lands at 70/255, visible.
      if (b.pct < BRI_HIGH)
        c = lerp565(fill, c, (uint8_t)(70 + (int)b.pct * 185 / 100));
    }
    wPip(px, py, SCENE_PIP_R, c, true);
  }

  // 88px of tile holds the name at full size, which is the entire reason this
  // grid dropped from four columns to three. textFit still guards it, because a
  // scene added to the table later cannot be checked against a measured width.
  textFit(F_TITLE, sc.name, nullptr, cx, y + SCENE_NAME_DY,
          SCENE_TILE_W - 2 * SP_2, fg);
}

// The gutter at x 300..319. While every scene fits on one page SCENE_MAX_ROW is a
// constexpr 0, so everything below the early return is unreachable and this is
// just a 20px margin costing one fillRect — but the arrows, the track and the
// thumb are all here ready for the first table entry that overflows the screen.
static void drawSceneScrollbar(uint8_t pressed) {
  // The GRID's bottom edge (SCENE_AC_Y0), which is neither the panel's nor the
  // body's: the header sits below the body, and the AC card sits below the grid.
  // The card is full card width, so it runs UNDER this column — clearing past
  // SCENE_AC_Y0 would eat its top rows, and nothing would put them back until
  // the next page switch.
  tft.fillRect(SCENE_SB_X0, ROWS_Y0, SCENE_SB_W, SCENE_AC_Y0 - ROWS_Y0, C_BG);
  if (SCENE_MAX_ROW == 0) return;

  const int16_t cx  = SCENE_SB_X0 + SCENE_SB_W / 2;
  const int16_t bot = SCENE_AC_Y0 - 1;
  const bool atTop = (S.sceneRow == 0), atBot = (S.sceneRow >= SCENE_MAX_ROW);

  // Dimmed at the ends rather than hidden: a control that vanishes moves the
  // other one's apparent target, and this is a resistive panel.
  icoChevron(cx, ROWS_Y0 + 13, CHEV_UP,
             pressed == 1 ? C_TEXT : (atTop ? C_DISABLED : C_TEXT2), 6, 5);
  icoChevron(cx, bot - 13, CHEV_DOWN,
             pressed == 2 ? C_TEXT : (atBot ? C_DISABLED : C_TEXT2), 6, 5);

  // Track between the arrows, with a thumb sized by how much of the list is on
  // screen — the only thing that tells you a long list is long.
  const int16_t tY = ROWS_Y0 + SP_6;
  const int16_t tH = (bot - SP_6) - tY;
  tft.drawFastVLine(cx, tY, tH, C_DIVIDER);

  int16_t th = (int16_t)((int32_t)tH * SCENE_VIS_ROWS / SCENE_ROWS_N);
  if (th < 10) th = 10;
  // The early return above makes SCENE_MAX_ROW non-zero here, but it is a
  // constexpr 0 today so the compiler constant-folds this into a literal
  // division by zero and warns. Substituting 1 in the unreachable case is what
  // keeps the build warning-free without an `if constexpr` the toolchain's
  // C++ level may not accept.
  const uint16_t maxRow = SCENE_MAX_ROW ? SCENE_MAX_ROW : 1;
  const int16_t ty = tY + (int16_t)((int32_t)(tH - th) * S.sceneRow / maxRow);
  tft.fillRect(cx - 2, ty, 5, th, C_ACCENT);
}

void drawScenes() {
  const uint32_t now = millis();

  bool    anyErr  = false, unknown = false;
  uint8_t offline = 0;
  for (uint8_t i = 0; i < NUM_BULBS; i++) {
    bool devStale, devErr;
    staleErr(S.dev[i], now, devStale, devErr);
    if (devErr) anyErr = true;
    if (!S.dev[i].known)      unknown = true;
    else if (!S.dev[i].avail) offline |= (uint8_t)(1u << i);
  }

  // A scroll re-points every slot at a different scene, so not one of the cached
  // vis bytes describes what is now meant to be there. Treat it as a first draw.
  // A bulb going offline or coming back recolours a pip on EVERY tile without
  // necessarily moving any tile's vis byte, so it forces a full redraw too.
  const bool     first = !sceneSnap.valid || sceneSnap.row != S.sceneRow
                      || sceneSnap.offline != offline;
  const uint16_t base  = (uint16_t)(S.sceneRow * SCENE_COLS);

  for (uint8_t slot = 0; slot < SCENE_PER_PAGE; slot++) {
    // The top tile row shares band 0 with the connectivity banner, which owns it
    // while shown — including across a scroll, which would otherwise repaint
    // every tile straight over the banner.
    if (slot < SCENE_COLS && noConnShown()) continue;

    const uint16_t idx = base + slot;

    uint8_t vis = BV_INACTIVE;
    // An empty slot shares BV_DISABLED, which is safe because slot -> idx is
    // fixed for a given scroll offset: a slot that is empty stays empty until
    // `row` changes, and that forces a full redraw anyway.
    // Only a NEVER-POLLED bulb greys the grid (boot: nothing is known yet). An
    // unavailable one used to as well, which blanked every tile and hid the
    // active scene for as long as one bulb was off the network; its pip turns
    // red instead (drawSceneTile) and the rest of the grid carries on.
    if (idx >= SCENE_N || unknown)                     vis = BV_DISABLED;
    else if (pressedNow(HIT_SCENE, (int16_t)idx, -1))  vis = BV_PRESSED;
    // The error belongs to the page, not to one tile — nothing here remembers
    // which scene was tapped once the press flash has expired.
    else if (anyErr)                                   vis = BV_ERR;
    else if (sceneActive(idx))                         vis = BV_ACTIVE;

    if (!first && vis == sceneSnap.vis[slot]) continue;
    sceneSnap.vis[slot] = vis;
    drawSceneTile(slot, idx, vis, offline);
  }

  // The thumb's position depends only on `row`, so `first` already covers moving
  // it; sbVis exists for the press flash, which expires by time and would
  // otherwise leave an arrow lit forever — same rule as every other vis byte.
  const uint8_t sb = pressedNow(HIT_SCROLL, -1, -1) ? 1
                   : pressedNow(HIT_SCROLL, +1, -1) ? 2 : 0;
  if (first || sb != sceneSnap.sbVis) {
    sceneSnap.sbVis = sb;
    drawSceneScrollbar(sb);
  }

  sceneSnap.row     = S.sceneRow;
  sceneSnap.offline = offline;
  sceneSnap.valid   = true;

  // The AC card, in the row band below the grid — the SAME card the Devices
  // page draws, through the same function, so its chips, stepper, press flash,
  // optimistic repaint and error border are not reimplemented here and cannot
  // drift from the ones on Devices. btnRect() derives y from cardTop(dev) and
  // the AC is the same row slot on both pages, so the rects are identical
  // without drawDeviceCard() knowing which page it is on. Three decisions:
  //
  //   * `false`, not `first`. The card owns its own RowSnap and its own
  //     compare, so forcing it on every scroll would repaint a card whose
  //     state did not move — and would hide a real bug in that compare.
  //   * NOT skipped for noConnShown(). The banner owns row band 0; this is the
  //     last band, and the AC stays drawn with HA down for exactly the reason
  //     rows 1..3 stay drawn on Devices.
  //   * It SHARES snap[SCENE_AC_SLOT] with the Devices page, which is safe only
  //     because bodyReset() zeroes snap[] on every page switch: the card's
  //     first draw then refills its whole band. An optimisation that skipped
  //     that wipe would leave this band empty on arrival, with the cached
  //     fingerprint suppressing the draw that would have filled it.
  //
  // Drawn LAST so it lands over the gutter's column, which it overlaps in x
  // (the card runs to x 311) — though drawSceneScrollbar()'s clear stops at
  // SCENE_AC_Y0, so the two are disjoint and the order is belt and braces.
  drawDeviceCard(SCENE_AC_SLOT, false);
}
