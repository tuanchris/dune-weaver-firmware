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
- **Release gate** (before every release, ~10 min): `python3 soak/soak.py
  --profile torture` — concurrent pollers + pattern cycling at 200% feed +
  scans + LED writes + abort storms against a live table; exit 0 = ship.
  Watch the `heap_largest` floor across releases (WARN <20k, alert <12k) —
  decay there is the OOM-panic early warning. The `household` profile is an
  optional long soak for structural changes only.
- **Overnight real-motion soak** (playlist/motion changes, complements the torture
  gate): upload a small all-verified playlist (probe each entry readable with a
  ranged `GET /sd/patterns/<f>` → 206), set `$Playlist/Mode=loop`, a short
  `$Playlist/PauseTime` (e.g. 60), `$Playlist/Run=<name>`. Back the on-board
  `/sand_log` ring with a **serial capture** — `dwg_configs/serial_logger.py`
  (detached/nohup, DTR/RTS untouched so it does NOT reset the board; timestamps
  every line, reconnects). Morning triage greps the log for `Low memory:` (carries
  the in-flight URI), `playlist: skipping`, `canceled`, a boot banner mid-file
  (= reboot). Restore `PauseTime`, delete the test playlist, `pkill -f
  serial_logger.py` when done.

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
- **A playlist tolerates unplayable entries; a single `$Sand/Run` does not.** In a
  playlist run a pattern that won't start (missing/renamed file, SD read hiccup) or an
  unreadable slot is logged (`playlist: skipping …`) and the run advances immediately;
  a failing clear is dropped and its pattern still tried. `MAX_CONSEC_FAIL` (5) failures
  in a row with no pattern ever starting = a dead SD, so the run cancels rather than
  spinning the carousel forever (the counter resets whenever a pattern actually starts).
  A deliberate one-shot `$Sand/Run` instead fails loudly (`canceled: file did not
  start`). Shared advance/wrap/reshuffle logic lives in `Playlist::advanceIndex`.

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
- **`POST /upload` (and `/files`) takes the destination path from the multipart
  FILENAME, not a `path` field/arg** — `filename=/playlists/x.txt` lands it there; a
  bare `filename=x.txt` with `path=/playlists` lands it at the **SD root** (the size
  field must match: `/playlists/x.txtS`). Delete is the query form
  `/upload?path=/playlists&action=delete&filename=x.txt`. (COMMANDS.md/API.md examples
  were wrong about `path=` until 2026-07-18.)
- **Never hold all playlist pattern paths in RAM.** A big playlist (188+ entries) as
  a contiguous buffer / vector-of-strings needs a multi-KB allocation the fragmented
  ESP32 heap often can't satisfy → `std::bad_alloc` → `terminate()` → `abort()`/reboot
  (hit us building the whole path list at boot auto-play). `Playlist` keeps ONLY the
  shuffled `_order` array of line indices; the path for the current item is read back
  off the SD `.txt` on demand (`Playlist::resolveCurrent` via `scanValidLines`), once
  per pattern advance. Anything that touches the SD in the poller must also degrade
  gracefully (catch `std::exception`), never let an allocation failure escape.

## Key files

`FluidNC/src/`: `SandApi.{h,cpp}` (`$Sand/*`, `/sand_*` JSON), `SandStatus.{h,cpp}`
(status JSON + progress math), `Playlist.{h,cpp}` + `PlaylistParse.{h,cpp}` (playlist
state machine + clear policy, single-run `$Sand/Run … clear=`), `Leds.{h,cpp}`,
`Kinematics/ThetaRho.cpp`, `InputFile.cpp` (progress), `Protocol.cpp` (homing event,
job stack), `WebUI/WebServer.cpp` (routes), `WebUI/WifiConfig.{h,cpp}` +
`WebUI/WifiPortalPage.h` (WiFi setup portal: fallback AP = captive setup page,
`$WiFi/Mode=AP` = standalone hotspot answering OS probes as "online"; `/wifi_*`
routes), `WebUI/CaptiveDns.{h,cpp}` + `DnsQuery.{h,cpp}` (captive DNS). Configs:
`dwg_configs/` (untracked, per-table + backups).

- **Captive DNS is a native lwIP responder** (`WebUI/CaptiveDns`, per-query policy
  in std-only `DnsQuery`), NOT the arduino `DNSServer`. The old `DNSServer` could
  only wildcard *every* name to the softAP IP, which funnelled the connected
  phone's entire background traffic at the single-client web server in AP mode
  (each hostname → a TCP connection it can't drain → heap/PCB exhaustion → wedge).
  The responder instead answers per-query: **fallback AP** resolves everything to
  the table (setup sheet pops); **standalone AP** resolves ONLY the OS
  connectivity-probe hosts (`DnsQuery::kProbeHosts` — keeps the phone attached and
  "online") and returns NXDOMAIN for everything else, so background apps never open
  a socket to the table. Extend the probe list for new OSes; keep it to genuine
  probe hosts, not general CDNs, or the storm returns.

**`libraries/WebServer/` is a vendored fork** of the Arduino WebServer (shadows the
framework copy via `lib_extra_dirs`): shorter head-of-line waits, RST-close on
ABORTED clients only (never after a served response — linger-0 close discards
unflushed response bytes), liveness accessors for the accept-queue self-heal
watchdog in `Web_Server::poll()` — an aborted-client storm (status poller racing an
async WiFi scan) used to wedge the single-threaded server for minutes — and layered
low-heap shedding: 503 "busy: low memory" under a 10 KB floor (`/sand_status` and
stop/pause/resume exempt), RST-at-accept under a 6 KB hard floor, and `/sd/` file
streams abort themselves under 12 KB. Root cause of the deep dips: **every client
queued behind the single-threaded server pins ~2 KB of lwIP buffers**, and a
monolithic multi-MB `/sd/` transfer (preview shards) keeps the server deaf for
30+ s while they stack (measured 3.3 KB floor in the wild). `/sd/` GETs support
HTTP Range (`HttpRange.{h,cpp}`, std-only) so clients pull big files in bounded
chunks instead.
Changes are tagged `DW fork:`; see `libraries/WebServer/README.md` before bumping
the framework.

## Docs — keep current

[`COMMANDS.md`](COMMANDS.md) is the copy-paste command cheatsheet; [`API.md`](API.md)
is the stable contract + JSON schema. **Whenever a command, route, setting, or its
behavior changes, update `COMMANDS.md` (and `API.md` if the interface changes) in the
same change** — add new commands, remove deleted ones, fix altered semantics. Treat
docs as shipping with the code.
