#include "theme.h"
#include <string.h>

#define TH_DAY_(slot, day, night) day,
#define TH_OVR_(slot, day, night) night,
static const uint16_t THEME_DAY[TH_COUNT] = { THEME_LIST(TH_DAY_) };
static const uint16_t THEME_OVR[TH_COUNT] = { THEME_LIST(TH_OVR_) };
uint16_t              THEME[TH_COUNT]     = { THEME_LIST(TH_DAY_) };
#undef TH_DAY_
#undef TH_OVR_

static bool gNight = false;

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

uint16_t themeMap(uint16_t c) { return gNight ? redOnly(c) : c; }
bool     themeIsNight()       { return gNight; }

bool themeSetNight(bool on) {
  if (on == gNight) return false;
  gNight = on;

  // Built lazily rather than as a constant: redOnly() is not a constant
  // expression under gnu++11, and a runtime constructor here would introduce a
  // static-initialisation-order dependency for no gain.
  static uint16_t night[TH_COUNT];
  static bool     built = false;
  if (!built) {
    for (uint8_t i = 0; i < TH_COUNT; i++)
      night[i] = (THEME_OVR[i] == THEME_DERIVE) ? redOnly(THEME_DAY[i])
                                                : THEME_OVR[i];
    built = true;
  }

  memcpy(THEME, on ? night : THEME_DAY, sizeof(THEME));
  return true;
}
