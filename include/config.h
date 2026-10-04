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
// All four devices refresh together in one templated POST /api/template
// (haPollAll(), src/net/ha.cpp) every HA_POLL_MS, so no device is ever more
// than HA_POLL_MS stale. This replaced an earlier round-robin of one entity
// per tick, which left each device up to 4x HA_POLL_MS behind.
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
// How long a failed service call flashes a card's border/state text red. Was
// hardcoded 1500 at three separate sites in screen.cpp (and mirrored by hand
// in simulator.html) with nothing tying them together — a live drift risk in
// a file whose comments call out exactly this failure mode elsewhere.
#define ERR_FLASH_MS       1500UL

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
// Speaker connector (P4, "SPEAK") — GPIO 26 drives the board's on-board
// SC8002B class-AB amp. It is also the ESP32's DAC2, which is what the tap
// sound uses: a square wave was enough for a beep, not for a tock.
#define PIN_SPEAKER    26
#define SPEAKER_DAC    DAC_CHANNEL_2   // GPIO 26's DAC; see TOCK_HZ below

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
// 25 kHz because 5 kHz was AUDIBLE on this unit — a constant high-pitched
// whine from the panel's backlight drive circuit, at every brightness step
// except 100%. 5 kHz sits near the peak of human hearing sensitivity, and
// what pinned the diagnosis was that 100% is silent: esp32-hal-ledc.c
// promotes a duty of exactly (1 << bits) - 1 to (1 << bits), i.e. 255 becomes
// a constant DC high with no edges at all, while the other four steps switch
// 5000 times a second. So the top step never drove the circuit and never sang.
// The frequency was original (baseline 113505c) and only became audible when
// brightness became a setting: the fixed BL_DUTY 230 it replaced sat near DC,
// where switching energy is small, and BRI_DEFAULT's 128 is 50% duty, which is
// the loudest point on the curve.
// Constraint is BL_PWM_HZ * (1 << BL_PWM_BITS) <= 80 MHz; 25 kHz * 256 = 6.4
// MHz, so there is a lot of room. Do NOT lower this back into the audible
// band — and note that raising BL_PWM_BITS lowers the frequency ceiling, so
// the two move against each other.
#define BL_PWM_HZ      25000
#define BL_PWM_BITS    8
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

// ── Tap sound ────────────────────────────────────────────
// A TOCK: a damped sine at one fixed pitch plus a faster-dying overtone,
// played through GPIO 26's DAC (the ESP32's DAC2) into the SC8002B amp. Two
// earlier versions were square waves off an LEDC channel, and both were wrong
// for reasons worth not repeating:
//   - a flat 2 kHz tone was SHRILL: 2 kHz sits in the ear's most sensitive band,
//     and a constant pitch with a hard stop is exactly what reads as a "beep";
//   - an octave falling 520 -> 260 Hz sounded like a DUCK QUACK. The downward
//     glide is most of a quack, and a square wave at low duty (which is how
//     that version did volume) is a narrow pulse, rich in harmonics — nasal.
// A knock is a single pitch that dies fast, and a sine has no harmonics to
// sound nasal with. The DAC is what makes a sine possible at all: LEDC can only
// ever put out a square wave.
//
// Timbre: the fundamental rings for TOCK_TAU1_US; the overtone at TOCK_F2_X
// times it (inharmonic, like a struck block of wood rather than a string) dies
// in TOCK_TAU2_US and gives the attack its "knock". Deeper -> lower TOCK_HZ;
// woodier / clickier -> raise TOCK_P2; rounder -> lengthen TOCK_TAU1_US.
// Don't take TOCK_HZ much under ~300: the CYD's small speaker rolls off fast
// there, and a sine has no harmonics to carry it the way a square wave did.
#define TOCK_HZ        420
#define TOCK_F2_X      2.76f     // overtone ratio, a wood-block partial
#define TOCK_P2        0.6f      // overtone level, relative to the fundamental
// 10 ms, up from 7: loudness is judged over a far longer window than this
// sound lasts, so a longer ring reads as louder at the same peak. It still
// dies fast enough to be a knock and not a note.
#define TOCK_TAU1_US   10000     // fundamental decay time constant
#define TOCK_TAU2_US   1800      // overtone decay — much faster, it's the knock
// 6 tau1: the tail is ~1.6 DAC steps by then, and TOCK_FADE_MS takes it the
// rest of the way to exactly 0 so the hand-back to GPIO LOW is seamless.
// BLOCKS loop() for this long — see beep() in main.cpp for why that is the
// right trade here.
#define TOCK_MS        60
#define TOCK_FADE_MS   5
// LOUDNESS. At 100% the DAC is already at full swing, so the only way to get
// louder is the waveform's shape — two things, both load-bearing:
//   - beep() scales by the waveform's MEASURED peak, not the worst case of both
//     partials peaking on the same sample, which never happens — the first DAC
//     version scaled for it and topped out at 87 of 127 steps.
//   - it is then driven into a tanh soft clip by TOCK_DRIVE. Only the loud
//     attack flattens toward a square, which adds harmonics in the 1-3 kHz
//     band this speaker is efficient in; the decaying tail falls back under
//     the knee and stays a clean sine, so it doesn't turn nasal the way the
//     LEDC-duty volume did. Output still never exceeds amp.
// Modelled together with the longer TOCK_TAU1_US: ~+9.4 dB of energy against
// the first DAC version. 3.5 buys only ~1.3 dB more, for audibly more fuzz.
// Still too quiet? The next lever is TOCK_HZ, not this: the speaker is simply
// more efficient higher up.
#define TOCK_DRIVE     2.5f
// 16 kHz is ~13x the overtone's ~1.16 kHz, plenty for a sine this short, and at
// 62.5 us per sample leaves the per-sample maths (two expf + two cosf) room.
#define TOCK_RATE_HZ   16000
// IDLE IS GROUND, and the tock rises from it and returns to it — there is no
// mid-scale bias. The first DAC version idled at mid-scale (128, ~1.65 V) and
// swung either side, which HISSED AND CRACKLED FOREVER: the DAC's output is a
// fraction of its own 3.3 V supply, so holding half-scale passes half of every
// supply disturbance — Wi-Fi TX bursts, the 50 Hz loop(), SPI to the panel —
// straight into the amp. Between tocks the pin is now a plain GPIO driven LOW
// with the DAC off, which is exactly what the (quiet) LEDC builds idled at.
// The price is that the tock is unipolar: each partial is (1 - cos), which
// starts at 0 with zero slope, so it needs no bias and makes no pop. Its
// average rides up and back down with the envelope — a thump mostly below
// what this speaker reproduces, which if anything adds body. Modelled at
// ~-1 dB in the audible band against the biased version.

// Volume is a setting (Settings page, persisted in NVS), 6 steps from mute to
// max, as the tock's PEAK in DAC steps above ground — a true amplitude, where
// the square-wave versions could only fake one with duty. 255 is the full
// swing. 0 is a true mute: beep() skips the sound and its stall.
// The labels are percentages but the amplitudes are an AUDIO TAPER, 5 dB
// apart (-20, -15, -10, -5, 0 dB): loudness is heard logarithmically, so a
// linear 51/102/153/204/255 would sound like one quiet step and four nearly
// identical loud ones. 5 dB keeps the whole ladder inside the -20 dB range the
// 4-step version spanned, so 20% is no quieter than the old quietest step.
#define VOL_STEPS      6
#define VOL_AMP_LIST   { 0, 26, 45, 81, 143, 255 }
#define VOL_LABEL_LIST { "0%", "20%", "40%", "60%", "80%", "100%" }
// 60% (-10 dB), the nearest to the 4-step version's 67% default, since this
// sits in a bedroom.
#define VOL_DEFAULT    3

// Boot chime: ONE soft, sustained F#-major chord, played once at power-on
// through the same DAC path as the tock — bootChime() in main.cpp. In the
// spirit of the macOS startup chime, on request, after a first version (a
// rising C6-E6-G6-C7 arpeggio with a glassy overtone) read as too busy and too
// bright. Composed rather than sampled: the firmware has no audio-file
// pipeline, and a synth already lives here.
// Each note is {Hz, onset ms, decay tau ms}. The onsets are only 12 ms apart —
// a ROLLED chord, heard as one sound with a soft edge rather than a strike,
// not as a melody. Lower notes ring a little longer, as on a real instrument.
// Voiced F#4 C#5 F#5 A#5 C#6: the Mac chime sits an octave or two lower, but
// this speaker barely reproduces anything under ~400 Hz, so the chord is moved
// up to where it can actually be heard, with only the root left below that.
#define CHIME_NOTE_LIST { {370, 0, 650}, {554, 12, 600}, {740, 24, 550}, \
                          {932, 36, 500}, {1109, 48, 450} }
// The second partial is NOT an overtone here — it is a slightly detuned TWIN
// of each note (0.4% sharp, same decay, a little quieter). The pair beats at
// 1.5-4.4 Hz, which is the slow shimmer that makes a chord sound lush rather
// than like five test tones. An overtone was what made the first version
// bright and glassy, i.e. the opposite of subtle.
#define CHIME_F2_X     1.004f
#define CHIME_P2       0.8f
#define CHIME_TAU2_DIV 1
// A swell, not a strike: each note fades in over ~this tau rather than
// starting at full level, which is most of what separates "chime" from "beep".
#define CHIME_ATTACK_MS 25
// Nearly linear. A soft chord stays pure; the tock's hard drive exists to make
// a knock louder, and loudness is not the goal here.
#define CHIME_DRIVE    0.6f
// Quieter than a tap at the same Volume setting: the peak lands at this share
// of the Volume step's amplitude. A boot sound announces; it shouldn't startle.
#define CHIME_GAIN_PCT 55
// Total length, BLOCKING. With creds in secrets.h it overlaps Wi-Fi
// association, which setupWifi() waits on anyway, so it is free; on the
// WiFiManager path it adds this much to boot (see setupWifi()). The long
// CHIME_FADE_MS is part of the sound, not just click insurance: the chord is
// meant to dissolve, and ~2.5 tau of natural decay plus this fade does that.
#define CHIME_MS       1600
#define CHIME_FADE_MS  400

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

// ── Idle return to Scenes ────────────────────────────────
// After this long without a touch, a panel left on Devices or Settings goes
// back to Scenes, so walking up to it always lands on the one-tap scene grid
// rather than wherever the last visit happened to end. Any contact counts as
// activity, even a tap on no control: the point is "nobody is using it".
// Scenes itself is left alone, scroll offset included (S.sceneRow survives a
// page switch on purpose).
#define IDLE_HOME_MS   (5UL * 60 * 1000)  // 5 min

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

// ── header (y 208..239, anchored to the BOTTOM edge) ─────
// 32px, unchanged in height — moved, not resized. The tab targets don't need to
// grow or shrink for the move: CAL_INSET (main.cpp, env:calib) insets 30px from
// EVERY corner of the 4-point fit, so the touch calibration extrapolates
// equally at the top bezel and the bottom one. The header was never made this
// tall because ITS particular edge was worse than the other — a resistive panel
// is worse at ANY bezel than in the middle — so relocating it costs nothing and
// needs no recalibration.
//
// THREE regions that TILE THE BAR EXACTLY (80 + 171 + 69 = 320):
//   room reading  x   0.. 79   STATUS_ROOM_W, left edge
//   tab strip     x  80..250   TAB_STRIP_W, the tabs CENTRED in it
//   clock         x 251..319   STATUS_CLK_W, right edge
// A gap leaves pixels nothing ever clears; an overlap is just as bad, since each
// region only clears its own rect, so whatever spills over is never repainted.
//
// ROOM | TABS | CLOCK IS A SWAP, made on request. It used to be tabs | room |
// clock, with the strip flush left at x 0 and the room reading sandwiched
// between it and the clock. Now the two numbers frame the bar from either edge
// and the navigation sits between them, which is where a tab bar is looked for.
// The widths did not change, only the order — so none of the F_NUM budgets
// below moved, and neither did any tap target's size.
//
// There used to be a fourth region, a 26px connectivity glyph at x 0..25
// (STATUS_ICO_W). It is gone — removed on request once the header moved to the
// bottom edge — and its job is done by an overlay banner in the BODY that only
// appears when there is something to say (NOCONN_W below, drawNoConn() in
// screen.cpp). The 26px it freed, along with another 8px from TAB_GAP, was
// spent on making the room reading and the clock a size bigger (F_NUM).
//
// Tab cells are sized to their OWN label — measured via textW(F_MICRO, ...),
// not a shared constant — with TAB_LBL_DX blank each side and a real TAB_GAP
// between tabs. A uniform cell does not work: the widest label sets the width
// every other tab shares, so most of the "gap" a viewer sees beside a short
// word is slack inside its own cell. See tabRect() in screen.cpp, which
// computes and caches the 3 cells once; simulator.html's tabRect() mirrors it.
//
// THE STRIP IS CENTRED in TAB_STRIP_W, the leftover split into two outer
// margins. It was flush left before the swap, because centred at x 0 the left
// margin was blank bottom-left corner with nothing beyond it, and the tabs read
// as adrift. Sitting between two blocks of digits, both margins are now gaps
// to a neighbour, so centring is what balances them. The gaps AND the margins
// are static background — nothing is ever drawn or hit-tested there, so they
// are painted once on a full invalidate, never per-frame.
//
// TAB_GAP HAS BEEN CUT TWICE FOR THE SAME REASON, and both times the buyer was
// the room reading's font: 18 -> 12 to get its numbers from F_MICRO up to
// F_TITLE, then 12 -> 8 to get them (and the clock) up to F_NUM. Both spent
// tab-strip slack rather than content: the cells are content-fit, so a smaller
// gap moves them closer together without shrinking any of them.
//
// The 3 shipped labels need 166px (cells 50/44/56 + 2*TAB_GAP) of TAB_STRIP_W's
// 171, leaving 5px: 2px before "Devices", 3px after "Settings". If TAB_LABEL
// ever grows a wider word there is very little room to absorb it —
// simulator.html's warning on this arithmetic is the thing to watch.
#define STATUS_H       32
#define STATUS_Y0      (SCR_H - STATUS_H) // 208 — top edge of the header band
#define TAB_GAP        SP_2                // real gap between tabs — was 18, then 12
#define TAB_COUNT      3                  // static_assert'd against PAGE_COUNT
// Blank between the screen edge and each number block — the clock's right
// margin and, mirrored, the room reading's left one. One constant, so the bar
// stays symmetric if either changes. Before the swap only the clock used it;
// the room reading started at x 0 and its digits touched the bezel.
#define STATUS_EDGE_DX 6
// The clock's own sub-region. Unchanged in role, and now on its THIRD width: 51
// originally (~10px of unused slack), cut to 41 to buy the AC's room reading its
// own space when that moved in beside it, and now 69 because the clock itself
// went up a size to F_NUM.
//
// The clock is still exactly 5 characters. In F_NUM "23:45" is 63px (digits are
// 14px apiece, ":" is 7) against F_TITLE's 35px, right-aligned STATUS_EDGE_DX
// off the screen edge — 63 + 6 = 69, with zero pixels of left slack. Anything
// wider paints into the tab strip, which only repaints on its own compare.
// "--:--" is 39px (F_NUM "-" is 8), so the pre-NTP placeholder is comfortably
// narrower, as it always was.
#define STATUS_CLK_W   69                 // x 251..319 — 24h clock
// The AC's live room reading, relocated here from its own card (see the AC
// card comment above BULB_ID_W) on request so it reads on every page rather
// than only Devices, and since moved to the LEFT edge by the swap above.
// drawStatusRoom() (screen.cpp) draws it by reusing roomTempText()/
// humidityText() — the same two functions the card used to call — as
// <temp><ring> <humidity>%, left-aligned STATUS_EDGE_DX in from the edge.
//
// ITS NUMBERS ARE F_NUM. This region has been up a size twice: F_MICRO ->
// F_TITLE to stop it looking undersized beside the clock, then F_TITLE -> F_NUM
// together WITH the clock. There is no size between F_TITLE and F_NUM —
// TFT_eSPI has no Font 3 or Font 5 — so "one size up" is 10px caps -> 18px caps
// and 1.8x the width, not a nudge.
//
// TWO PIECES DELIBERATELY DID NOT GROW WITH THE DIGITS:
//   * "%" stays F_TITLE. At F_NUM it is 21px against 9 — width this region does
//     not have. It is a unit marker rather than data, and is drawn
//     BASELINE-ALIGNED to the big digits so it reads as a suffix rather than as
//     a smaller number.
//   * The degree ring stays a 2px drawn circle with a 5px advance, matching the
//     one wValue() draws beside the AC setpoint's F_NUM digits.
// The trailing "/" that used to separate it from the clock is gone too.
//
// Sized for the worst REALISTIC case, not the worst POSSIBLE one — an indoor
// sensor in an air-conditioned bedroom does not read 100% RH any more than it
// reads -10C:
//   6 (STATUS_EDGE_DX) + "27"(28) + ring(5) + gap(4) + "99"(28) + "%"(9) = 80
// ZERO slack inside the region, and that is fine now where it was not before
// the swap: the gap to the next block is no longer this region's job. The tab
// strip's own 2px margin plus "Devices"' TAB_LBL_DX put 6px between "%" and the
// first tab label, the same air the clock gets from "Settings".
//
// AN OUT-OF-RANGE READING DROPS ITS DIGITS TO F_TITLE rather than spilling
// (ha.cpp's temperature band is a broad -10..60C, and humidity has no check, so
// "-10" or "100" can arrive). Before the swap a spill ran into the clock, which
// repaints every minute and so healed it. Now it would run into the tab strip,
// which repaints only when a tab's vis changes, so the stray pixels would stay
// for as long as you stayed on the page. A smaller reading for a reading that
// is already wrong is the cheaper failure.
#define STATUS_ROOM_W  80                 // x 0..79 — AC room temp + humidity
#define TAB_X0         STATUS_ROOM_W      // 80 — tab strip starts after it
#define TAB_STRIP_W    (SCR_W - STATUS_ROOM_W - STATUS_CLK_W) // 171
// The divider is now at the TOP of the header band — the seam with the body
// above it — rather than the bottom, because the header sits below the body
// instead of above it. Content sits BELOW the divider (mirrored from the old
// top-anchored layout, not merely shifted), so every header clear starts at
// STATUS_DIV_Y+1 and is STATUS_H-1 tall, and the 1px rule survives every one of
// those clears because it sits outside all of them and is painted once.
#define STATUS_DIV_Y   STATUS_Y0          // 208
#define STATUS_CY      (STATUS_DIV_Y + 16) // 224 — header content centre line
// Tab indicator: a 2px underline SEATED ON the header rule, not a pill — from
// BELOW now, since the rule sits at the top of the band instead of the bottom.
// Everything this used to say about being an underline rather than a filled
// pill, and about spanning the word rather than the cell, is unchanged; only
// which edge of the cell it seats against has flipped, and wTab() flipped with
// it (see widgets.cpp).
//
// TAB_UL_Y sits exactly one row past STATUS_DIV_Y, so the bar stacks directly
// under the 1px rule and the two read as one line: a 1px gap between them would
// look like a misprint. It must also stay at or below STATUS_DIV_Y+1 for the
// mirror-image reason the old bound existed — one row higher and it would
// overwrite the rule, which is painted once and never restored, while the
// indicator has to be inside a rect that gets cleared, or a tab that stops
// being current would keep its bar forever.
//
// 2px and not 3, though the margin is no longer what set it: the label used to
// centre on STATUS_CY in F_BODY, whose Font 2 descends 7px below that centre —
// "Settings"' g reached STATUS_CY+7 with the bar only 8px clear of it. The tab
// label is F_MICRO now (one step down from F_BODY, on request, to shrink the
// label enough that TAB_GAP could open up between tabs without widening the
// header), and F_MICRO's Font 1 descends only 3px below centre — MORE
// clearance than before, not less, so TAB_UL_H=2 needed no change.
// simulator.html asserts the clearance rather than trusting either number here.
//
// Each cell is its own label's width plus TAB_LBL_DX a side, by construction
// (tabRect()), so there is no separate "label budget" to check the way a
// fixed-width cell needed one — textFit()'s maxW is exactly the label's own
// measured width and can never be tight. F_MICRO's monospace 6px advance
// puts the widest label ("Settings") at 48px, against 49px in F_BODY's
// Font 2 — shrinking the font bought almost no width back on its own, which
// is the same reason a uniform cell couldn't be shrunk into a tight gap
// either; see the TAB_STRIP_W comment above.
#define TAB_UL_H       2
#define TAB_UL_Y       (STATUS_DIV_Y + 1)            // 209..210
#define TAB_UL_PAD     2                             // bleed each side of label
#define TAB_LBL_DX     SP_1                          // blank each side of a tab's own label
#define TAB_TAP_H      STATUS_H

// ── body (y 0..207) ──────────────────────────────────────
// 4 row bands of 52 tile the body EXACTLY: 4*52 = 208 = STATUS_Y0, now that the
// header sits below the body instead of above it. Devices and Settings both use
// this grid via rowTop(); a leftover sliver above the header is the failure
// this arithmetic exists to prevent.
#define ROWS_Y0        0
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
// 2 pad = 48. This is now the Settings brightness card ONLY — every device
// card (bulbs and the AC) is inline and uses the BULB_* block below instead.
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
// Settings' segmented-chip rows (brightness, night mode) ONLY now — every
// device card, AC included, uses BULB_CTL_DY/BULB_CTL_H instead. 26px, up from
// 22: the card inset and pads paid for it, so the drawn control is nearer the
// 52px row band screenHitTest() has always accepted vertically.
#define CTL_DY         20                 // control row top
#define CTL_H          26                 // control row height

// ── connectivity overlay (BODY, not header) ──────────────
// What replaced the deleted header glyph (see the header block above), on
// request. It is a BANNER, not a persistent readout: it exists only while there
// is a fault, so a healthy system shows nothing at all rather than spending
// pixels to say so — the same argument that shrank the old "WIFI"/"HA" dots to
// a 26px glyph, followed to its conclusion.
//
// It lives in the BODY because the header has no room: three regions tile that
// bar exactly and each clears only its own rect. So it must occlude a page, and
// on every page — which is the point, since the fault is not page-specific.
//
// IT CLEARS THE WHOLE OF ROW BAND 0 FIRST, and that is what makes it read as an
// overlay rather than as a clipping bug. Painted straight on top it leaves
// fragments of whatever it does not quite cover: the first attempt left the
// bottom of a yellow brightness chip poking out under its border on Devices,
// and a 2px sliver of a cyan one on Settings, whose STACKED card puts its
// controls 2px lower than an inline device card puts its own. Clearing the band
// is not a new kind of operation either — it is pixel-for-pixel what a device
// card's own first draw already does, so "row 0 holds nothing but the banner"
// is a state the renderer can hit exactly on both card kinds instead of
// approximately on each.
//
// Row band 0 rather than a free position near the top edge, because the row grid
// is the only vertical structure the body has: a rect that respected no row
// would have to clear parts of two of them to avoid fragments, and would
// therefore cover MORE, not less.
//
// The cost is Scenes, whose 64px tiles on a 72px pitch outlive a 52px band by
// 12px, so their names stay visible under the banner. Accepted rather than
// widened: the two grids genuinely do not share a pitch, so no single rect is
// exact on both, and being exact on the two pages that DO share the row grid
// (Devices, the default, and Settings) beats being equally approximate on all
// three.
//
// The width is a hand-measured literal, not a runtime measurement like
// tabRect()'s cells: there is exactly one label here with no sibling to balance
// against, so the arithmetic is just spelled out, the same way BULB_CHIP_W's
// comment measures "100%". textW(F_BODY, "No Connection") is 90px and wChip()
// centres text on the box, so the padding is (114-90)/2 = 12 = SP_3 a side.
// wChip()'s own textFit() re-measures at draw time regardless, so this is
// documentation of the number rather than a second source of truth for it.
#define NOCONN_H       CTL_H                    // 26 — a chip's height
#define NOCONN_W       114                      // 90 + 2*SP_3
#define NOCONN_X0      ((SCR_W - NOCONN_W) / 2) // 103 — 320-114 halves exactly
#define NOCONN_Y0      (ROWS_Y0 + (ROW_H - NOCONN_H) / 2)  // 13 — centred in band 0

// CHIP GRID — 5 across the Settings brightness card. 5*54 + 4*4 = 286 (x 16..301).
// This USED to be shared with the device rows, so a control on either page was
// the same size. The bulb rows went inline (below) and now carry their own
// narrower chip, so the two pages no longer match: a bulb chip is 43x40 against
// this one's 54x26. That is a real cost of the inline layout, not an oversight —
// the AC row's ACM_W 60 had already made "one shared pitch" approximate.
#define CHIP_W         54
#define CHIP_GAP       SP_1
#define CHIP_PITCH     (CHIP_W + CHIP_GAP)           // 58
// Volume's 6 chips can't share that width: 6*54 + 5*4 = 344 against the card's
// 288. 44 is the widest that fits — 6*44 + 5*4 = 284 (x 16..299) — and "100%"
// is 33px of Font 2, so it still has 5px a side. A fourth chip width on the
// panel, joining BULB_CHIP_W/CHIP_W/ACM_W; same trade as the bulb chip made.
#define VOL_CHIP_W     44
#define VOL_CHIP_PITCH (VOL_CHIP_W + CHIP_GAP)       // 48

// ── bulb card: ONE INLINE ROW ────────────────────────────
// [icon name] [OFF][1%][30%][100%] (o)(o)  — identity and controls on the same
// line, so the controls get the card's full height instead of the 26px strip
// under a state line. The AC card now shares this same identity column and
// control band (BULB_ID_W, BULB_CTL_DY, BULB_CTL_H below) — its mode chips and
// stepper only differ in what they draw and how many slots they use, not in
// where the row sits. btnRect() is the only place that knows the difference.
//
// THE BULB CARD HAS NO STATE TEXT. With the controls spanning the full width
// there is nowhere to put "30%  2700K", so the live reading is carried by the
// icon (real colour temperature, blended by real brightness) plus which chip is
// lit. See the OFFLINE note in screen.cpp for what replaced the one state string
// that was doing safety work rather than reporting a value. The AC now follows
// the same rule — see the AC identity block below for what replaced ITS state
// string.
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

// AC card: now INLINE like the bulb cards — an icon + a single centred name
// line in the same BULB_ID_W column, then 3 mode chips and the setpoint
// stepper filling the same BULB_CTL_X0..CARD_IN_X1 span the bulb chips and
// swatches fill, at the same BULB_CTL_DY/BULB_CTL_H height. That is the whole
// point of this pass: the AC's control band used to sit in its own 26px strip
// (CTL_DY/CTL_H) lower in a shorter card, visibly out of step with the bulb
// rows above and below it on the Devices page. The AC's live room reading
// used to live here too, as two extra lines stacked under the name — it has
// since moved to the header (STATUS_ROOM_W above), on request, so it reads on
// every page rather than only Devices, and the identity column is back to
// the bulb's plain single line as a result.
//
// Budget: CARD_IN_W(288) - BULB_ID_W(44) = 244, identical to what the bulb's 4
// chips + 3 gaps + 2 swatches already fill after BULB_CTL_X0 — the AC's 3 chips
// + stepper now tile that SAME 244px, not the AC card's old undivided 288px.
// 3*43 + 2*4 = 137 (chips), + one more CHIP_GAP before the stepper, then
// 28 + 47 + 28 = 103 (stepper) = 137 + 4 + 103 = 244 exactly.
// ACM_W dropped from 60 to 43 — the same width as BULB_CHIP_W, not because the
// two share a pitch (they don't; this is coincidence of both needing to fit the
// same 244px after the same 44px column), but "COOL" only needs 31px of Font
// 2's, so a 43px chip (35px label budget) still has room to spare. ACS_VAL_W
// grew from 44 to 51 on the first cut, absorbing what shrinking the chips
// freed with no gap at all before the stepper — DRY sat flush against the
// down chevron, the one seam on the row with no CHIP_GAP of air, which read as
// a mistake next to the evenly-spaced bulb chips beside it. ACS_VAL_W gave
// back 4px to that gap (51 -> 47), landing 3px above the 44 it used before
// this pass ever touched it, rather than shrinking ACS_BTN_W — 28 was already
// a proven, comfortable chevron target.
#define ACM_W          43
#define ACM_PITCH      (ACM_W + CHIP_GAP)            // 47
#define ACS_X0         (BULB_CTL_X0 + 3 * ACM_PITCH)   // 201 — CHIP_GAP clear of DRY
#define ACS_BTN_W      28                 // one chevron
#define ACS_VAL_W      47                 // the readout between them


// ── Settings page ────────────────────────────────────────
// 5 settings on the same 4-row grid. Rows 0..2 are discrete segmented controls
// (5 chips, 3 chips, 6 chips); row 3 is the two toggles SIDE BY SIDE, as two
// half-width cards. That pairing is what made room for Volume without a 5th
// row — the rows still tile the body exactly, and the price was the toggles'
// captions ("23:45 - 08:00", "Rotate 180 degrees") and their long titles,
// which a 150px card has no room for. Chosen on request over re-cutting the
// page into five shorter rows, which would have shrunk every target on it.
#define SET_ROWS       4
#define SET_ROW_BRI    0                  // 5 chips on the shared chip pitch
#define SET_ROW_NIGHT  1                  // 3 chips: Off / Shift / Red
#define SET_ROW_VOL    2                  // 6 chips: 0% (mute) .. 100%, narrower
#define SET_ROW_TGL    3                  // two half-width toggle cards
#define NIGHT_CHIPS    3     // Off / Shift / Red — see NIGHT_CHIP_MODE in state.h
// The toggle row's cells, in x order. This is Hit::sub on SET_ROW_TGL, the same
// way a chip's position is Hit::sub on a chip row.
#define SET_TGLS       2
#define SET_TGL_SCHED  0
#define SET_TGL_FLIP   1
// SP_1 between the two cards, the same 4px gutter every card on the page has
// against its vertical neighbours. (304 - 4) / 2 = 150 exactly.
#define TGL_CARD_GAP   SP_1
#define TGL_CARD_W     ((CARD_W - TGL_CARD_GAP) / 2)          // 150
#define TGL_CARD_PITCH (TGL_CARD_W + TGL_CARD_GAP)            // 154 -> x 8, 162
// Where a tap switches from one toggle to the other: the middle of the gap, so
// neither card's own pixels ever belong to its neighbour.
#define TGL_SPLIT_X    (CARD_X + TGL_CARD_W + TGL_CARD_GAP / 2)  // 160
// A toggle card's title is ONE line, centred in the card on the icon's own
// centre line. It used to sit over a C_TEXT3 caption (SET_TITLE_CY 14 /
// SET_CAP_CY 31); the half-width card dropped the caption, so there is no pair
// left to centre. Budget: "Schedule" runs from the text column (x+28) to SP_2
// short of the toggle (x+97), i.e. 61px — simulator.html asserts it fits.
#define TOGGLE_W       44
#define TOGGLE_H       24
// Offset from a half card's left edge, so both cells share it. Same "last
// content px minus TOGGLE_W" convention the full-width card used, which is why
// the right-hand one still lands at x 259 exactly where Flip's toggle was.
#define TOGGLE_DX      (TGL_CARD_W - CARD_PAD - 1 - TOGGLE_W)  // 97 -> x 105, 259
#define TOGGLE_DY      ((CARD_H - TOGGLE_H) / 2)     // 11
// The knob is a square block inset TGL_PAD on all four sides — 18x18 in the 24px
// track — so it reads at a glance from across a dark room while the track's
// remaining 20px of travel is what says which end it is at. 3 is the same inset
// the disc used as its radius margin, so the control's weight is unchanged from
// the pill it replaced; anything larger closes the gap the travel needs.
#define TGL_PAD        3

#define NUM_DEVICES    4
// Devices 0..2 are the bulbs, device 3 is the AC. Scenes act on the bulbs only.
#define NUM_BULBS      3

// ── Scenes page ──────────────────────────────────────────
// A 3-column grid of 88x64 tiles, TWO rows visible, scrolled a page at a time
// from the gutter on the right, with the AC card sitting below it. This is the
// one scrolling surface in the firmware — Devices and Settings are still fixed —
// and it exists so the scene list can grow past the five defined today with no
// layout work. How many scenes there are is NOT declared here: it is
// sizeof(SCENE[]) in screen_scenes.cpp, so adding one is a single table line.
//
// 3 columns, not the 4 it used to be: a 66px square could hold three cryptic
// dots and a name in the fallback font, and nothing else. 88px holds the name at
// full size with room to spare, which is what a scene tile is actually for —
// legible tiles beat a denser grid of illegible ones, and the page still scales.
//
// THE GRID NO LONGER FILLS THE BODY, and that is the one rule this page gave up
// on purpose. The AC card is COPIED here from the Devices page — same device,
// same row SLOT, same rects — so the bottom 52px of the body (SCENE_AC_Y0..
// STATUS_Y0-1) belongs to that card, and the grid stops at SCENE_AC_Y0. Two
// visible rows of 64px tiles on a 72px pitch reach y 135, which leaves a
// deliberate 20px blank band at y 136..155 between the last tile row and the
// card.
//
// That band is NOT a stranded-pixel hazard, which is what the old exact-tiling
// assert existed to prevent. Across x 0..299 nothing writes y 136..155 at all
// after bodyReset()'s one-time fill — the tiles stop at 135 and drawDeviceCard()'s
// first-draw band fill starts at rowTop(SCENE_AC_SLOT) == 156 — so there is no
// content to go stale. The gutter's column (x 300..319) is the one exception and
// is fine for the opposite reason: drawSceneScrollbar() OWNS that rect down to
// SCENE_AC_Y0-1 and clears it every time it draws, and once scrolling goes live
// the down chevron's ink lands at roughly y 137..147, inside this band by design.
//
// What DOES still have to hold is that no tile reaches INTO the card's band,
// which is now a <= bound rather than an == one (screen.cpp), exactly like the
// x-side rule against SCENE_SB_X0 — each region only ever clears its own rect,
// so an overlap leaves pixels nothing repaints.
//
// The gaps still differ per axis (12 across, 8 down). The vertical budget USED to
// force the 8 — 2*PITCH_Y + TILE_H == 208 had exactly one solution keeping tiles
// above 60px. It no longer does: at two rows the budget is 156 and PITCH_Y +
// TILE_H == 156 would want a 74px tile or a 28px gap instead. 8 survives because
// the 88x64 tile was KEPT on request, spending the slack as the blank band above
// rather than growing the tiles into it. Both values are still on the spacing
// scale; 28 would not have been.
#define SCENE_COLS      3
#define SCENE_VIS_ROWS  2                 // was 3, before the AC card took a band
// The AC's row band, identical to the row it occupies on the Devices page — that
// identity is the whole feature, so this is a SLOT into the shared row grid
// (rowTop()/cardTop()), not a scene-specific position. Derived from NUM_BULBS
// rather than written as 3, which is the same idiom drawStatusRoom() already
// uses to reach the AC as S.dev[NUM_BULBS]: "the bulbs are a prefix and the AC
// is the one device after them" is stated once, in the comment above, and not
// re-declared here.
#define SCENE_AC_SLOT   NUM_BULBS                        // 3 — the last row band
// FIRST pixel of that band, 156 — and _Y0, not _Y1, deliberately. CARD_IN_X1
// fixes _X1/_Y1 in this file as the LAST pixel of a span, so naming 156 _Y1
// would read as 135 to anyone who trusted that convention, and every `<=` bound
// against it would be off by one in whichever direction the reader guessed.
// Everything that bounds against this wants "stops before the AC band", which
// is what the _Y0 spelling says.
#define SCENE_AC_Y0     (ROWS_Y0 + SCENE_AC_SLOT * ROW_H)  // 156
#define SCENE_TILE_W    88
#define SCENE_TILE_H    64
#define SCENE_X0        SP_2                            // 8
#define SCENE_GAP_X     SP_3                            // 12
#define SCENE_GAP_Y     SP_2                            // 8
#define SCENE_PITCH_X   (SCENE_TILE_W + SCENE_GAP_X)    // 100
#define SCENE_PITCH_Y   (SCENE_TILE_H + SCENE_GAP_Y)    // 72
#define SCENE_PER_PAGE  (SCENE_COLS * SCENE_VIS_ROWS)   // 6 tiles on screen

// Tile contents, as offsets within the 88x64 tile. Chosen so the pips+name block
// is vertically CENTRED: the pips span y+15..23 and the name's 13px ascent box
// spans y+36..49, so the content runs 15..49 — centre 32, exactly half of
// SCENE_TILE_H.
#define SCENE_PIP_DY    19                // one pip per bulb, centre line
#define SCENE_PIP_R     4
#define SCENE_PIP_GAP   16                // 2*16 + 2*4 = 40 in 88px
#define SCENE_NAME_DY   42                // scene name centre line

// Scroll gutter: x 300..319, chevrons top and bottom. 20 x 78 per arrow —
// mostly in the band the 4-point touch fit INTERPOLATES, unlike the tab strip
// which sits entirely inside a bezel band it extrapolates, so it is nothing
// like as marginal. Drawn empty and dead to taps whenever every scene fits on
// one page, which is the case today, so the current UI gains no affordance it
// can't use.
//
// It is 78px per arrow rather than the 104 it was because the gutter belongs to
// the GRID, not to the body: below SCENE_AC_Y0 the AC card runs the full card
// width (x 8..311) and so passes UNDER this column. The gutter must therefore
// stop where the grid does, in its clear (drawSceneScrollbar()) and in the hit
// test alike — screenHitTest() checks the card's band BEFORE this column for
// exactly that reason, or the card's up chevron at x 276..303 would be
// unreachable.
#define SCENE_SB_X0     300
#define SCENE_SB_W      (SCR_W - SCENE_SB_X0)           // 20
// Tap split between the two arrows: y 0..77 scrolls up, 78..155 down — the
// GRID's range, which is the body's less the AC card's row band at the bottom.
#define SCENE_SB_MID    ((ROWS_Y0 + SCENE_AC_Y0) / 2)

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
