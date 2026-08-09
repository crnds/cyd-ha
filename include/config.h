#pragma once

// Single tuning surface for the whole firmware: pin map, touch calibration,
// layout geometry, poll cadences, and the light/AC presets the buttons send.
// Host, token and entity IDs live in secrets.h (gitignored).

// ── Wi-Fi ────────────────────────────────────────────────
#define AP_PORTAL_NAME     "CYD-HA-Setup"
#define WIFI_CONNECT_MS    20000UL   // give up and open the portal after this

// ── Clock ────────────────────────────────────────────────
// NTP rather than an HTTP time API: keyless, works anywhere, and the SNTP client
// in the ESP32 core keeps itself resynced with no code from us.
#define NTP_SERVER_1       "pool.ntp.org"
#define NTP_SERVER_2       "time.google.com"
// POSIX TZ string, and note the sign is INVERTED from what you would expect:
// "ICT-7" means UTC+7, i.e. Asia/Bangkok. Bangkok has no DST, so no rule half.
// Lookup table: https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
#define TZ_INFO            "ICT-7"

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
// Poll reads are a local template render — measured 14 ms, so this is huge.
#define HTTP_READ_MS       1500
// Service calls are a different workload: light.* is local Zigbee and fast, but
// climate.* goes out to the Sensibo cloud. Measured REAL changes at 1135-1643 ms
// (a no-op write short-circuits in 24 ms, which is what misled the first tuning
// pass into 1500 and made every genuine setpoint step report a read timeout).
//
// 1500 rather than the 2500 that comment justifies: haPostService() already
// treats a read timeout as delivered (the reconcile poll establishes truth
// either way), so the extra 1000 ms bought only a cleaner log line at the cost
// of a longer frozen screen. Still comfortably above the 1135-1643 ms measured
// real-change range, so a genuine climate call still reads back clean far more
// often than it times out.
#define HTTP_READ_SVC_MS   1500

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
// Brightness is a setting now (Settings page, persisted in NVS) rather than a
// fixed #define. Duty and label share one index (Settings::briIdx), so they are
// declared together — splitting them across translation units is how they
// drift. 1 / 25 / 50 / 75 / 100 % of 255.
#define BRI_STEPS      5
#define BRI_DUTY_LIST  { 3, 64, 128, 191, 255 }
#define BRI_LABEL_LIST { "1%", "25%", "50%", "75%", "100%" }
// Default index. 128 is the nearest step to the old fixed BL_DUTY of 120
// (~47%) — chosen for a bedroom, where a 90% panel is glaring at night.
#define BRI_DEFAULT    2
// Night mode forces this step regardless of briIdx, then restores the user's
// choice when night ends.
#define BRI_NIGHT      0

// ── Night mode ───────────────────────────────────────────
// The schedule WRITES the Night mode toggle at these boundaries rather than
// overriding it: between them a manual toggle always wins and sticks until the
// next edge. Minutes since local midnight.
#define NIGHT_ON_MIN   (23 * 60 + 45)     // 23:45
#define NIGHT_OFF_MIN  (8 * 60)           // 08:00

// A daily restart at a quiet hour, inside the night window so it is never
// visible: the one multi-minute heap sample taken during development drifted
// -400 B, and hours-long soaks are unproven, so this makes slow drift a
// non-issue for a 24/7 appliance rather than something that has to be proven
// absent. 05:30 rather than midnight: comfortably inside NIGHT_ON_MIN..
// NIGHT_OFF_MIN so a reboot mid-restart never straddles the night-mode edge,
// and far from either boundary so the two scheduled events can't land on the
// same minute.
#define RESTART_MIN    (5 * 60 + 30)      // 05:30

// ── Persistence ──────────────────────────────────────────
// NVS namespace for the Settings page. Shares the 20 KB nvs partition with
// WiFiManager's own credential store (a separate namespace).
#define NVS_NAMESPACE  "cydha"

// ── Layout (320x240 landscape) ───────────────────────────
// GEOMETRY TOKENS — the other half of the design system. Colour and type live
// in src/ui/theme.h and src/ui/gfx.h; every margin, gap, size and radius lives
// here, and simulator.html mirrors this block. Nothing in screen.cpp computes a
// position from a literal: change the #defines together, not the arithmetic.
#define SCR_W          320
#define SCR_H          240

// SPACING SCALE. Every gap and pad in the UI is one of these five values, which
// is what makes the layout read as deliberate rather than nudged. Anything that
// needs a sixth value is a sign the layout is wrong, not that the scale is.
#define SP_1           4
#define SP_2           8
#define SP_3           12
#define SP_4           16
#define SP_6           24

// CORNERS ARE SQUARE, EVERYWHERE. There is deliberately no radius token: cards,
// scene tiles, chips, steppers and the settings toggle are all plain rects, so
// every surface and every control on it share one corner treatment and nothing
// has to decide which of two radii it belongs to. This replaced an R_SM 6 /
// R_LG 8 / pill h/2 set — don't reintroduce one for a single component, since a
// lone rounded control on a square page reads as a rendering bug rather than a
// style. Two consequences worth knowing, both of which USED to need care and no
// longer do: a partial repaint can no longer strand a corner arc (fillRoundRect
// left the four corner pixels of its bounding box untouched, so blanking had to
// be square on purpose), and a clear that reaches a card's edge column now
// erases a border PIXEL rather than an arc — still wrong, still guarded by
// starting every in-card dirty rect at CARD_IN_X0, just for a simpler reason.

// ── header (y 0..31) ─────────────────────────────────────
// 32px, up from 22. The tab targets grew 24 -> 32px with it, which matters
// because the 4-point touch fit EXTRAPOLATES above y=30 (CAL_INSET is 30) and
// resistive panels are worst near the bezel: this is still the least accurate
// band on the panel, so it gets the most height per target of anything here.
//
// Three regions that TILE THE BAR EXACTLY (26 + 3*81 + 51 = 320). A gap leaves
// pixels nothing ever clears; an overlap is just as bad, since each region only
// clears its own rect, so whatever spills over is never repainted.
#define STATUS_H       32
#define STATUS_ICO_W   26                 // x   0..25  — one connectivity glyph
#define TAB_X0         26
#define TAB_W          81                 // x  26..268 — TAB_COUNT * TAB_W
#define TAB_COUNT      3                  // static_assert'd against PAGE_COUNT
#define STATUS_CLK_W   51                 // x 269..319 — 24h clock
// Content sits above the divider, so every header clear is STATUS_DIV_Y tall and
// the 1px rule at the bottom survives them all and is painted once.
#define STATUS_DIV_Y   (STATUS_H - 1)     // 31
#define STATUS_CY      15                 // header content centre line
#define STATUS_ICO_CX  13
// Tab indicator: a 2px underline SEATED ON the header rule, not a pill. The
// filled pill made the three tabs the most button-like things on the panel,
// competing with the 23 controls in the body that actually are buttons — and it
// used the same solid-accent fill that means "selected" on a chip, so the header
// read as a fourth row of controls. An underline says "you are here" and nothing
// about being pressable, which is what the header is for.
//
// TAB_UL_Y + TAB_UL_H lands exactly on STATUS_DIV_Y, so the bar stacks directly
// on the 1px rule and the two read as one line: a 1px gap between them would
// look like a misprint. It must also stay at or above STATUS_DIV_Y because every
// header clear is exactly that tall — the rule below is painted once and must
// survive, while the indicator has to be inside a rect that gets cleared, or a
// tab that stops being current would keep its bar forever.
//
// 2px and not 3, though the margin is no longer what set it: the label centres
// on STATUS_CY in F_BODY, and Font 2 descends only 7px below that centre where
// FreeSans went 11, so "Settings"' g now reaches y 22 against a bar at y 29 —
// 7px of air rather than the 1px that originally forced 2 over 3. It stays 2
// because it is an underline and not a rule; simulator.html asserts the
// clearance rather than trusting either number in this comment.
//
// The label budget is the whole cell less SP_1 a side (73px), and the widest
// label ("Settings") is 49px in Font 2, down from 65px in FreeSans. Nothing here
// is remotely close to a fit failure — but TAB_W stays 81, since it is fixed by
// the header tiling above, not by the widest word.
#define TAB_UL_H       2
#define TAB_UL_Y       (STATUS_DIV_Y - TAB_UL_H)     // 29..30
#define TAB_UL_PAD     2                             // bleed each side of label
#define TAB_LBL_DX     SP_1
#define TAB_TAP_H      STATUS_H

// ── body (y 32..239) ─────────────────────────────────────
// 4 row bands of 52 tile the body EXACTLY: 32 + 4*52 = 240. Devices and
// Settings both use this grid via rowTop(); a leftover sliver at the bottom is
// the failure this arithmetic exists to prevent.
#define ROWS_Y0        32
#define ROW_H          52

// Each band holds one CARD inset 2px top and bottom, which is what produces
// the uniform 4px gutter between cards (2 + 2) and 2px against the header and
// the bottom edge. The inset used to be 3: the pixels that bought went into
// the control row, so the drawn control is nearer the full 52px band the hit
// test has always accepted. Grouping the row's contents into a surface —
// instead of separating them with a hairline — is the single biggest reason
// the page reads as calm: a card says "these things belong together" without
// drawing a line.
#define CARD_DY        2
#define CARD_H         (ROW_H - 2 * CARD_DY)         // 48
#define CARD_X         SP_2                          // 8
#define CARD_W         (SCR_W - 2 * SP_2)            // 304 -> x 8..311
#define CARD_PAD       SP_2                          // inner padding
// X1 is the LAST content pixel, not one past it, so a right-aligned datum can
// use it directly. Width is derived from the padding rather than from X1 - X0 to
// keep the off-by-one in one place.
#define CARD_IN_X0     (CARD_X + CARD_PAD)               // 16  — first content px
#define CARD_IN_X1     (CARD_X + CARD_W - CARD_PAD - 1)  // 303 — last content px
#define CARD_IN_W      (CARD_W - 2 * CARD_PAD)           // 288

// STACKED card internals, as offsets from the card top: line 1 is identity +
// live state, line 2 is the controls — 1 pad + 18 line1 + 1 gap + 26 controls +
// 2 pad = 48. This is now the AC card and the Settings brightness card only;
// the three bulb cards went inline and use the BULB_* block below instead.
// CARD_L1_CY is 8, which under FreeSans was the largest value that kept a
// lowercase name's descenders out of the control row. Font 2's ink spans only
// cy-5..cy+7, so those tails now stop at y+15 with 5px to spare — the constraint
// that PICKED 8 has gone slack, but 8 is also what centres the shorter face in
// the 18px line, so it stays. A name is user data from secrets.h and cannot be
// assumed to be the all-caps it happens to be today.
#define CARD_L1_CY     8                  // identity / state line, MC datum
// The dirty rect for that line. It runs to y+19, comfortably past the ink,
// because it MUST cover descenders: clear only the cap box and renaming
// "Reading lamp" to "Lamp" leaves the g's tail on the card forever. It must also
// start at or above the ink's top row (y+3 here) — a shorter face rides higher
// in the line, which is the failure a taller one could not have. Both bounds are
// asserted in simulator.html.
// It also starts at CARD_IN_X0 rather than CARD_X, which keeps every clear clear
// of the card's own border columns; filling x 8..15 with the background would
// eat the left edge of the outline one repaint at a time.
#define CARD_L1_Y      1
#define CARD_L1_H      19                 // y+1..y+19, ending just above CTL_DY
#define CARD_ICO_CX    (CARD_X + 15)      // 23 — status icon centre
#define CARD_ICO_R     7                  // 14px optical icon box
#define CARD_TXT_X     (CARD_X + 28)      // 36 — text starts clear of the icon
#define CTL_DY         20                 // control row top
// 26px, up from 22: the card inset and pads paid for it, so the drawn control
// is nearer the 52px row band screenHitTest() has always accepted vertically.
#define CTL_H          26                 // control row height

// CHIP GRID — 5 across the Settings brightness card. 5*54 + 4*4 = 286 (x 16..301).
// This USED to be shared with the device rows, so a control on either page was
// the same size. The bulb rows went inline (below) and now carry their own
// narrower chip, so the two pages no longer match: a bulb chip is 43x40 against
// this one's 54x26. That is a real cost of the inline layout, not an oversight —
// the AC row's ACM_W 60 had already made "one shared pitch" approximate.
#define CHIP_W         54
#define CHIP_GAP       SP_1
#define CHIP_PITCH     (CHIP_W + CHIP_GAP)           // 58

// ── bulb card: ONE INLINE ROW ────────────────────────────
// [icon name] [OFF][1%][30%][100%] (o)(o)  — identity and controls on the same
// line, so the controls get the card's full height instead of the 26px strip
// under a state line. The AC card is unchanged and still stacks its two lines;
// btnRect() is the only place that knows the difference.
//
// THE BULB CARD HAS NO STATE TEXT. With the controls spanning the full width
// there is nowhere to put "30%  2700K", so the live reading is carried by the
// icon (real colour temperature, blended by real brightness) plus which chip is
// lit. See the OFFLINE note in screen.cpp for what replaced the one state string
// that was doing safety work rather than reporting a value.
//
// x tiles the card EXACTLY: 44 + 4*43 + 3*4 + 2*30 = 288 = CARD_IN_W.
// The chips are narrower than the 54 they were (the identity column is paid for
// out of their width) and much taller: 43x40 against 54x26 is +22% of area, but
// the HORIZONTAL tap target shrinks by 11px, and horizontal is the axis that
// matters — screenHitTest() already accepts the full 52px row band vertically.
// That is the trade the inline layout costs; there is no arrangement that keeps
// a 15px icon, a name, four chips and two swatches in 288px without it.
#define BULB_ID_W      44                 // x  16..59  — icon + name column
#define BULB_CTL_X0    (CARD_IN_X0 + BULB_ID_W)      // 60 — first chip
#define BULB_CHIP_W    43                 // label budget 35px; "100%" is 33
#define BULB_CHIP_PITCH (BULB_CHIP_W + CHIP_GAP)     // 47
// The name's budget after the icon: 20px, or two Font 2 digits with air. The
// shipped names are single ordinals ("1", "2", "3") and this column is sized for
// them — a longer name still renders, textTrunc() just cuts it hard. That is a
// much tighter budget than the ~180px the old stacked line gave, and it is the
// other thing the inline layout costs.
#define BULB_NAME_W    (BULB_CTL_X0 - SP_1 - CARD_TXT_X)          // 20
// Vertically the whole card less a 4px pad, which is where "bigger buttons"
// actually comes from: 40px drawn against 26.
#define BULB_CTL_DY    4
#define BULB_CTL_H     (CARD_H - 2 * BULB_CTL_DY)    // 40

// The two colour-temperature swatches fill the rest of the row exactly:
// 44 + 228 + 2*30 = 288. Circles rather than labelled buttons — the bulbs are
// white-spectrum, so the control IS its colour, and a "2202K" caption in a
// control this size was unreadable anyway. Note the swatches did NOT move when
// the row went inline: the identity column takes exactly what the four chips
// gave up, so SW_X0 is still 244.
#define SW_X0          (BULB_CTL_X0 + 4 * BULB_CHIP_PITCH - CHIP_GAP)   // 244
#define SW_CELL_W      30                 // tap cell; the circle is smaller
#define SW_R           13                 // 26px circle in the 40px row

// AC card: 3 mode chips then the setpoint stepper, tiling the same 288px.
// 3*60 + 2*4 = 188 (x 16..203), then 28 + 44 + 28 = 100 (x 204..303).
// Mode chips became 60 rather than 54 because "COOL" needed 51px of the 52 a
// 60px chip leaves — the one place a label ever decided a width. In Font 2 that
// same word is 31px and would fit a 54px chip easily, so the original reason is
// spent; 60 stays because these three chips and the 100px stepper tile the
// card's 288px exactly, and that is now what fixes the number.
#define ACM_W          60
#define ACM_PITCH      (ACM_W + CHIP_GAP)            // 64
#define ACS_X0         (CARD_IN_X0 + 3 * ACM_PITCH - CHIP_GAP)    // 204
#define ACS_BTN_W      28                 // one chevron
#define ACS_VAL_W      44                 // the readout between them

// ── Settings page ────────────────────────────────────────
// 4 cards on the same row grid. Rows 0 and 1 are discrete segmented controls
// (5 chips, 3 chips); rows 2..3 remain toggles.
#define SET_ROWS       4
#define SET_ROW_BRI    0                  // 5 chips on the shared chip pitch
#define SET_ROW_NIGHT  1                  // 3 chips: Off / Shift / Red
#define SET_ROW_SCHED  2                  // toggle
#define SET_ROW_FLIP   3                  // toggle
#define NIGHT_CHIPS    3     // Off / Shift / Red — see NIGHT_CHIP_MODE in state.h
// Title + caption stacked and vertically centred in the 48px card. With Font 2's
// 10px caps the title's ink runs y+9..21 and the caption's y+26..38, so the pair
// is centred with a 4px gap between them and the captions' descenders land 9px
// clear of the bottom edge (it was 4px under FreeSans). Both are the same FACE
// now — the built-in set has no bold — so the two are told apart by colour tier
// alone, C_TEXT against C_TEXT3.
#define SET_TITLE_CY   14
#define SET_CAP_CY     31
#define TOGGLE_W       44
#define TOGGLE_H       24
#define TOGGLE_X       (CARD_IN_X1 - TOGGLE_W)       // 259
#define TOGGLE_DY      ((CARD_H - TOGGLE_H) / 2)     // 11
// The knob is a square block inset TGL_PAD on all four sides — 18x18 in the 24px
// track — so it reads at a glance from across a dark room while the track's
// remaining 20px of travel is what says which end it is at. 3 is the same inset
// the disc used as its radius margin, so the control's weight is unchanged from
// the pill it replaced; anything larger closes the gap the travel needs.
#define TGL_PAD        3

// ── Scenes page ──────────────────────────────────────────
// A 3-column grid of 88x64 tiles, three rows visible, scrolled a page at a time
// from the gutter on the right. This is the one scrolling surface in the
// firmware — Devices and Settings are still fixed — and it exists so the scene
// list can grow past the five defined today with no layout work. How many
// scenes there are is NOT declared here: it is sizeof(SCENE[]) in screen.cpp,
// so adding one is a single table line.
//
// 3 columns, not the 4 it used to be: a 66px square could hold three cryptic
// dots and a name in the fallback font, and nothing else. 88px holds the name at
// full size with room to spare, which is what a scene tile is actually for —
// 9 legible tiles per page beat 12 illegible ones, and the page still scales.
//
// The grid TILES the body exactly in y (32 + 2*72 + 64 = 240), the same
// no-stranded-pixels rule the device rows follow. In x it stops short of
// SCENE_SB_X0 so the gutter and the tiles never overlap — each region only ever
// clears its own rect, so an overlap leaves pixels nothing repaints.
//
// The gaps differ per axis (12 across, 8 down) because the vertical budget is
// fixed: 2*PITCH_Y + TILE_H == 208 has exactly one solution keeping tiles above
// 60px, and it is 8. Both values are still on the spacing scale.
#define SCENE_COLS      3
#define SCENE_VIS_ROWS  3
#define SCENE_TILE_W    88
#define SCENE_TILE_H    64
#define SCENE_X0        SP_2                            // 8
#define SCENE_GAP_X     SP_3                            // 12
#define SCENE_GAP_Y     SP_2                            // 8
#define SCENE_PITCH_X   (SCENE_TILE_W + SCENE_GAP_X)    // 100
#define SCENE_PITCH_Y   (SCENE_TILE_H + SCENE_GAP_Y)    // 72
#define SCENE_PER_PAGE  (SCENE_COLS * SCENE_VIS_ROWS)   // 9 tiles on screen

// Tile contents, as offsets within the 88x64 tile. Chosen so the pips+name block
// is vertically CENTRED: the pips span y+15..23 and the name's 13px ascent box
// spans y+36..49, so the content runs 15..49 — centre 32, exactly half of
// SCENE_TILE_H.
#define SCENE_PIP_DY    19                // one pip per bulb, centre line
#define SCENE_PIP_R     4
#define SCENE_PIP_GAP   16                // 2*16 + 2*4 = 40 in 88px
#define SCENE_NAME_DY   42                // scene name centre line

// Scroll gutter: x 300..319, chevrons top and bottom. 20 x 104 per arrow —
// thin, but it sits in the middle band the 4-point touch fit INTERPOLATES rather
// than the top band it extrapolates, so it is nothing like as marginal as the
// tab strip. Drawn empty and dead to taps whenever every scene fits on one page,
// which is the case today, so the current UI gains no affordance it can't use.
#define SCENE_SB_X0     300
#define SCENE_SB_W      (SCR_W - SCENE_SB_X0)           // 20
// Tap split between the two arrows: y 32..135 scrolls up, 136..239 down.
#define SCENE_SB_MID    ((ROWS_Y0 + SCR_H) / 2)

#define NUM_DEVICES    4
// Devices 0..2 are the bulbs, device 3 is the AC. Scenes act on the bulbs only.
#define NUM_BULBS      3
#define BULB_BTNS      6                  // OFF, 1%, 30%, 100%, warm, cool
#define AC_BTNS        6                  // OFF, AC, DRY, up, <setpoint>, down

// The AC row's stepper: [up] 29C [down], with the value in the slot BETWEEN the
// two controls that change it rather than in the row's top line. Named here
// because both doAction() and the renderer have to agree on which slot is which,
// and because AC_BTN_TEMP is a READOUT: it draws no button, takes no press
// flash, and screenHitTest() reports a tap on it as a miss. That dead cell
// between the arrows is deliberate — it is what stops a slightly-off tap from
// stepping the temperature the wrong way.
#define AC_BTN_TUP     3
#define AC_BTN_TEMP    4
#define AC_BTN_TDN     5

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
// the device instead of letting the setpoint steps clamp somewhere it
// wouldn't accept.
#define AC_TEMP_MIN_DEF   18.0f
#define AC_TEMP_MAX_DEF   31.0f
#define AC_TEMP_STEP_DEF  1.0f
#define AC_MODE_COOL      "cool"
#define AC_MODE_DRY       "dry"
