# cyd-ha — Home Assistant bedroom controller

Firmware turning a CYD "Cheap Yellow Display" (ESP32-2432S028R) into a one-tap
wall/nightstand controller for four bedroom devices. No phone, no app, no
scrolling — every action is a single button on a fixed 320×240 screen, and the
screen doubles as a status display.

```
┌────────────────────────────────────────────────┐
│ ●WIFI ●HA                                LIVE │
├────────────────────────────────────────────────┤
│ TRADFRI BULB 1                     30%  2700K │
│ [OFF][ 1%][30%][100%][2202K][4000K]           │
│ TRADFRI BULB 2                            OFF │
│ [OFF][ 1%][30%][100%][2202K][4000K]           │
│ TRADFRI BULB 3                    100%  4000K │
│ [OFF][ 1%][30%][100%][2202K][4000K]           │
│ SENSIBO SKY AC          COOL  set 24  room 27 │
│ [ OFF ][ AC  ][ DRY ][ T+  ][ T-  ]           │
└────────────────────────────────────────────────┘
```

All four devices refresh together every 1.5 s via a single templated request, so
a change made from the HA app shows up here in about a second (measured
1009–1036 ms). The right-hand readout stays `LIVE` while polling is healthy and
only becomes an elapsed time once data actually goes stale.

The currently-active state is filled in Home Assistant cyan. On a lit bulb a
brightness button *and* a colour swatch can both be active — they are
independent axes, not one choice.

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
> places the hotspot name appears. The portal times out after 180 s, after which
> the device reboots and tries again.

## simulator.html

The fast iteration path for layout work. It re-implements the exact geometry
from `include/config.h` in canvas, flags any clipping or overflow, and its click
handling mirrors `screenHitTest()`. Validate layout changes here before flashing:

```sh
python3 -m http.server 8765     # then open http://localhost:8765/simulator.html
```

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
| Backlight won't dim | `BL_DUTY` only works if LEDC is configured **after** `screenBegin()` — `TFT_eSPI::init()` reclaims `TFT_BL` as a plain output |
| `HA` dot red | Token wrong/expired, or `HA_HOST` unreachable — check for `ha: POST /api/template -> 401` on serial |
| Colour swatches show `n/a` | The bulb doesn't report `color_temp` support — it's a plain dimmable-white TRADFRI, not white-spectrum |
| A row reads `UNAVAILABLE` | HA itself can't reach that device (Zigbee dropout). Buttons grey out deliberately — there is no current state to highlight |
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
