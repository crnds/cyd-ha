# cyd-ha — Home Assistant bedroom controller

Firmware turning a CYD "Cheap Yellow Display" (ESP32-2432S028R) into a one-tap
wall/nightstand controller for four bedroom devices. No phone, no app, no
drill-down — every action is a single tap on a fixed 320×240 screen, and the screen
doubles as a status display.

Three pages, switched from the tab bar in the header. That is the whole
navigation model: no drill-down, no fourth page, and scrolling only on Scenes.

**Devices** — the four entities, 23 controls, one card each.

```
┌────────────────────────────────────────────────┐
│ ⠿  ( Devices )  Scenes   Settings        23:00 │
├────────────────────────────────────────────────┤
│ ╭────────────────────────────────────────────╮ │
│ │ ◐ 1  [ OFF ][ 1% ][ 30% ][ 100% ]   ◉   ○  │ │
│ ╰────────────────────────────────────────────╯ │
│ ╭────────────────────────────────────────────╮ │
│ │ ○ 2  [ OFF ][ 1% ][ 30% ][ 100% ]   ◉   ○  │ │
│ ╰────────────────────────────────────────────╯ │
│ ╭────────────────────────────────────────────╮ │
│ │ ● 3  [ OFF ][ 1% ][ 30% ][ 100% ]   ◉   ◉  │ │
│ ╰────────────────────────────────────────────╯ │
│ ╭────────────────────────────────────────────╮ │
│ │ ❄ AC                              Room 27° │ │
│ │ [  OFF  ][ COOL  ][  DRY  ] [▼]  24°  [▲]  │ │
│ ╰────────────────────────────────────────────╯ │
└────────────────────────────────────────────────┘
```

A bulb card is a single line — icon, number, then every control at the card's full
height. The AC card stacks its live reading over its controls instead, because a
room temperature is a number nothing else on the card can show.

The icon at the left of each card is the fastest thing to read on the page, and it
is derived from live state rather than being a label: a bulb is a filled glyph in
its **actual colour temperature, dimmed by its actual brightness**, or a hollow
outline when off. So the icon column tells you what the room is doing before you
read a word. The AC shows a snowflake, a droplet or a power symbol to match its
mode.

On a bulb card that icon is the *only* readout — there is no room on the line for
a number, and the chips are only presets, so a bulb set to 47% from the phone
lights no chip and shows no percentage. Its colour and dimness still tell you what
it is doing. An unreachable bulb is unmistakable a different way: the whole card
outlines in red, icon and number with it, and every control greys out. The two
circles are the colour-temperature ends (the bulbs are white-spectrum, so the
control *is* its colour). The AC's setpoint sits between the two chevrons that
change it, and the dead cell between them is deliberate: it stops a slightly-off
tap from stepping the wrong way.

**Scenes** — macros over the three bulbs (the AC is not touched), in a 3-column
grid. Five are defined; nine fit on screen, and past that the page scrolls a page
at a time from the gutter on the right.

```
┌────────────────────────────────────────────────┐
│ ⠿   Devices   ( Scenes )  Settings       23:00 │
├────────────────────────────────────────────────┤
│ ╭──────────╮ ╭──────────╮ ╭──────────╮      ▲ │
│ │  ○ ○ ○   │ │  ○ ○ ●   │ │  ● ● ●   │      ▪ │
│ │   OFF    │ │  RELAX   │ │   WORK   │      ▪ │
│ ╰──────────╯ ╰──────────╯ ╰──────────╯      ▪ │
│ ╭──────────╮ ╭──────────╮                   ▪ │
│ │  ● ● ●   │ │  ● ● ●   │                   ▪ │
│ │  AWAKE   │ │   DAY    │                   ▪ │
│ ╰──────────╯ ╰──────────╯                   ▪ │
│                                             ▼ │
└────────────────────────────────────────────────┘
```

The three pips on a tile are one per bulb, read straight off the scene's own
definition: a ring means that bulb will be off, a filled pip means on — amber for
2202 K, blue for 4000 K, and dimmer for a lower level. They replaced the text
captions the older full-width cards carried, which no longer fit and which could
drift from what the tap actually sends.

The grid is three columns rather than four so the name fits at full size; nine
legible tiles beat twelve cramped ones, and the page still scales past a hundred.

The arrows and thumb only appear once the scene table outgrows one screen, so with
five scenes the gutter is a plain margin and ignores taps. Adding a scene is one
line in `SCENE[]` in `src/ui/screen_scenes.cpp` — there is no count to update anywhere
else, and the renderer's memory does not grow with the list.

A scene highlights when the bulbs *actually match* it, rather than when you last
tapped it. So overriding one bulb on the Devices page deselects it, and a change
made from the HA app selects the matching one — there is no stored "current
scene" to drift out of sync. Applying one is a single `light.turn_on` for all
three bulbs (two calls for RELAX), not six.

**Settings** — the board's own knobs, persisted to NVS.

```
┌────────────────────────────────────────────────┐
│ ⠿   Devices    Scenes  ( Settings )      23:00 │
├────────────────────────────────────────────────┤
│ ╭────────────────────────────────────────────╮ │
│ │ ☀ Brightness                           50% │ │
│ │ [ 1% ][ 25% ][ 50% ][ 75% ][ 100% ]        │ │
│ ╰────────────────────────────────────────────╯ │
│ ╭────────────────────────────────────────────╮ │
│ │ ☾  Night mode                              │ │
│ │    Red display, 1% backlight       (●══)   │ │
│ ╰────────────────────────────────────────────╯ │
│ ╭────────────────────────────────────────────╮ │
│ │ ◷  Night schedule                          │ │
│ │    23:45 - 08:00                   (══●)   │ │
│ ╰────────────────────────────────────────────╯ │
│ ╭────────────────────────────────────────────╮ │
│ │ ↻  Flip screen                             │ │
│ │    Rotate 180 degrees              (══●)   │ │
│ ╰────────────────────────────────────────────╯ │
└────────────────────────────────────────────────┘
```

Night mode turns the entire UI red-only and drops the backlight to 1%. The
schedule *writes* that toggle at 23:45 and 08:00 rather than overriding it — in
between, flipping it by hand always works and sticks until the next boundary. A
reboot inside the window comes up already in night mode.

All four devices refresh together every 1.5 s via a single templated request, so
a change made from the HA app shows up here in about a second (measured
1009–1036 ms).

The top-right shows a 24-hour clock in **Asia/Bangkok**, set over NTP — keyless,
no API account, and the ESP32's SNTP client resyncs itself with no polling code.
It reads `--:--` for the second or two before the first sync lands. Change the
zone via `TZ_INFO` in `include/config.h`; the POSIX sign is inverted, so
`"ICT-7"` means UTC**+**7.

Connection health shows up only when there is nothing good to report: if Home
Assistant becomes unreachable, a red **No Connection** banner takes over the top
row on whichever page you are on, and while everything is working it costs no
pixels at all. It does not distinguish "Wi-Fi is down" from "Home Assistant is
not answering" — either way nothing you tap will reach the house, and the serial
log has the detail if you want it. Individual cards dim when their own data goes
stale, and a card whose last command *failed* flashes a red border. It is a word
rather than a colour, so it survives night mode, which is red-only by design.

The currently-active state is filled in Home Assistant cyan, and it is the only
saturated colour on a resting screen — if everything is highlighted, nothing is.
On a lit bulb a brightness chip *and* a colour swatch can both be active; they are
independent axes, not one choice.

The interface is built on a small design system rather than per-screen styling: 16
semantic colour tokens and four type roles (`src/ui/theme.h`, `src/ui/gfx.h`), one
spacing scale and square corners throughout (the LAYOUT block of
`include/config.h`), and a
component library every control is built from (`src/ui/widgets.cpp`). Text is set
in proportional FreeSans rather than the blocky bitmap fonts these panels usually
use.

## Setup

**1. Fill in secrets.** `include/secrets.h` is gitignored; a template is committed.

```sh
cp include/secrets.h.example include/secrets.h
$EDITOR include/secrets.h
```

You need a **long-lived access token** from
`http://192.168.1.117:8123/profile/security` → "Long-lived access tokens", plus
the four entity IDs. List the candidates with:

```sh
TOKEN='paste-token'
curl -s -H "Authorization: Bearer $TOKEN" http://192.168.1.117:8123/api/states \
  | python3 -c 'import json,sys; [print(e["entity_id"], "|", e["attributes"].get("friendly_name","")) for e in json.load(sys.stdin) if e["entity_id"].startswith(("light.","climate."))]'
```

**2. Verify the assumptions** before flashing — a mismatch here changes the
button map, and it is much easier to debug over curl than over serial:

```sh
# Bulbs: confirm color_temp is supported and check the real kelvin range
curl -s -H "Authorization: Bearer $TOKEN" \
  http://192.168.1.117:8123/api/states/light.YOUR_BULB \
  | python3 -m json.tool | grep -i -A4 'color_modes\|kelvin'

# AC: confirm "cool" and "dry" exist, and read the step / limits
curl -s -H "Authorization: Bearer $TOKEN" \
  http://192.168.1.117:8123/api/states/climate.YOUR_AC \
  | python3 -m json.tool | grep -i 'hvac_modes\|min_temp\|max_temp\|target_temp_step'
```

If `min_color_temp_kelvin`/`max_color_temp_kelvin` differ from 2200/4000, update
`KELVIN_WARM`/`KELVIN_COOL` in `include/config.h`.

**3. Build and flash.** With the CYD on USB:

```sh
pio run                # compile only
pio run -t upload      # build + flash
pio device monitor      # serial log @ 115200
```

Keep `upload_speed = 115200`. Anything faster caused serial corruption while
flashing this board.

**4. Wi-Fi.** Leave `WIFI_SSID`/`WIFI_PASS` empty in `secrets.h` and the device
opens a captive portal on first boot — join it from a phone and enter
credentials. Or hardcode them in `secrets.h` to skip the portal.

> ### ⚠ If the screen shows only the Home Assistant logo and never reaches the controls
>
> It is waiting for Wi-Fi setup. **Join the hotspot `CYD-HA-Setup` from a phone**
> and enter your credentials.
>
> The boot splash is deliberately logo-only with no text, so **nothing on screen
> tells you this** — the portal state and the normal connecting state look
> identical. This README and the serial log (`pio device monitor`) are the only
> places the hotspot name appears. The three pips under the logo cycle while the
> device is working, so a live board is at least distinguishable from a hung one.
> The portal times out after 180 s, after which the device reboots and tries again.

## simulator.html

The fast iteration path for layout work. It re-implements the exact geometry from
`include/config.h` in canvas — all three pages, the tab bar, the night palette and
the flipped tap mapping — flags any clipping or overflow, and its click handling
mirrors `screenHitTest()` (including the AC row's reversed stepper slots). It also
re-checks every `static_assert` from `screen.cpp` and asserts the night palette
keeps every semantically-paired colour apart.

Crucially it carries the **real font metrics**: the `xAdvance` tables are lifted
from TFT_eSPI's font headers and text is drawn character by character at those
advances, so its width measurements are byte-identical to the device's. That is
what makes a "this label will clip" warning worth acting on.

```sh
python3 -m http.server 8765     # then open http://localhost:8765/simulator.html
```

Any state can be deep-linked, which is how a specific case gets reproduced or
screenshotted — `?page=1&night=1&scenes=100&stale=1&ha=0&mode=heat&avail=0`; the
full list is at the bottom of the file. The buttons in the side panel cover the
same ground interactively, including growing the scene table to 24 or 100 entries
to exercise the scroll path that is dead code at five.

Keep its constants in sync with `config.h` — that is the whole point of it.

## Troubleshooting

| Symptom | Fix |
|---|---|
| White screen on boot | You have the dual-USB CYD variant — swap `ILI9341_2_DRIVER` for `ST7789_DRIVER` in `platformio.ini` |
| Whites look cyan, oranges look green | Remove `-D TFT_RGB_ORDER=TFT_BGR` (your panel doesn't swap R/B) |
| Colours inverted | Add `-D TFT_INVERSION_ON=1` |
| **Taps land on the wrong button** | Run the calibration below. Check `TOUCH_SWAP_XY` **first** — on this unit that flag, not the ranges, was the cause. Wrong swap collapses every tap into the left third of the screen, so only buttons 0-2 respond and the AC row is unreachable |
| Taps never register at all | `TOUCH_Z_MIN` too high, or `TOUCH_REQUIRE_IRQ` is 1 on a board whose PENIRQ sticks high — set it to 0 and retest |
| Phantom taps change lights by themselves | `TOUCH_Z_MIN` is inside the noise band. Read the `touch idle: rawZ=..` serial lines for the real noise floor and set the threshold well above it (this unit: noise ~50-130, real contact ~2100-2500) |
| **High-pitched whine at every brightness step except 100%** | Backlight PWM inside the audible band. `BL_PWM_HZ` (`config.h`) must stay above ~20 kHz; it is 25 kHz. 100% is silent even at a bad frequency because `ledcWrite()` promotes duty 255 to a constant DC high, so that step is the tell, not an exception. A whine that persists at 100% too, or during flashing, is not the backlight |
| Backlight won't dim | The Settings brightness only works if LEDC is configured **after** `screenBegin()` — `TFT_eSPI::init()` reclaims `TFT_BL` as a plain output |
| Screen flipped but taps land 180° out | The flip mirrors the *tap*, not the digitiser — check the `if (S.set.flip)` mirror in `handleTouch()`. Never "fix" this by recalibrating while flipped; `CALIB_MODE` forces flip off for that reason |
| Stuck in night mode, screen barely visible | It is 1% backlight by design. The Settings tab is still there — tap it and toggle Night mode off. Turn off Night schedule too, or 23:45 will re-enable it |
| Tapping the right edge of Scenes does nothing | Expected while every scene fits on one screen — the scroll gutter is drawn empty and ignores taps until the table needs a second page |
| Two scene tiles lit at once | Shouldn't happen: an unknown colour temp counts as "cannot confirm", not a match. If it does, a bulb is reporting `kelvin <= 0` while `supportsCT` is true |
| Tab taps miss but card controls are fine | The tab band is above `CAL_INSET`, so the 4-point fit extrapolates there. Re-run `-e calib` and check the tab outlines on the verify screen |
| `HA` dot red | Token wrong/expired, or `HA_HOST` unreachable — check for `ha: POST /api/template -> 401` on serial |
| Colour swatches show `n/a` | The bulb doesn't report `color_temp` support — it's a plain dimmable-white TRADFRI, not white-spectrum |
| A card reads `OFFLINE` | HA itself can't reach that device (Zigbee dropout). Its controls grey out and its icon goes to a red outline deliberately — there is no current state to highlight |
| T+/T- do nothing | Expected until the first successful AC poll lands; they're relative to the current setpoint |

## Touch calibration

The touch mapping is measured, not guessed. To redo it:

```sh
pio run -e calib -t upload     # then tap the 4 crosshairs on screen
pio device monitor             # reads back a ready-to-paste TOUCH_* block
```

Tap the centre of each crosshair (top-left, top-right, bottom-right,
bottom-left). The firmware fits the four points, prints the constants, and then
shows a **verify** screen drawing the real button grid — tap boxes and confirm
the dot lands inside before committing to the values. Paste the printed block
into `include/config.h`, then reflash the normal firmware with `pio run -t upload`.

> `env:calib` has **no Wi-Fi and no Home Assistant** — it cannot touch your
> lights, so tap freely. But the device is inert as a controller until you flash
> `env:cyd` again.

Current values were verified across all 23 buttons with sub-pixel residuals.

## Roadmap

- **WebSocket API** (`subscribe_entities`) instead of polling — would cut
  external-change latency from ~6s to instant.
- **LDR auto-dim** on GPIO34 for night use (calibration constants already exist
  in the sibling `btcticker-cyd` project).
- **Burn-in pixel-shift** if left on 24/7.
