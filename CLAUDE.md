# CLAUDE.md

Project guidance for working in this repo.

## What this is

A fork of **FluidNC v3.9.5** that turns a **single MKS-DLC32 (ESP32) board** into the
whole brain of a **Dune Weaver sand table** — no Raspberry Pi, no WLED. The host's
features (ThetaRho kinematics, `.thr` playback, playlists, LEDs, scheduler/quiet
hours, JSON API) are ported into firmware. Work happens on branch **`thr-kinematics`**
(base v3.9.5). Roadmap/porting notes: `PORTING.md`.

The table is **headless**: it exposes an HTTP API and holds no web UI. Clients (a
future iOS/Android/web app, scripts) drive it over HTTP.

## Hardware (active test board)

**Dune Weaver Mini Pro** on **MKS-DLC32 V2.1**, plain **stepstick** drivers (so no
Trinamic/StallGuard). On the LAN at **`DWMP.local` / 192.168.68.160** (prefer the IP;
mDNS is flaky here), USB serial **`/dev/cu.usbserial-8320` @ 115200**. The hostname
is set by **`hostname: DWMP`** in config.yaml (a top-level key that overrides the
`$Hostname` NVS setting; empty/absent → the NVS default `fluidnc`). (A second table
exists: the **DWG "Desert
Compass"**.)

- Kinematics **ThetaRho**: theta 50 mm/rev, rho 20 mm, coupling 0.195.
- Homing = **limit switches** (X/theta neg `gpio.36`, Y/rho neg `gpio.35`); X homes
  positive, Y homes negative; `soft_limits: false`. **Auto-homes on boot**
  (`macros: startup_line0: $H`; `after_homing` re-centers to 0,0). Crash/sensorless
  homing is **not** implemented.
- **LEDs**: 49 WS2812 on `gpio.18`, color order **RGB**, via RMT (channel 7). (`uart1`
  was removed to free gpio.18.)

## Build / test / flash

- pio at `/opt/homebrew/bin/pio`.
- **Native tests** (fast, every change): `pio test -e tests` (~10s, googletest +
  ASan/UBSan).
- **Firmware build**: `pio run -e sandtable` (flash ~80% full).
- **Flash over USB**: `pio run -e sandtable -t upload --upload-port /dev/cu.usbserial-8320`
  (~2 min; board then needs ~25-30 s to rejoin WiFi). The port is often grabbed by a
  Chrome **Web Serial** tab — if "port busy", ask the user to close it.
- **Edit config.yaml without USB**: it lives on littlefs; upload over HTTP with
  `POST /files` (multipart: `path=/`, `<name>S`=size, `<name>`=bytes). Always pull the
  live config first (`GET /config.yaml`), back it up, make a minimal diff.
- **Testing convention**: put pure logic in std-only files so it's unit-testable in
  `[env:tests]` (e.g. `PlaylistParse`, `SandStatus`, `ThrTranslator`); keep I/O +
  machine state in the module. HIL tests: `python3 -m pytest hil -v`.

## Architecture / conventions

- **API rides command dispatch, not REST routes** (rebase-safe, serial-testable):
  `$Sand/*`, `$Playlist/*`, `$LED/*`, `$THR/Feed`, etc., plus a few `/sand_*` HTTP
  action routes (`/sand_home|stop|pause|resume|feed|led`). `GET /` serves the WiFi
  setup portal (every mode); the plain API map lives at `GET /help`.
- **WebUI WebSockets are disabled** (`_socket_server = NULL`) — they raced motion and
  panicked the board. Drive everything over stateless HTTP (`/command` + `/sand_*`),
  poll `/sand_status` ~1 Hz.
- **Settings framework = the way to make things app-configurable**: any NVS-backed
  `Setting` is auto-readable via `GET /sand_settings` and writable via
  `$Key=value` over `/command`. Add a setting + its key in `settingsJson()`.
  **But `Setting` writes are idle-gated** (`Setting::check_state` → `IdleError`
  "Command requires idle state"; flash/NVS writes are blocked mid-motion). To
  change something **while a pattern runs**, add a non-gated path that applies
  in-memory and persists on the return to idle — e.g. `/sand_led` + `$Sand/Led`
  (a `UserCommand` with `cmdChecker=nullptr`) driving `Leds::setLive()`.
- **Motion from a web/non-protocol task must signal the main loop**, never block
  inline — `$H`/jogs run in `protocol_main_loop` via an event (`/sand_home` →
  `startHomeEvent`); running blocking motion in the web task starves segment prep
  (homing-crawl bug).
- The global **Job stack is shared across tasks** and guarded by a `recursive_mutex`
  (concurrent push vs pop corrupted the deque → panic).
- **Config-gated modules**: `Playlist` and `Leds` only exist if their config section
  is present.
- **Progress %** = executed motion, not file-read position (the reader leads motion by
  the queued planner blocks); reported `-1` during a pre-execution clear.

## Gotchas (these bit us; don't repeat)

- **Never `$SD/List=/patterns`, and no `$SD/ListJSON` on it either** — any on-device
  scan of that ~2000-entry tree can hang the web server / wedge an SD read mid-scan
  → poller task-WDT reboot, after which the card fails init (`ESP_ERR_INVALID_CRC`)
  until a **physical power cycle**. By design the host-pushed `/patterns/index.json`
  manifest IS the catalog — use `/sand_patterns` (serves it verbatim; ignores `?path`).
- **Never send raw realtime/control bytes in the `/command` query** (`%9F`, `%18`,
  etc.) — a high byte breaks URL parsing and wedges the web server. Use the dedicated
  routes or the WebSocket.
- **Always `$H` before motion tests** — unhomed position/rho readings are garbage.
- **FluidNC YAML**: comments only as full lines at **column 0**; keep lines short
  (long early-line values broke XModem parsing).
- **Runtime `$/path=value` config changes do NOT persist** — config.yaml is reloaded
  fresh each boot; edit the file to persist.

## Key files

`FluidNC/src/`: `SandApi.{h,cpp}` (`$Sand/*`, `/sand_*` JSON), `SandStatus.{h,cpp}`
(status JSON + progress math), `Playlist.{h,cpp}` + `PlaylistParse.{h,cpp}` (playlist
state machine + clear policy, single-run `$Sand/Run … clear=`), `Leds.{h,cpp}`,
`Kinematics/ThetaRho.cpp`, `InputFile.cpp` (progress), `Protocol.cpp` (homing event,
job stack), `WebUI/WebServer.cpp` (routes), `WebUI/WifiConfig.{h,cpp}` +
`WebUI/WifiPortalPage.h` (WiFi setup portal: fallback AP = captive setup page,
`$WiFi/Mode=AP` = standalone hotspot answering OS probes as "online"; `/wifi_*`
routes). Configs: `dwg_configs/` (untracked, per-table + backups).

## Docs — keep current

[`COMMANDS.md`](COMMANDS.md) is the copy-paste command cheatsheet; [`API.md`](API.md)
is the stable contract + JSON schema. **Whenever a command, route, setting, or its
behavior changes, update `COMMANDS.md` (and `API.md` if the interface changes) in the
same change** — add new commands, remove deleted ones, fix altered semantics. Treat
docs as shipping with the code.
