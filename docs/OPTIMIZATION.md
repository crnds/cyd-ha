# cyd-ha — audit findings and optimization plan

> **Status: applied 2026-08-05.** P0–P4 are implemented, flashed and verified on
> hardware; see §5 for what was measured and what remains open. The findings
> below are kept as written for the record — they explain *why* the code looks
> the way it does, and several are the reasoning behind comments in the source.
> P5 (WebSocket) is deliberately **not** done.
>
> **A second, structural pass followed on 2026-09-04** — dedup, a file split,
> `main.cpp` restructuring — with **no hardware attached**, so it could only
> be applied and verified host-side (`pio run` + `tools/host_check` +
> headless `simulator.html`); see §6. Visual/tap/timing verification on the
> device is still outstanding for that pass specifically.

Audit date: 2026-08-05. Firmware state: flashed and working on hardware
(`/dev/cu.usbserial-110`), touch calibrated, 18 real taps with 0 HA errors.

Resource headroom is **not** a constraint — RAM 47.7 KB / 320 KB (14.5%), flash
971 KB / 3 MB (30.9%). So nothing here optimizes for size. The targets are
**latency, responsiveness under failure, and correctness**.

---

## 1. Confirmed bugs

Ranked by user-visible impact. Each has evidence, not suspicion.

### B1 — UI freezes for up to 8 s per tap when HA is unreachable  ⚠ highest impact

`HTTP_TIMEOUT_MS` is **4000**, applied to *both* `setConnectTimeout()` and
`setTimeout()` (`ha.cpp:24-25`). Every HA call is blocking, and both callers run
inside `loop()` — `servicePoll()` (`main.cpp:153`) and `doAction()`
(`main.cpp:234-239`). While one blocks, `handleTouch()` and `screenRender()` do
not run at all, so the screen is frozen and taps are dropped.

Worst case is connect-timeout + read-timeout = **8 s of dead UI per tap**.

Measured LAN latency for comparison: **16–64 ms**. The timeout is 60–250× the
observed round trip, buying nothing.

### B2 — `BL_DUTY` does nothing; the backlight is permanently at 100%

`setup()` configures LEDC on `PIN_BACKLIGHT` (`main.cpp:490-492`) *before*
`screenBegin()` (`main.cpp:511`). `screenBegin()` calls `tft.init()`, and
`TFT_eSPI::init()` — confirmed at `TFT_eSPI.cpp:786-789`, inside the function
starting at line 611 — does:

```cpp
#if defined (TFT_BL) && defined (TFT_BACKLIGHT_ON)
  if (TFT_BL >= 0) { pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, TFT_BACKLIGHT_ON); }
```

That reclaims GPIO 21 as a plain output driven HIGH, detaching the PWM. So
`BL_DUTY = 230` (intended ~90%) is silently ignored. Matters for a bedroom
device, and blocks any future night-dimming work.

### B3 — `readTouch()`'s comment now contradicts the code

`main.cpp:60-63` states *"the 0xD1 (Y-cmd) samples feed the screen X axis and
0x91 (X-cmd) samples feed screen Y"*. With `TOUCH_SWAP_XY = 1` the opposite is
true: screen X comes from the 0x91 channel. This is the single trickiest
function in the project and the comment actively misleads on the thing that took
three flash cycles to find.

### B4 — `ts.setCalibration()` is dead, and wrong if ever revived

`main.cpp:495` passes `TOUCH_X_MIN/MAX` to the XPT2046 library. Nothing reads
the library's mapping — every sample goes through the hand-rolled `xptRead()`.
Post-swap those constants belong to the *other* channel, so the call is not
merely dead but would map incorrectly if someone later switched to `ts.get*()`.

### B5 — `HA` dot stays green after Wi-Fi drops

`loop()` only calls `servicePoll()` when `S.netState == 1` (`main.cpp:523`), and
`S.haOk` is only ever written by `markResult()` (`ha.cpp:13-16`). With Wi-Fi
down, nothing clears `haOk`, so the bar shows `● WIFI` red beside `● HA` green —
contradictory, and it hides the real fault.

### B6 — An `unavailable` bulb renders as plainly OFF

`parseLight()` does `d.on = (strcmp(st, "on") == 0)` (`ha.cpp:55`), so
`unavailable` and `unknown` both collapse to `on == false`. The row then prints
`OFF` and lights the OFF button as the active state — indistinguishable from a
healthy bulb that is genuinely off. `parseClimate()` avoids this only by
accident, because `prettyMode()` happens to special-case `"unavailable"`
(`screen.cpp:175`).

### B7 — `d5 = xptRead(0x00)` feeds garbage into the screen-X average

`main.cpp:84` issues a null command after power-down, then `main.cpp:101` feeds
its result into `avg2(d1, d3, d5)`. It survives only because `avg2()` picks the
closest *pair* and `d5` is a far outlier. Post-swap this is the channel driving
**screen X**. Verified accurate today (<0.6 px residuals), so this is latent
fragility rather than an active fault — but a third real X sample would be
strictly better than a discarded one.

### B8 — `simulator.html` has drifted from `config.h`

`CLAUDE.md` states keeping them in sync is the whole point of the file. Current
drift:

| | config.h | simulator.html |
|---|---|---|
| `KELVIN_WARM` | 2202 | 2200 |
| status bar | `LIVE` | `'2s ago'` |

### B9 — Docs don't mention either hard-won fix

`grep` finds **zero** references to `env:calib` or `TOUCH_SWAP_XY` in `README.md`
or `CLAUDE.md`. The README troubleshooting row for "taps land on the wrong
button" still says only *"retune TOUCH_X_MIN/MAX"* — which is precisely the wrong
advice, since the ranges were never the problem.

### B10 — Not a git repository; the token is unprotected

`git rev-parse` fails: there is no repo. So `.gitignore` is inert, and
`include/secrets.h` — holding a **183-char long-lived token with full HA
control** — has no protection against a future careless `git add -A`. There is
also no history for a firmware that has now been through four flash cycles.
Separately, `calib.log` is not in `.gitignore` (only `monitor.log` is).

---

## 2. Behaviour still unverified on hardware

Stated plainly because none of this has been observed working:

- **Heap stability.** Two `JsonDocument`s are allocated and freed per poll
  (`ha.cpp:108-124`), ~40 polls/min. No `ESP.getFreeHeap()` logging exists, so
  fragmentation over hours is unmeasured.
- **The reconcile poll actually correcting a wrong optimistic guess.** The happy
  path was exercised; a divergence between guess and truth was not.
- **Stale dimming** after `DEVICE_STALE_MS` (20 s).
- **External-change latency** — a change made in the HA app appearing on screen.
- **`T+`/`T-` stepping and clamping.** Taps registered as `AC btn 3`, but the
  setpoint was never confirmed to move exactly one step or clamp at 18/31.
- **Rollback path** (`main.cpp:247`) on a failed service call.

---

## 3. Optimization plan

### P0 — Correctness and responsiveness (do first)

**P0.1 Cut the blocking-I/O freeze (B1).**
- `HTTP_TIMEOUT_MS` 4000 → **1200** connect / **1500** read. Still ~20× the
  measured 64 ms worst case.
- Add a circuit breaker: when `S.haOk` is false and the last failure was under
  `RETRY_BASE_MS` ago, have `doAction()` skip the HTTP call entirely and just
  flash `errMs`. A tap should never cost more than one timeout.
- Verify by pulling HA off the network and confirming taps still repaint
  immediately.

**P0.2 Fix the backlight (B2).** Move the `ledcSetup`/`ledcAttachPin`/`ledcWrite`
block to *after* `screenBegin()`. Verify by setting `BL_DUTY` to 60 and
confirming the screen visibly dims.

**P0.3 Clear `haOk` when Wi-Fi drops (B5).** One line in `updateNetState()`:
`if (!up) S.haOk = false;`

**P0.4 Represent unavailable devices honestly (B6).** Add
`bool avail` to `DeviceState`; set false on `unavailable`/`unknown`. Render the
row's state text as `UNAVAIL` in `C_RED` and grey every button so no button
falsely reads as the current state.

**P0.5 Fix the misleading comment and delete the dead call (B3, B4).**

### P1 — One templated request instead of four  ← biggest measured win

Replace the 4× round-robin `GET /api/states/<id>` with a single
`POST /api/template` that returns only the fields actually rendered.

**Measured against the live instance (best of 7 runs):**

| | requests | latency | payload |
|---|---|---|---|
| current | 4 | 64.0 ms | 2403 B |
| templated | 1 | **16.1 ms** | **43 B** |

**55.9× less data, 4.0× lower latency, 4× fewer requests.** And because all four
devices arrive in one response, per-device freshness improves from
`4 × HA_POLL_MS = 6000 ms` to **1500 ms — 4× fresher**, which directly fixes the
"external change takes 6 s to show up" complaint without a WebSocket rewrite.

Validated payload shape:

```
on,3,2202;off,0,0;off,0,0;off,29,27,18,31,1
```

Three light triples (`state,brightness,kelvin`) then the climate sextuple
(`state,target,room,min,max,step`), semicolon-delimited.

Secondary win: this parses with `strtok`/`sscanf`, so **ArduinoJson leaves the
poll path entirely** — eliminating both per-poll heap allocations and with them
the fragmentation risk in §2.

Implementation notes:
- Build the template string with `snprintf` from the `ENT_*` macros so entity IDs
  are not duplicated between `secrets.h` and the template.
- A bulb that is off reports `0` for brightness/kelvin. Keep the existing
  "only update when > 0" rule so last-known values survive (`ha.cpp:59-66`).
- Keep `haPollDevice()` for the reconcile path, or reuse the same template and
  refresh all four — the latter is simpler and now costs only 16 ms.

### P2 — Per-button dirty tracking

`drawRow()` repaints **all six buttons** whenever any single field changes
(`screen.cpp:228` gates the whole row). Each button is a `fillRoundRect` +
`drawRoundRect` + text. Since `activeMask` already encodes per-button state,
store the previous mask and repaint only the bits that changed. Cuts a
brightness change from 6 button redraws to 2.

Also cheap: `drawFittedLabel()` calls `tft.textWidth()` on every draw
(`screen.cpp:115-116`) for labels that are compile-time constant. Measure once at
boot into a small table.

### P3 — Repair the safety net

- `git init`, commit, and confirm `include/secrets.h` is ignored **before** the
  first `git add`. Add `calib.log` (or just `*.log`) to `.gitignore` (B10).
- Sync `simulator.html` constants and the status-bar text (B8).
- Update `README.md` and `CLAUDE.md`: document `pio run -e calib -t upload`, the
  4-crosshair flow, and that `TOUCH_SWAP_XY` — not the ranges — is the usual
  cause of mis-landing taps. Fix the misleading troubleshooting row (B9).

### P4 — Fit for a 24/7 bedroom appliance

- **Night dimming** via the LDR on GPIO 34 (`PIN_LDR` is defined but unused).
  Depends on P0.2 — pointless while the backlight is stuck at 100%.
- **Heap logging**, then a one-hour soak to close out §2.
- **Daily restart** at a quiet hour, as `btcticker-cyd` does
  (`DAILY_RESTART_HOUR`), to make heap drift a non-issue on an always-on device.
- **Calibration without reflashing** — hold a touch during boot to enter
  `calibRun()`, so recalibration doesn't require a 66 s flash and a trip through
  `env:calib` (which has no Wi-Fi and leaves the device inert as a controller).

Explicitly *not* recommended: btcticker's burn-in pixel-shift. That panel is an
ILI9341 **LCD**, not OLED — permanent burn-in is not a real failure mode here.

### P5 — WebSocket push (only if P1 proves insufficient)

`subscribe_entities` over HA's WebSocket API would make updates instant instead
of 1.5 s and drop steady-state traffic to near zero. But P1 already delivers a 4×
freshness improvement for a fraction of the work, so this is worth doing only if
1.5 s still feels laggy in practice. Revisit after P1 has run for a while.

---

## 4. Suggested order

1. **P0** — real bugs, all small, one flash cycle.
2. **P3 git init** — before any further edits, so there's a rollback point.
3. **P1** — the measured 4×/56× win, and it removes the heap risk.
4. **P4 heap logging + soak** — closes the unverified list.
5. **P2**, then remaining **P3** docs, then reassess **P5**.

---

## 5. Outcome

`git init` was done **first**, not second, so every phase below has a rollback
point. Branch: `apply-optimization-plan`, off `main` at the baseline commit.

### Applied

| Item | Result |
|---|---|
| **B1** blocking-I/O freeze | `HTTP_CONNECT_MS` 1200 / `HTTP_READ_MS` 1500 + `haBreakerOpen()`. Worst-case tap freeze 8 s → ~1.2 s. **Verified against a blackholed host** — see below |
| **B2** dead `BL_DUTY` | LEDC setup moved after `screenBegin()`. **Verified visually**: `BL_DUTY` 230 → 120 produced a clearly dimmer panel, which it could not have done while the PWM was detached. Left at 120 (~47%) as the bedroom default |
| **B3/B4** stale comment, dead call | `readTouch()` axis mapping documented correctly; `ts.setCalibration()` removed |
| **B5** `haOk` stuck green | cleared in `updateNetState()` when Wi-Fi drops |
| **B6** unavailable → false OFF | `DeviceState::avail`; row renders `UNAVAILABLE` in red, all buttons greyed. Verified in the simulator |
| **B8** simulator drift | constants synced, swatch captions derived from `KELVIN_*`, `LIVE` text, plus an unavailable toggle so the new state is checkable |
| **B9** docs | `README` calibration section + corrected troubleshooting; `CLAUDE.md` covers the swap finding, the template constraints, the backlight trap and the `avail` rule |
| **B10** no repo | `git init`, `*.log` ignored, token confirmed unstaged by scanning every staged file for it |
| **P1** one templated request | 64.0 ms/2403 B → **14.2 ms/50 B**. Poll path is now allocation-free (no ArduinoJson) |
| **P2** per-button dirty tracking | buttons compared by visual state; a brightness change repaints 2 buttons, not 6 |
| **P4** heap logging | `logHeap()` every 30 s |

### Measured after the change

- **External-change latency: 1009–1036 ms** (was a 6000 ms worst case). Verified by
  driving `light.bulb_1` to 1% / 100% / 30% over the API and timing the device's
  own log line.
- **Heap: not yet proven flat.** Over 62 s it looked stable (244016 → 244044 →
  243772), but a later sample at `up=422s` read 243608 — roughly −400 B across
  7 minutes. That is either settling or a slow leak of ~60 B/min, and 62 s of
  data cannot tell the two apart. `getMaxAllocHeap` is steady at 110580, which
  argues against fragmentation. A 25-minute soak is running; see §6.
  The earlier "flat" reading was over too short a window to claim.
- **Template parse cross-checked against HA's own API**, not merely self-consistent:
  brightness matched, AC setpoint `29.0` vs HA `29` (so the tenths encoding
  round-trips), AC limits `18.0/31.0` matched exactly.
- **17/17 functional checks passed** on a from-clean rebuild and flash: boot,
  Wi-Fi, first poll inside 2.6 s, template shape, external-change latency, and
  25 s of idle with zero errors, zero phantom taps and zero redundant state logs.
- **B1 verified against a blackholed host** (`192.168.1.240`, confirmed unused —
  ARP incomplete). Failed-poll intervals: **1.27 → 2.00 → 4.02 → 8.01 → 16.01 s**.
  The 1.27 s floor *is* the connect timeout; with the old 4000 ms it could not
  have been under 4 s. Backoff doubling confirmed.
  Incidental finding: backoff is measured from call **start**, not completion, so
  the effective gap is `max(backoff, callDuration)`. That is why the first
  interval is 1.27 s rather than the ~2.2 s a naive reading predicts. Not a bug.
- Builds clean from scratch, both `env:cyd` and `env:calib`. RAM 15.1%, flash 31.1%.

### Deliberately not done

- **P5 WebSocket.** P1 already brought external-change latency to ~1 s. Revisit
  only if that proves laggy.
- **P4 night dimming / daily restart / boot-gesture calibration.** Unblocked now
  that B2 is fixed, but they add behaviour rather than fixing anything.
- **B7** the `xptRead(0x00)` garbage sample still feeds `avg2()`. Latent
  fragility only — verified accurate to <0.6 px — and touching the verified-good
  touch path for cosmetics is a poor trade.

### Still unverified

All of these need either a finger on the glass or HA taken offline mid-run, so
none is reachable from a script.

- **Long soak.** Every window measured so far is minutes, not hours. Heap has
  gone *up* as often as down (it rose during the blackhole test) and `largest`
  has never moved off 110580, which argues against both a leak and
  fragmentation — but that is an argument, not a measurement.
- **`haBreakerOpen()`'s tap path.** The *timeout* and *backoff* are now measured,
  but the breaker's purpose — a tap failing fast instead of paying a timeout —
  requires tapping while HA is unreachable.
- **Stale dimming** at `DEVICE_STALE_MS`. Needs a device to go stale *after* a
  successful poll; booting against a dead host leaves `known == false`, so the
  row shows `--` and the stale path is never entered.
- **Rollback path** on a failed service call. Same requirement as the breaker.
- **`T+`/`T-`** stepping by exactly one step and clamping at 18/31.
- **`UNAVAILABLE` rendering on real hardware.** Verified in the simulator only;
  no device has actually dropped off the Zigbee mesh during testing.

---

## 6. Structural refactor (branch `refactor-structure`)

Date: 2026-09-04. A second pass, deliberately separate from §1–5: this repo's
resource headroom was never the constraint (see the top of this doc), and the
large latency wins were already taken by §5's P1. What §5 never touched was
structure — `screen.cpp` was 1652 lines with a 170-line `drawDeviceCard()`,
`main.cpp` had a 129-line `handleTouch()` and a 105-line `doAction()` carrying
three switches over one enum, and `src/net/ha.cpp` still dragged in
ArduinoJson for a code path with no callers. This pass targets structure and
duplication, not size or the network path — explicitly **not** the one
remaining large lever (non-blocking HTTP), which is a behaviour change and
stays a follow-up.

**No CYD was attached for this pass** (`/dev/cu.usbserial-*` did not exist),
unlike §5's audit. That is why `tools/host_check/` (see its own README) was
built first: it is the only regression net available for the C++
dirty-region/derivation logic without hardware, since `simulator.html`
mirrors geometry constants rather than that logic. Everything below was
verified through `pio run` (both envs, every `static_assert`), that harness
(`--diff` against a golden captured before touching `screen.cpp`), and a
headless `simulator.html` render — never on the device itself. Visual
pixel-identity, tap accuracy, and the press-flash/night-mode timing paths are
therefore still hardware-only and unverified, exactly as this section's
predecessor once was.

### Applied

| Item | Result |
|---|---|
| Dead single-entity poll path | `haPollDevice()`/`parseLight()`/`parseClimate()` had no callers anywhere in `src/`; deleted along with the `ArduinoJson` dependency (`platformio.ini`). `DeviceState::supportsCT` is now documented as permanently `true` in the shipped firmware, since its only writer was the deleted path |
| Raw-input pre-filter | `CardFingerprint`/`RoomFingerprint` (`screen_int.h`) let `drawDeviceCard()`/`drawStatusRoom()` return before deriving anything when nothing changed, not just before drawing. See `CLAUDE.md`'s "Rendering" section for the full mechanism and the NaN/`memcmp` caution |
| Dedup in `screen.cpp` | `staleErr()`/`alarmFg()`/`clearHeaderRegion()` helpers, `briRect`/`nightRect` collapsed into one `settingChipRect(row, i, ...)`, `btnRect`/`settingChipRect` return a `Rect{x,y,w,h}` instead of four out-params, 7 label tables made `const char* const` |
| File split | `screen.cpp` → `screen.cpp` (core) + `screen_devices.cpp` + `screen_scenes.cpp` + `screen_settings.cpp` + `screen_int.h`. See `CLAUDE.md`'s "Rendering" section for the file-ownership table |
| `main.cpp` structure | `doAction()`'s button-decode extracted to `decodeAction()` (not table-driven — real per-button side effects); `handleTouch()` split into `sampleTouch()`/`dispatchHit()`; one `getLocalTime()` per `loop()` pass via a shared `hhmm` parameter and a new `screenSetClock()` setter; `doSetting()`'s two toggle rows (not all four — the other two are index-setters, a different shape) collapsed into `toggleSetting()` |
| Stale comments | `config.h`'s `HA_POLL_MS` comment still described the pre-P1 round-robin poll; `ha.cpp`'s request-size comment was measured against different entity IDs (879 vs the current 958, computed from this repo's own `secrets.h`); `main.cpp`'s scene-rollback stack comment said ~204 B, actual `sizeof(DeviceState)` on this target is 72 B (measured via the real `xtensa-esp32-elf-g++`) so 216 B; `CLAUDE.md` stated `HTTP_READ_SVC_MS` as 2500 in two places where `config.h` has held 1500 (with its own explanatory comment) since before this pass — the reasoning for the reversion was undocumented in `CLAUDE.md` until now |

### Measured after the change

- **Host harness: byte-for-byte match** against the pre-refactor golden after
  every step (`tools/host_check/build.sh --diff`), including an
  exhaustive 320×240×3-page `screenHitTest()` sweep (hashed) and
  draw-call-delta checks proving unchanged inputs draw nothing and a changed
  field draws something — added a press-flash cycle to that coverage
  specifically before touching `drawDeviceCard()`'s own `pressedNow()` call,
  since no prior sweep had ever exercised `S.pressKind != HIT_NONE`.
- **Flash, step by step:** baseline 1,001,517 B → ArduinoJson removal
  1,000,565 B → pre-filter 1,000,825 B → dedup 1,000,785 B → file split
  1,001,521 B → `main.cpp` restructure 1,001,529 B. **Net: +12 B** across the
  whole pass — the file split's ~736 B inlining loss came out almost exactly
  offset by dead-code removal and dedup elsewhere. RAM: 50,056 → 50,208 B
  (+152 B, the two fingerprint structs; unchanged since).
- **`pio run` clean on both envs at every step**, no new compiler warnings
  beyond the pre-existing `TOUCH_CS` notice from `TFT_eSPI.h` (expected — see
  this repo's touch-path notes).
- **`simulator.html` re-checked headlessly** (Chromium `--headless=new`,
  screenshotted) after every step that could plausibly affect it; unaffected,
  since no step here changed `config.h`'s LAYOUT block or `simulator.html`
  itself except mirroring the new `ERR_FLASH_MS` constant.

### Deliberately not done

- **Non-blocking HTTP.** The actual dominant responsiveness cost — up to
  ~2.7 s frozen per poll, ~5.4 s for a two-call scene — is the blocking HTTP
  calls inside `loop()`, not per-frame CPU. Fixing that is a behaviour
  change and was out of scope for a "zero behaviour change" pass; it remains
  the largest real lever and belongs to a future pass, not this one.
- **Deadline-based `loop()` pacing** instead of the unconditional `delay(20)`.
  Changes frame timing, so out of scope under the same constraint.
- **A table-driven `doAction()`/`doSetting()`.** Both were examined and
  rejected: `doAction()`'s AC-stepper branch carries real side effects
  (`errMs` stamps, distinct log lines, the `fabsf` at-limit check) a table
  would need an escape hatch per row for, and `doSetting()`'s four rows are
  two genuinely different shapes (index-setters vs plain toggles), not four
  uniform arms — see `main.cpp`'s comments at each site for the specific
  reasoning.
- **Narrowing `int pct`/`kelvin`/`humidity` to `int16_t`.** Saves ~24 B of a
  72 B struct and risks signedness bugs at every comparison site — not worth
  it under a resource budget that was never tight.

### Still unverified

All of these need the device physically on the desk, which it was not for
this entire pass.

- **Visual pixel-identity.** The host harness proves the C++ logic is
  unchanged; it does not draw a single pixel. `simulator.html`'s headless
  render is geometry-accurate but font-approximate for anything the harness
  doesn't already cover structurally.
- **Tap accuracy** after the `btnRect`/`settingChipRect` signature change and
  the file split moving `screenHitTest()`'s callees across TU boundaries.
  `tools/host_check`'s exhaustive hit-test sweep proves the *mapping* is
  byte-identical; it says nothing about the physical touch path, which this
  pass did not touch at all (`readTouch()`/`xptWrite()`/`xptRead()` are
  untouched — confirmed by `git diff` producing nothing in that region for
  every commit in this pass).
- **Press-flash and night-mode timing** on the real 20 Hz touch-poll /
  20 ms render cadence. The harness controls a simulated `millis()`
  precisely, which proves the *logic* is unchanged but cannot stand in for
  real timing jitter, a real finger, or a real PENIRQ line.
