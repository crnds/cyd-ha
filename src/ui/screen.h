#pragma once
#include "state.h"

// `flip` comes from persisted settings, so the panel is only ever painted in
// the right orientation — passing it in (rather than defaulting to rotation 1
// and correcting later) is what keeps the rotation memo owned by one file.
void screenBegin(bool flip);

// Dirty-region render — safe (and intended) to call every loop pass. Each row
// compares against a last-drawn snapshot and returns early if unchanged. Also
// notices a page change and wipes the body before repainting it.
void screenRender();

// Force a full repaint on the next screenRender() (used after Wi-Fi portal exit).
// Deliberately does NOT fillScreen here: screenSplash() draws the logo and then
// calls this, so an immediate wipe would erase it. The wipe is deferred to the
// next screenRender().
void screenInvalidate();

// Palette / rotation switches. Both are no-ops when nothing changed, so they
// are safe to call every loop pass. They live here because `tft` is file-static
// in screen.cpp — main.cpp must never poke the panel directly.
// mode is 0=off / 1=red / 2=shift, mirroring state.h's NightMode.
void screenSetNightMode(uint8_t mode);
void screenSetFlip(bool flip);

// Maps a touch point to whatever control is under it on the current page.
// Returns { HIT_NONE, -1, -1 } for a miss. Vertically, a whole row band (or
// scene pitch) counts as its control, so a tap slightly high or low registers.
Hit screenHitTest(int16_t px, int16_t py);

// True when the live state of all three bulbs matches scene `idx`. Derived
// every render rather than latched, which is what makes overriding one bulb on
// the Devices page deselect the scene with no state to fall out of sync.
bool sceneActive(uint16_t idx);

// Scene name, for the tap log. Returns "?" out of range.
const char* sceneName(uint16_t idx);

// How many scenes SCENE[] defines. The table is the single source of truth for
// this — there is no NUM_SCENES to keep in step — so callers that bound-check an
// index must ask rather than assume.
uint16_t sceneCount();

// The largest scroll offset that still fills the screen (0 while every scene
// fits on one page, which is what makes the whole scroll affordance compile
// out — see SCENE_MAX_ROW's own comment). Exists so screenHitTest()'s scroll-
// gutter dead-region check can ask this rather than needing SCENE_MAX_ROW
// itself, which stays private to the file that owns SCENE[] — the table
// staying the only place the count lives is the same invariant sceneCount()
// exists for.
uint16_t sceneMaxRow();

// Moves the Scenes grid by `rows` (negative = up), clamped to the list. Returns
// true only when the offset actually changed; the renderer notices the new
// offset by itself, so a caller needs nothing but the tap.
bool sceneScrollBy(int16_t rows);

// The call plan for scene `idx`: which bulbs it turns off, which it turns on,
// and the single level/colour the ON set shares. Every scene's ON set is
// homogeneous, which is exactly what lets a scene collapse into one
// light.turn_on — so the plan is (mask, pct, kelvin), not a per-bulb list.
// kelvin is 0 when the scene does not set one.
void scenePlan(uint16_t idx, uint8_t& offMask, uint8_t& onMask,
               int& pct, int& kelvin);

// Boot splash: the Home Assistant logo, centred, over three progress pips. No
// text.
//
// Used for BOTH boot states — connecting, and the Wi-Fi captive portal — so the
// two are visually indistinguishable. That is intentional, but note the cost:
// when no saved credentials work, the device sits on a bare logo with nothing
// on screen naming the AP_PORTAL_NAME hotspot to join. Recovery then depends on
// the README or the serial log. Deliberate trade for a clean splash.
void screenSplash();

// Advances the splash's progress pips; call it from a blocking wait loop. A
// still logo for the length of WIFI_CONNECT_MS is indistinguishable from a hung
// board, and "is it working?" is the one question a boot screen has to answer.
// Wordless, so it does not undo the choice above.
void screenSplashProgress(uint8_t phase);

// Calibration mode (env:calib) — draws a crosshair target at (x,y) with
// progress text. Not compiled into the normal firmware's flow, but harmless.
void screenCalibTarget(int idx, int total, int16_t x, int16_t y);

// Verify pass: draws the real button grid as outlines, then plots each mapped
// tap as a dot so accuracy can be judged against the actual targets.
void screenCalibVerifyScreen();
void screenCalibDot(int16_t x, int16_t y);
