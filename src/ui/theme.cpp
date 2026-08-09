#include "theme.h"
#include <string.h>

#define TH_DAY_(slot, day, night, shift)       day,
#define TH_OVR_(slot, day, night, shift)       night,
#define TH_SHIFT_OVR_(slot, day, night, shift) shift,
static const uint16_t THEME_DAY[TH_COUNT]       = { THEME_LIST(TH_DAY_) };
static const uint16_t THEME_OVR[TH_COUNT]       = { THEME_LIST(TH_OVR_) };
static const uint16_t THEME_SHIFT_OVR[TH_COUNT] = { THEME_LIST(TH_SHIFT_OVR_) };
uint16_t              THEME[TH_COUNT]           = { THEME_LIST(TH_DAY_) };
#undef TH_DAY_
#undef TH_OVR_
#undef TH_SHIFT_OVR_

// Mirrors state.h's NightMode by VALUE, not by #include, so this file stays
// the dependency-free leaf module the design-system table describes.
enum { TM_OFF = 0, TM_RED = 1, TM_SHIFT = 2 };
static uint8_t gMode = TM_OFF;

// Luminance pushed into the red channel only (green/blue zeroed), carried from
// ../btc-cyd-v2's ui.cpp. Keeps every element's relative contrast while making
// the panel emit nothing but red — the point being that red at 1% backlight
// does not wake anyone or wreck dark adaptation.
static uint16_t redOnly(uint16_t c) {
  uint32_t r8 = (((c >> 11) & 0x1F) * 255 + 15) / 31;
  uint32_t g8 = (((c >>  5) & 0x3F) * 255 + 31) / 63;
  uint32_t b8 = (( c        & 0x1F) * 255 + 15) / 31;
  uint32_t lum = (r8 * 299 + g8 * 587 + b8 * 114) / 1000;
  return (uint16_t)((lum & 0xF8) << 8);
}

// Night Shift: green/blue scaled DOWN rather than zeroed, so text and icons
// keep a hint of colour instead of collapsing to pure red. These two
// percentages are the whole knob for how strong the effect reads on the
// panel; the pinned semantic overrides in theme.h were computed at these
// exact values and would need recomputing (not hand-editing) if they change.
// Started at 60/35, then cut in two rounds of hardware feedback: blue alone
// to 15% (ACCENT still read as a saturated blue against an otherwise
// warm/amber screen), then both channels to 45/10 (the palette still carried
// too much green/blue overall, not just in one token).
#define NIGHT_SHIFT_GREEN_PCT 45
#define NIGHT_SHIFT_BLUE_PCT  10
static uint16_t warmShift(uint16_t c) {
  uint32_t r  = (c >> 11) & 0x1F;
  uint32_t g8 = (((c >> 5) & 0x3F) * 255 + 31) / 63;
  uint32_t b8 = (( c        & 0x1F) * 255 + 15) / 31;
  uint32_t g6 = ((g8 * NIGHT_SHIFT_GREEN_PCT / 100) & 0xFC) >> 2;
  uint32_t b5 = ((b8 * NIGHT_SHIFT_BLUE_PCT  / 100) & 0xF8) >> 3;
  return (uint16_t)((r << 11) | (g6 << 5) | b5);
}

uint16_t themeMap(uint16_t c) {
  return gMode == TM_RED ? redOnly(c)
       : gMode == TM_SHIFT ? warmShift(c) : c;
}
uint8_t themeNightMode() { return gMode; }

bool themeSetNightMode(uint8_t mode) {
  if (mode == gMode) return false;
  gMode = mode;

  // Built lazily rather than as a constant: redOnly()/warmShift() are not
  // constant expressions under gnu++11, and a runtime constructor here would
  // introduce a static-initialisation-order dependency for no gain.
  static uint16_t red[TH_COUNT], shift[TH_COUNT];
  static bool     builtRed = false, builtShift = false;
  if (!builtRed) {
    for (uint8_t i = 0; i < TH_COUNT; i++)
      red[i] = (THEME_OVR[i] == THEME_DERIVE) ? redOnly(THEME_DAY[i])
                                              : THEME_OVR[i];
    builtRed = true;
  }
  if (!builtShift) {
    for (uint8_t i = 0; i < TH_COUNT; i++)
      shift[i] = (THEME_SHIFT_OVR[i] == THEME_DERIVE) ? warmShift(THEME_DAY[i])
                                                       : THEME_SHIFT_OVR[i];
    builtShift = true;
  }

  memcpy(THEME, mode == TM_RED ? red : mode == TM_SHIFT ? shift : THEME_DAY,
         sizeof(THEME));
  return true;
}
