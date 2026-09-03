#include "screen_int.h"
#include <math.h>
#include <string.h>

// The Devices page: 4 inline cards, one per entity, 23 controls total. See
// screen_int.h for the cross-file snapshot/geometry contract this plugs into,
// and CLAUDE.md's "Two card internals" / "The device card's icon" sections for
// the design rationale behind what's here.

// Chip captions are UPPERCASE and card titles are not, and that is a fit
// decision rather than a stylistic one: caps have no descenders, so a 13px label
// centres cleanly in a 26px chip, while a lowercase 'y' would touch its edge.
static const char* const BULB_LABEL[BULB_BTNS]     = {"OFF", "1%", "30%", "100%", nullptr, nullptr};
static const char* const BULB_LABEL_ALT[BULB_BTNS] = {"OFF", "1",  "30",  "100",  nullptr, nullptr};
// AC slots 3..5 are the setpoint stepper — two chevrons around the value — so
// they carry no label. See btnRect() for how those three slots are placed.
static const char* const AC_LABEL[AC_BTNS]         = {"OFF", "COOL", "DRY", nullptr, nullptr, nullptr};

// Icon shapes, for the RowSnap compare.
enum IcoShape : uint8_t { IS_BULB_OFF = 0, IS_BULB_ON, IS_SNOW, IS_DROP, IS_POWER };

RowSnap snap[NUM_DEVICES];

// Where slot `b` of device row `dev` sits. The two kinds lay out the same six
// slots differently IN X, and this is the ONLY function that knows that — every
// caller (renderer, hit test, calibration verify) goes through here, so the
// drawn rect and the tappable rect cannot drift apart.
//
// Bulb:  (i) 1 [OFF][1%][30%][100%] (o)(o)     slots 0..3 chips, 4..5 swatches
// AC:    (i) AC [OFF][COOL][DRY]  [v] 30 [^]    slots 0..2 chips, 3/4/5 stepper
//
// Slot order here is DOWN-left/UP-right, on request — down is slot 5 (left),
// up is slot 3 (right). The slot numbers themselves are fixed by doAction(),
// so mapping them here is what buys whichever order is wanted without
// touching the action layer.
Rect btnRect(uint8_t dev, uint8_t b) {
  int16_t x, y, w, h;
  y = cardTop(dev) + BULB_CTL_DY;
  h = BULB_CTL_H;

  if (S.dev[dev].kind == DEV_CLIMATE) {
    if (b < 3) { x = BULB_CTL_X0 + b * ACM_PITCH; w = ACM_W; return {x, y, w, h}; }
    switch (b) {
      case AC_BTN_TDN:  x = ACS_X0;                         w = ACS_BTN_W; return {x, y, w, h};
      case AC_BTN_TEMP: x = ACS_X0 + ACS_BTN_W;             w = ACS_VAL_W; return {x, y, w, h};
      default:          x = ACS_X0 + ACS_BTN_W + ACS_VAL_W; w = ACS_BTN_W; return {x, y, w, h};
    }
  }

  if (b < 4) { x = BULB_CTL_X0 + b * BULB_CHIP_PITCH; w = BULB_CHIP_W; return {x, y, w, h}; }
  x = SW_X0 + (b - 4) * SW_CELL_W;
  w = SW_CELL_W;
  return {x, y, w, h};
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
    // a snowflake next to a mode this page cannot name is simply wrong.
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

// The AC setpoint as drawn between the chevrons. "--" rather than a guessed
// number until a real setpoint is known — the same rule that makes a step tap a
// no-op until then (doAction()), so the readout and the control agree.
static void tempText(const DeviceState& d, char* out, size_t n) {
  if (!d.known || !d.avail || isnan(d.target)) { snprintf(out, n, "--"); return; }
  snprintf(out, n, "%.0f", d.target);
}

void drawDeviceCard(uint8_t dev, bool force) {
  DeviceState& d   = S.dev[dev];
  const uint32_t now = millis();

  bool stale, err;
  staleErr(d, now, stale, err);

  // Whichever sub-index is currently recorded as pressed, if it's this row's
  // press and still inside its flash window — pressedNow() already knows
  // that whole rule; asking it about S.pressSub (trivially matching itself)
  // is the same result as the timing formula this used to re-implement here.
  const int8_t press = pressedNow(HIT_ROW, (int16_t)dev, S.pressSub) ? S.pressSub : -1;

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

  // Raw-input pre-filter. Everything below is read somewhere in this
  // function's drawing decisions, so if none of it moved since last pass, no
  // region's appearance can have changed either — every compare further down
  // would no-op anyway, at the cost of an iconVis()/bulbHue() call and the
  // button loop's btnActive() calls first. See CardFingerprint in screen_int.h.
  CardFingerprint fp;
  memset(&fp, 0, sizeof(fp));
  fp.on = d.on; fp.avail = d.avail; fp.known = d.known; fp.supportsCT = d.supportsCT;
  fp.pct = d.pct; fp.kelvin = d.kelvin; fp.target = d.target;
  memcpy(fp.mode, d.mode, sizeof(fp.mode));
  fp.stale = stale; fp.err = err; fp.alarm = alarm; fp.press = press;
  if (!first && memcmp(&fp, &sn.fp, sizeof(fp)) == 0) return;
  sn.fp = fp;

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
    // once OFFLINE (or, for the AC, an exceptional mode's name) is gone.
    const uint16_t nameFg = alarmFg(alarm, stale, C_TEXT);
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
    // than the accent. A bulb's 1%/30%/100% chip (not the AC's mode chips, not
    // a swatch) gets its own yellow vis on request — see BV_ACTIVE_BRI in
    // widgets.h.
    else if (btnActive(d, b)) {
      vis = (b == 0)               ? BV_ACTIVE_OFF
          : (!ac && !isSwatch)     ? BV_ACTIVE_BRI
                                    : BV_ACTIVE;
    }

    if (!first && vis == sn.btnVis[b]) continue;
    sn.btnVis[b] = vis;

    const Rect r = btnRect(dev, b);
    const int16_t x = r.x, y = r.y, w = r.w, h = r.h;

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
      const Rect r = btnRect(dev, AC_BTN_TEMP);
      const int16_t x = r.x, y = r.y, w = r.w, h = r.h;
      const bool known = (tv[0] != '-');
      // Greyed the same as a disabled chevron beside it (C_DISABLED) when the
      // AC is off — the setpoint can't be stepped, so it shouldn't read as
      // live text.
      // Not alarmFg(): this ladder has a fourth rung (acOff) sitting between
      // the alarm and stale checks, so the two-branch helper doesn't fit
      // without contorting it — `alarm` is still reused here rather than
      // re-deriving err || !d.avail a second time in the same function.
      wValue(x, y, w, h, tv, known,
             alarm ? C_ERROR
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
