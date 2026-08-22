# Candis-S31 Design

## Source of truth

- Status: Active for the EVT comprehensive demo.
- Last refreshed: 2026-08-21.
- Product surface: 2.0-inch 460×460 AMOLED watch-style development board.
- Implementation: `firmware/demo`, ESP-IDF + LVGL 9.5.
- Hardware reference: EasyEDA project `【ESP32-S31】Candis-S31_copy`, board `v0.5_260803_1544打样`, schematic `Schematic_3`.
- Evidence reviewed: the live 11-page schematic, all documents under the project Feishu folder, all 19 documents in SiYuan notebook `20260817115926-6du34ni`, local ESP32-S31 vendor material, official Espressif documentation, `AGENT-AI.md`, the current demo source, and existing EVT logs.
- This file supersedes `.work/evt1/demo/DESIGN.md` where the implementation or latest hardware evidence differs.

The current hardware evidence allows display, touch, keys, RTC, PMIC/battery, TF, dual microphone, speaker, Wi-Fi, BLE, RGB LED, and USB OTG work. Camera remains unavailable until the mirrored FPC is corrected with the adapter board. The UI must state that dependency; it must not imply that preview or capture works.

## Brand

- Personality: precision test instrument combined with a premium AMOLED watch.
- Trust signals: live measurements, explicit device state, reproducible steps, timestamps, error codes, and honest capability boundaries.
- Visual character: pure-black canvas, restrained high-contrast color, large type, generous targets, and locally animated information.
- Avoid: desktop forms shrunk onto a watch, three-column icon grids, decorative full-screen effects, ambiguous status color, and placeholder features presented as working.

## Product goals

- Exercise every usable EVT peripheral through one coherent demo.
- Let an engineer locate a board fault quickly and repeat a single test.
- Let a presenter use the same firmware as a pleasant watch-style showcase.
- Make the 460×460 AMOLED the strongest part of the experience.
- Preserve smooth touch response while audio, wireless, storage, and USB services run in the background.
- Keep unsupported or physically blocked features visible with an accurate reason.

Non-goals:

- Claiming full-screen 60 fps. A 460×460 RGB565 frame is 423,200 bytes and the verified 48 MHz QSPI path cannot guarantee that rate.
- Hiding hardware failures behind simulated success states.
- Building a desktop-style multitasking shell.
- Enabling the camera before the FPC adapter and electrical checks are complete.

Success signals:

- All frequent actions have at least a 56×56 px hit area.
- Primary actions are readable and reachable with one hand.
- Chinese titles and primary actions are at least 20 px once the project font subset is enabled; 16 px is reserved for diagnostic metadata.
- UI work does not perform TF directory scanning, WAV probing, network scanning, or blocking storage flushes on the LVGL thread.
- Measured pipeline fact: adapter TE_SYNC hard-maps to LVGL RENDER_MODE_FULL with a single full-frame PSRAM buffer; a full 460×460 redraw costs about 17.6 ms against the 16.67 ms TE period, capping full-screen updates near 30 fps. Smoothness comes from sparse local invalidation, PPA-accelerated fills/blends (`enable_ppa_accel`), and no continuous animation sources; PARTIAL + TE is unavailable on this QSPI panel.
- Every hardware test ends in one of: untested, running, pass, warning, fail, or unavailable.

## Personas and jobs

Primary personas:

- EVT engineer: identify which rail, bus, peripheral, or workflow failed.
- Firmware developer: reproduce service lifecycle, hot-plug, and concurrency faults.
- Demo presenter: show audio, AMOLED, wireless, storage, USB, and games without entering engineering tools.

Key jobs:

- Check board health in under one minute.
- Run one peripheral test without disturbing unrelated services.
- See live values while a test runs.
- Record whether a human-observed result passed, such as hearing a channel or seeing a color.
- Recover from missing TF, failed connection, hot unplug, or service timeout without rebooting.

Key contexts:

- Bench bring-up while powered from USB-C1.
- Hand-held demonstration in normal and low light.
- TF and USB-C2 hot-plug testing.
- Battery, charging, screen-off, deep-sleep, and wake verification.

## Information architecture

Primary navigation:

```text
Watchface
├── tap or swipe up: launcher
│   ├── Test Center
│   ├── Audio: Recorder / Player
│   ├── Connectivity: Wi-Fi / BLE
│   ├── Storage & I/O: Files / USB OTG
│   ├── Hardware: RGB / Camera status / Power / System info
│   ├── Tools: Settings
│   └── Games: 2048 / Snake
├── status indicators: time / battery / Wi-Fi / BLE / TF / USB
└── current quick controls
    ├── Settings: brightness / volume / microphone gain / timeout
    └── Power: screen off / deep sleep / shutdown
```

The launcher uses two large cards per row. Test Center is the first card and owns the integrated EVT flow; individual apps remain available for direct demonstrations.
A swipe-down Control Center is intentionally deferred until the rebuilt launcher
and gesture routing have been validated on the physical panel; the current EVT
build keeps every control reachable through Settings or Power instead of
shipping an unverified gesture surface.

Implementation status: the watchface root is implemented in `ui_watchface.c` —
minute-level time refresh, change-only battery updates, and deliberately no
second hand; tap or swipe up opens the launcher, and BOOT/PWR keep their
back-home semantics. USB Device is implemented as a TinyUSB CDC command channel
(`ping/hello/status/led/echo`) that switches mutually exclusively with the USB
Host stack.

Test Center hierarchy:

```text
Overview
├── Quick self-test: display, touch, keys, RTC, battery, TF, USB state
├── Audio: LMIC, RMIC, dual mic, basic noise reduction, speaker, playback
├── Wireless: Wi-Fi scan/connect, BLE scan/connect/GATT
├── Storage & USB: TF read/write/hot-plug, OTG Host, OTG Device
├── Power: charge state, screen off/wake, deep sleep/wake, power-off guard
├── Visual: AMOLED patterns, color, gradients, local-animation stress
└── Session result: timestamp, measurements, user confirmations, failures
```

The current offline phase implements the Test Center as an honest live-state
overview and router into focused test apps. Persisted pass/fail sessions and
one-minute automation remain a post-flash phase because display, touch, audio,
USB, and power results require human or on-board evidence rather than simulated
success.

Content hierarchy within a test page:

1. Current state and dependency.
2. Primary action.
3. Live metric or visual response.
4. Test steps and evidence.
5. Secondary settings and diagnostics.

## Design principles

- Large and explicit: frequent controls are at least 56 px; primary controls are 72–88 px high.
- Evidence over decoration: show measured state, progress, result, and error reason.
- Smoothness comes from locality: animate high-value small regions, not the entire panel.
- Dangerous actions are explicit: delete, deep sleep, and power off require a named action and confirmation.
- Honest terminology: use `LMIC`, `RMIC`, `双麦`, `基础降噪`, `Host`, `Device`, and `硬件未装` precisely.
- Services outlive screens; screens never own blocking hardware work.
- Recovery is a first-class state: missing media, disconnects, and timeouts always expose a retry or next step.

## Visual language

Color:

| Token | Value | Use |
| --- | --- | --- |
| Background | `#000000` | AMOLED canvas |
| Surface | `#1A1A1E` | cards and controls |
| Surface raised | `#232329` | pressed/selected surface |
| Border | `#2C2C34` | component separation |
| Accent | `#4A9EFF` | focus and primary action |
| Text | `#F0F0F0` | primary content |
| Text dim | `#8A8A92` | secondary metadata |
| Success | `#30D158` | passed/ready |
| Warning | `#FFB020` | needs attention/confirmation |
| Error | `#FF453A` | failed/destructive |

Typography:

- Page title: 24 px CJK target.
- Primary button and card title: 20–22 px CJK target.
- Body: 20 px CJK target.
- Secondary information: 18 px target.
- Diagnostic-only metadata: minimum 16 px.
- Numeric display: Montserrat 24/32/48 until a dedicated narrow numeric font is evaluated.
- Implemented: the project-owned 20/24 px CJK subset (`candis_ui_20`/`candis_ui_24`) is compiled into the build and applied to page titles (24 px) and launcher card titles (20 px). Body text still runs primarily on the existing 16 px Source Han Sans SC, with the subset falling back to CJK 16 for uncovered glyphs; a full 20 px body remains a follow-up item.

Spacing and shape:

- Base unit: 8 px.
- Screen side padding: 16 px.
- Card gap: 12 px.
- Card radius: 18–20 px.
- Status bar height: 32–36 px, subject to first hardware screenshot review.
- Two-column launcher card: approximately 208×112 px.
- Single-line list row: at least 64 px.
- Two-line list row: 72–80 px.
- Slider hit object: at least 56 px high; track 8–10 px; knob 28–32 px.

Motion:

- Press: 100–120 ms from 100% to 94%; release 140–160 ms.
- Local content entrance: 160–180 ms, limited to the title, primary card, or first visible rows.
- Toast: existing 200/220 ms entrance/exit may remain.
- Audio meter: up to 30 Hz within an approximately 420×80 region.
- Playback progress: 10 Hz.
- Wi-Fi/BLE list presentation: batch at 200–250 ms; do not reorder on every advertisement.
- 2048 merge/spawn: 120–160 ms on affected tiles only.
- Snake logic: 10–15 Hz with local redraw; avoid invalidating the whole board each step.
- Never animate a full-screen opacity or slide transition on this transport budget.
- AMOLED inspection may temporarily use full-screen static red, green, blue,
  white, or black fields. Those are test states, not animations; tap advances
  the field and long-press or the physical BOOT key exits.

## Components

Existing components to retain and improve:

- `ui_app_scaffold`: standard screen shell; enlarge the title row and back target.
- `ui_toast`: transient feedback.
- `ui_msgbox`: confirmation and destructive-action guard.
- `ui_keyboard`: on-screen password/text entry.
- Global status APIs for time, battery, Wi-Fi, BLE, TF, and USB.
- Existing press-scale feedback and local value caching.

Shared components to add or standardize:

- `AppTile`: large two-column launcher card with title, icon, and availability badge.
- `StatusBadge`: icon + text + color for all states.
- `TestStatusCard`: dependency, current mode, and key measurement.
- `PrimaryTestAction`: 72–88 px start/stop/retry control.
- `MetricPanel`: live level, RSSI, voltage, capacity, or transfer rate.
- `StepList`: pending/running/pass/warn/fail steps.
- `ResultBar`: explicit fail/pass confirmation with timestamp.
- `LargeListRow`: 64–80 px reusable row.

State variants:

- `untested`, `running`, `pass`, `warning`, `fail`, `unavailable`, `disabled`.
- State must use text and icon in addition to color.
- Camera launcher state is `unavailable` with reason `等待 FPC 镜像转接板`.

Ownership:

- Global visual tokens and shared helpers belong in `main/ui`.
- Hardware state and blocking work belong in `main/services` or a dedicated background task.
- App files compose components and translate service events; they do not block the LVGL thread.

## Accessibility

- Optimize for physical readability and one-hand touch accuracy on a 2-inch panel.
- Preserve the current high-contrast palette; dim text must not be darker than `#8A8A92` on the existing surfaces.
- Never encode status through color alone.
- Provide visible pressed, disabled, running, and focus states.
- BOOT remains a reliable back/menu key; PWR returns home on short press and retains the hardware long-press behavior.
- A future reduced-motion setting disables nonessential entrance motion; charging state must remain understandable without blinking.
- Embedded screen-reader support is currently out of scope, but labels and status text must remain semantically clear.

## Responsive behavior

- Supported panel: fixed 460×460 only.
- Layout is density-token-driven rather than generically responsive.
- No hover behavior; every interactive element needs touch feedback.
- The status bar and title row define safe regions used by every app.
- Games may hide the global status bar but must provide an in-game exit target of at least 56×56 px.

## Interaction states

- Loading: spinner or progress plus the named operation, such as `正在扫描 Wi-Fi`.
- Empty: explain why and offer the next action, such as `未插入 TF 卡` plus retry.
- Error: human-readable reason, stable error code when available, and retry.
- Success: pass label, timestamp, and the relevant measurement or user confirmation.
- Disabled: state the dependency, such as `请先插入 TF 卡`.
- Offline: Wi-Fi/BLE screens retain scan and recovery actions.
- Saving: navigation must not silently interrupt a WAV flush; show `正在保存` until the background service completes or safely cancels.
- Hot unplug: invalidate stale results and return the screen to a recoverable media-missing state.

The default action on a file is view, play, or details as appropriate. Delete is a visible secondary action with confirmation; it is never the only behavior of a normal tap.

## Content voice

- Tone: short, direct, technically trustworthy.
- Buttons name an action: `开始扫描`, `停止录音`, `重新测试`.
- Status text names a result: `TF 已挂载`, `连接失败`, `等待用户确认`.
- Avoid an unqualified `处理中`; name the object and operation.
- Never call the current software route simply `降噪`; use `基础降噪`.
- Do not translate protocol role names inconsistently: use `USB Host` and `USB Device` throughout.

## Implementation constraints

- Framework: ESP-IDF `v6.1-beta1-dirty @ b1d13e9f` baseline, LVGL 9.5, C.
- Do not upgrade ESP-IDF or locked components as part of the demo redesign.
- Display: CO5300, 460×460 RGB565, verified 48 MHz QSPI, TE synchronized. The active path is esp_lvgl_adapter TE_SYNC, hard-mapped to RENDER_MODE_FULL with one full-frame PSRAM draw buffer (full redraw ≈17.6 ms, full-screen ceiling ≈30 fps); smoothness relies on sparse local invalidation plus PPA acceleration (`enable_ppa_accel`). PARTIAL + TE is not usable on this QSPI panel.
- LVGL private heap: 64 KiB; all font and component changes require build-size and runtime-memory review.
- Wireless: Wi-Fi, BLE, and 802.15.4 share one RF; the demo must not assume unrestricted simultaneous throughput.
- Audio: ES8389, two analog MEMS microphones, mono NS4150B speaker path.
- TF socket has no card-detect pin; insertion/removal recovery is polling and I/O-result based.
- USB-C1 is the normal power/debug port. USB-C2 is OTG data/role; Host testing uses C1 power plus C2 data.
- Camera remains compiled as a status/blocked page until the adapter and electrical validation are complete.
- All build, flash, and Git commands run inside tmux session `candis-s31`.

Test expectations:

- Fresh compile with warnings reviewed.
- Image size and component memory delta recorded.
- Static checks for LVGL thread violations and stale callback ownership.
- Hardware screenshot review for typography and hit targets when the board returns.
- Interactive EVT run covering display/touch, keys, audio routes and playback, Wi-Fi, BLE, TF hot-plug, USB Host/Device, screen-off/wake, deep sleep/wake, charging, and power-off guard.
- FPS/flush-region evidence for animated surfaces; no unmeasured full-screen smoothness claims.

## Power and charging boundaries

- Charge-current control ramps the TG28 REG62 ceiling
  50 → 100 → 200 → 300 → 400 → 500 mA under evidence gates (VBUS and
  battery present, actually charging, VBAT within
  [3000, min(Vchg+50, 4450)] mV, VBUS ADC ≥ configured VINDPM + 320 mV;
  configured values are read from the PMIC, never assumed). UI surfaces
  show the verified target register, never a measured current: the power
  path can back the actual current off under VINDPM at any moment.
- Safety boundaries: the 500 mA ceiling is pending cell/connector thermal
  sign-off; the board has a fixed TS divider and no NTC, so there is no
  cell-temperature loop - protection relies on the protected cell and the
  TG28 hardware limits. Repeated VBUS droop (2) or consecutive PMIC
  failures (3) collapse the target to 50 mA and latch until replug.
- Fuel gauge: the SOC comes from the vendor generic 4.2 V-class reference
  model (provisional/reference accuracy). A materially different battery
  SKU or chemistry requires a new model, not per-unit calibration. While
  no model is programmed, the percent renders as 电量模型未装入 and battery
  voltage stays a clearly-labeled diagnostic - a voltage-derived percent
  must never be presented as SOC. The reference model is labeled 参考
  (reference SOC); a custom model may be shown as 专用, but neither is
  ever presented as per-battery calibrated.

## Open questions

- [ ] Confirm 32 px versus 36 px status-bar height from the first hardware screenshot.
- [ ] Decide whether Test Center session results persist in NVS, TF, or both after the first end-to-end flow is implemented.
- [ ] Decide whether a separate presentation mode is useful after the engineering flow is stable.
- [ ] Measure flash/RAM cost and visual quality of the project-owned 20/24 px CJK subset.
- [ ] Decide whether BOOT should gain focus/confirm combinations beyond its existing navigation role.
- [ ] Define the camera enable gate after the FPC adapter arrives: rail check, SCCB identification, frame capture, then preview.
