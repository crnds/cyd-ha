# cyd-ha — Home Assistant bedroom controller

Firmware turning a CYD "Cheap Yellow Display" (ESP32-2432S028R) into a one-tap
wall/nightstand controller for four bedroom devices. No phone, no app, no
scrolling — every action is a single button on a fixed 320×240 screen, and the
screen doubles as a status display.

```
┌────────────────────────────────────────────────┐
│ ●WIFI ●HA                              2s ago │
├────────────────────────────────────────────────┤
│ TRADFRI BULB 1                     30%  2700K │
│ [OFF][ 1%][30%][100%][2200K][4000K]           │
│ TRADFRI BULB 2                            OFF │
│ [OFF][ 1%][30%][100%][2200K][4000K]           │
│ TRADFRI BULB 3                    100%  4000K │
│ [OFF][ 1%][30%][100%][2200K][4000K]           │
│ SENSIBO SKY AC          COOL  set 24  room 27 │
│ [ OFF ][ AC  ][ DRY ][ T+  ][ T-  ]           │
└────────────────────────────────────────────────┘
```

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
opens a captive-portal AP named `CYD-HA-Setup` on first boot — join it from a
phone and enter credentials. Or hardcode them in `secrets.h` to skip the portal.

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
| Taps land on the wrong button | Read the `touch dbg: z=.. raw=..` serial lines and retune `TOUCH_X_MIN/MAX`, `TOUCH_Y_MIN/MAX` in `config.h` |
| Taps never register | Lower `TOUCH_Z_MIN` (idle noise sits ~50-80, a firm tap is 150+) |
| `HA` dot red | Token wrong/expired, or `HA_HOST` unreachable — check the `ha: GET ... -> 401` line on serial |
| Colour swatches show `n/a` | The bulb doesn't report `color_temp` support — it's a plain dimmable-white TRADFRI, not white-spectrum |
| T+/T- do nothing | Expected until the first successful AC poll lands; they're relative to the current setpoint |

## Roadmap

- **WebSocket API** (`subscribe_entities`) instead of polling — would cut
  external-change latency from ~6s to instant.
- **LDR auto-dim** on GPIO34 for night use (calibration constants already exist
  in the sibling `btcticker-cyd` project).
- **Burn-in pixel-shift** if left on 24/7.
