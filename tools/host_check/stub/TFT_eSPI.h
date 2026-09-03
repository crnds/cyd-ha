#pragma once
// Host-side stand-in for TFT_eSPI. Every method is a no-op EXCEPT that it
// increments hostDrawCalls — the harness's only way to observe "did this
// function paint anything", since none of these calls have an observable
// return value. That counter is what lets the driver assert a dirty-region
// early-out actually fired (delta 0 on an unchanged repaint) without needing
// real pixels.
//
// textWidth() always returns 0. That makes every textFit()/textTrunc() call
// take its "fits" branch regardless of string length — not accurate to real
// glyph metrics (simulator.html already owns that), but deterministic, which
// is all an equivalence harness needs: the same input produces the same
// output before and after a refactor step, and that's the property being
// verified here, not pixel-perfect fidelity to hardware.
#include <cstdint>

extern long hostDrawCalls;

#define MC_DATUM 4
#define ML_DATUM 3
#define MR_DATUM 5
#define TL_DATUM 0
#define TC_DATUM 1
#define TR_DATUM 2

class TFT_eSPI {
 public:
  void init() { hostDrawCalls++; }
  void setRotation(int) { hostDrawCalls++; }
  void fillScreen(uint16_t) { hostDrawCalls++; }
  void fillRect(int32_t, int32_t, int32_t, int32_t, uint16_t) { hostDrawCalls++; }
  void drawRect(int32_t, int32_t, int32_t, int32_t, uint16_t) { hostDrawCalls++; }
  void drawFastHLine(int32_t, int32_t, int32_t, uint16_t) { hostDrawCalls++; }
  void drawFastVLine(int32_t, int32_t, int32_t, uint16_t) { hostDrawCalls++; }
  void drawLine(int32_t, int32_t, int32_t, int32_t, uint16_t) { hostDrawCalls++; }
  void drawPixel(int32_t, int32_t, uint16_t) { hostDrawCalls++; }
  void drawCircle(int32_t, int32_t, int32_t, uint16_t) { hostDrawCalls++; }
  void fillCircle(int32_t, int32_t, int32_t, uint16_t) { hostDrawCalls++; }
  void drawCircleHelper(int32_t, int32_t, int32_t, uint8_t, uint16_t) { hostDrawCalls++; }
  void fillTriangle(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint16_t) { hostDrawCalls++; }
  void pushImage(int32_t, int32_t, int32_t, int32_t, const uint16_t*) { hostDrawCalls++; }
  void setTextFont(int) { hostDrawCalls++; }
  void setTextColor(uint16_t) { hostDrawCalls++; }
  void setTextDatum(uint8_t) { hostDrawCalls++; }
  void drawString(const char*, int32_t, int32_t) { hostDrawCalls++; }
  int16_t textWidth(const char*) { return 0; }
};
