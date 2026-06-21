# Dune Weaver / FluidNC sand-table API

The controller (MKS-DLC32 / ESP32, FluidNC `sandtable` build) is a **headless controller**: it exposes
this WiFi API and holds no rich UI. Clients (the native app, scripts) drive it over HTTP + WebSocket.
This document is the stable contract — treat command names and the `$Sand/Status` schema as the interface.

> Source of truth: `FluidNC/src/SandApi.cpp`, `SandStatus.cpp`, `Playlist.cpp`, `FileCommands.cpp`,
> `Leds.cpp`, `Kinematics/ThetaRho.cpp`, `WebUI/WebServer.cpp`, `WebUI/Mdns.cpp`.

## Transports

| Transport | Port | Use | Execution context |
|-----------|------|-----|-------------------|
| **WebSocket (webui-v3)** | HTTP+2 (default **82**) | **Primary** — commands + live status stream | queued to **main loop** ✅ |
| HTTP `/command` | HTTP (default **80**) | Fire-and-forget cmds + `$/` reads (output of `$Sand/*` goes to WS, not the HTTP body) | runs **inline in the polling_loop task** ⚠️ |
| HTTP file routes | 80 | Upload, fetch `.thr`/files | — |
| Telnet | 23 | Raw line channel (scripting) | main loop |

⚠️ **Prefer WebSocket for anything that starts motion.** Commands sent over `/command?plain=` execute
synchronously inside `Web_Server::poll()` (the polling_loop task); a blocking motion command there fights
the main loop's segment prep (this caused the homing-crawl bug — see `web-motion-main-loop` note). The
same command over the WebSocket is queued to `protocol_main_loop` and runs cleanly. The `/sand_home`,
`/sand_stop`, `/sand_feed` HTTP routes exist as safe one-shot fallbacks (they only *signal* the main loop).

## Discovery (mDNS)

Advertised when connected in STA mode (`WebUI/Mdns.cpp`):

- Service: **`_http._tcp.local`** on the HTTP port; hostname `<host>.local`.
- TXT records identify a sand table and give the WebSocket port:
  - `model=dune-weaver`
  - `api=sandtable/1`
  - `ws=<httpPort+2>`  (the webui-v3 socket)
- Also advertises `_telnet._tcp` on port 23.

App flow: browse `_http._tcp`, keep services whose TXT has `model=dune-weaver`, read `ws=` for the socket
port, connect WebSocket to `ws://<host>:<ws>/`. Manual-IP entry is the fallback.

### WebSocket handshake (webui-v3)

- Protocol name: `webui-v3`. **Single client** — a new connection drops the previous one.
- On connect the server sends `CURRENT_ID:<n>` and `ACTIVE_ID:<n>`; it sends periodic `PING:` frames.
- Send commands as text frames (e.g. `$Sand/Status\n`). Responses + status come back as frames.

## Commands

Commands can be sent on any transport, **but command output routing differs** (verified live against the
board, 2026-06):

- **HTTP `GET/POST /command?plain=<urlencoded cmd>`** runs the command (the *action* always takes effect),
  but its **text response is racy** except for `$/` settings reads — plain `$` / `[ESP…]` output is emitted
  asynchronously and may not reach the request. So use `/command` for **fire-and-forget actions**
  (`$SD/Run=`, `$THR/Feed=`, `$LED/*`, `$Playlist/*`, settings writes) and confirm via a status poll; for
  *reads*, use the dedicated `/sand_*` JSON routes below. This fork ships **`HTTP/BlockDuringMotion`
  defaulted OFF**, so `/command` and `/sd/...` file serving work **while a pattern is playing** (the
  block-during-motion gate is a CNC-safety default that doesn't apply to a slow sand table); the `/sand_*`
  routes bypass the gate regardless.
- **WebSocket (port 82, `webui-v3`)** is the channel for **commands + their responses + live status**.
  Hold **one persistent connection** (it is single-client — a new connect drops/rejects the old one).
  Send commands as text frames terminated with `\n`; responses and `<…>` status frames come back on the
  same socket. Verified: `$RI=200` → `[MSG:INFO: websocket auto report interval set to 200 ms]` + `ok`.

→ **App rule of thumb (multi-client):** *read* via the `/sand_*` JSON routes (status / patterns / playlists
/ settings), *act* via `/command` + the `/sand_*` action routes, *fetch files* via `/sd/...`. All of that
is stateless and multi-client — any number of app instances at once, including during playback. Reserve
the WebSocket for a *single* "active controller" that wants smooth high-rate live animation (single-client).

> Coordination note: the table is one machine and commands are **last-writer-wins** (a second `$SD/Run`
> preempts the first — they are not queued). Multi-user works mechanically; any "who's driving" UX is the
> app's job. Scale assumption is household (a handful of pollers), not crowds — the ESP32 serves HTTP
> serially and has a small socket pool.

### Status & listing — multi-client HTTP routes (JSON body, work during motion)
| Endpoint | Returns |
|----------|---------|
| `GET /sand_status` | status object (schema below) |
| `GET /sand_patterns` | JSON array of `.thr` paths. **If `/patterns/index.json` (a prebuilt manifest) exists, it is served verbatim** — a fast single-file read of the full recursive catalog (paths relative to `/patterns`, e.g. `custom_patterns/x.thr`; run via `$Sand/Run=/patterns/<path>`). Generate it on the host and upload to the card whenever patterns change (see COMMANDS.md). **Without a manifest** it falls back to a non-recursive top-level listing (subfolders omitted — enumerating the ~1000-file nested library on the slow SD froze the single-threaded server). Chunked/streamed |
| `GET /sand_playlists` | JSON array of `.txt` files in the top level of `/playlists` (non-recursive) |
| `GET /sand_settings` | JSON object of app settings (speed, LED, playlist, quiet hours), values as strings |
| `GET /sand_time` | wall clock `{epoch, synced, local, tz}`. `?epoch=<unix>` sets the clock (app auto-sync / AP mode); `?tz=<POSIX>` sets + persists the timezone. Also surfaced in `/sand_status` under `time` |

These are read-only, fast, and skip the block-during-motion gate, so clients keep reading while a pattern
runs. The `$Sand/Status` / `$Sand/Patterns` / `$Sand/Playlists` **commands** return the same data but only
reliably over the (single-client) WebSocket. Pattern/playlist **contents** are fetched as files:
`GET /sd/patterns/<name>.thr`, `GET /sd/playlists/<name>.txt`.

### Playback (asynchronous — poll `$Sand/Status` for progress)
| Command | Action |
|---------|--------|
| `$SD/Run=/patterns/<file>.thr` | run a pattern (`.thr` translated on the fly by ThetaRho kinematics) |
| `$Sand/Run=/patterns/<file>.thr [clear=<mode>]` | run a pattern, optionally preceded by a clear ("pre-execution"). `clear=none\|adaptive\|in\|out\|sideway\|random` (default none); `adaptive` picks in/out from the pattern's first rho. Sequenced by the firmware (clear→pattern, then stop); aborts any running job first. Requires a `playlist:` config section (the clear files live there). |
| `$SD/Show=/patterns/<file>.thr` | dump file contents (preview; requires Idle/Alarm) |
| `$Playlist/Run=<name>` | run playlist `/playlists/<name>.txt` |
| `$Playlist/Stop` | stop after the current pattern |
| `$Playlist/Skip` | abort current pattern, advance to next |
| `$Playlist/List` | text listing + active playlist index/total/name |
| `$Sand/Goto theta=<rad> rho=<0..1>` / `GET /sand_goto?theta=&rho=` | jog to an absolute θ (radians) and/or ρ (0..1); either or both axes (omitted axis stays put). For manual positioning **between patterns** — requires Idle (rejects with IdleError/HTTP 409 if a pattern is running or unhomed). ρ clamped to 0..1; uses the current feed; runs as a `G1` move (through ThetaRho kinematics) in the main loop — stop with `/sand_stop` |
| `$H` | home (sets θ=0, normalizes; **send over WebSocket**) |
| `!` / `~` | realtime feed-hold (pause) / cycle-start (resume) |
| `0x18` (Ctrl-X) | realtime soft reset (loses position) |

### Speed
| Command | Notes |
|---------|-------|
| `/sand_feed?mm=<0..100000>` | set base feed rate (motor mm/min) live; works mid-pattern. Idle → persists to `$THR/Feed`; running → in-memory override that **persists across the whole playlist/run** (no longer reset per pattern), cleared back to `$THR/Feed` when the run ends |
| `$THR/Feed=<mm_per_min>` | same base rate, NVS-persisted; idle-gated; applied on the next `.thr` move |
| `/sand_feed?pct=<10..200>` | scale the base rate by an absolute override percentage; works mid-pattern |
| `/sand_feed?d=up\|down\|reset` | HTTP one-shot coarse feed-override ±10% / reset to 100% |

Effective speed = base feed (`feed`) × `feed_override`/100; both are in `/sand_status`.

### LEDs (NVS-persisted; only present if `leds:` is configured)
`$LED/Effect=<name>` · `$LED/Palette=<name>` · `$LED/Color=RRGGBB` · `$LED/Color2=RRGGBB` ·
`$LED/Brightness=0..255` · `$LED/Speed=1..255` · `$LED/RunEffect=none|<name>` · `$LED/IdleEffect=none|<name>`
`$LED/Direction=cw|ccw` · `$LED/Align=0..359` · `$LED/BallSize=1..200` (the `ball` effect: ring winding vs theta, angular offset, glow size in LEDs).
More `ball` settings: `$LED/Color`=blob colour · `$LED/BallBright=0..255`=blob brightness · `$LED/Color2`=solid background colour · `$LED/BallBg=<effect>`=background sub-effect (`static`=solid Color2, `off`=black, or any effect name like `rainbow`/`fire`/`plasma` rendered behind the blob) · `$LED/BallBgBright=0..255`=background brightness · `$LED/Speed`=tracking smoothness (low=smoother/laggier, high=snappier). Motion is sub-pixel/anti-aliased; master `$LED/Brightness` still scales the whole strip.
Effect names: `off static rainbow breathe colorloop theater scan running sine gradient sinelon twinkle sparkle fire candle meteor bouncing wipe dualscan juggle multicomet glitter dissolve ripple drip lightning fireworks plasma heartbeat strobe police chase railway pacifica aurora pride colorwaves bpm ball`
Palette names: `rainbow ocean lava forest party cloud heat sunset`
Live LED keys (`/sand_led?…` or `$Sand/Led=`): `effect palette color color2 brightness speed direction align size bg fgbright bgbright` (`bg`=ball background sub-effect, `fgbright`=blob brightness, `bgbright`=background brightness)

The `$LED/*` settings are idle-gated (NVS writes are blocked mid-motion). For
live control during a pattern use:
| Command | Notes |
|---------|-------|
| `/sand_led?effect=&palette=&color=&color2=&brightness=&speed=` | HTTP; any subset of keys; applies in-memory live, persisted to NVS on return to idle |
| `$Sand/Led=<key>=<val> [<key>=<val> ...]` | command form, not idle-gated; same behavior |
`/sand_status` `led` reflects the live value during a run.

### Playlist / quiet-hours settings (NVS)
`$Playlist/Mode=single|loop` · `$Playlist/Shuffle=ON|OFF` · `$Playlist/PauseTime=<sec>` ·
`$Playlist/PauseFromStart=ON|OFF` · `$Playlist/ClearPattern=none|adaptive|in|out|sideway|random` ·
`$Playlist/AutoHome=<n>` · `$Playlist/Autostart=<name>` · `$Sands/Enabled=ON|OFF` · `$Sands/Slots=HH:MM-HH:MM@days,...`
`$Sands/LedOff=ON|OFF` (turn LEDs off during quiet hours; works headless/idle) ·
`$Sands/FinishPattern=ON|OFF` (ON=pause between patterns/finish current [default]; OFF=feed-hold mid-pattern and resume when the window ends). Quiet hours need a set clock (`time:`/NTP).

`$Playlist/Autostart=<name>` auto-runs playlist `/playlists/<name>.txt` on every boot,
once the table reaches Idle (after homing). Empty (default) disables it. The boot run
uses its **own** parameters (independent of the manual-run `$Playlist/*` settings) so
auto-play can behave differently:
`$Playlist/AutostartMode=single|loop` · `$Playlist/AutostartShuffle=ON|OFF` ·
`$Playlist/AutostartPause=<sec>` · `$Playlist/AutostartPauseFromStart=ON|OFF` ·
`$Playlist/AutostartClear=none|adaptive|in|out|sideway|random`
(defaults: loop, OFF, 0, OFF, none).

### Clock (needed by quiet hours)
Configure NTP + timezone in `config.yaml` (`time:` section: `ntp`, `server`, `tz`).
Runtime/over-HTTP:
`$Time/Show` (current local time + sync state) · `$Time/Set=<unix epoch>` (set the
clock manually) · `$Time/Zone=<POSIX>` (override + persist the tz, empty = use config).
Prefer the JSON `GET /sand_time` for apps (read + `?epoch=` / `?tz=` to sync).
| Command | Notes |
|---------|-------|
| `$RI=<ms>` | set this channel's auto-report interval (min 50 ms). Streams standard `<state\|MPos:…\|…>` frames |
| `?` | one-shot status report |

`MPos:x,y,z` is motor-space; the app can convert X/Y → θ/ρ, or just poll `$Sand/Status` (already
converted) at 1–2 Hz for the sand-specific JSON.

### WiFi onboarding (used by the AP-mode page; ASCII SSIDs only over serial)
`[ESP410]` (scan → `{"AP_LIST":[{SSID,SIGNAL,IS_PROTECTED},…]}`) · `$Sta/SSID=` · `$Sta/Password=` ·
`$WiFi/Mode=STA>AP` · `$Bye` (reboot). Non-ASCII SSIDs only work over the HTTP/WS path, not serial.

## `$Sand/Status` JSON schema

Single-line JSON (`SandStatus.cpp:encode`). Float precision: θ/ρ 4 dp, feed 0 dp, progress 3 dp.

```json
{
  "state": "Idle|Run|Hold|Alarm|Home|Jog|...",
  "theta": 1.2340,            // radians, accumulates turns
  "rho": 0.5000,              // 0.0 center .. 1.0 perimeter
  "feed": 100,                // $THR/Feed programmed rate, motor mm/min
  "feed_override": 110,       // live override % (set via /sand_feed); effective = feed * pct/100
  "running": true,            // an SD job is active
  "file": "/sd/patterns/star.thr",
  "progress": 0.425,          // 0..1 fraction of executed motion, or -1 if unknown
  "playlist": { "active": true, "index": 2, "total": 10, "name": "evening",
                "clearing": false, "quiet": false, "pause_remaining": -1, "pause_total": -1 },
  //  clearing=true means a pre-execution clear is running before the chosen
  //  pattern; progress then tracks the CLEAR file's own 0..1 progress (file is the
  //  clear pattern). Render it as a separate "Clearing…" bar that resets when the
  //  real pattern starts, rather than the chosen pattern's progress.
  //  pause_remaining = seconds left in the between-patterns pause ($Playlist/PauseTime),
  //  counting down live; pause_total = that pause's full length in seconds; both -1
  //  when not pausing. Progress bar fill = (pause_total - pause_remaining) / pause_total.
  "led": { "effect": "rainbow", "brightness": 40 },  // omitted if no leds: config
  "time": { "epoch": 1718971402, "synced": true, "local": "2026-06-21 14:03:22", "tz": "ICT-7" }
  //  synced=false means the clock isn't set (no NTP yet / no $Time/Set) -> quiet
  //  hours won't fire; the app can push time via GET /sand_time?epoch=<unix>.
}
```

## Files

### Storage layout (SD card)
- `/patterns/*.thr` — patterns. Line format: `<theta_rad> <rho_0..1>`; `#` comments; blanks ignored.
- `/playlists/*.txt` — one SD-relative pattern path per line; `#` comments. Max 64 KB / 1024 items.
- `/clear_from_in.thr`, `/clear_from_out.thr`, `/clear_sideway.thr` — clear templates.

### Fetch a file (for in-app preview)
`GET /sd/patterns/<file>.thr` (and any `/sd/...` path) — served by the catch-all file handler. The app
fetches the `.thr` and renders the polar preview locally (no server-side thumbnails).

### Upload
`POST /upload` (SD) multipart: field `<filename>S` = size in bytes, field `<filename>` = file bytes;
optional `?path=/patterns`. Returns JSON `{status, files:[{name,size,…}], path, total, used, occupation}`.
Pre-checks free space. (Serial alternative: `$Xmodem/Receive=<path>` / `$Xmodem/Send=<path>`.)

## Security / constraints
- **No authentication** and **no CORS headers** in the `sandtable` build. Native apps are unaffected by
  CORS; a browser/Electron app on another origin would need CORS added to `WebServer.cpp`.
- WebSocket v3 is single-client; design the app to hold one connection per table.
