#include "screen_int.h"
#include "logo_ha.h"
#include <math.h>
#include <string.h>

// Layout (320x240, rotation 1 — the rotation the touch calibration in config.h
// was measured against. Rotation 3 (the Flip screen setting) is the 180 case and
// is handled by mirroring the TAP, not by recalibrating; see handleTouch(). No
// other rotation is valid.)
//
//   y   0..51    row band 0   card at y   2..49
//   y  52..103   row band 1   card at y  54..101
//   y 104..155   row band 2   card at y 106..153
//   y 156..207   row band 3   card at y 158..205
//   y    208     divider
//   y 209..239   header   27 55% | Devices  Scenes  Settings | 23:45
//
// The header is anchored to the BOTTOM edge, not the top — moved there on
// request, not discovered there. Every constant that used to be measured from
// y=0 is now measured from STATUS_Y0 (208) instead, and the header's internal
// layout is mirrored top-to-bottom around its own centre: the divider sits at
// the TOP of the band (the seam with the body) rather than the bottom, and the
// tab underline seats on it from below rather than above. See config.h's header
// block for the full arithmetic.
//
// Scenes uses its own grid over the same body: 3 columns of 88x64 tiles on a
// 100 x 72 pitch, three rows visible (y 0..63, 72..135, 144..207), with the
// scroll gutter at x 300..319 over the same range.
//
// EVERY NUMBER ABOVE IS DERIVED FROM config.h's LAYOUT BLOCK. Change those
// #defines together; never the arithmetic here.
//
// ── the shape of a page ──────────────────────────────────
// One idea carries all three pages: a row band holds a CARD, and controls sit on
// the card. That is what replaced four hairline-separated rows of loose outlined
// buttons. Consequences worth knowing before editing:
//
//   * A card's first draw fills its whole BAND (full width, C_BG) and then the
//     card, so Devices and Settings self-clear and the bands tile the body
//     exactly. Scenes does not self-clear — see bodyReset(). That is still true
//     of the grid even though Scenes now carries a card of its own in the bottom
//     band: the card clears ITS band, and the gaps between tiles and the blank
//     band above the card are what nothing repaints.
//   * Every dirty rect inside a card starts at CARD_IN_X0, never at CARD_X.
//     Filling x 8..15 would paint over the card's own left border column and
//     erase the outline, one repaint at a time. (This used to be about the R_LG
//     corner arcs; the corners are square now, the rule is not.)
//   * An inactive control has no border (see ctlColour). The old page outlined
//     all 23 controls at once; removing those outlines, not changing any colour,
//     is what made this page quiet.

// ── header must TILE exactly ─────────────────────────────
// A gap leaves pixels nothing ever clears; an overlap is just as bad, because
// each region only clears its own rect, so whatever spills over is never
// repainted.
// The bar is three regions now — room reading (x 0..79), tab strip
// (x 80..250, centred), and clock (x 251..319). TAB_STRIP_W (config.h) is
// DERIVED from STATUS_ROOM_W and STATUS_CLK_W, so "tabs and the adjacent
// regions do not meet" cannot go wrong at compile time. What tabRect() below
// can still get wrong (labels too wide for TAB_STRIP_W, margin going negative)
// is a runtime concern; see its comment.
// The indicator must sit INSIDE the band a tab clears (STATUS_DIV_Y+1..SCR_H-1),
// or a tab that stops being current keeps its underline forever; one row higher
// and it would overwrite the rule, which is painted once and never restored.
static_assert(TAB_UL_Y > STATUS_DIV_Y,
              "tab underline overruns the header divider");
// wTab places the bar flush with the TOP of the rect it is given, and drawTab
// gives it the clear rect starting right after the divider. This ties that to
// the documented constant, so a change to STATUS_H or TAB_UL_Y cannot silently
// move the bar off the rule.
static_assert(TAB_UL_Y == STATUS_DIV_Y + 1,
              "the tab cell does not seat the underline at TAB_UL_Y");
static_assert(ROWS_Y0 + NUM_DEVICES * ROW_H == STATUS_Y0, "rows do not fill the body");

// A STACKED card's two lines must not collide with each other or run out of the
// card. That is the Settings brightness card ONLY now — every device card
// (bulbs and the AC) is inline and gets the bound below instead.
static_assert(CARD_L1_Y + CARD_L1_H <= CTL_DY, "card line 1 overlaps the controls");
static_assert(CTL_DY + CTL_H <= CARD_H, "control row overruns the card");
static_assert(BULB_CTL_DY + BULB_CTL_H <= CARD_H,
              "inline device control row overruns the card");
// The identity column has to leave the name a positive budget after the icon,
// or textTrunc() would be handed a negative width. Shared by every device card
// now, not just bulbs.
static_assert(BULB_NAME_W > 0, "device identity column has no room for a name");

// The connectivity overlay must sit wholly inside the row band it clears (see
// config.h): the band is what removes the content underneath, so a banner
// hanging out of it would be drawn over pixels nothing cleared and would leave
// fragments — the exact failure clearing a whole band exists to avoid. Both
// bounds matter, and the arithmetic spans two blocks of config.h.
static_assert(NOCONN_Y0 >= ROWS_Y0 && NOCONN_Y0 + NOCONN_H <= ROWS_Y0 + ROW_H,
              "no-connection banner hangs out of the row band it clears");
// The Scenes clear stops at the scroll gutter, so the banner has to as well.
static_assert(NOCONN_X0 >= 0 && NOCONN_X0 + NOCONN_W <= SCENE_SB_X0,
              "no-connection banner reaches the scene scroll gutter");

// Both device rows keep SIX logical slots and the same slot meanings, even though
// the two kinds now lay those slots out differently (4 chips + 2 swatches vs
// 3 chips + a stepper). That is deliberate: doAction()'s switch, the press-flash
// sub-index and RowSnap::btnVis all key off the slot number, so only btnRect()
// knows about the difference.
static_assert(AC_BTNS == BULB_BTNS, "the two row kinds share one slot count");

// Bulb row: the identity column, then 4 chips, then 2 swatch cells, filling the
// card's inner width EXACTLY. Exactness matters less here than on the body grid
// (the card fill is behind it either way) but an overrun would paint over the
// card's rounded edge.
static_assert(BULB_CTL_X0 + 4 * BULB_CHIP_PITCH - CHIP_GAP + 2 * SW_CELL_W
                  == CARD_IN_X1 + 1,
              "bulb row does not fill the card");
static_assert(SW_X0 == BULB_CTL_X0 + 4 * BULB_CHIP_PITCH - CHIP_GAP,
              "swatch cells do not follow the chips");
// AC control row: same BULB_CTL_X0 start as a bulb's chips now (the AC is
// inline too), then 3 mode chips, one more CHIP_GAP of air, and the stepper's
// three cells filling what's left of the card exactly. The extra gap (vs. the
// bulb row assert above, which has none between its last swatch and the card
// edge) is deliberate: without it DRY sits flush against the down chevron.
static_assert(ACS_X0 == BULB_CTL_X0 + 3 * ACM_PITCH,
              "AC stepper does not follow the mode chips with a CHIP_GAP");
static_assert(ACS_X0 + 2 * ACS_BTN_W + ACS_VAL_W == CARD_IN_X1 + 1,
              "AC control row does not fill the card");
// Settings brightness: 5 chips on the shared pitch.
static_assert(CARD_IN_X0 + (BRI_STEPS - 1) * CHIP_PITCH + CHIP_W
                  <= CARD_IN_X1 + 1,
              "brightness chips run past the card");
// Settings night mode: 3 chips on the shared pitch.
static_assert(CARD_IN_X0 + (NIGHT_CHIPS - 1) * CHIP_PITCH + CHIP_W
                  <= CARD_IN_X1 + 1,
              "night-mode chips run past the card");
// Settings volume: 4 chips on the shared pitch.
static_assert(CARD_IN_X0 + (VOL_STEPS - 1) * CHIP_PITCH + CHIP_W
                  <= CARD_IN_X1 + 1,
              "volume chips run past the card");
// Settings toggle row: the two half cards and their gap must tile the full
// card's width exactly, or the right card's border lands off the column every
// other card on the page ends on.
static_assert(2 * TGL_CARD_W + TGL_CARD_GAP == CARD_W,
              "toggle cards do not tile the card width");
static_assert(CARD_X + TGL_CARD_PITCH + TOGGLE_DX + TOGGLE_W == CARD_IN_X1,
              "right-hand toggle does not end where a full-width one did");
// icoSpeaker draws one wave per step above mute, and the 15px grid has room
// for three.
static_assert(VOL_STEPS == 4, "icoSpeaker draws VOL_STEPS - 1 waves, max 3");

// Same rule for the Scenes grid, asserted twice for two distinct failures, and
// BOTH are bounds now rather than one bound and one equality. The y one used to
// demand the grid fill the body exactly; the AC card copied onto this page owns
// the bottom row band, so what it demands instead is that no tile reaches into
// that band. The 20px of background the two-row grid leaves above the card is
// deliberate and safe — it has no content to go stale, and the one region that
// does reach into it (the scroll gutter's column) owns and clears its own rect
// there (see the Scenes block in config.h). The x one is unchanged: the grid and the
// gutter each clear only their own rect, so an overlap is a permanently wrong
// pixel.
static_assert(ROWS_Y0 + (SCENE_VIS_ROWS - 1) * SCENE_PITCH_Y + SCENE_TILE_H
                  <= SCENE_AC_Y0,
              "scene grid runs into the AC card's row band");
static_assert(SCENE_X0 + (SCENE_COLS - 1) * SCENE_PITCH_X + SCENE_TILE_W
                  <= SCENE_SB_X0,
              "scene grid overlaps the scroll gutter");
// The card the Scenes page borrows is the AC's, at the AC's own row slot — that
// sameness IS the feature, so pin the slot to the device list and the band to
// the header. Between them these say "the bottom row band of the body, on both
// pages", which is what SCENE_AC_Y0 is meant to mean.
static_assert(SCENE_AC_Y0 + ROW_H == STATUS_Y0,
              "the AC card's band on Scenes does not end at the header");
static_assert(SCENE_AC_SLOT < NUM_DEVICES,
              "the Scenes page's AC slot is not a device row");
// The gutter's tap split has to land inside the gutter's OWN range, which is no
// longer the body's. Halving the old ROWS_Y0..STATUS_Y0 range would leave it at
// 104 and give the down arrow 52px against the up arrow's 104.
static_assert(SCENE_SB_MID > ROWS_Y0 && SCENE_SB_MID < SCENE_AC_Y0,
              "scene scroll split is outside the gutter's own range");
// Which device sits at SCENE_AC_SLOT is RUNTIME wiring (main.cpp assigns
// DeviceState::kind), so no assert can check it is the climate one. The closest
// compile-time statement of the same fact is that the bulbs are a prefix with
// exactly one device after them — which is what makes NUM_BULBS the AC's index,
// the same basis drawStatusRoom() already reads S.dev[NUM_BULBS] on.
static_assert(SCENE_AC_SLOT == NUM_DEVICES - 1,
              "the card copied onto Scenes is not the AC's row");

// ── labels ───────────────────────────────────────────────
// Indexed by PageId. Sentence case, and they fit with room to spare: the widest
// ("Settings") is 49px in Font 2 of the 73px a cell leaves after TAB_LBL_DX,
// measured rather than guessed. That width is also what the underline spans.
// The per-page label tables (BULB_LABEL, AC_LABEL, BRI_LABEL, SET_LABEL, ...)
// moved out to the .cpp that owns the only page drawing them.
static const char* const TAB_LABEL[TAB_COUNT] = {"Devices", "Scenes", "Settings"};

// ── dirty-region snapshots ───────────────────────────────
// The scene table (SCENE[], SceneBulb/Scene, SCENE_N/SCENE_MAX_ROW) moved to
// screen_scenes.cpp, the only file that reads it — see sceneCount()/
// sceneMaxRow() (screen.h) for how this file learns the two numbers it still
// needs for hit-testing without a second copy of "how many scenes".
// CardFingerprint/RowSnap/snap[] (devices), SceneSnap/sceneSnap (scenes) and
// SettingSnap/setSnap (settings) all moved to screen_int.h (the types, so
// this file's screenInvalidate()/bodyReset() can still sizeof() and memset()
// them) and to the .cpp that owns each instance — see that header's "shared
// dirty-region snapshots" section for the full contract.
//
// EVERY PAGE SNAPSHOT MUST KEEP A PER-ITEM BtnVis BYTE, and that is load-bearing
// rather than an optimisation: the press flash expires by TIME, not by any state
// change, so only a per-item vis compare notices BV_PRESSED -> BV_ACTIVE and
// repaints. A page that skips it leaves the tapped control inverted forever.
//
// Each snapshot also needs its own `valid` flag on top, because BV_INACTIVE is 0
// and a memset alone reads as "already drawn as inactive".

// Split into independently-dirty regions (room reading | tabs | clock) so a
// change in one never repaints the others. Repainting the full bar for a
// one-character change is what made it visibly flash.
//
// tabVis rather than a bare `page`: it encodes the active page AND handles
// press-flash expiry, which a page field could not.
// Raw-input fingerprint for drawStatusRoom()'s pre-filter — same idea and same
// caution as CardFingerprint above (float compared by bit pattern: d.room can
// be NaN). Lets the function skip its two snprintf()s and two strcmp()s
// entirely when nothing that feeds them has moved, rather than doing that
// work every pass and only avoiding the *draw*.
struct RoomFingerprint {
  bool  known, avail, stale, err;
  int   humidity;
  float room;
};

struct StatusSnap {
  bool     valid;
  int16_t  hhmm;    // local time as hour*60+min; -1 while NTP is unsynced
  uint8_t  tabVis[TAB_COUNT];
  RoomFingerprint roomFp;  // see above; compared before any formatting
  char     roomStr[8];   // last rendered room temperature (relocated from the
                         // AC card's own RowSnap — see drawStatusRoom())
  char     humStr[8];    // last rendered humidity
  uint16_t roomFg;       // resolved colour, so a stale/err transition with no
                         // string change still repaints (mirrors icoColour).
};
static StatusSnap statusSnap;

// The connectivity overlay's own one-bit snapshot. It is not part of StatusSnap
// because it is not part of the header: it lives in the body, over whatever the
// current page drew, and is the only region whose HIDE path cannot repaint
// itself — see drawNoConn().
struct NoConnSnap {
  bool valid;
  bool shown;
};
static NoConnSnap noConnSnap;

// What is physically on the glass, as opposed to S.page (what should be).
// 0xFF means "nothing valid" and forces a body wipe on the next render.
static uint8_t shownPage = 0xFF;

// IcoShape (devices only) and rowTop/cardTop/sceneTileX/sceneTileY (shared
// geometry, now in screen_int.h) moved out with the rest of this section.
// btnRect/settingChipRect (per-page control geometry — see btnRect's own
// comment in screen_devices.cpp for the slot-layout rationale),
// pressedNow/staleErr/alarmFg/noConnShown/pctMatches/kelvinMatches (shared
// time-derived/matching predicates) and Rect itself all moved to
// screen_int.h or the .cpp that owns them. btnActive/sceneActive (active-
// state derivation) and bulbHue/iconVis/drawIcon (derived colour) moved with
// the page that's their only caller: screen_devices.cpp and
// screen_scenes.cpp respectively.

// ── room reading (AC only — drawn in the header now, not the card) ────
//
// These two functions used to feed the AC card's identity column (two short
// lines under the name). That moved to the header on request — see
// STATUS_ROOM_W (config.h) and drawStatusRoom() below — so it reads on every
// page, not just Devices. The text-generation stayed here rather than moving
// with its caller: it is pure DeviceState -> string logic with no drawing
// coordinates in it, so both drawDeviceCard() (historically) and
// drawStatusRoom() (now) can share it unchanged.
//
// Two things a card-based version of this used to also do are gone rather
// than left unreachable, for the same reason the bulb's old "30%  2700K"
// branch was deleted outright: an unused code path that still looks
// authoritative is how a future edit reintroduces a string with nowhere to
// go.
//   - The "OFFLINE" word: the card border, the icon and the name colour
//     already carry that alarm — exactly the signal set a bulb card relies on
//     for the same failure.
//   - Naming an exceptional mode (heat/fan_only/auto, set from the HA app): the
//     chips don't offer those modes and the icon falls back to a plain POWER
//     glyph for them (see iconVis()) — a real loss of specificity, accepted
//     because there is nowhere on the card left to put the word.

// `degree` comes back true when the caller should draw a degree ring after the
// digits — the fonts have no U+00B0 and a trailing "C" reads as another digit.
static void roomTempText(const DeviceState& d, char* out, size_t n, bool& degree) {
  degree = false;
  if (!d.known || !d.avail || isnan(d.room)) { snprintf(out, n, "--"); return; }
  degree = true;
  snprintf(out, n, "%.0f", d.room);
}

// Humidity DIGITS ONLY, without the "%" — the sign is drawn separately, and a
// size smaller, by drawStatusRoom(); see STATUS_ROOM_W in config.h for why it
// could not come along when the digits went up to F_NUM. It is still the whole
// string statusSnap.humStr compares on, so the dirty-compare is unaffected by
// the split.
//
// Unconditionally blank rather than "--" when there is nothing to show: unlike
// the room temperature (always drawn, even as "--", so its position in the header
// never jumps), a missing humidity reading simply means two fewer pieces drawn.
static void humidityText(const DeviceState& d, char* out, size_t n) {
  out[0] = '\0';
  if (!d.known || !d.avail || d.humidity < 0) return;
  snprintf(out, n, "%d", d.humidity);
}

// tempText() and drawDeviceCard() (the Devices page's 4 cards, 23 controls)
// moved to screen_devices.cpp, the only file that needs them.

// ── header ───────────────────────────────────────────────

// Each tab's cell hugs its OWN label (measured, not a shared constant — see
// the TAB_STRIP_W comment in config.h). Computed once and cached: TAB_LABEL
// never changes at runtime, so there is nothing to invalidate.
//
// THE STRIP IS CENTRED in TAB_STRIP_W, which starts at TAB_X0 (80, right after
// the room reading), with the leftover split into two outer margins. It was
// flush left at x 0 before the room reading and the tabs swapped places: centred
// against the screen edge, the left margin was blank corner with nothing beyond
// it and the tabs read as adrift. Between two blocks of digits both margins are
// gaps to a neighbour, so centring is what balances them.
//
// A future-proofing note rather than a live bug: if TAB_LABEL ever grew wide
// enough that 3 cells + 2*TAB_GAP exceeded TAB_STRIP_W, the margin would go
// negative and the cells would run into BOTH neighbours. The 3 shipped labels
// leave 5px to spare, so this is deliberately not runtime-guarded —
// simulator.html's tabRect() warns on the same arithmetic.
static void tabRect(uint8_t i, int16_t& x, int16_t& w) {
  static int16_t cellX[TAB_COUNT];
  static int16_t cellW[TAB_COUNT];
  static bool ready = false;
  if (!ready) {
    int16_t total = 0;
    for (uint8_t k = 0; k < TAB_COUNT; k++) {
      cellW[k] = textW(F_MICRO, TAB_LABEL[k]) + 2 * TAB_LBL_DX;
      total += cellW[k];
    }
    total += (TAB_COUNT - 1) * TAB_GAP;
    int16_t cx = TAB_X0 + (TAB_STRIP_W - total) / 2;
    for (uint8_t k = 0; k < TAB_COUNT; k++) {
      cellX[k] = cx;
      cx += cellW[k] + TAB_GAP;
    }
    ready = true;
  }
  x = cellX[i];
  w = cellW[i];
}

// One tab. Clears its own cell, so the strip needs no separate clear — and the
// cell IS the rect handed to wTab, which is what seats the underline on the
// header rule: the clear starts at STATUS_DIV_Y+1, so the bar lands on the
// first row below the rule rather than on top of it.
// The one rect shape every header region clears: full region height below the
// rule, callee's own x/w. All three regions (tabs, room reading, clock) drew
// this identically inline.
static inline void clearHeaderRegion(int16_t x, int16_t w) {
  tft.fillRect(x, STATUS_DIV_Y + 1, w, STATUS_H - 1, C_BG);
}

static void drawTab(uint8_t i, const char* label, uint8_t vis) {
  int16_t x, w;
  tabRect(i, x, w);
  clearHeaderRegion(x, w);
  wTab(x, STATUS_DIV_Y + 1, w, STATUS_H - 1, label, vis);
}

// The AC's room reading, at the LEFT edge of the header (x 0..79). Relocated
// here from its own card (see the "room reading" comment above roomTempText())
// so it reads on every page rather than only Devices, then swapped with the tab
// strip on request. `S.dev[NUM_BULBS]` is the AC by construction (config.h:
// "Devices 0..2 are the bulbs, device 3 is the AC") — same assumption main.cpp
// already makes when it wires up ENT_AC at that index, so this is not a new
// coupling, just a second place that relies on it.
//
// Drawn as <temp><ring> <humidity>%, left-aligned STATUS_EDGE_DX in from the
// screen edge — the mirror of the clock's right margin.
//
// THE NUMBERS ARE F_NUM, THE "%" IS NOT, and that split is the whole reason this
// fits — see STATUS_ROOM_W in config.h. F_NUM's "%" is 21px against F_TITLE's 9,
// which this region does not have and never would.
//
// IT NEVER DRAWS PAST ITS OWN REGION. Its right-hand neighbour is the tab strip
// now, which repaints only when a tab's vis changes — so unlike the clock it
// used to spill into, nothing would ever clean up after it. A reading too wide
// for the budget ("-10", "100") drops its digits to F_TITLE instead, the same
// try-the-role-then-step-down idea as textFit(). The choice is derived from the
// strings, so the existing string compare already covers it.
//
// Humidity is skipped entirely when there is nothing to show (humidityText()
// returns ""), digits and sign together.
static void drawStatusRoom(bool force) {
  DeviceState& d   = S.dev[NUM_BULBS];
  const uint32_t now = millis();
  bool stale, err;
  staleErr(d, now, stale, err);
  const uint16_t fg = alarmFg(err || !d.avail, stale, C_TEXT2);

  // Raw-input pre-filter (see RoomFingerprint above): skip the snprintf()s
  // and strcmp()s below entirely when nothing that feeds them has moved,
  // rather than doing that work every pass and only avoiding the draw.
  RoomFingerprint rfp;
  memset(&rfp, 0, sizeof(rfp));
  rfp.known = d.known; rfp.avail = d.avail; rfp.stale = stale; rfp.err = err;
  rfp.humidity = d.humidity; rfp.room = d.room;
  if (!force && memcmp(&rfp, &statusSnap.roomFp, sizeof(rfp)) == 0) return;
  statusSnap.roomFp = rfp;

  char room[8]; bool degree;
  roomTempText(d, room, sizeof(room), degree);
  char hum[8];
  humidityText(d, hum, sizeof(hum));

  if (!force && strcmp(room, statusSnap.roomStr) == 0 &&
      strcmp(hum, statusSnap.humStr) == 0 && fg == statusSnap.roomFg)
    return;

  clearHeaderRegion(0, STATUS_ROOM_W);

  // Width of the whole reading with its digits in `role`: the same sequence of
  // advances the draw below steps through.
  auto readingW = [&](FontRole role) -> int16_t {
    int16_t w = textW(role, room) + (degree ? 5 : 0) + SP_1;
    if (hum[0]) w += textW(role, hum) + textW(F_TITLE, "%");
    return w;
  };
  const FontRole role =
      readingW(F_NUM) <= STATUS_ROOM_W - STATUS_EDGE_DX ? F_NUM : F_TITLE;

  // The small "%" sits on the BIG digits' baseline, not on their centre line:
  // F_NUM's baseline is cy+10 and F_TITLE's is cy+5, so shifting its datum down
  // by the difference lands the two on the same row. Centred instead, it would
  // float in the middle of the tall digits and read as a smaller number beside
  // them rather than as their unit. (In the F_TITLE fallback the difference is
  // 0, and both sit on the same centre line, as they should.)
  const int16_t pctCy = STATUS_CY + (fontBaseline(role) - fontBaseline(F_TITLE));

  int16_t x = STATUS_EDGE_DX;
  textAt(role, room, x, STATUS_CY, ML_DATUM, fg);
  x += textW(role, room);
  if (degree) {
    // Same idiom as the AC setpoint: ring rides the digit tops, fontInkTop()
    // locates them, a small fixed gap clears them. Ring right edge is x+5 (3px
    // gap + 2px radius) and STAYS a 2px radius at F_NUM — wValue() draws exactly
    // this ring beside the setpoint's F_NUM digits, so growing it here would make
    // the two disagree. Only fontInkTop()'s role argument tracks the digits.
    const int16_t rcx = x + 3;
    const int16_t rcy = STATUS_CY + fontInkTop(role) + 2;
    tft.drawCircle(rcx, rcy, 2, fg);
    x += 5;
  }
  x += SP_1;
  if (hum[0]) {
    textAt(role, hum, x, STATUS_CY, ML_DATUM, fg);
    x += textW(role, hum);
    textAt(F_TITLE, "%", x, pctCy, ML_DATUM, fg);
  }

  snprintf(statusSnap.roomStr, sizeof(statusSnap.roomStr), "%s", room);
  snprintf(statusSnap.humStr,  sizeof(statusSnap.humStr),  "%s", hum);
  statusSnap.roomFg = fg;
}

// Written by screenSetClock() (public API — see screen.h), read by
// drawStatus() below instead of it calling getLocalTime() itself:
// screenRender() (and so drawStatus()) can run up to 4 times in one loop()
// pass — doAction()/doScene()/doSetting()'s immediate optimistic repaint,
// plus loop()'s own — and the wall clock cannot have changed within a pass.
static int16_t cachedHhmm = -1;

static void drawStatus(bool force) {
  // Wall clock, 24h, -1 = NTP still unsynced (drives the "--:--" placeholder).
  // Read from screenSetClock()'s cache rather than calling getLocalTime()
  // itself: this runs on every render pass — up to 4 times in one loop() pass
  // — for a value that cannot have changed within a pass. See screen.h.
  const int16_t hhmm = cachedHhmm;

  const bool force_ = force || !statusSnap.valid;

  // The three regions below tile the bar exactly (see the top of this file), so
  // between them they cover every pixel and no full-width clear is needed to
  // catch a gap.
  uint8_t tabVis[TAB_COUNT];
  for (uint8_t i = 0; i < TAB_COUNT; i++)
    tabVis[i] = pressedNow(HIT_TAB, (int16_t)i, -1) ? BV_PRESSED
              : (i == (uint8_t)S.page ? BV_ACTIVE : BV_INACTIVE);

  for (uint8_t i = 0; i < TAB_COUNT; i++) {
    if (!force_ && tabVis[i] == statusSnap.tabVis[i]) continue;
    drawTab(i, TAB_LABEL[i], tabVis[i]);
  }

  // The AC's room reading. Not minute-rate like the clock below — it changes
  // whenever a poll sees a new temperature/humidity, same cadence as the
  // Devices card it used to live on — but it is still its OWN region with its
  // own compare, so a temperature change never repaints the clock and a new
  // minute never repaints the room reading.
  drawStatusRoom(force_);

  // Repaints once a minute. Nothing else lives in this region, so a minute-rate
  // repaint is invisible — unlike the per-second freshness counter that used to
  // live here and made the whole bar flicker. KEEP THIS REGION MINUTE-RATE:
  // connection health belongs to drawNoConn()'s overlay and staleness to the
  // card dimming, not here.
  //
  // The clock is always exactly 5 characters, and it is F_NUM now — up a size
  // on request, spending the space the deleted connectivity glyph and a TAB_GAP
  // cut freed (see STATUS_CLK_W in config.h). "23:45" is 63px against F_TITLE's
  // 35, and the region grew 41 -> 69 to hold it. Anything wider paints into the
  // tab strip, which only repaints on ITS OWN compare and would leave the
  // overflow permanent — don't put anything else here.
  if (force_ || hhmm != statusSnap.hhmm) {
    clearHeaderRegion(SCR_W - STATUS_CLK_W, STATUS_CLK_W);
    char clk[8];
    if (hhmm < 0) snprintf(clk, sizeof(clk), "--:--");
    else          snprintf(clk, sizeof(clk), "%02d:%02d", hhmm / 60, hhmm % 60);
    textAt(F_NUM, clk, SCR_W - STATUS_EDGE_DX, STATUS_CY, MR_DATUM,
           hhmm < 0 ? C_DIM : C_TEXT);
  }

  // Sits at STATUS_DIV_Y, outside every fillRect above, so it survives their
  // clears and only needs painting once. C_DIVIDER rather than C_BORDER: this is
  // a rule between chrome and content, and it should be felt, not read.
  if (force_) tft.drawFastHLine(0, STATUS_DIV_Y, SCR_W, C_DIVIDER);

  // Field-by-field, NOT an aggregate init. `statusSnap = { ... }` compiles
  // silently when a field is added (there is no -Wmissing-field-initializers
  // here) and value-initialises it to zero, which for tabVis means "every tab
  // inactive" — computed always has one active, so the strip would repaint on
  // every single render pass. That is a flicker bug with no compiler warning.
  // roomStr/humStr/roomFg are NOT reset here — drawStatusRoom() owns and
  // updates those three itself, the same way RowSnap fields used to be owned
  // by drawDeviceCard() alone.
  statusSnap.valid  = true;
  statusSnap.hhmm   = hhmm;
  memcpy(statusSnap.tabVis, tabVis, sizeof(tabVis));
}

// ── connectivity overlay (body, not header) ──────────────

// What replaced the header's connectivity glyph, on request. Three things about
// it are decisions rather than mechanics:
//
// IT IS GATED ON !S.haOk ALONE, so it says "No Connection" and not which end
// failed. The glyph distinguished Wi-Fi-down (0 bars, red) from HA-down (3 bars
// + amber badge); that distinction is deliberately dropped, not overlooked.
// updateNetState() already forces S.haOk false whenever Wi-Fi drops (main.cpp),
// so this one bit covers both failure modes correctly, and it is the bit that
// actually matters to someone standing in front of the panel: nothing they tap
// is going to work. The finer diagnosis survives in the serial log — the same
// trade this firmware already made when the literal "WIFI"/"HA" labels went.
//
// IT IS wChip() + BV_ERR, not new drawing code. ctlColour(BV_ERR) resolves to
// C_BG fill / C_ERROR border / C_ERROR text, which is this codebase's standing
// idiom for a fault: the alarm is the border and the text, never a loud fill
// (that is BV_PRESSED's, and it stays the only full-brightness fill the UI
// draws). Note this is the FIRST wChip() call that actually reaches that table
// entry — BV_ERR has until now only been consumed by drawSceneTile()'s own
// local switch, which picks different values for a 88x64 tile — so what is
// being reused here is the colour-table idiom, not an already-rendered
// appearance.
//
// THE HIDE PATH CANNOT REPAINT ITSELF, and that is why it calls
// screenInvalidate(). Every other region owns its rect and clears it; this one
// sits ON TOP of a card or a tile that has already been drawn and cached as
// clean, so clearing to C_BG would leave a banner-shaped hole in whatever is
// underneath. fillScreen() would fix that and flash the whole panel for one
// frame to remove a 114x24 banner. screenInvalidate() writes no pixels at all:
// it just drops every snapshot, so the NEXT screenRender() (~20ms, next loop
// pass) runs bodyReset() and redraws the body from scratch with real content.
// That is the same one-frame lag night mode and screen flip already accept.
static void drawNoConn(bool force) {
  const bool show  = !S.haOk;
  const bool first = force || !noConnSnap.valid;

  if (!first && show == noConnSnap.shown) return;

  if (show) {
    // Clear the band first, then centre the banner in it — see config.h for why
    // clearing is what makes this read as an overlay rather than as a clipping
    // fault. The rect is the CURRENT PAGE's first row, because the two grids do
    // not share a pitch: a 52px row band leaves 12px of a 64px scene tile
    // showing under the banner. And on Scenes it must stop at the scroll gutter,
    // which is its own region drawn by drawSceneScrollbar() on its own compare —
    // clearing across it erased the up arrow with nothing to put it back.
    const bool    scenes = (S.page == PAGE_SCENES);
    const int16_t w      = scenes ? SCENE_SB_X0   : SCR_W;
    const int16_t h      = scenes ? SCENE_TILE_H  : ROW_H;
    tft.fillRect(0, rowTop(0), w, h, C_BG);
    wChip(NOCONN_X0, NOCONN_Y0, NOCONN_W, NOCONN_H, "No Connection", nullptr,
          BV_ERR);
  } else if (!first) {
    // A genuine falling edge. Never on a first draw — there is nothing on the
    // glass to erase then, and invalidating would loop. screenInvalidate()
    // re-zeroes noConnSnap itself, so nothing below may run.
    screenInvalidate();
    return;
  }

  noConnSnap.valid = true;
  noConnSnap.shown = show;
}

// The Scenes page (drawSceneTile/drawSceneScrollbar/drawScenes) and the
// Settings page (tglVal/setIcon/drawSettingChrome/drawSettings) moved
// to screen_scenes.cpp and screen_settings.cpp respectively.
// ── public API ───────────────────────────────────────────

// Tracks what rotation the panel is actually in. Owned here, alongside the only
// two calls to tft.setRotation(), so the boot value and the settings value can
// never disagree.
static int8_t flipCur = -1;

void screenBegin(bool flip) {
  tft.init();
  // TFT_eSPI::init() ends with digitalWrite(TFT_BL, HIGH) — 100% backlight —
  // and setup() does not attach LEDC for another few ms. With a persisted 1%
  // or night-mode setting that is a full-brightness flash on every boot in a
  // dark bedroom. Hold the panel dark until PWM comes up at the real duty.
  digitalWrite(PIN_BACKLIGHT, LOW);

  // rotation 1 == the orientation config.h's TOUCH_* calibration assumes;
  // rotation 3 is the same landscape frame turned 180, which handleTouch()
  // compensates for by mirroring both axes.
  flipCur = flip ? 1 : 0;
  tft.setRotation(flip ? 3 : 1);
  tft.fillScreen(C_BG);
  screenInvalidate();
}

void screenSetFlip(bool flip) {
  if (flipCur == (int8_t)(flip ? 1 : 0)) return;
  flipCur = flip ? 1 : 0;
  tft.setRotation(flip ? 3 : 1);
  // setRotation only changes how the panel is addressed; the pixels already in
  // it stay put, now upside down.
  tft.fillScreen(C_BG);
  screenInvalidate();
}

void screenSetNightMode(uint8_t mode) {
  if (!themeSetNightMode(mode)) return;
  // Every dirty-region compare is on a VALUE (state string, BtnVis, toggle
  // bool) and a palette swap changes none of them, so without an explicit
  // invalidate the screen would keep day colours until something else moved.
  tft.fillScreen(C_BG);
  screenInvalidate();
}

// No invalidate needed here: drawStatus()'s own compare against
// statusSnap.hhmm already repaints the clock exactly when this changes, the
// same as it always did with a freshly-read value. See cachedHhmm's own
// comment (above drawStatus()) for why this exists.
void screenSetClock(int16_t hhmm) { cachedHhmm = hhmm; }

void screenInvalidate() {
  memset(snap,        0, sizeof(snap));
  memset(&sceneSnap,  0, sizeof(sceneSnap));
  memset(&setSnap,    0, sizeof(setSnap));
  memset(&statusSnap, 0, sizeof(statusSnap));
  memset(&noConnSnap, 0, sizeof(noConnSnap));
  shownPage = 0xFF;   // -> bodyReset() on the next render
}

// Wipes everything above the header and invalidates only the BODY snapshots.
// Deliberately leaves statusSnap alone — a page switch changes neither the tabs'
// meaning nor the clock, and repainting them would reintroduce the flicker the
// split regions exist to prevent. noConnSnap IS reset, because unlike the
// header the overlay lives in the rect this function just filled: its pixels
// are gone, so the cached "already shown" byte would be a lie.
//
// The wipe is mandatory, not defensive. Devices and Settings happen to
// self-clear (each card's first draw fills its whole row band, and the four
// bands tile the body), but Scenes does not: 88x64 tiles on a 100 x 72 pitch
// leave 12px and 8px gaps that would hold the previous page's pixels
// permanently, and the 20px band between the last tile row and the AC card is
// painted HERE and nowhere else — no per-frame path writes it, which is exactly
// why it is safe to leave blank. A SCROLL within Scenes is different and needs
// no wipe — see the square-fillRect note in drawSceneTile().
//
// The memset of snap[] is also what lets ONE RowSnap serve the AC card on two
// pages: arriving on either page drops the cached bytes, so the card always does
// a first draw (band clear + outline) rather than trusting a snapshot taken
// while the other page was on the glass.
static void bodyReset() {
  tft.fillRect(0, 0, SCR_W, STATUS_Y0, C_BG);
  memset(snap,        0, sizeof(snap));
  memset(&sceneSnap,  0, sizeof(sceneSnap));
  memset(&setSnap,    0, sizeof(setSnap));
  memset(&noConnSnap, 0, sizeof(noConnSnap));
}

void screenRender() {
  bool bodyWasReset = false;
  if (shownPage != (uint8_t)S.page) {
    shownPage = (uint8_t)S.page;
    bodyReset();
    bodyWasReset = true;
  }

  drawStatus(false);
  switch (S.page) {
    // drawScenes() draws the AC card in the bottom row band too — it is part of
    // that page now, so it stays behind that page's one entry point rather than
    // becoming a second call here. See screen_int.h.
    case PAGE_SCENES:   drawScenes();   break;
    case PAGE_SETTINGS: drawSettings(); break;
    default:
      for (uint8_t i = 0; i < NUM_DEVICES; i++) {
        if (i == 0 && noConnShown()) continue;   // band 0 belongs to the banner
        drawDeviceCard(i, false);
      }
      break;
  }

  // LAST, and over the page: it is an overlay, so whatever the page just drew
  // has to already be on the glass. bodyReset() having run counts as a force —
  // it wiped the overlay's pixels along with everything else.
  drawNoConn(bodyWasReset);
}

// Decodes the RLE logo a row at a time. A full 96x96 RGB565 buffer would be
// 18 KB, far past the task stack, so this keeps one 192-byte row and blits it.
// Runs may cross row boundaries, hence the run counter living outside the loop.
static void drawLogo(int16_t ox, int16_t oy) {
  uint16_t row[LOGO_HA_W];
  size_t  ri  = 0;
  uint8_t idx = 0, run = 0;

  // The logo's palette is baked RGB565, not C_* names, so it would render in
  // full colour over a red-only UI. Map the 9 entries once rather than per
  // pixel. Its background entry is exactly C_BG's day value (0x0041) and both
  // map to the same night value, so it stays an opaque rectangle in either
  // palette — which is why TH_BG is pinned. See theme.h.
  uint16_t pal[sizeof(LOGO_HA_PAL) / sizeof(LOGO_HA_PAL[0])];
  for (size_t i = 0; i < sizeof(pal) / sizeof(pal[0]); i++)
    pal[i] = themeMap(LOGO_HA_PAL[i]);

  for (int16_t y = 0; y < LOGO_HA_H; y++) {
    for (int16_t x = 0; x < LOGO_HA_W; x++) {
      if (run == 0 && ri + 1 < LOGO_HA_RLE_LEN) {
        idx = LOGO_HA_RLE[ri++];
        run = LOGO_HA_RLE[ri++];
      }
      row[x] = pal[idx];
      if (run) run--;
    }
    tft.pushImage(ox, oy + y, LOGO_HA_W, 1, row);
  }
}

// Where the splash's progress pips sit: below the centred logo, on the spacing
// scale, in the band the device rows would otherwise occupy.
#define SPLASH_PIP_CY  (SCR_H / 2 + LOGO_HA_H / 2 + SP_4)
#define SPLASH_PIP_GAP SP_4

void screenSplash() {
  tft.fillScreen(C_BG);
  // Logo only, exactly centred. Both boot states (connecting and Wi-Fi portal)
  // render identically — see the note in screen.h about what that costs.
  drawLogo((SCR_W - LOGO_HA_W) / 2, (SCR_H - LOGO_HA_H) / 2);
  screenSplashProgress(0);
  screenInvalidate();
}

void screenSplashProgress(uint8_t phase) {
  // Three pips with one lit, cycling. The splash used to be a frozen logo for as
  // long as WIFI_CONNECT_MS, which is indistinguishable from a hung board — this
  // is the smallest thing that says "still working" and it costs three
  // fillCircles. Deliberately still wordless, keeping the two boot states
  // visually identical (screen.h documents that trade).
  for (uint8_t i = 0; i < 3; i++)
    tft.fillCircle(SCR_W / 2 + ((int16_t)i - 1) * SPLASH_PIP_GAP,
                   SPLASH_PIP_CY, 3,
                   i == (phase % 3) ? C_ACCENT : C_DIVIDER);
}

void screenCalibTarget(int idx, int total, int16_t x, int16_t y) {
  tft.fillScreen(C_BG);

  char msg[40];
  snprintf(msg, sizeof(msg), "TOUCH CALIBRATION  %d / %d", idx + 1, total);
  textAt(F_TITLE, msg, SCR_W / 2, SCR_H / 2 - 14, MC_DATUM, C_TEXT2);
  textAt(F_BODY, "tap the centre of the crosshair", SCR_W / 2, SCR_H / 2 + 8,
         MC_DATUM, C_TEXT3);

  // Crosshair drawn last so it sits over the text if they overlap.
  tft.drawFastHLine(x - 14, y, 29, C_ACCENT);
  tft.drawFastVLine(x, y - 14, 29, C_ACCENT);
  tft.drawCircle(x, y, 9, C_ACCENT);
  tft.fillCircle(x, y, 2, C_TEXT);
}

void screenCalibVerifyScreen() {
  tft.fillScreen(C_BG);
  // Draw the real control rects as outlines, so a tap can be judged against the
  // actual targets rather than an abstract coordinate. It goes through btnRect(),
  // which means it automatically shows the AC row's different layout — including
  // its setpoint cell, which is not a target but whose boundaries decide which
  // chevron a near-miss reaches.
  for (uint8_t d = 0; d < NUM_DEVICES; d++) {
    for (uint8_t b = 0; b < btnCount(S.dev[d]); b++) {
      const Rect r = btnRect(d, b);
      tft.drawRect(r.x, r.y, r.w, r.h, C_BORDER);
    }
  }
  // The tab strip too. It is the thinnest target in the firmware and sits in
  // the bottom bezel band the 4-point fit EXTRAPOLATES rather than interpolates
  // (CAL_INSET is 30, from every edge), so it is the most likely place for the
  // calibration to be off — and the one place a verify pass that skipped it
  // would never reveal.
  // The whole cell, which is the real target — the pill outline this used to draw
  // was smaller than what screenHitTest() actually accepts, so a tap landing in
  // the cell but outside the pill looked like a miss the firmware would have
  // taken. Now the outline and the hit rect are the same rect.
  for (uint8_t t = 0; t < TAB_COUNT; t++) {
    int16_t x, w;
    tabRect(t, x, w);
    tft.drawRect(x, STATUS_Y0, w, TAB_TAP_H, C_BORDER);
  }

  textAt(F_BODY, "VERIFY: tap boxes, dot should land inside", CARD_IN_X0,
         ROWS_Y0 + 10, ML_DATUM, C_TEXT3);
}

void screenCalibDot(int16_t x, int16_t y) {
  tft.fillCircle(x, y, 3, C_ACCENT);
}

// One device row's button sweep, shared by the Devices page and by the AC card
// the Scenes page now carries. Factored out so the two call sites cannot drift
// about the two things that make a row's targets correct: that btnRect() is the
// only source of a control's x extent, and that the AC's setpoint cell is not a
// control at all.
//
// The caller has already decided WHICH row, so only px is tested here —
// vertically the whole 52px row band counts as the control strip (a bedroom
// device, often used in the dark, on a resistive panel), which is why the drawn
// 40px strip is never consulted.
static Hit hitDeviceRow(uint8_t i, int16_t px) {
  const bool ac = (S.dev[i].kind == DEV_CLIMATE);
  for (uint8_t b = 0; b < btnCount(S.dev[i]); b++) {
    // The AC setpoint cell is a readout, so a tap there is a deliberate miss
    // rather than a third action. That dead cell between the chevrons is also
    // what stops a slightly-off tap from stepping the wrong way.
    if (ac && b == AC_BTN_TEMP) continue;
    const Rect r = btnRect(i, b);
    if (px >= r.x && px < r.x + r.w) return { HIT_ROW, (int16_t)i, (int8_t)b };
  }
  return { HIT_NONE, -1, -1 };
}

Hit screenHitTest(int16_t px, int16_t py) {
  const Hit miss = { HIT_NONE, -1, -1 };

  // Tabs are the only live region at/below STATUS_Y0, now that the header sits
  // at the bottom of the screen. Bound px explicitly rather than clamping:
  // folding a tap on the clock into tab 2 would switch pages whenever a sleeve
  // brushed the bottom-right corner.
  if (py >= STATUS_Y0) {
    // Cells are content-fit and unevenly spaced (tabRect()), so there is no
    // single stride to divide by any more — a tap in TAB_GAP or in either
    // outer margin is a genuine miss rather than snapping to a neighbour.
    for (uint8_t t = 0; t < TAB_COUNT; t++) {
      int16_t x, w;
      tabRect(t, x, w);
      if (px >= x && px < x + w) return { HIT_TAB, (int16_t)t, -1 };
    }
    return miss;
  }

  switch (S.page) {
    case PAGE_SCENES: {
      // The AC card's band FIRST, and that order is load-bearing. The card is
      // full card width (x 8..311), so it passes UNDER the gutter's column,
      // while the gutter's own rect now stops at SCENE_AC_Y0. Testing the
      // gutter first would swallow every tap on the card's up chevron, which
      // sits at x 276..303.
      if (py >= SCENE_AC_Y0) return hitDeviceRow(SCENE_AC_SLOT, px);

      // Gutter next — it owns everything from SCENE_SB_X0 right, down to
      // SCENE_AC_Y0, and the grid static_assert guarantees no tile reaches
      // into it.
      if (px >= SCENE_SB_X0) {
        if (sceneMaxRow() == 0) return miss;   // nothing to scroll: dead region
        return { HIT_SCROLL, (int16_t)(py < SCENE_SB_MID ? -1 : +1), -1 };
      }

      // Full pitch counts as the tile, not just the 88x64 rect — same
      // dark-bedroom rule as the device cards, so the gaps and the left margin
      // fold into the nearest tile rather than missing.
      const int col  = (px - SCENE_X0) / SCENE_PITCH_X;
      const int vrow = (py - ROWS_Y0)  / SCENE_PITCH_Y;
      if (col < 0 || col >= SCENE_COLS || vrow < 0 || vrow >= SCENE_VIS_ROWS)
        return miss;

      // Absolute index, so a tap means the same scene regardless of scroll —
      // this is what Hit::idx had to widen past int8_t for.
      const uint32_t idx = (uint32_t)(S.sceneRow + vrow) * SCENE_COLS + col;
      if (idx >= sceneCount()) return miss;    // empty slot on the last page
      return { HIT_SCENE, (int16_t)idx, -1 };
    }

    case PAGE_SETTINGS: {
      const int r = (py - ROWS_Y0) / ROW_H;
      if (r < 0 || r >= SET_ROWS) return miss;
      // Toggle row: two cells split at the gap between the half cards, each the
      // full height of the band and its whole half of the width. The track is
      // an affordance, not the hit area — a resistive-touch accommodation, and
      // consistent with the generous targets everywhere else here.
      if (r == SET_ROW_TGL)
        return { HIT_SETTING, SET_ROW_TGL,
                 (int8_t)(px < TGL_SPLIT_X ? SET_TGL_SCHED : SET_TGL_FLIP) };
      // Chip rows. A tap between or beside chips is a miss, as on a device card.
      const uint8_t n = r == SET_ROW_BRI ? BRI_STEPS
                      : r == SET_ROW_NIGHT ? NIGHT_CHIPS : VOL_STEPS;
      for (uint8_t c = 0; c < n; c++) {
        const Rect cr = settingChipRect((uint8_t)r, c);
        if (px >= cr.x && px < cr.x + cr.w) return { HIT_SETTING, (int16_t)r, (int8_t)c };
      }
      return miss;
    }

    default: {
      const int i = (py - ROWS_Y0) / ROW_H;
      if (i < 0 || i >= NUM_DEVICES) return miss;
      return hitDeviceRow((uint8_t)i, px);
    }
  }
}
