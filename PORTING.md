# Dune Weaver → FluidNC single-board porting plan

Goal: **one MKS-DLC32 (ESP32) runs the whole sand table** — motion, patterns,
playlists, LEDs, web control. The Raspberry Pi host (`dune-weaver` repo,
~14k lines of Python + React SPA) shrinks to an optional companion and
eventually disappears. A phone/desktop app may come later; it would talk to
the firmware API directly.

Assessment date: 2026-06-12, branch `thr-kinematics` (base v3.9.5).

## Where things stand

Already in firmware (branch `thr-kinematics`):

- `ThetaRho` kinematics (`FluidNC/src/Kinematics/ThetaRho.{h,cpp}`): theta/rho
  → motor math with gear coupling, native `.thr` execution via the
  `translate_line` hook (preamble skip, leading-revolution normalization,
  feed injection, post-job theta relabel). Hardware-verified on the DWG.
- `after_homing` centering macro in the example config.

## The flash budget (binding constraint)

- `env:wifi` uses `min_littlefs.csv`: 2 × 1.92 MB OTA app slots + 192 KB littlefs.
- Baseline `firmware.bin` (env:wifi): **1,859,776 B ≈ 95% of the app slot**.
- Every spindle/motor/ATC driver is force-linked via static
  `InstanceBuilder` registrations (`Configuration::GenericFactory`), so
  excluding source files from the build is the only way to reclaim that
  flash — and the Module system is explicitly designed for it (see
  `FluidNC/src/Module.h`).
- The React SPA build is ~3.6 MB → can never live in 192 KB littlefs;
  serve any rich UI **from the SD card**.
- Previews are pre-rendered WebP (host renders 512×512 with Pillow today).
  **Never render previews on the ESP32** — generate at upload time
  (browser/app) and store sidecars on SD.

## `env:sandtable` (the strip)

New PlatformIO env extending `env:wifi`, excluding subsystems a sand table
can never use. All of these are reachable only via factory registration —
no core references (verified by grep):

| Excluded | Why safe |
|---|---|
| All `Spindles/*` except `Spindle.cpp` (base+factory) and `NullSpindle.cpp` | `MachineConfig.cpp:141` default-creates `NoSpindle`; nothing else references concrete spindles |
| `Spindles/VFD/` + `VFDSpindle.cpp` | Modbus VFD protocol zoo, factory-only |
| `Motors/`: Dynamixel2, RcServo, Servo, Solenoid, Trinamic*/TMC* | DWG uses `stepstick` (kept: `StandardStepper`, `StepStick`, `NullMotor`). Dropping Trinamic also drops the TMCStepper lib dep |
| `ToolChangers/atc_manual.cpp` | `atc.cpp` base kept (`Main.cpp` references the factory) |
| `OLED.cpp`, `Status_outputs.cpp` | Modules-by-file; also drops the SSD1306 lib dep |
| `Kinematics/`: CoreXY, Midtbot, WallPlotter, ParallelDelta | Factory-only; `Cartesian` (ThetaRho's base) and `ThetaRho` kept |
| `WebUI/NotificationsService.cpp` | **The big one**: sole user of WiFiClientSecure, which links the ~160 KB mbedTLS stack. `notify()` in Report.cpp is `WEAK_LINK` (no-op default); the two statics WifiConfig.cpp calls are stubbed in `FluidNC/sandtable/NotificationsStub.cpp` (a dir only this env compiles) |

Kept although CNC-ish (entangled with core, individually small): Probe
(G38 paths), CoolantControl (M7-M9), Parking, Limits, Expression/Flowcontrol
(useful for macros), xmodem (config upload).

> Fleet note: dune-weaver Mini configs use `unipolar:` motors, which do
> not exist in this FluidNC version at all. Mini needs its own treatment
> later (re-add a unipolar driver or run different firmware).

## LED support (new, in firmware)

`leds:` config section + `Leds` module (`FluidNC/src/Leds.{h,cpp}`),
WS2812/NeoPixel over RMT:

- Effects: `off`, `static` (hex color), `rainbow` (animated wheel).
- Runtime control via persistent NVS settings, usable from serial, WebUI,
  telnet, and macros:
  - `$LED/Effect=off|static|rainbow`
  - `$LED/Color=RRGGBB`
  - `$LED/Brightness=0..255`
  - `$LED/Speed=1..255` (rainbow cycle speed)
- Config (`config.yaml`):
  ```yaml
  leds:
    data_pin: gpio.32      # DLC32: laser/TTL header is a convenient real GPIO
    num_leds: 60
    color_order: GRB       # any permutation of R,G,B
    frame_ms: 33           # ~30 fps animation tick
  ```
- Implementation notes: follows the `Status_Outputs` pattern
  (Channel + ConfigurableModule registered with `allChannels` so the
  channel-poll task drives animation frames; that task keeps running
  during jobs — `Protocol.cpp:140`). Uses the IDF RMT *translator* API
  (3 bytes/pixel buffer, no item buffer) on **RMT_CHANNEL_7** — step
  engines allocate RMT channels from 0 upward (`esp32/rmt_engine.c:29`),
  and the DLC32 steps via I2S shift register anyway, so no conflict.
- Strip must be wired to a real GPIO (RMT cannot drive I2SO expander pins).
  On DLC32 V2.1 use the laser TTL output (GPIO 32) or probe pin (GPIO 22);
  3.3 V data into a 5 V strip usually works at short wire lengths, add a
  level shifter if not.

## Porting matrix (host → firmware)

Done / free (already native, host code just disappears):

| Host behavior | Firmware equivalent |
|---|---|
| theta/rho→XY math, gear coupling | `ThetaRho` kinematics |
| .thr parsing, preamble, theta normalization | `translate_line` |
| Line streaming, ok-counting, serial retry | firmware reads SD directly |
| Position mirroring, `reset_theta`/`$Bye` dance | firmware owns MPos; relabel at job start |
| Homing orchestration + angular offset | `$H` + `after_homing` macro |
| Pause/resume/stop | `!` / `~` / reset realtime commands |
| Progress % | SD job percent in `<...>` status reports |
| Firmware detection, XModem config editing | native `$/path=`, `$CD` |
| WiFi provisioning (Pi nmcli/hotspot) | FluidNC STA→AP fallback |
| Software update (git pull) | OTA |

To port (in order):

1. **Strip** → `env:sandtable` (this commit). Re-measure budget.
2. **LED module** (this commit): rainbow + static colors + brightness.
3. **Playlist runner** — DONE 2026-06-12 (`FluidNC/src/Playlist.{h,cpp}`,
   `playlist:` config section). Plain-text playlists on SD
   (`/playlists/<name>.txt`, one SD-relative path per line), commands
   `$Playlist/Run|Stop|Skip|List`, NVS settings `$Playlist/Mode`
   (single|loop), `Shuffle`, `PauseTime`, `PauseFromStart`,
   `ClearPattern` (none|adaptive|in|out|sideway|random), `AutoHome`
   (every N patterns). Auto-play on boot = `$Playlist/Run=<name>` as a
   startup line. Design: Channel+ConfigurableModule ticked by the
   polling task; starts work by *returning* `$SD/Run=...`/`$H` command
   lines so the protocol task executes them with stock semantics;
   `Job::abort()` for stop/skip runs in the polling task, which already
   owns abort for EOF/alarm unwinding. Mid-pattern, serial `$` lines are
   queued by FluidNC until the job ends, so instant mid-pattern
   stop = realtime reset (cancels the playlist via the alarm path);
   instant Stop/Skip work during pauses and from non-protocol channels.
4. **Clear patterns** — DONE, folded into the runner (adaptive =
   first non-preamble rho of next pattern, `<0.5 → clear_from_out`,
   matching `pattern_manager.py:1146-1160`; missing clear files are
   skipped with a warning). Separate `clear_pattern_speed` is NOT yet
   ported — needs the speed-control phase.
5. **Speed control** — DONE 2026-06-12. `$THR/Feed` (NVS, created by
   ThetaRho; config `default_feed_mm_per_min` seeds the first-boot
   default) is read per translated line, and the translator emits an F
   word whenever the value changed — so an absolute speed change takes
   effect mid-pattern within one move, exactly the dune-weaver slider
   semantics. Realtime feed override (10-200%) still works on top.
   `$Playlist/ClearSpeed` remains deferred: the clean implementation is
   the playlist injecting `$THR/Feed=` lines around clear jobs, but
   that writes NVS twice per pattern (flash wear) — needs a RAM-only
   override first.
6. **NTP + Still Sands** — DONE 2026-06-12. `time:` config section
   (`TimeKeeper` module: SNTP via esp_sntp + POSIX `tz`, plus
   `$Time/Show` and `$Time/Set=<epoch>` for AP-mode/manual operation).
   Quiet hours gate the playlist between patterns: `$Sands/Enabled`,
   `$Sands/Slots=21:00-08:00@daily,...` (syntax in `QuietHours.h`;
   parsing/matching is a pure unit, `src/QuietHours.{h,cpp}`, with day
   filters, inclusive bounds, and midnight wrap matching
   `pattern_manager.py:263-320`). `$Playlist/Skip` during quiet hours
   overrides them for one pattern, like the host. With `$Sands/Enabled`
   on but no valid clock, the playlist warns once and keeps running.
   Deferred: mid-pattern hold (finish-pattern-first is the only mode,
   matching the user's host settings) and LED-off during quiet hours.
7. **Status/control API for a UI** — DONE 2026-06-12. Instead of REST
   routes (which would mean editing the private/static upstream
   `WebServer.cpp`, can't be unit-tested, and need STA WiFi to test),
   the API rides the existing command dispatch — so it works
   identically over serial, the websocket, and the `/command` HTTP
   endpoint, and it's testable over serial in the HIL suite. Queries
   emit JSON (`src/SandApi.cpp`); actions stay as existing commands:
   - `$Sand/Status` → one JSON object: state, theta/rho (from the
     active kinematics), feed, current file + progress, playlist
     index/total/name/clearing, quiet, and an led block when `leds:`
     is configured. The pure model (`src/SandStatus.{h,cpp}` — encode,
     array encode, escaping, SD-percent parse) is unit-tested; the
     command gathers live state. Cross-task safe: Playlist publishes a
     POD snapshot (`Playlist::runtimeStatus`, fixed buffers not
     std::string) for the handler in the protocol task to read.
   - `$Sand/Patterns`, `$Sand/Playlists` → JSON arrays of `/patterns`
     and `/playlists` (clean `error:60` without an SD card).
   All three are asynchronous, so status polls fine mid-pattern. The UI
   polls `$Sand/Status` at ~1 Hz (ample for a sand table; no custom
   websocket push needed yet).

   **UI<->firmware command map** (the "routes"):
   | UI action | Command |
   |---|---|
   | live status | `$Sand/Status` (poll) |
   | list patterns / playlists | `$Sand/Patterns` / `$Sand/Playlists` |
   | run pattern | `$SD/Run=<path>` |
   | run / stop / skip playlist | `$Playlist/Run=<name>` / `Stop` / `Skip` |
   | pause / resume | `!` / `~` (realtime) |
   | set speed | `$THR/Feed=<n>` |
   | LED | `$LED/Effect|Color|Brightness|Speed`, `$LED/RunEffect|IdleEffect` |
   | quiet hours | `$Sands/Enabled`, `$Sands/Slots` |
   | home | `$H` |
   | upload / delete files | `/files`, `/upload`, `$SD/Delete=` |
   | playlist behavior | `$Playlist/Mode|Shuffle|PauseTime|ClearPattern|AutoHome` |

   NOT yet done: serve a custom UI from SD (needs a ~20-line
   `myStreamFile`/`handle_not_found` SD-fallback patch — the one small
   upstream touch worth taking) and the lean SPA itself. Pattern list
   is unpaginated (~1080 files → ~40 KB JSON, fine on the current heap;
   paginate if it grows).
8. **MQTT/Home Assistant**: esp-mqtt (plain, no TLS) + HA discovery,
   trimmed to ~10 entities (state, current pattern, progress, speed,
   pause/stop/skip, playlist select). Host version has 30+.
9. **LED event hooks** — DONE 2026-06-12. `$LED/RunEffect` and
   `$LED/IdleEffect` (`none|off|static|rainbow`, default `none` =
   manual). The Leds module subscribes to its own auto-reports
   (Status_Outputs pattern, 500 ms) and applies a RAM-only effect
   override on Run/Jog/Home vs Idle/Hold transitions — no NVS wear,
   `$LED/Effect` stays the manual base. Alarm/Door leave the strip
   untouched.

Dropped (not ported): preview rendering (pre-render at upload), Pi wifi
manager, Pi updater, multi-table `known_tables` (UI concern),
`dune-weaver-touch` Qt app (would point at firmware API if kept), WLED
proxy (native LEDs replace it; WLED can still be driven by HA directly).

ETA estimation (rho-weighted smoothed estimator + `execution_times.jsonl`
history, `pattern_manager.py:1218-1227`) → v1 ships byte-progress only;
per-pattern history file on SD later if missed.

## Risks / constraints

- ESP32-WROOM has no PSRAM: HTTP + websocket + SD streaming + stepping +
  LEDs concurrently; keep JSON small, paginate file listings (~1080
  patterns in the library).
- `.thr` text on SD is fine (tens of MB); 1.7 GB host folder is mostly
  preview cache.
- MQTT must stay non-TLS; HA discovery payloads are chatty — build them
  streaming, not in RAM.
- Plain MQTT and the web stack share one lwIP; test under playlist load.

## Build

```sh
pio run -e sandtable          # stripped sand-table firmware
pio run -e wifi               # stock-equivalent baseline
```

Board flashes reliably only at 115200 (`upload_speed` already set).

### Tests

```sh
pio test -e tests             # native googletest with ASan/UBSan, ~5s
```

Upstream's `[env:tests]` compiles a whitelist of dependency-free
sources (see `tests_common` in platformio.ini) against tests in
`FluidNC/tests/`. Sand-table logic is kept testable by extracting pure
units that join that whitelist:

- `src/Kinematics/ThrTranslator.{h,cpp}` — the `.thr` line translation
  state machine (preamble pairs, whole-revolution offset, feed
  injection). `ThetaRho` owns one and layers job/channel tracking on
  top. Covered by `tests/ThrTranslatorTest.cpp`.
- `src/PlaylistParse.{h,cpp}` — playlist file parsing, first-rho
  extraction, clear-pattern policy. `Playlist` keeps the I/O and state
  machine. Covered by `tests/PlaylistParseTest.cpp`.

New firmware logic should follow the same split: pure logic in a
std-only file (testable), I/O and machine state in the module.

### Hardware-in-the-loop tests

```sh
python3 -m pytest hil -v               # board attached over USB serial
HIL_MOTION=1 python3 -m pytest hil -v  # also run tests that move the arm
HIL_PORT=/dev/ttyUSB0 ...              # non-default port
```

`hil/` drives the real board over serial (pytest + pyserial, no other
deps): restart via `$Bye` asserting a clean boot log, `$I`/status
shape, settings round-trips, and crash regressions (file errors must
produce `error:NN`, not a reboot — guards d44087f0). The whole session
skips cleanly when no board is attached, so it is safe in any
environment. Tests for the `leds:`/`playlist:` modules activate
automatically once those sections are in the board's config. Close any
serial monitor first — the port is exclusive. Motion tests are opt-in
via `HIL_MOTION=1` and expect a homed, clear table.

Suggested cadence: run the native tests on every change, and the HIL
suite after every flash.

Measured 2026-06-12 (both envs include the new Leds module):

| env | Flash | RAM |
|---|---|---|
| `wifi` (stock) | 1,861,329 B — 94.7% | 70,524 B — 21.5% |
| `sandtable` (drivers only) | 1,705,177 B — 86.7% | 61,376 B — 18.7% |
| `sandtable` (+ kinematics zoo, notifications/mbedTLS) | 1,555,113 B — **79.1%** | 60,196 B — 18.4% |

The strip recovers **299 KB flash and 10 KB static RAM** total, leaving
~401 KB (20.9%) headroom for the playlist runner, NTP/scheduler, JSON
API, and MQTT phases. Breakdown of the second round: notifications
8.6 KB + WiFiClientSecure 5.2 KB + mbedTLS 162.6 KB; kinematics ~11 KB.

Known remaining bloat, deliberately kept:

- ~140 KB of libstdc++ locale/iostream instantiations dragged in by
  FluidNC's std::filesystem usage (`stdfs`) — upstream design, not
  fixable with build filters.
- `esp32/gpio_dump.cpp` (11 KB, `$GPIO/Dump`) — directly referenced by
  ProcessSettings and genuinely useful during hardware bring-up
  (e.g. LED wiring). Stub it later if space gets tight.
- Probe, CoolantControl, Parking: entangled with the G-code core, a few
  KB each.
