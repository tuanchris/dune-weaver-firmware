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

- **HTTP `GET/POST /command?plain=<urlencoded cmd>`** returns the output in the HTTP body reliably **only
  for `$/` settings reads** (e.g. `$/axes/x/steps_per_mm` → `…=200.000`). Plain `$` commands and `[ESP…]`
  emit their output **asynchronously to the channel**, so over `/command` the body is **racy** — often
  empty, sometimes the data, and never safe for concurrent clients. So HTTP `/command` is fine for
  fire-and-forget actions (`$SD/Run=`, `$THR/Feed=`, `$LED/*`) and `$/` reads, but **do not** rely on it
  to retrieve `$Sand/Status` / `$Sand/Patterns` JSON — use the dedicated **`GET /sand_status`** route.
- **WebSocket (port 82, `webui-v3`)** is the channel for **commands + their responses + live status**.
  Hold **one persistent connection** (it is single-client — a new connect drops/rejects the old one).
  Send commands as text frames terminated with `\n`; responses and `<…>` status frames come back on the
  same socket. Verified: `$RI=200` → `[MSG:INFO: websocket auto report interval set to 200 ms]` + `ok`.

→ **App rule of thumb (multi-client):** poll **`GET /sand_status`** over HTTP for live status, and send
actions over the HTTP routes / `/command`. All of that is **stateless and multi-client** — any number of
app instances can do it at once. Reserve the WebSocket for a *single* "active controller" that wants
smooth high-rate live animation (it's single-client — a new WS connection drops the previous one).

> Coordination note: the table is one machine and commands are **last-writer-wins** (a second `$SD/Run`
> preempts the first — they are not queued). Multi-user works mechanically; any "who's driving" UX is the
> app's job. Scale assumption is household (a handful of pollers), not crowds — the ESP32 serves HTTP
> serially and has a small socket pool.

### Status & listing
| Endpoint | Returns | Transport |
|----------|---------|-----------|
| **`GET /sand_status`** | one JSON object (schema below), in the **HTTP body** | HTTP, **multi-client** — preferred for polling |
| `$Sand/Status` | same JSON | WebSocket (output goes to the WS channel, not the HTTP body) |
| `$Sand/Patterns` | JSON array of `.thr` filenames in `/patterns` | WebSocket |
| `$Sand/Playlists` | JSON array of playlist filenames in `/playlists` | WebSocket |

`GET /sand_status` is read-only, fast, and works **during motion** (it skips the block-during-motion gate),
so clients keep getting progress while a pattern runs. (`$Sand/Patterns` / `$Sand/Playlists` are still
WS-only today; they're fetched rarely. Add `/sand_patterns` + `/sand_playlists` HTTP routes the same way if
the app needs fully WS-free listing — or list via the SD file-list route.)

### Playback (asynchronous — poll `$Sand/Status` for progress)
| Command | Action |
|---------|--------|
| `$SD/Run=/patterns/<file>.thr` | run a pattern (`.thr` translated on the fly by ThetaRho kinematics) |
| `$SD/Show=/patterns/<file>.thr` | dump file contents (preview; requires Idle/Alarm) |
| `$Playlist/Run=<name>` | run playlist `/playlists/<name>.txt` |
| `$Playlist/Stop` | stop after the current pattern |
| `$Playlist/Skip` | abort current pattern, advance to next |
| `$Playlist/List` | text listing + active playlist index/total/name |
| `$H` | home (sets θ=0, normalizes; **send over WebSocket**) |
| `!` / `~` | realtime feed-hold (pause) / cycle-start (resume) |
| `0x18` (Ctrl-X) | realtime soft reset (loses position) |

### Speed
| Command | Notes |
|---------|-------|
| `$THR/Feed=<mm_per_min>` | motor mm/min, 0..100000, NVS-persisted; applied on the next `.thr` move (live) |
| `/sand_feed?d=up\|down\|reset` | HTTP one-shot coarse feed-override ±/reset |

### LEDs (NVS-persisted; only present if `leds:` is configured)
`$LED/Effect=off|static|rainbow` · `$LED/Color=RRGGBB` · `$LED/Brightness=0..255` ·
`$LED/Speed=1..255` · `$LED/RunEffect=none|off|static|rainbow` · `$LED/IdleEffect=none|off|static|rainbow`

### Playlist / quiet-hours settings (NVS)
`$Playlist/Mode=single|loop` · `$Playlist/Shuffle=ON|OFF` · `$Playlist/PauseTime=<sec>` ·
`$Playlist/PauseFromStart=ON|OFF` · `$Playlist/ClearPattern=none|adaptive|in|out|sideway|random` ·
`$Playlist/AutoHome=<n>` · `$Sands/Enabled=ON|OFF` · `$Sands/Slots=HH:MM-HH:MM@days,...`

### Live status stream
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

Single-line JSON (`SandStatus.cpp:encode`). Float precision: θ/ρ 4 dp, feed 0 dp, progress 1 dp.

```json
{
  "state": "Idle|Run|Hold|Alarm|Home|Jog|...",
  "theta": 1.2340,            // radians, accumulates turns
  "rho": 0.5000,              // 0.0 center .. 1.0 perimeter
  "feed": 100,                // $THR/Feed, motor mm/min
  "running": true,            // an SD job is active
  "file": "/sd/patterns/star.thr",
  "progress": 42.5,           // 0..100, or -1.0 if unknown
  "playlist": { "active": true, "index": 2, "total": 10, "name": "evening",
                "clearing": false, "quiet": false },
  "led": { "effect": "rainbow", "brightness": 40 }   // omitted if no leds: config
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
