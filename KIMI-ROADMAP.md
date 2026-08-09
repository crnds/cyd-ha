# Idea catalog — UI/UX + performance for cyd-ha

Goal: a grounded menu of improvements, each tied to real code, with effort/risk and a
suggested execution order. Constraints honoured: 3 pages, no sub-pages, one tap acts
everywhere, scrolling only on Scenes, single loop() task (no RTOS), and per
docs/OPTIMIZATION.md the targets are latency, responsiveness under failure,
correctness — not RAM/flash (both have ample headroom).

Note on tree state: the working tree still carries an uncommitted redesign (built-in
bitmap fonts, square corners, the unfilled-Devices-card change) plus everything below
that has since shipped on top of it. Idea C0 covers committing all of it. Nothing in
this file has been committed to git yet — A1/A2/A3/A4/B6 are flashed and verified on
hardware, but still working-tree changes only.

---

## A. Performance & responsiveness

### A1 — Make the poll path actually allocation-free — **DONE**

haPollAll() still does String resp = http.getString() (src/net/ha.cpp:140) — a
heap alloc/free every 1.5 s (~40/min), then copies into a fixed buffer anyway.
OPTIMIZATION.md §5 claims "poll path is now allocation-free"; this was the one
exception that made the claim (and the soak hypothesis) untestable.
- Fix: read http.getStreamPtr()->readBytes() straight into char buf[192], drop
  the String and the copy. ~10 lines in ha.cpp.
- Effort: tiny. Risk: none new — same timeout, same parse.
- **Shipped as:** `haPollAll()` now bounds the read with `http.getSize()` (trustworthy
  because `useHTTP10(true)` keeps the response unchunked) and reads exactly that many
  bytes via `getStreamPtr()->readBytes()` into `buf[192]` — no `String`, no double
  buffer. An implausible/missing size or a short read is treated as a failed poll
  (same retry/backoff path as any other failure), never a blocking wait.

### A2 — Cap the worst-case tap freeze at ~1.2 s instead of 2.5 s — **DONE**

Every HA call blocks loop(). HTTP_READ_SVC_MS is 2500 (include/config.h:39),
tuned because real Sensibo changes take 1135–1643 ms. But haPostService()
already treats a read timeout as delivered (src/net/ha.cpp:54-65) and the
reconcile poll establishes truth either way — so the 2500 only buys a cleaner log
line, paid for with a frozen screen. Cutting to ~1500 halves the worst-case freeze
and shortens time-to-truth on slow climate calls (1.5 s + 0.9 s reconcile vs
2.5 s + 0.9 s).
- Fix: one #define. Verify by stepping the AC with the serial log open.
- Effort: trivial. Trade-off: more "read-timeout (assuming delivered)" log lines.
- **Shipped as:** `HTTP_READ_SVC_MS` 2500 → 1500 in `include/config.h`, comment updated
  with the rationale. Not yet re-verified against a live AC setpoint step with the
  serial log open — worth doing before calling this fully closed.

### A3 — Gate the touch bit-bang on PENIRQ when idle — **DONE**

handleTouch() runs the full 3-sample bit-bang (~1.5 ms) every 50 ms even when
nothing is touching the panel (src/main.cpp:498-503). PENIRQ already has to agree
for a tap to be accepted (TOUCH_REQUIRE_IRQ), so when IRQ reads high the whole
read can be skipped for one digitalRead. Keep a throttled idle sample (the
existing ~5 s "touch idle" log) so the noise floor stays visible.
- Fix: small, in main.cpp. Gain: ~3% CPU and idle SPI quiet. Risk: low — the
  accept path is unchanged; first-contact detection is IRQ's job already.
- **Shipped as:** a cheap pre-check at the top of `handleTouch()` — `digitalRead
  (PIN_TOUCH_IRQ) == HIGH` skips `readTouch()` entirely (resetting `wasDown`) unless
  ~5 s have passed since the last debug log, in which case one full read still runs
  so `TOUCH_Z_MIN`'s noise floor stays visible. The existing post-read IRQ re-check
  (the one that actually gates tap acceptance) is untouched. Verified on hardware:
  touch still responds normally (tab/scene taps logged correctly after flashing).

### A4 — Reliability: optional daily restart (from P4, still open) — **DONE**

The one multi-minute heap sample drifted −400 B (OPTIMIZATION.md §5); hours-long
soaks are still unproven. An ESP.restart() at a quiet hour (e.g. 05:30, inside
the night window) makes slow drift a non-issue for a 24/7 appliance — the pattern
btcticker-cyd already uses. Boot is ~3 s to a usable UI.
- Fix: trivial (mirror serviceNightSchedule()'s edge-trigger). Risk: none
  real — restart while HA is down costs nothing.
- **Shipped as:** `serviceDailyRestart()` in `main.cpp`, a new `RESTART_MIN` (05:30) in
  `config.h`, edge-triggered the same way `serviceNightSchedule()` is — including the
  "missed boundary because loop() was blocked" catch-up clause — except the first
  valid clock reading only adopts the clock rather than restarting immediately.
  Not yet soak-verified (that requires actually leaving the board running to
  05:30), but the trigger logic is in place and building clean.

### Parked, with reasons

- HTTP keep-alive (HTTP/1.1 + setReuse): removes one TCP handshake per poll,
  but useHTTP10 was chosen so responses arrive unchunked and stream-parseable;
  1.1 reopens that question for a ~5 ms/poll win. Not worth it.
- P5 WebSocket push: still deliberately deferred — ~1 s external-change
  latency has held up.
- Async/RTOS networking: ends the freeze class entirely but breaks the
  single-loop no-locking rule the whole codebase assumes. A2 gets most of it.

---

## B. UI/UX

### B1 — Ramp the backlight through night transitions

At 23:45 the palette snaps to red-only and the duty jumps to 1% in one frame —
a hard cut in a dark bedroom. The RGB565 ladder can't crossfade, but the
backlight can: step the duty toward its target every ~30 ms over ~1.5–2 s
(non-blocking, driven from loop(); applyBacklight() at src/main.cpp:179 is
already the single choke point). Same ramp serves manual Brightness changes.
- Effort: small, main.cpp only. High perceived-polish value.

### B2 — Hold-to-repeat on the AC chevrons

One tap = one step, debounced at 350 ms — moving 24°→18° is six deliberate taps.
Hold a chevron ≥ ~600 ms and it repeats at ~4 Hz until release. Restricted to
HIT_ROW on AC_BTN_TUP/TDN so nothing else changes character; doAction()'s
breaker and clamp logic already make each repeat safe.
- Effort: medium-small — handleTouch() (src/main.cpp, now ~560-570 after A3's
  early-out was added) currently returns early on wasDown; it needs to track
  the held hit and re-fire on cadence.

### B3 — Put the WiFiManager hotspot name on screen

If the portal opens, its name appears only on serial (src/main.cpp:648-656 — the
comment calls this out explicitly). Add screenPortal() to screen.cpp (one text
line under the splash pips: "Join CYD-HA-Setup to configure Wi-Fi") so a failed
reconnect is actionable from the glass. Keeps the layering rule (main.cpp never
touches tft).
- Effort: tiny. Value: turns a head-scratcher into a self-explaining state.

### B4 — LDR auto-brightness (P4, unblocked since the backlight fix)

GPIO 34 photoresistor is wired and unused (include/config.h:65). Map its reading
to the existing 5 duty steps with hysteresis. Open design question: the
Settings page tiles exactly 4 rows — an "Auto" mode needs a UI decision (replace
the Night-schedule row? a 5th row breaks the exact tiling?). Worth doing only
after that call is made.
- Effort: medium. Deferred pending the settings-layout decision.

### B5 — Boot-gesture calibration (P4)

Hold a corner during boot to enter calibRun() in the real firmware, instead of
flashing env:calib (66 s + a controller that does nothing else). Requires
CALIB_MODE's code to compile into env:cyd behind the gesture.
- Effort: medium. Nice insurance for drift; not urgent — calibration is stable.

### B6 — Humidity on the AC card (if wanted) — **DONE**

Sensibo reports current_humidity; the template doesn't fetch it. "Room 27°"
could read "27° · 60%". One template field + one sscanf field + width check
against the name-truncation budget. Pure addition, no layout change.
- Effort: small. Optional — the card is intentionally sparse today.
- **Shipped as:** `current_humidity` added to `TPL_CLIMATE` (default sentinel -1,
  distinct from a genuine 0%, kept as "last known value" on a rejected/missing
  reading — same convention as the other climate fields), a new
  `DeviceState::humidity`, and a "· NN%" suffix drawn right of the room reading's
  degree ring on the AC card, with the name-truncation width budget in
  `drawDeviceCard()` updated to account for it. Mirrored into `simulator.html`
  (day + night palette screenshotted, no width-budget warnings triggered) and
  verified on hardware — the polled state line now ends `...,68` (68% humidity)
  and the card renders "Room 27° 68%"-style text correctly.

---

## C. Hygiene (docs / safety net)

### C0 — Commit the in-flight redesign

git status: modified files (fonts/corners redesign, card-fill removal, and now
A1/A2/A3/A4/B6 on top) + 2 untracked scripts. The repo's own rule (OPTIMIZATION.md P3)
is a rollback point before further edits. First step of any batch — more overdue
than when this was written, since there's now more uncommitted work, not less.

### C1 — Fix the stale poll comment

include/config.h:21-24 still says "One entity polled per tick, round-robin over
4 devices… 6 s" — P1 replaced that with one templated request every 1500 ms
(main.cpp has it right). One comment block.

### C2 — Reconcile the radius contradiction

config.h's LAYOUT block declares "CORNERS ARE SQUARE, EVERYWHERE… no radius token";
DESIGN.md still lists R_SM 6 / R_LG 8 as live tokens in places. Pick the
canonical truth (square, per current code) and sync DESIGN.md.

### C3 — Make the simulator's assertion harness runnable

CLAUDE.md describes a headless pattern (eval the page's `<script>` against a
Proxy-stubbed canvas, drive every page/palette/fault, read the warn log). Today
it's a described pattern, exercised ad hoc via headless Chrome screenshots (as
done for B6), not a checked-in script. Write scripts/sim_check.js (node, no deps)
so geometry/layout regressions are checkable in one command without a browser.

---

## Suggested batches

- **Batch 1 — hygiene, one flash:** C0, C1, C2. All small, all low-risk, each
  independently revertable. (A1/A2/A3 originally in this batch are done.)
- **Batch 2 — UX polish:** B1, B2, B3. Small-to-medium, visible every day.
- **Batch 3 — needs a decision or more appetite:** B4 (settings layout), B5, C3.
  (A4 and B6 originally in this batch are done.)

Verification for anything that ships: `pio run` clean; simulator screenshots for
visual changes; serial log for A2/A3 (A3 done; A2 still wants a live AC-tap
re-verification); flash and eyeball for B1/B2/B3.
