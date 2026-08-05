#pragma once
#include <stdint.h>

// Palette carried from ../btcticker-cyd/src/ui/theme.h so both devices in the
// house read as one family. Values chosen to stay distinct after RGB565
// quantization (5/6/5 bits).

// RGB888 -> RGB565, compile-time
#define RGB565(r, g, b) \
  (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

#define C_BG      RGB565(0x07, 0x09, 0x0D)  // ground
#define C_SURFACE RGB565(0x15, 0x1B, 0x26)  // inactive button fill
#define C_BORDER  RGB565(0x27, 0x30, 0x3F)  // hairlines / row separators
#define C_TEXT    RGB565(0xF2, 0xF5, 0xFA)  // active button label, device names
#define C_TEXT2   RGB565(0x9A, 0xA7, 0xBD)  // inactive button label, state text
#define C_MUTED   RGB565(0x5C, 0x69, 0x80)  // micro-caps, disabled buttons
#define C_DIM     RGB565(0x6E, 0x7A, 0x8F)  // stale (poll-failed) values
#define C_GREEN   RGB565(0x00, 0xE1, 0x7B)  // online dot
#define C_RED     RGB565(0xFF, 0x3B, 0x5F)  // offline dot, failed service call
#define C_ACCENT  RGB565(0x18, 0xBC, 0xF2)  // active fill — Home Assistant cyan

// Colour-temperature swatches. These are what KELVIN_WARM / KELVIN_COOL look
// like to the eye, not a physical blackbody conversion — the point is that the
// two buttons are instantly distinguishable across a dark bedroom.
#define C_WARM    RGB565(0xFF, 0xA5, 0x3C)  // ~2200K amber
#define C_COOL    RGB565(0x9C, 0xC8, 0xFF)  // ~4000K cold white-blue

// per-channel blend a -> b, t = 0..255. RGB565 has no alpha, so tinted fills
// are precomputed blends against the background.
inline uint16_t lerp565(uint16_t a, uint16_t b, uint8_t t) {
  int32_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int32_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int32_t r  = ar + ((br - ar) * t) / 255;
  int32_t g  = ag + ((bg - ag) * t) / 255;
  int32_t bl = ab + ((bb - ab) * t) / 255;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// dark tint of an accent (~22% toward it from bg) — inactive swatch fills
inline uint16_t tint565(uint16_t c) { return lerp565(C_BG, c, 56); }
