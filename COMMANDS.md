# Sand-table command reference (copy-paste)

Practical quick-reference for driving the headless FluidNC sand table over HTTP.
For the stable contract + JSON schema see [API.md](API.md); for the firmware
behind each command see `FluidNC/src/SandApi.cpp`, `Playlist.cpp`, `Leds.cpp`,
`Kinematics/ThetaRho.cpp`, `WebUI/WebServer.cpp`.

> Maintained doc — keep this in sync when commands change.

## Setup

```bash
B=http://fluidnc.local        # mDNS name; or the LAN IP, e.g. http://192.168.68.160
```

- In a shell, `\$` escapes the `$` so it isn't treated as a variable.
- Encode **spaces** in a URL as `%20` (matters for `$Sand/Run ... clear=`).
- **Reads** → use the `/sand_*` JSON routes (multi-client, work during motion).
- **Actions** → `/command?plain=$...` (fire-and-forget; confirm via a status poll)
  or the dedicated `/sand_*` action routes.
- All `$.../...` **settings are NVS-persisted** (survive reboot).

---

## Read state (JSON, safe during motion, any number of clients)

```bash
curl "$B/sand_status"        # state, theta, rho, feed, feed_override, running, file,
                             #   progress, playlist{active,index,total,name,clearing,quiet}, led{}
curl "$B/sand_patterns"      # JSON array of /patterns/*.thr (dotfiles filtered)
curl "$B/sand_playlists"     # JSON array of /playlists/*.txt
curl "$B/sand_settings"      # JSON of all app settings (speed, LED, playlist, quiet hours)
```

`progress` is `0..100`, or `-1` when unknown / **during a pre-execution clear**
(`playlist.clearing` is `true` then — show "Clearing…", not a 0→100 bar).

---

## Run / stop patterns

```bash
# Plain run (.thr translated on the fly by ThetaRho kinematics)
curl "$B/command?plain=\$SD/Run=/patterns/star.thr"

# Run with a pre-execution clear (clear sequenced first, then the pattern)
#   clear = none | adaptive | in | out | sideway | random   (adaptive picks in/out
#   from the pattern's first rho).  Needs a playlist: config section.
curl "$B/command?plain=\$Sand/Run=/patterns/star.thr%20clear=adaptive"

# Clean stop (decel to Idle, keeps position, no re-home needed)
curl "$B/sand_stop"

# Pause / resume mid-pattern
curl "$B/sand_pause"
curl "$B/sand_resume"
```

---

## Homing

```bash
curl "$B/sand_home"                       # sensor homing ($H) via the main loop (safe)
curl "$B/command?plain=\$H"               # same, raw
curl "$B/command?plain=\$X"               # unlock from Alarm without homing
```

- Auto-homes on every boot (config `macros: startup_line0: $H`; `after_homing`
  re-centers to 0,0).
- Sensor homing uses the limit switches (gpio.36 theta, gpio.35 rho).
- Crash (sensorless) homing is **not implemented** in firmware.

---

## Speed

```bash
curl "$B/command?plain=\$THR/Feed=2000"   # absolute base rate, mm/min (applies to next move)
curl "$B/sand_feed?d=up"                  # live override + (coarse)
curl "$B/sand_feed?d=down"                # live override -
curl "$B/sand_feed?d=reset"               # back to 100%
```

`/sand_status` reports `feed` (programmed) and `feed_override` (live %); effective
rate = `feed * feed_override / 100`.

---

## LEDs (only if `leds:` is configured; this table: 49 px on gpio.18, RGB)

```bash
curl "$B/command?plain=\$LED/Effect=rainbow"     # off | static | rainbow
curl "$B/command?plain=\$LED/Color=FF0000"       # RRGGBB hex (used by 'static')
curl "$B/command?plain=\$LED/Brightness=80"      # 0..255
curl "$B/command?plain=\$LED/Speed=50"           # rainbow animation speed 1..255

# Motion-reactive (override the manual effect by machine state; none = don't override)
curl "$B/command?plain=\$LED/RunEffect=rainbow"  # while Run/Jog/Home
curl "$B/command?plain=\$LED/IdleEffect=static"  # while Idle/Hold
#   values: none | off | static | rainbow
```

---

## Playlists

```bash
curl "$B/command?plain=\$Playlist/Run=evening"   # /playlists/evening.txt
curl "$B/command?plain=\$Playlist/Skip"          # abort current, go to next
curl "$B/command?plain=\$Playlist/Stop"          # stop after current pattern
curl "$B/command?plain=\$Playlist/List"          # folder listing + active position
```

### Playlist / quiet-hours settings (NVS)

```bash
curl "$B/command?plain=\$Playlist/Mode=loop"                 # single | loop
curl "$B/command?plain=\$Playlist/Shuffle=ON"               # ON | OFF
curl "$B/command?plain=\$Playlist/PauseTime=30"             # seconds between patterns
curl "$B/command?plain=\$Playlist/PauseFromStart=ON"        # measure cadence from start
curl "$B/command?plain=\$Playlist/ClearPattern=adaptive"    # none|adaptive|in|out|sideway|random
curl "$B/command?plain=\$Playlist/AutoHome=10"              # home every n patterns (0=off)

# Still Sands quiet hours (needs a set clock / time: config section)
curl "$B/command?plain=\$Sands/Enabled=ON"
curl "$B/command?plain=\$Sands/Slots=21:00-08:00@daily"     # HH:MM-HH:MM@days,...
```

---

## Files

```bash
# Fetch a pattern/playlist (for in-app preview)
curl "$B/sd/patterns/star.thr"
curl "$B/sd/playlists/evening.txt"

# Upload to the SD card (multipart): field <name>S = size, field <name> = bytes
curl -F "path=/patterns" -F "my.thrS=$(wc -c < my.thr)" \
     -F "my.thr=@my.thr;filename=my.thr" "$B/upload"

# Upload to the on-board flash / littlefs (e.g. config.yaml)
curl -F "path=/" -F "config.yamlS=$(wc -c < config.yaml)" \
     -F "config.yaml=@config.yaml;filename=config.yaml" "$B/files"
```

---

## System

```bash
curl "$B/command?plain=\$Bye"             # reboot (board needs ~25-30s to rejoin WiFi)
curl "$B/command?plain=\$/macros/startup_line0"   # read a config value at runtime
#   NOTE: $/path=value runtime changes are NOT persisted; edit config.yaml to persist.
```

---

## Live status stream (single client, WebSocket on port 82)

```
$RI=<ms>   set this channel's auto-report interval (min 50ms) -> <state|MPos:...> frames
?          one-shot status report
```

App rule of thumb: read via `/sand_*`, act via `/command` + `/sand_*` routes, fetch
files via `/sd/...` (all stateless/multi-client); reserve the WebSocket for one
"active controller" wanting smooth high-rate live status.

---

## ⚠️ Don't

- **`$SD/List=/patterns`** — the recursive listing of that tree **hangs the web
  server** (needs a physical reset). Use `/sand_patterns` instead.
- **Raw realtime/control bytes in the `/command` query** (e.g. `%9F`, `%18`) — a
  high byte breaks the URL parser and **wedges the single-threaded web server**.
  Use the dedicated routes (`/sand_stop`, `/sand_pause`, `/sand_feed`) or the WebSocket.
