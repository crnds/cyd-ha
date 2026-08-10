#include "screen.h"
#include "config.h"
#include "gfx.h"
#include "icons.h"
#include "logo_ha.h"
#include "theme.h"
#include "widgets.h"
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
//   y 209..239   header   [wifi] | Devices  Scenes  Settings | 27 55% / 23:45
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
//     exactly. Scenes does not self-clear — see bodyReset().
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
static_assert(STATUS_ICO_W == TAB_X0, "status glyph and tabs leave a gap");
// TAB_STRIP_W (config.h) is now DERIVED from STATUS_ROOM_W, STATUS_CLK_W and
// TAB_X0 rather than an independent constant, so "tabs and the room/clock
// block do not meet" can no longer go wrong at compile time — there is
// nothing left for a static_assert to catch here. What tabRect() below can
// still get wrong (labels too wide for TAB_STRIP_W, margin going negative) is
// a runtime concern; see its comment.
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

// Same rule for the Scenes grid, asserted twice for two distinct failures. It
// must fill the body EXACTLY in y, because a leftover band below the last tile
// row would hold whatever the previous page left there; and it must stop at or
// before the gutter in x, because the grid and the gutter each clear only their
// own rect, so an overlap is a permanently wrong pixel.
static_assert(ROWS_Y0 + (SCENE_VIS_ROWS - 1) * SCENE_PITCH_Y + SCENE_TILE_H
                  == STATUS_Y0,
              "scene grid does not fill the body exactly");
static_assert(SCENE_X0 + (SCENE_COLS - 1) * SCENE_PITCH_X + SCENE_TILE_W
                  <= SCENE_SB_X0,
              "scene grid overlaps the scroll gutter");

// ── labels ───────────────────────────────────────────────
// Chip captions are UPPERCASE and card titles are not, and that is a fit
// decision rather than a stylistic one: caps have no descenders, so a 13px label
// centres cleanly in a 26px chip, while a lowercase 'y' would touch its edge.
// Titles and tab labels have the vertical room, so they get sentence case, which
// reads considerably calmer at this size.
static const char* BULB_LABEL[BULB_BTNS]     = {"OFF", "1%", "30%", "100%", nullptr, nullptr};
static const char* BULB_LABEL_ALT[BULB_BTNS] = {"OFF", "1",  "30",  "100",  nullptr, nullptr};
// AC slots 3..5 are the setpoint stepper — two chevrons around the value — so
// they carry no label. See btnRect() for how those three slots are placed.
static const char* AC_LABEL[AC_BTNS]         = {"OFF", "COOL", "DRY", nullptr, nullptr, nullptr};

// Indexed by PageId. Sentence case, and they fit with room to spare: the widest
// ("Settings") is 49px in Font 2 of the 73px a cell leaves after TAB_LBL_DX,
// measured rather than guessed. That width is also what the underline spans.
static const char* TAB_LABEL[TAB_COUNT] = {"Devices", "Scenes", "Settings"};

static const char* const BRI_LABEL[BRI_STEPS] = BRI_LABEL_LIST;

// Settings rows. Each caption says what the toggle will actually do, so the
// control is never asking about a value the user has to remember. Night
// mode has no caption of its own now — a chip row replaces it, one label per
// state, since there's no longer a single fixed effect to describe.
static const char* SET_LABEL[SET_ROWS] = {
  "Brightness", "Night mode", "Night schedule", "Flip screen"
};
static const char* SET_CAPTION[SET_ROWS] = {
  nullptr, nullptr, "23:45 - 08:00", "Rotate 180 degrees"
};
static const char* NIGHT_LABEL[NIGHT_CHIPS] = { "OFF", "SHIFT", "RED" };

// ── scenes ───────────────────────────────────────────────
// Macros over the three bulbs. The AC is deliberately untouched: it runs on a
// different comfort schedule than the lighting, and folding it in would make
// every scene tap a Sensibo cloud round trip.
//
// THIS TABLE IS THE ONLY DECLARATION OF HOW MANY SCENES THERE ARE. It is
// unsized on purpose and SCENE_N below is derived from it, so adding a scene is
// one line here and nothing else — no count in config.h to forget, and no
// snapshot to resize (the renderer's snapshot is per visible TILE, not per
// scene). That is what makes the page scale to a list far longer than the screen.
//
// The tile's three pips are DERIVED from the row below rather than written as a
// caption, so unlike a hand-written string they cannot describe a scene the tap
// won't send — which a caption already had, reading "2200K" while KELVIN_WARM
// was corrected to the bulbs' real 2202K limit.
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
// affordance compile out and the page look exactly like a fixed one.
static constexpr uint16_t SCENE_ROWS_N = (SCENE_N + SCENE_COLS - 1) / SCENE_COLS;
static constexpr uint16_t SCENE_MAX_ROW =
    SCENE_ROWS_N > SCENE_VIS_ROWS ? (uint16_t)(SCENE_ROWS_N - SCENE_VIS_ROWS) : 0;

// ── dirty-region snapshots ───────────────────────────────
// A card is tracked as independent regions: its identity/state line, each
// control, and (AC only) the setpoint readout. A brightness change repaints the
// two chips whose appearance differs, not the card.
//
// EVERY PAGE SNAPSHOT MUST KEEP A PER-ITEM BtnVis BYTE, and that is load-bearing
// rather than an optimisation: the press flash expires by TIME, not by any state
// change, so only a per-item vis compare notices BV_PRESSED -> BV_ACTIVE and
// repaints. A page that skips it leaves the tapped control inverted forever.
//
// Each snapshot also needs its own `valid` flag on top, because BV_INACTIVE is 0
// and a memset alone reads as "already drawn as inactive".
struct RowSnap {
  bool     valid;
  char     tempStr[8];          // AC row only: last rendered setpoint
  bool     stale;
  bool     err;
  bool     acOff;                // AC row only: mode == "off", greys the setpoint
  bool     alarm;               // err OR offline — what the card's border shows
  // The status icon's RESOLVED appearance, not the values behind it. A bulb's
  // icon is its real colour temperature blended by its real brightness, so
  // comparing the colour is the same "compare by visual state" rule the chips
  // follow — and a 1% change that quantises to the same RGB565 costs nothing.
  // It has to be tracked separately from the name string because the AC's icon
  // does not mention its mode (the chips do), so a cool->dry change would
  // otherwise repaint no region at all and leave the wrong glyph on screen.
  uint16_t icoColour;
  uint8_t  icoShape;
  uint8_t  btnVis[BULB_BTNS];   // BULB_BTNS == AC_BTNS, so this covers both
};
static RowSnap snap[NUM_DEVICES];

// Sized per visible TILE rather than per scene, which is the whole reason this
// page can scale: 9 bytes whether the table holds 5 scenes or 500. The cost is
// that `row` has to be part of the snapshot — a scroll changes which scene every
// slot holds, so the old bytes describe different scenes and none of them can be
// compared. That case is handled as a first draw.
struct SceneSnap {
  bool     valid;
  uint16_t row;                     // scroll offset these slots were drawn at
  uint8_t  vis[SCENE_PER_PAGE];
  uint8_t  sbVis;                   // gutter: 0 idle, 1 up held, 2 down held
};
static SceneSnap sceneSnap;

struct SettingSnap {
  bool    valid;
  uint8_t briVis[BRI_STEPS];
  int8_t  briShown;             // level named on the Brightness card's own line
  uint8_t nightVis[NIGHT_CHIPS];
  bool    toggle[SET_ROWS];     // indices SET_ROW_BRI, SET_ROW_NIGHT unused
};
static SettingSnap setSnap;

// Split into independently-dirty regions (glyph | tabs | room reading | clock)
// so a change in one never repaints the others. Repainting the full bar for a
// one-character change is what made it visibly flash.
//
// tabVis rather than a bare `page`: it encodes the active page AND handles
// press-flash expiry, which a page field could not.
struct StatusSnap {
  bool     valid;
  bool     wifiOk;
  bool     haOk;
  int16_t  hhmm;    // local time as hour*60+min; -1 while NTP is unsynced
  uint8_t  tabVis[TAB_COUNT];
  char     roomStr[8];   // last rendered room temperature (relocated from the
                         // AC card's own RowSnap — see drawStatusRoom())
  char     humStr[8];    // last rendered humidity
  uint16_t roomFg;       // resolved colour, so a stale/err transition with no
                         // string change still repaints (mirrors icoColour).
};
static StatusSnap statusSnap;

// What is physically on the glass, as opposed to S.page (what should be).
// 0xFF means "nothing valid" and forces a body wipe on the next render.
static uint8_t shownPage = 0xFF;

// Icon shapes, for the RowSnap compare above.
enum IcoShape : uint8_t { IS_BULB_OFF = 0, IS_BULB_ON, IS_SNOW, IS_DROP, IS_POWER };

// ── geometry ─────────────────────────────────────────────

// Takes a row SLOT, not a device index — the Settings page reuses this grid.
static inline int16_t rowTop(uint8_t slot)  { return ROWS_Y0 + slot * ROW_H; }
static inline int16_t cardTop(uint8_t slot) { return rowTop(slot) + CARD_DY; }

// Take an on-screen SLOT (0..SCENE_PER_PAGE-1), not a scene index — which scene
// a slot shows depends on S.sceneRow.
static inline int16_t sceneTileX(uint8_t slot) {
  return SCENE_X0 + (slot % SCENE_COLS) * SCENE_PITCH_X;
}
static inline int16_t sceneTileY(uint8_t slot) {
  return ROWS_Y0 + (slot / SCENE_COLS) * SCENE_PITCH_Y;
}

// Where slot `b` of device row `dev` sits. The two kinds lay out the same six
// slots differently IN X, and this is the ONLY function that knows that — every
// caller (renderer, hit test, calibration verify) goes through here, so the
// drawn rect and the tappable rect cannot drift apart.
//
// Bulb:  (i) 1 [OFF][1%][30%][100%] (o)(o)     slots 0..3 chips, 4..5 swatches
// AC:    (i) AC [OFF][COOL][DRY]  [^] 30 [v]    slots 0..2 chips, 3/4/5 stepper
//
// The two kinds share one Y band now (BULB_CTL_DY/BULB_CTL_H) — the AC's
// control row used to sit in its own shorter CTL_DY/CTL_H strip under a
// separate state line, visibly out of step with the bulb rows around it; that
// distinction is gone along with the AC's stacked layout (see drawDeviceCard).
//
// Slot order here is UP-left/DOWN-right, on request — up is slot 3 (left),
// down is slot 5 (right). This used to be the other way round, deliberately,
// to read left-to-right as less-to-more; that reasoning is retired along with
// the layout. The slot numbers themselves are fixed by doAction(), so mapping
// them here is what buys whichever order is wanted without touching the
// action layer.
static void btnRect(uint8_t dev, uint8_t b,
                    int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
  y = cardTop(dev) + BULB_CTL_DY;
  h = BULB_CTL_H;

  if (S.dev[dev].kind == DEV_CLIMATE) {
    if (b < 3) { x = BULB_CTL_X0 + b * ACM_PITCH; w = ACM_W; return; }
    switch (b) {
      case AC_BTN_TUP:  x = ACS_X0;                            w = ACS_BTN_W; return;
      case AC_BTN_TEMP: x = ACS_X0 + ACS_BTN_W;                w = ACS_VAL_W; return;
      default:          x = ACS_X0 + ACS_BTN_W + ACS_VAL_W;    w = ACS_BTN_W; return;
    }
  }

  if (b < 4) { x = BULB_CTL_X0 + b * BULB_CHIP_PITCH; w = BULB_CHIP_W; return; }
  x = SW_X0 + (b - 4) * SW_CELL_W;
  w = SW_CELL_W;
}

// The five brightness chips share the device rows' chip pitch — one grid, so a
// control on the Settings page and a control on a device card are the same size
// and the pages read as one product.
static void briRect(uint8_t b, int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
  x = CARD_IN_X0 + b * CHIP_PITCH;
  w = CHIP_W;
  y = cardTop(SET_ROW_BRI) + CTL_DY;
  h = CTL_H;
}

// The Night mode row's 3 chips (Off/Shift/Red), on the same shared chip pitch.
// NIGHT_CHIPS(3) uses far less of CARD_IN_W than BRI_STEPS(5) does, so there's
// no static_assert risk here in practice — added anyway to match the
// brightness row's and catch a future edit that widens the chips.
static void nightRect(uint8_t c, int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
  x = CARD_IN_X0 + c * CHIP_PITCH;
  w = CHIP_W;
  y = cardTop(SET_ROW_NIGHT) + CTL_DY;
  h = CTL_H;
}

// One place that knows the press-flash timing rule. pressKind is what keeps it
// page-safe — matching on the index alone would light row 2 on the Devices page
// when scene 2 was tapped.
static inline bool pressedNow(HitKind k, int16_t idx, int8_t sub) {
  return S.pressKind == k && S.pressIdx == idx && S.pressSub == sub &&
         S.pressMs && (millis() - S.pressMs) < PRESS_FLASH_MS;
}

// ── active-state derivation ──────────────────────────────

// Round-trip tolerances, factored out so a scene tile and the row chip it
// corresponds to can never disagree about what "30%" means. HA rounds
// brightness_pct through a 0-255 byte, so exact compares would never match.
static inline bool pctMatches(int want, int got) {
  if (want >= BRI_HIGH) return got >= PCT_HIGH_MIN;
  if (want >= BRI_MID)  return got >= PCT_MID_MIN && got <= PCT_MID_MAX;
  return got >= 0 && got <= PCT_LOW_MAX;
}

static inline bool kelvinMatches(int want, int got) {
  return (want <= KELVIN_WARM_MAX) ? (got > 0 && got <= KELVIN_WARM_MAX)
                                   : (got >= KELVIN_COOL_MIN);
}

static bool btnActive(const DeviceState& d, uint8_t b) {
  // An unreachable device has no current state to highlight. Lighting OFF here
  // would claim the bulb is off when we simply cannot see it.
  if (!d.avail) return false;

  if (d.kind == DEV_CLIMATE) {
    switch (b) {
      case 0:  return strcmp(d.mode, "off") == 0;
      case 1:  return strcmp(d.mode, AC_MODE_COOL) == 0;
      case 2:  return strcmp(d.mode, AC_MODE_DRY) == 0;
      default: return false;   // the setpoint steps are momentary, never a state
    }
  }
  if (!d.on) return b == 0;    // bulb off: only OFF lit

  // A brightness chip AND a colour swatch can both be active — they are
  // independent axes of an on-bulb, not mutually exclusive choices.
  switch (b) {
    case 1:  return pctMatches(BRI_LOW,  d.pct);
    case 2:  return pctMatches(BRI_MID,  d.pct);
    case 3:  return pctMatches(BRI_HIGH, d.pct);
    case 4:  return d.supportsCT && kelvinMatches(KELVIN_WARM, d.kelvin);
    case 5:  return d.supportsCT && kelvinMatches(KELVIN_COOL, d.kelvin);
    default: return false;
  }
}

// A scene is ACTIVE when the LIVE state of all three bulbs matches its
// definition. Derived every render, never latched — that is exactly what makes
// overriding one bulb on the Devices page deselect the scene, and what makes a
// change from the HA app select the matching one, with no "current scene"
// variable to fall out of sync.
bool sceneActive(uint16_t idx) {
  if (idx >= SCENE_N) return false;
  const Scene& sc = SCENE[idx];

  for (uint8_t i = 0; i < NUM_BULBS; i++) {
    const DeviceState& d = S.dev[i];
    const SceneBulb&   w = sc.b[i];

    // Same rule as btnActive(): with no current state there is nothing to
    // highlight, and a lit tile would claim the room is in a state we cannot
    // actually see.
    if (!d.known || !d.avail) return false;

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
  return true;
}

const char* sceneName(uint16_t idx) {
  return idx < SCENE_N ? SCENE[idx].name : "?";
}

uint16_t sceneCount() { return SCENE_N; }

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

// ── derived colour ───────────────────────────────────────

// What a bulb's colour temperature looks like, interpolated rather than bucketed.
// The active-highlight tests bucket (a swatch is either the warm one or it is
// not), but the ICON is a readout, not a choice: a bulb sitting at 3000K from the
// HA app should look like 3000K, not snap to one end.
static uint16_t bulbHue(const DeviceState& d) {
  if (!d.supportsCT || d.kelvin <= 0) return C_TEXT2;
  int32_t t = ((int32_t)d.kelvin - KELVIN_WARM) * 255
              / (KELVIN_COOL - KELVIN_WARM);
  if (t < 0)   t = 0;
  if (t > 255) t = 255;
  return lerp565(C_WARM, C_COOL, (uint8_t)t);
}

// The card's status icon, resolved to a shape and a colour. Returned rather than
// drawn so the caller can compare it against the snapshot — this IS the icon's
// visual state.
static void iconVis(const DeviceState& d, bool stale, uint8_t& shape,
                    uint16_t& colour) {
  if (!d.known || !d.avail) {
    // Never let an unreachable device read as a normal state: a dark bulb
    // outline is exactly what a healthy off bulb looks like.
    shape  = (d.kind == DEV_CLIMATE) ? IS_POWER : IS_BULB_OFF;
    colour = d.known ? C_ERROR : C_DISABLED;
    return;   // an offline device is not ALSO dimmed: red outranks stale
  }

  if (d.kind == DEV_CLIMATE) {
    if (!strcmp(d.mode, AC_MODE_COOL))     { shape = IS_SNOW;  colour = C_ACCENT; }
    else if (!strcmp(d.mode, AC_MODE_DRY)) { shape = IS_DROP;  colour = C_ACCENT; }
    else if (!strcmp(d.mode, "off"))       { shape = IS_POWER; colour = C_DISABLED; }
    // A mode outside the three the chips offer gets the POWER glyph, not a
    // snowflake: "running, in a mode this page does not chart" is true, whereas
    // a snowflake next to a mode this page cannot name is simply wrong. Naming
    // it (heat/fan_only/auto — set from the HA app) used to be stateText()'s
    // job; the inline identity column has no room left for that word once the
    // room reading moved into it, so this glyph is now the ONLY signal an
    // exceptional mode gets. That is the same cost the bulb cards already pay
    // for their own state, accepted here for the same reason.
    else                                   { shape = IS_POWER; colour = C_TEXT2; }
  } else if (!d.on) {
    shape = IS_BULB_OFF; colour = C_DISABLED;
  } else {
    shape = IS_BULB_ON;
    // Level as a blend toward what the icon sits on. The card carries no fill,
    // so that is C_BG — anything lighter would leave a halo at low brightness.
    // 1% still lands at 70/255 of the hue, which stays visible.
    const uint16_t c = bulbHue(d);
    colour = (d.pct >= 0 && d.pct < BRI_HIGH)
               ? lerp565(C_BG, c, (uint8_t)(70 + d.pct * 185 / 100))
               : c;
  }

  // The icon is a readout of the same values the state line shows, so it obeys
  // the same fail-soft rule: keep the last known reading, but dim it once the
  // poll goes stale rather than presenting old data at full confidence. Dimming
  // the text and not the icon left half the card claiming to be current.
  if (stale) colour = lerp565(C_BG, colour, 110);
}

static void drawIcon(uint8_t shape, uint16_t colour, int16_t cx, int16_t cy) {
  switch (shape) {
    case IS_BULB_ON:  icoBulb(cx, cy, colour, true);  break;
    case IS_SNOW:     icoSnow(cx, cy, colour);        break;
    case IS_DROP:     icoDrop(cx, cy, colour);        break;
    case IS_POWER:    icoPower(cx, cy, colour);       break;
    default:          icoBulb(cx, cy, colour, false); break;
  }
}

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

// Humidity alone, unconditionally blank rather than "--" when there is nothing
// to show: unlike the room temperature (always drawn, even as "--", so its
// position in the header never jumps), a missing humidity reading simply
// means one less piece drawn — see drawStatusRoom(), the sole caller now.
static void humidityText(const DeviceState& d, char* out, size_t n) {
  out[0] = '\0';
  if (!d.known || !d.avail || d.humidity < 0) return;
  snprintf(out, n, "%d%%", d.humidity);
}

// The AC setpoint as drawn between the chevrons. "--" rather than a guessed
// number until a real setpoint is known — the same rule that makes a step tap a
// no-op until then (doAction()), so the readout and the control agree.
static void tempText(const DeviceState& d, char* out, size_t n) {
  if (!d.known || !d.avail || isnan(d.target)) { snprintf(out, n, "--"); return; }
  snprintf(out, n, "%.0f", d.target);
}

// ── device cards ─────────────────────────────────────────

static void drawDeviceCard(uint8_t dev, bool force) {
  DeviceState& d   = S.dev[dev];
  const uint32_t now = millis();

  const bool stale = d.known && (now - d.okMs > DEVICE_STALE_MS);
  const bool err   = d.errMs && (now - d.errMs < 1500);

  const int8_t press = (S.pressKind == HIT_ROW && S.pressIdx == (int16_t)dev &&
                        S.pressMs && now - S.pressMs < PRESS_FLASH_MS)
                           ? S.pressSub : -1;

  RowSnap&      sn    = snap[dev];
  const bool    first = force || !sn.valid;
  const int16_t top   = cardTop(dev);
  const bool    ac    = (d.kind == DEV_CLIMATE);
  // "off" for the AC is a mode string, not DeviceState::on (that field is
  // light-only) — mirrors the check btnActive() already uses for chip 0.
  const bool    acOff = ac && strcmp(d.mode, "off") == 0;

  // The card's OUTLINE is its alarm channel. A failed service call has always
  // flashed it; an unreachable device now HOLDS it, which is what replaced the
  // word "OFFLINE" on the bulb cards when their state line went away. The
  // fail-loud rule — never let an unreachable device read as a normal state —
  // has to survive the inline layout, and a border costs none of the width the
  // controls now take. The AC card keeps its word too and takes the border as
  // well, so both kinds speak one alarm language.
  //
  // `!avail` cannot fire before the first poll: DeviceState::avail starts true
  // and only a poll that actually saw "unavailable" clears it, so this does not
  // paint every card red for the first 1.5 s after boot.
  const bool alarm = err || !d.avail;

  // On a first/forced draw, clear the whole row BAND and lay the card down. The
  // band is full width and the four bands tile the body exactly, which is what
  // makes this page self-clearing: the per-region clears below cover only the
  // text strip and the control rects, leaving the 4px gutters between cards and
  // the 8px side margins to hold whatever was underneath. Boot text used to
  // survive in exactly those slivers.
  if (first) {
    tft.fillRect(0, rowTop(dev), SCR_W, ROW_H, C_BG);
    wCard(CARD_X, top, CARD_W, CARD_H, C_BG, alarm ? C_ERROR : C_BORDER);
  } else if (alarm != sn.alarm) {
    // The whole surface carries the notification rather than one word of text
    // going red. Redrawing the outline alone is enough — the fill and everything
    // on it is unchanged.
    tft.drawRect(CARD_X, top, CARD_W, CARD_H, alarm ? C_ERROR : C_BORDER);
  }

  // ── region 1: status icon + identity text ──
  // One region, because the icon sits inside the strip the text clears. Its
  // compare therefore has to include the icon's appearance, or an AC mode change
  // (which does not alter either string) would repaint nothing.
  //
  // Every device card is inline and shares one identity column
  // (BULB_ID_W/BULB_NAME_W): a single centred name line, the icon its only
  // other content. The AC used to draw two more lines here (room temperature,
  // humidity) — that reading now lives in the header instead (drawStatusRoom()
  // below), so the AC's identity column is back to the same one-line shape a
  // bulb's already was.
  uint8_t  icoShape;
  uint16_t icoColour;
  iconVis(d, stale, icoShape, icoColour);

  if (first || stale != sn.stale || err != sn.err || alarm != sn.alarm ||
      icoShape != sn.icoShape || icoColour != sn.icoColour) {
    const int16_t idCy = top + CARD_H / 2;   // icon centre — shared by every card

    // Text is drawn transparent (see textAt), so clear first. Starts at
    // CARD_IN_X0 to stay clear of the card's own left border column, and clears
    // only the identity COLUMN (not the card width): the controls beside it own
    // their own rects and repaint on their own compares, so wiping the full
    // width here would erase chips that nothing was going to redraw.
    tft.fillRect(CARD_IN_X0, top + BULB_CTL_DY, BULB_ID_W, BULB_CTL_H, C_BG);

    drawIcon(icoShape, icoColour, CARD_ICO_CX, idCy);

    // The name takes the alarm colour on EVERY card kind now, because for both
    // it is the only text on the card that still says "this one has a problem"
    // once OFFLINE (or, for the AC, an exceptional mode's name) is gone: a red
    // name beside a red icon inside a red border, with every control greyed, is
    // what that failure looks like regardless of kind.
    const uint16_t nameFg = alarm ? C_ERROR : (stale ? C_DIM : C_TEXT);
    textTrunc(F_TITLE, d.name, CARD_TXT_X, idCy, BULB_NAME_W, nameFg);

    sn.icoShape  = icoShape;
    sn.icoColour = icoColour;
  }

  // ── region 2: each control, independently dirty ──
  const uint8_t n = btnCount(d);
  for (uint8_t b = 0; b < n; b++) {
    // The AC's middle slot is the setpoint readout, not a control: it is a text
    // region compared by its rendered string, like the identity line, so it is
    // handled as its own region below and takes no BtnVis byte.
    if (ac && b == AC_BTN_TEMP) continue;

    const bool isSwatch = (d.kind == DEV_LIGHT && b >= 4);
    const bool isAcTemp = ac && (b == AC_BTN_TUP || b == AC_BTN_TDN);
    const bool disabled = !d.avail || (isSwatch && !d.supportsCT) ||
                          (isAcTemp && acOff);

    uint8_t vis = BV_INACTIVE;
    if (disabled)                 vis = BV_DISABLED;
    else if (press == (int8_t)b)  vis = BV_PRESSED;
    // Slot 0 is OFF on both card kinds, and it gets the neutral highlight rather
    // than the accent: with the bulbs off, an accented OFF put three cyan chips
    // on the page for a room that is doing nothing. Which fill that means is
    // ctlColour()'s business — this only says what kind of selection it is.
    //
    // A bulb's 1%/30%/100% chip (not the AC's mode chips, not a swatch) gets
    // its own yellow vis on request, scoped to exactly this control — see
    // BV_ACTIVE_BRI in widgets.h.
    else if (btnActive(d, b)) {
      vis = (b == 0)               ? BV_ACTIVE_OFF
          : (!ac && !isSwatch)     ? BV_ACTIVE_BRI
                                    : BV_ACTIVE;
    }

    if (!first && vis == sn.btnVis[b]) continue;
    sn.btnVis[b] = vis;

    int16_t x, y, w, h;
    btnRect(dev, b, x, y, w, h);

    if (ac) {
      if (b == AC_BTN_TUP || b == AC_BTN_TDN)
        wStepBtn(x, y, w, h, b == AC_BTN_TUP, vis);
      else
        wChip(x, y, w, h, AC_LABEL[b], nullptr, vis);
    } else if (isSwatch) {
      // A swatch's circle is centred in a cell wider than itself: the cell is
      // the tap target, the circle is the affordance.
      wSwatch(x + w / 2, y + h / 2, SW_R, b == 4 ? C_WARM : C_COOL, vis);
    } else {
      wChip(x, y, w, h, BULB_LABEL[b], BULB_LABEL_ALT[b], vis);
    }
  }

  // ── region 3 (AC only): the setpoint, between the two chevrons ──
  // Compared on stale/err/acOff as well as the string, because the readout
  // follows the same colour rules as the state line and none of those change
  // the text.
  if (ac) {
    char tv[8];
    tempText(d, tv, sizeof(tv));
    if (first || stale != sn.stale || err != sn.err || acOff != sn.acOff ||
        strcmp(tv, sn.tempStr) != 0) {
      int16_t x, y, w, h;
      btnRect(dev, AC_BTN_TEMP, x, y, w, h);
      const bool known = (tv[0] != '-');
      // Greyed the same as a disabled chevron beside it (C_DISABLED) when the
      // AC is off — the setpoint can't be stepped, so it shouldn't read as
      // live text.
      wValue(x, y, w, h, tv, known,
             err || !d.avail ? C_ERROR
                              : (acOff ? C_DISABLED : (stale ? C_DIM : C_TEXT)),
             C_BG);
      snprintf(sn.tempStr, sizeof(sn.tempStr), "%s", tv);
    }
  }
  sn.acOff = acOff;

  // Hoisted out of region 1 so regions 2 and 3 can compare against the same
  // previous values: updating them up there would make every setpoint repaint
  // miss a stale/err transition that region 1 had already consumed.
  sn.stale = stale;
  sn.err   = err;
  sn.alarm = alarm;
  sn.valid = true;
}

// ── header ───────────────────────────────────────────────

// The whole connectivity readout, in 26px. It replaced two labelled dots reading
// "WIFI" and "HA" — permanent debug chrome that spent 88px telling a healthy
// system it was healthy. Now the glyph is quiet when there is nothing to say and
// specific when there is:
//
//   both up      3 bars in C_TEXT3      present, unobtrusive, reassuring
//   HA down      3 bars + amber badge   the link is up, the service is not
//   Wi-Fi down   0 bars, all in red     nothing is reachable
//
// Colour AND a shape/badge change, never colour alone — the palette collapses to
// red at night, and this is the one region a colour-blind reading of "is it
// working" must survive.
static void drawStatusGlyph(bool wifiOk, bool haOk) {
  tft.fillRect(0, STATUS_DIV_Y + 1, STATUS_ICO_W, STATUS_H - 1, C_BG);
  if (!wifiOk) {
    icoWifi(STATUS_ICO_CX, STATUS_CY, 0, C_ERROR, C_ERROR);
    return;
  }
  icoWifi(STATUS_ICO_CX, STATUS_CY, 3, C_TEXT3, C_DIVIDER);
  if (!haOk) icoBadge(STATUS_ICO_CX + 6, STATUS_CY - 6, C_WARNING, C_BG);
}

// Each tab's cell hugs its OWN label (measured, not a shared constant — see
// the TAB_STRIP_W comment in config.h), with the leftover TAB_STRIP_W split
// evenly as a margin on the two OUTER edges rather than spent as slack inside
// every cell. Computed once and cached: TAB_LABEL never changes at runtime,
// so there is nothing to invalidate.
//
// A future-proofing note rather than a live bug: if TAB_LABEL ever grew wide
// enough that 3 cells + 2*TAB_GAP exceeded TAB_STRIP_W, `margin` goes negative
// and tabs would overlap. The 3 shipped labels leave 19px to spare now (it
// was 57px before the header grew a room-reading region and TAB_GAP was cut
// to make room for that region's F_TITLE text; see STATUS_ROOM_W in config.h
// for why the margin partly grew back — a humidity-budget fix, not a second
// deliberate squeeze), so this is deliberately not runtime-guarded —
// simulator.html's draw() warns on the same arithmetic, which is where a
// future label or font change would be caught.
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
static void drawTab(uint8_t i, const char* label, uint8_t vis) {
  int16_t x, w;
  tabRect(i, x, w);
  tft.fillRect(x, STATUS_DIV_Y + 1, w, STATUS_H - 1, C_BG);
  wTab(x, STATUS_DIV_Y + 1, w, STATUS_H - 1, label, vis);
}

// The AC's room reading, relocated here from its own card (see the "room
// reading" comment above roomTempText()) so it reads on every page rather
// than only Devices. `S.dev[NUM_BULBS]` is the AC by construction (config.h:
// "Devices 0..2 are the bulbs, device 3 is the AC") — same assumption main.cpp
// already makes when it wires up ENT_AC at that index, so this is not a new
// coupling, just a second place that relies on it.
//
// Drawn as <temp><ring> <humidity> / , left-aligned from the region's left
// edge (SCR_W - STATUS_ROOM_W - STATUS_CLK_W), immediately before the clock's
// own region. The temperature and humidity are F_TITLE — bumped up from
// F_MICRO on request, to match the clock they sit beside, which is what this
// region cost TAB_GAP (config.h) to afford. The "/" separator stayed
// F_MICRO: it is punctuation, not data, so it stays quiet rather than
// competing with the two numbers on either side of it. Humidity is skipped
// when there's nothing to show (humidityText() returns "") but the "/" still
// draws, so the separator's position doesn't jump around with it.
static void drawStatusRoom(bool force) {
  DeviceState& d   = S.dev[NUM_BULBS];
  const uint32_t now = millis();
  const bool stale = d.known && (now - d.okMs > DEVICE_STALE_MS);
  const bool err   = d.errMs && (now - d.errMs < 1500);
  const uint16_t fg = (err || !d.avail) ? C_ERROR : (stale ? C_DIM : C_TEXT2);

  char room[8]; bool degree;
  roomTempText(d, room, sizeof(room), degree);
  char hum[8];
  humidityText(d, hum, sizeof(hum));

  if (!force && strcmp(room, statusSnap.roomStr) == 0 &&
      strcmp(hum, statusSnap.humStr) == 0 && fg == statusSnap.roomFg)
    return;

  const int16_t x0 = SCR_W - STATUS_ROOM_W - STATUS_CLK_W;
  tft.fillRect(x0, STATUS_DIV_Y + 1, STATUS_ROOM_W, STATUS_H - 1, C_BG);

  int16_t x = x0;
  textAt(F_TITLE, room, x, STATUS_CY, ML_DATUM, fg);
  x += textW(F_TITLE, room);
  if (degree) {
    // Same idiom as the old AC card and the AC setpoint used: ring rides the
    // digit tops, fontInkTop() locates them, a small fixed gap clears the
    // digits. Ring right edge is x+5 (3px gap + 2px radius) regardless of
    // font — only fontInkTop()'s role argument moved from MICRO to TITLE when
    // the digits did, which is where STATUS_ROOM_W's budget charges it now.
    const int16_t rcx = x + 3;
    const int16_t rcy = STATUS_CY + fontInkTop(F_TITLE) + 2;
    tft.drawCircle(rcx, rcy, 2, fg);
    x += 5;
  }
  x += SP_1;
  if (hum[0]) {
    textAt(F_TITLE, hum, x, STATUS_CY, ML_DATUM, fg);
    x += textW(F_TITLE, hum) + SP_1;
  }
  textAt(F_MICRO, "/", x, STATUS_CY, ML_DATUM, C_TEXT3);

  snprintf(statusSnap.roomStr, sizeof(statusSnap.roomStr), "%s", room);
  snprintf(statusSnap.humStr,  sizeof(statusSnap.humStr),  "%s", hum);
  statusSnap.roomFg = fg;
}

static void drawStatus(bool force) {
  const bool wifiOk = (S.netState == 1);

  // Local wall clock, 24h. getLocalTime with a 0 ms timeout returns immediately
  // — it must never block, since this runs on every render pass. It reports
  // false until SNTP has landed, which is what drives the "--:--" placeholder.
  int16_t hhmm = -1;
  struct tm tmv;
  if (getLocalTime(&tmv, 0)) hhmm = (int16_t)(tmv.tm_hour * 60 + tmv.tm_min);

  const bool force_ = force || !statusSnap.valid;

  // The four regions below tile the bar exactly (static_assert'd at the top of
  // this file), so between them they cover every pixel and no full-width clear
  // is needed to catch a gap.
  if (force_ || wifiOk != statusSnap.wifiOk || S.haOk != statusSnap.haOk)
    drawStatusGlyph(wifiOk, S.haOk);

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
  // connection health belongs to the glyph and staleness to the card dimming,
  // not here.
  //
  // The clock is always exactly 5 characters. "23:45" is 35px in F_TITLE
  // against the 41px this region now leaves (it was 51px, ~10px of slack,
  // before the room reading moved in beside it — see STATUS_CLK_W in
  // config.h), so anything wider paints into the room region, which only
  // repaints on ITS OWN compare and would leave the overflow permanent.
  // There is now a 6px right margin and ZERO px of left slack — don't put
  // anything else here.
  if (force_ || hhmm != statusSnap.hhmm) {
    tft.fillRect(SCR_W - STATUS_CLK_W, STATUS_DIV_Y + 1, STATUS_CLK_W,
                 STATUS_H - 1, C_BG);
    char clk[8];
    if (hhmm < 0) snprintf(clk, sizeof(clk), "--:--");
    else          snprintf(clk, sizeof(clk), "%02d:%02d", hhmm / 60, hhmm % 60);
    textAt(F_TITLE, clk, SCR_W - SP_2 + 2, STATUS_CY, MR_DATUM,
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
  statusSnap.wifiOk = wifiOk;
  statusSnap.haOk   = S.haOk;
  statusSnap.hhmm   = hhmm;
  memcpy(statusSnap.tabVis, tabVis, sizeof(tabVis));
}

// ── scenes page ──────────────────────────────────────────

// One tile. `slot` is where it sits on screen, `idx` which scene it shows.
static void drawSceneTile(uint8_t slot, uint16_t idx, uint8_t vis) {
  const int16_t x = sceneTileX(slot), y = sceneTileY(slot);

  // A slot past the end of the table is blanked with the same rect the tile
  // occupies, which is why a scroll needs no body wipe: every tile either
  // repaints its own rect or blanks it, and the gaps never change content.
  // (When tiles were rounded this clear had to be square ON PURPOSE, since
  // fillRoundRect leaves the four corner pixels of its bounding box untouched
  // and a rounded clear stranded the previous tile's corners. Now the drawn
  // shape and the clear are the same rect and it cannot drift.)
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
  // The body's own bottom edge (STATUS_Y0), not the panel's — the header now
  // sits below it and must not be touched by this clear.
  tft.fillRect(SCENE_SB_X0, ROWS_Y0, SCENE_SB_W, STATUS_Y0 - ROWS_Y0, C_BG);
  if (SCENE_MAX_ROW == 0) return;

  const int16_t cx  = SCENE_SB_X0 + SCENE_SB_W / 2;
  const int16_t bot = STATUS_Y0 - 1;
  const bool atTop = (S.sceneRow == 0), atBot = (S.sceneRow >= SCENE_MAX_ROW);

  // Dimmed at the ends rather than hidden: a control that vanishes moves the
  // other one's apparent target, and this is a resistive panel.
  icoChevron(cx, ROWS_Y0 + 13, true,
             pressed == 1 ? C_TEXT : (atTop ? C_DISABLED : C_TEXT2), 6, 5);
  icoChevron(cx, bot - 13, false,
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

static void drawScenes() {
  const uint32_t now = millis();

  bool anyErr = false, unavail = false;
  for (uint8_t i = 0; i < NUM_BULBS; i++) {
    if (S.dev[i].errMs && now - S.dev[i].errMs < 1500) anyErr = true;
    if (!S.dev[i].known || !S.dev[i].avail)            unavail = true;
  }

  // A scroll re-points every slot at a different scene, so not one of the cached
  // vis bytes describes what is now meant to be there. Treat it as a first draw.
  const bool     first = !sceneSnap.valid || sceneSnap.row != S.sceneRow;
  const uint16_t base  = (uint16_t)(S.sceneRow * SCENE_COLS);

  for (uint8_t slot = 0; slot < SCENE_PER_PAGE; slot++) {
    const uint16_t idx = base + slot;

    uint8_t vis = BV_INACTIVE;
    // An empty slot shares BV_DISABLED, which is safe because slot -> idx is
    // fixed for a given scroll offset: a slot that is empty stays empty until
    // `row` changes, and that forces a full redraw anyway.
    if (idx >= SCENE_N || unavail)                     vis = BV_DISABLED;
    else if (pressedNow(HIT_SCENE, (int16_t)idx, -1))  vis = BV_PRESSED;
    // The error belongs to the page, not to one tile — nothing here remembers
    // which scene was tapped once the press flash has expired.
    else if (anyErr)                                   vis = BV_ERR;
    else if (sceneActive(idx))                         vis = BV_ACTIVE;

    if (!first && vis == sceneSnap.vis[slot]) continue;
    sceneSnap.vis[slot] = vis;
    drawSceneTile(slot, idx, vis);
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

  sceneSnap.row   = S.sceneRow;
  sceneSnap.valid = true;
}

// ── settings page ────────────────────────────────────────

// Night mode is chip-driven now, not a toggle — it has no entry here.
static bool setToggleVal(uint8_t row) {
  switch (row) {
    case SET_ROW_SCHED: return S.set.nightSched;
    case SET_ROW_FLIP:  return S.set.flip;
    default:            return false;
  }
}

static void setIcon(uint8_t row, int16_t cx, int16_t cy) {
  switch (row) {
    case SET_ROW_BRI:   icoSun(cx, cy, C_TEXT2);              break;
    // The crescent's bite is painted in the CARD's fill, not C_BG.
    case SET_ROW_NIGHT: icoMoon(cx, cy, C_TEXT2, C_SURFACE);  break;
    case SET_ROW_SCHED: icoClock(cx, cy, C_TEXT2);            break;
    default:            icoRotate(cx, cy, C_TEXT2);           break;
  }
}

// Static parts of a settings card: the surface, its icon, title and caption. The
// controls repaint independently, so nothing here is redrawn once it is down.
static void drawSettingChrome(uint8_t row) {
  const int16_t top = cardTop(row);
  tft.fillRect(0, rowTop(row), SCR_W, ROW_H, C_BG);
  wCard(CARD_X, top, CARD_W, CARD_H, C_SURFACE, C_BORDER);

  if (row == SET_ROW_BRI || row == SET_ROW_NIGHT) {
    setIcon(row, CARD_ICO_CX, top + CARD_L1_CY);
    textAt(F_TITLE, SET_LABEL[row], CARD_TXT_X, top + CARD_L1_CY, ML_DATUM,
           C_TEXT);
  } else {
    // Title over caption, the pair vertically centred in the card. Two type
    // roles doing the work one size and two colours used to: weight separates
    // what the setting IS from what it will do.
    setIcon(row, CARD_ICO_CX, top + CARD_H / 2);
    textAt(F_TITLE, SET_LABEL[row],   CARD_TXT_X, top + SET_TITLE_CY, ML_DATUM,
           C_TEXT);
    textAt(F_BODY,  SET_CAPTION[row], CARD_TXT_X, top + SET_CAP_CY,   ML_DATUM,
           C_TEXT3);
  }
}

static void drawSettings() {
  const bool first = !setSnap.valid;
  if (first) {
    for (uint8_t r = 0; r < SET_ROWS; r++) drawSettingChrome(r);
    setSnap.briShown = -1;
  }

  // The chosen level, named on the Brightness card's own identity line. A
  // segmented control shows WHICH of five is selected; it does not say what the
  // selection means, and "50%" spelled out is the difference between a row of
  // chips and a row of chips you can read.
  if (setSnap.briShown != (int8_t)S.set.briIdx) {
    setSnap.briShown = (int8_t)S.set.briIdx;
    const int16_t top = cardTop(SET_ROW_BRI);
    tft.fillRect(CARD_IN_X1 - 44, top + CARD_L1_Y, 45, CARD_L1_H, C_SURFACE);
    textAt(F_BODY, BRI_LABEL[S.set.briIdx], CARD_IN_X1, top + CARD_L1_CY,
           MR_DATUM, C_TEXT2);
  }

  for (uint8_t b = 0; b < BRI_STEPS; b++) {
    const uint8_t vis = pressedNow(HIT_SETTING, SET_ROW_BRI, (int8_t)b)
                            ? BV_PRESSED
                            : (b == S.set.briIdx ? BV_ACTIVE : BV_INACTIVE);
    if (!first && vis == setSnap.briVis[b]) continue;
    setSnap.briVis[b] = vis;

    int16_t x, y, w, h;
    briRect(b, x, y, w, h);
    wChip(x, y, w, h, BRI_LABEL[b], nullptr, vis);
  }

  // Night mode's 3-chip row, same shape as the brightness loop above.
  for (uint8_t c = 0; c < NIGHT_CHIPS; c++) {
    const uint8_t vis = pressedNow(HIT_SETTING, SET_ROW_NIGHT, (int8_t)c)
                            ? BV_PRESSED
                            : (NIGHT_CHIP_MODE[c] == S.set.nightMode ? BV_ACTIVE
                                                                     : BV_INACTIVE);
    if (!first && vis == setSnap.nightVis[c]) continue;
    setSnap.nightVis[c] = vis;

    int16_t x, y, w, h;
    nightRect(c, x, y, w, h);
    wChip(x, y, w, h, NIGHT_LABEL[c], nullptr, vis);
  }

  // Toggles get no press flash: the flip IS the feedback, and it is immediate.
  for (uint8_t r = SET_ROW_NIGHT + 1; r < SET_ROWS; r++) {
    const bool on = setToggleVal(r);
    if (!first && on == setSnap.toggle[r]) continue;
    setSnap.toggle[r] = on;
    wToggle(TOGGLE_X, cardTop(r) + TOGGLE_DY, TOGGLE_W, TOGGLE_H, on);
  }

  setSnap.valid = true;
}

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

void screenInvalidate() {
  memset(snap,        0, sizeof(snap));
  memset(&sceneSnap,  0, sizeof(sceneSnap));
  memset(&setSnap,    0, sizeof(setSnap));
  memset(&statusSnap, 0, sizeof(statusSnap));
  shownPage = 0xFF;   // -> bodyReset() on the next render
}

// Wipes everything above the header and invalidates only the BODY snapshots.
// Deliberately leaves statusSnap alone — a page switch changes neither the
// connectivity glyph nor the clock, and repainting them would reintroduce the
// flicker the split regions exist to prevent.
//
// The wipe is mandatory, not defensive. Devices and Settings happen to
// self-clear (each card's first draw fills its whole row band, and the four
// bands tile the body), but Scenes does not: 88x64 tiles on a 100 x 72 pitch
// leave 12px and 8px gaps that would hold the previous page's pixels
// permanently. A SCROLL within Scenes is different and needs no wipe — see the
// square-fillRect note in drawSceneTile().
static void bodyReset() {
  tft.fillRect(0, 0, SCR_W, STATUS_Y0, C_BG);
  memset(snap,       0, sizeof(snap));
  memset(&sceneSnap, 0, sizeof(sceneSnap));
  memset(&setSnap,   0, sizeof(setSnap));
}

void screenRender() {
  if (shownPage != (uint8_t)S.page) {
    shownPage = (uint8_t)S.page;
    bodyReset();
  }

  drawStatus(false);
  switch (S.page) {
    case PAGE_SCENES:   drawScenes();   break;
    case PAGE_SETTINGS: drawSettings(); break;
    default:
      for (uint8_t i = 0; i < NUM_DEVICES; i++) drawDeviceCard(i, false);
      break;
  }
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
      int16_t x, y, w, h;
      btnRect(d, b, x, y, w, h);
      tft.drawRect(x, y, w, h, C_BORDER);
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
      // Gutter first — it owns everything from SCENE_SB_X0 right, and the grid
      // static_assert guarantees no tile reaches into it.
      if (px >= SCENE_SB_X0) {
        if (SCENE_MAX_ROW == 0) return miss;   // nothing to scroll: dead region
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
      if (idx >= SCENE_N) return miss;         // empty slot on the last page
      return { HIT_SCENE, (int16_t)idx, -1 };
    }

    case PAGE_SETTINGS: {
      const int r = (py - ROWS_Y0) / ROW_H;
      if (r < 0 || r >= SET_ROWS) return miss;
      if (r == SET_ROW_BRI) {
        for (uint8_t b = 0; b < BRI_STEPS; b++) {
          int16_t x, y, w, h;
          briRect(b, x, y, w, h);
          if (px >= x && px < x + w) return { HIT_SETTING, SET_ROW_BRI, (int8_t)b };
        }
        return miss;
      }
      if (r == SET_ROW_NIGHT) {
        for (uint8_t c = 0; c < NIGHT_CHIPS; c++) {
          int16_t x, y, w, h;
          nightRect(c, x, y, w, h);
          if (px >= x && px < x + w) return { HIT_SETTING, SET_ROW_NIGHT, (int8_t)c };
        }
        return miss;
      }
      // Toggle rows: the whole row is the target, at any x. The pill is an
      // affordance, not the hit area — a resistive-touch accommodation, and
      // consistent with the generous targets everywhere else here.
      return { HIT_SETTING, (int16_t)r, -1 };
    }

    default: {
      const int i = (py - ROWS_Y0) / ROW_H;
      if (i < 0 || i >= NUM_DEVICES) return miss;
      const bool ac = (S.dev[i].kind == DEV_CLIMATE);
      for (uint8_t b = 0; b < btnCount(S.dev[i]); b++) {
        // The AC setpoint cell is a readout, so a tap there is a deliberate
        // miss rather than a third action. That dead cell between the chevrons
        // is also what stops a slightly-off tap from stepping the wrong way.
        if (ac && b == AC_BTN_TEMP) continue;
        int16_t x, y, w, h;
        btnRect(i, b, x, y, w, h);
        // Vertically the whole row band counts as the control strip: this is a
        // bedroom device often used in the dark, so targets are 52px not 26px.
        // `i` already came from that band, so only px needs testing.
        if (px >= x && px < x + w) return { HIT_ROW, (int16_t)i, (int8_t)b };
      }
      return miss;
    }
  }
}
