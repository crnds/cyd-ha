#pragma once

// Single tuning surface for the whole firmware: pin map, touch calibration,
// layout geometry, poll cadences, and the light/AC presets the buttons send.
// Host, token and entity IDs live in secrets.h (gitignored).

// ── Wi-Fi ────────────────────────────────────────────────
#define AP_PORTAL_NAME     "CYD-HA-Setup"
#define WIFI_CONNECT_MS    20000UL   // give up and open the portal after this

// ── Cadences / retry ─────────────────────────────────────
// One entity polled per tick, round-robin over 4 devices, so each device
// refreshes every ~4 * HA_POLL_MS = 6s. That is the worst-case latency for a
// change made elsewhere (phone app, automation) showing up here.
#define HA_POLL_MS         1500UL

// Every HA call is blocking and runs inside loop(), so a timeout is also a
// UI-freeze budget: while one is outstanding, touch and rendering stop dead.
// Measured LAN round trip is 16-64 ms, so these are ~20x headroom. The old
// single 4000 ms value applied to BOTH connect and read, meaning an
// unreachable HA froze the screen for up to 8 s per tap.
#define HTTP_CONNECT_MS    1200
#define HTTP_READ_MS       1500

// Circuit breaker: after a failed call, don't let further taps each pay another
// timeout. Fail fast (flash the row red) until this window expires, then allow
// one probe through.
#define HA_BREAKER_MS      2500UL

#define RETRY_BASE_MS      1000UL
#define RETRY_MAX_MS       60000UL

// After a tap we optimistically repaint, fire the service call, then re-poll
// that one entity to reconcile. IKEA Zigbee round-trips run 1-2s, so waiting
// less than this usually reads back the OLD state and makes the UI flicker.
#define RECONCILE_MS       900UL

// A device with no successful poll for this long renders dimmed rather than
// blanking — never destroy a known-good value on a failed fetch.
#define DEVICE_STALE_MS    20000UL

// touch poll cadence: the 3-sample bit-bang read costs ~1.5 ms, and 20 Hz is
// still far faster than any finger tap — no need to pay it every loop pass
#define TOUCH_POLL_MS      50UL
#define PRESS_FLASH_MS     120UL     // invert the tapped button this long, for tactility

// ── CYD pins (ESP32-2432S028R) ───────────────────────────
#define PIN_BACKLIGHT  21
#define PIN_LDR        34   // photoresistor, analog (unused in v1)
// XPT2046 sits on its own VSPI bus (not the TFT HSPI) — classic single-USB CYD
#define PIN_TOUCH_IRQ  36   // PENIRQ, LOW while touched
#define PIN_TOUCH_CS   33
#define PIN_TOUCH_CLK  25
#define PIN_TOUCH_MOSI 32
#define PIN_TOUCH_MISO 39
#define PIN_LED_R      4    // RGB LED, active LOW
#define PIN_LED_G      16
#define PIN_LED_B      17

// ── Touch calibration (raw ADC -> 320x240 landscape) ─────
// Measured on THIS unit with `pio run -e calib -t upload` (4-crosshair fit,
// then verified by tapping all 23 buttons: every tap landed on the right one,
// residuals <0.6px). Re-run that env if taps ever drift.
//
// NOTE these ranges are almost identical to the values inherited from
// ../btcticker-cyd (200/3700/240/3800) — the ranges were never the problem.
// TOUCH_SWAP_XY below is what was actually wrong. Don't "simplify" this back.
#define TOUCH_X_MIN    208
#define TOUCH_X_MAX    3717
#define TOUCH_Y_MIN    307
#define TOUCH_Y_MAX    3890
// Pressure gate. btcticker-cyd used 95 with the note "idle noise sits ~50-80",
// but measured on THIS firmware the idle band reaches z=127 while real contact
// reads z=2143-2458 — a threshold of 95 sat inside the noise and fired phantom
// taps that actually drove the bulbs. There is a ~2000-wide dead zone between
// noise and contact, so 400 is comfortably clear of both.
#define TOUCH_Z_MIN    400
#define TOUCH_TAP_MS   350   // debounce between accepted taps

// Require PENIRQ to agree with the pressure reading before accepting a tap.
// btcticker-cyd deliberately ignores this pin ("unreliable on many CYD boards
// — stays high"), but on this unit it is correct and decisive: every noise
// sample read irq=0 (not touched) while every real contact read irq=1. Two
// independent signals is what makes a stray tap unable to command a light.
// Set to 0 if a future board's PENIRQ sticks high and no touch ever registers.
#define TOUCH_REQUIRE_IRQ 1

// THE fix for "taps misfire everywhere". readTouch() assigns bestSx from the
// XPT2046's Y-command (0xD1) samples and bestSy from the X-command (0x91)
// samples, which is correct for btcticker-cyd's orientation but inverted for
// this one — screen X is actually driven by the 0x91 channel here. Without the
// swap, every tap collapsed into the left third of the screen (raw 700-1941 of
// a 208-3717 range), so only buttons 0-2 were ever reachable and the AC row
// was dead. Verified: all 23 buttons hit correctly with this set to 1.
#define TOUCH_SWAP_XY  1
#define TOUCH_INVERT_X 0
#define TOUCH_INVERT_Y 0

// ── Backlight ────────────────────────────────────────────
#define BL_CHANNEL     0
#define BL_DUTY        230   // fixed brightness, ~90% of 255

// ── Layout (320x240 landscape) ───────────────────────────
#define SCR_W          320
#define SCR_H          240
#define STATUS_H       18                 // status bar occupies y 0..17
#define ROWS_Y0        20                 // first device row top
#define ROW_H          55                 // 4 * 55 = 220; 20 + 220 = 240 exactly
#define ROW_LABEL_DY   1                  // name/state line offset within row
#define ROW_BTN_DY     15                 // button strip offset within row
#define BTN_H          36                 // >= 30px comfortable finger target

// Bulb rows: 6 buttons. 6 + 5*52 = 266, + 48 = 314 (6px right margin).
#define BTN_X0         6
#define BTN_W          48
#define BTN_GAP        4
#define BTN_PITCH      (BTN_W + BTN_GAP)  // 52

// AC row: 5 buttons. 6 + 4*62 = 254, + 58 = 312 (8px right margin).
#define AC_BTN_W       58
#define AC_BTN_PITCH   (AC_BTN_W + BTN_GAP)  // 62

#define NUM_DEVICES    4
#define BULB_BTNS      6                  // OFF, 1%, 30%, 100%, warm, cool
#define AC_BTNS        5                  // OFF, AC, DRY, T+, T-

// ── Light presets ────────────────────────────────────────
// The three brightness buttons and the two colour-temp swatches.
#define BRI_LOW        1
#define BRI_MID        30
#define BRI_HIGH       100

// IKEA TRADFRI "white spectrum" (WS) bulbs are warm-to-cool white only, with
// no RGB — so the requested Orange/Blue buttons map onto the ends of the
// bulb's colour-temperature range instead. 2202K reads amber, 4000K reads a
// cold blue-white.
// These are the exact limits all three bulbs report as
// min_color_temp_kelvin / max_color_temp_kelvin — verified against the live
// entities, not guessed. 2202 (not 2200) is what HA derives from the bulb's
// 454 mired warm limit; sending 2200 would be clamped anyway, but matching
// exactly keeps the "warm swatch is active" compare honest.
#define KELVIN_WARM    2202
#define KELVIN_COOL    4000

// Active-highlight tolerances. HA rounds brightness_pct to a 0-255 byte and
// back, so an exact compare against 1/30/100 would almost never match.
#define PCT_LOW_MAX    5
#define PCT_MID_MIN    25
#define PCT_MID_MAX    35
#define PCT_HIGH_MIN   95
#define KELVIN_WARM_MAX 2500
#define KELVIN_COOL_MIN 3700

// ── Climate (Sensibo Sky) ────────────────────────────────
// Fallbacks only — real values come from the entity's own attributes
// (min_temp / max_temp / target_temp_step) once the first poll lands. Set to
// what climate.bedroom2 actually reports so the pre-first-poll window matches
// the device instead of letting T+/T- clamp somewhere it wouldn't accept.
#define AC_TEMP_MIN_DEF   18.0f
#define AC_TEMP_MAX_DEF   31.0f
#define AC_TEMP_STEP_DEF  1.0f
#define AC_MODE_COOL      "cool"
#define AC_MODE_DRY       "dry"
