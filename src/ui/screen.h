#pragma once
#include "state.h"

void screenBegin();

// Dirty-region render — safe (and intended) to call every loop pass. Each row
// compares against a last-drawn snapshot and returns early if unchanged.
void screenRender();

// Force a full repaint on the next screenRender() (used after Wi-Fi portal exit).
void screenInvalidate();

// Maps a touch point to a (device row, button) pair. Returns false if the tap
// landed outside any button. The whole 55px row band is treated as the button
// strip vertically, so a tap slightly high or low still registers.
bool screenHitTest(int16_t px, int16_t py, int8_t& devIdx, int8_t& btnIdx);

// Boot splash: the Home Assistant logo with captions beneath, used for the
// connecting and Wi-Fi-portal states. `line2` may be null.
void screenSplash(const char* line1, const char* line2 = nullptr);

// Calibration mode (env:calib) — draws a crosshair target at (x,y) with
// progress text. Not compiled into the normal firmware's flow, but harmless.
void screenCalibTarget(int idx, int total, int16_t x, int16_t y);

// Verify pass: draws the real button grid as outlines, then plots each mapped
// tap as a dot so accuracy can be judged against the actual targets.
void screenCalibVerifyScreen();
void screenCalibDot(int16_t x, int16_t y);
