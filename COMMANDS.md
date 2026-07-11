# Sand-table command reference (copy-paste)

Practical quick-reference for driving the headless FluidNC sand table over HTTP.
For the stable contract + JSON schema see [API.md](API.md); for the firmware
behind each command see `FluidNC/src/SandApi.cpp`, `Playlist.cpp`, `Leds.cpp`,
`Kinematics/ThetaRho.cpp`, `WebUI/WebServer.cpp`.

> Maintained doc — keep this in sync when commands change.

## Setup

```bash
B=http://192.168.68.160       # LAN IP (preferred); or the mDNS name, e.g. http://DWMP.local
#   hostname comes from `hostname:` in config.yaml (this table: DWMP), else $Hostname (default fluidnc)
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
curl "$B/sand_status"        # state, theta, rho, feed, feed_override, running, file, progress,
                             #   playlist{active,index,total,name,clearing,quiet,pause_remaining,pause_total}, led{}
                             #   pause_remaining/pause_total = sec left / full length of between-patterns pause
                             #   (both -1 if not pausing); bar fill = (pause_total-pause_remaining)/pause_total
curl "$B/sand_patterns"      # serves /patterns/index.json manifest if present (full recursive catalog,
                             #   paths relative to /patterns); else top-level /patterns/*.thr (non-recursive).
                             #   Run any pattern by full path: $Sand/Run=/patterns/sub/x.thr
curl "$B/sand_playlists"     # JSON array of /playlists/*.txt
curl "$B/sand_settings"      # JSON of all app settings (speed, LED, playlist, quiet hours)
curl "$B/sand_bootlog"       # plain-text boot log ($SS); after a PANIC reset it still holds the
                             #   previous boot's log (RTC RAM) -> the on-device crash breadcrumb
curl "$B/sand_log"           # rolling session log (last ~8KB, [+uptime_s] prefixed): playlist end
                             #   reasons, SD errors, alarms. RAM-only, lost on reboot
curl "$B/sand_coredump"      # last crash: task, PC, backtrace addrs (survives reboot; ?erase=1 clears)
# decode: ~/.platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-addr2line \
#           -pfiaC -e .pio/build/sandtable/firmware.elf <backtrace addrs>
```

`/sand_status` also reports health: `sd_ok` (boot-time SD readability probe; omitted
if no SD configured), `last_reset` (`power_on|software|panic|task_wdt|brownout|…` —
panic/wdt/brownout mean a crash), `uptime` (seconds; a drop between polls means
the board silently rebooted), and heap telemetry: `heap` (free now), `heap_min`
(low-water since boot), `heap_largest` (largest free block — heap flat while this
decays = fragmentation heading for an OOM panic).

Identity: `mac` (lowercase STA MAC — the table's stable ID; also in the mDNS TXT
record as `mac=`, so apps key saved tables by it) and `hostname` (the configured
network name, e.g. `DWMP`).

`progress` is a `0..1` fraction, or `-1` when unknown. During a **pre-execution
clear** `playlist.clearing` is `true` and `progress` tracks the *clear* file's own
0→1 — show a separate "Clearing…" bar that resets when the chosen pattern starts.

---

## Run / stop patterns

```bash
# Plain run (.thr translated on the fly by ThetaRho kinematics)
curl "$B/command?plain=\$SD/Run=/patterns/star.thr"

# Run with a pre-execution clear (clear sequenced first, then the pattern)
#   clear = none | adaptive | in | out | sideway | random   (adaptive picks in/out
#   from the pattern's first rho).  Needs a playlist: config section.
curl "$B/command?plain=\$Sand/Run=/patterns/star.thr%20clear=adaptive"

# Clean stop (decel to Idle, keeps position, no re-home needed).
# Stops the WHOLE sequence: if a pre-execution clear or a playlist is running,
# it halts everything rather than advancing clear->pattern or pattern->next.
curl "$B/sand_stop"

# Pause / resume mid-pattern
curl "$B/sand_pause"
curl "$B/sand_resume"
```

---

## Homing

```bash
curl "$B/sand_home"                       # home honoring $Sand/HomingMode, via the main loop (safe)
curl "$B/command?plain=\$Sand/Home"       # same, over /command (mode-aware)
curl "$B/command?plain=\$H"               # force a sensor ($H) home, raw
curl "$B/command?plain=\$X"               # unlock from Alarm without homing

# Homing mode (persisted setting; default sensor):
curl "$B/command?plain=\$Sand/HomingMode=crash"    # blind crash-into-stop homing
curl "$B/command?plain=\$Sand/HomingMode=sensor"   # limit-switch $H homing
curl "$B/command?plain=\$Sand/HomingMode"          # read current mode

# Theta zero offset / "Sensor Offset" (degrees; persisted; -360..360; default 0):
curl "$B/command?plain=\$Sand/ThetaOffset=90"      # pattern theta=0 sits 90 deg from home
curl "$B/command?plain=\$Sand/ThetaOffset"         # read current offset
```

- `/sand_home`, `$Sand/Home`, playlist homes (`$Playlist/AutoHome`, the auto-play
  fallback home) and the boot startup line all honor `$Sand/HomingMode`.
- **Sensor** mode (default): the limit-switch `$H` cycle (gpio.36 theta, gpio.35 rho),
  with `after_homing` re-centering to 0,0.
- **Crash** mode: drives the rho (Y) carriage blindly into its physical center stop
  (no switch, no stall detection — plain stepsticks), then declares that position
  theta=0, rho=0. Theta is **not** moved, just zeroed in place. Jog feed is a fixed
  gentle seek rate; overshoot is full rho travel + margin.
- **Theta offset** (`$Sand/ThetaOffset`, degrees): applied at home time by **both**
  modes — pattern theta=0 is placed that many degrees from the home reference (the
  limit switch in sensor mode, the crash position in crash mode). Idle-gated;
  re-applied on the next home (so change it, then re-home for it to take effect).
  Honored on boot only via `startup_line0: $Sand/Home` (not raw `$H`).
- For the **boot** auto-home to honor crash mode, set the startup line to the
  mode-aware command in config.yaml: `macros: startup_line0: $Sand/Home`
  (the older `startup_line0: $H` always does sensor homing).
- `$Sand/HomingMode` is also returned by `GET /sand_settings`.

```bash
# Goto an absolute theta (radians) and/or rho (0..1) - manual positioning between
# patterns (idle only; either or both axes, omitted axis stays put).
curl "$B/sand_goto?theta=3.14159&rho=0.5"   # both
curl "$B/sand_goto?rho=0"                    # rho only -> center
curl "$B/sand_goto?theta=6.28318"            # theta only -> one turn from 0
#   409 if a pattern is running / unhomed. Same via command: $Sand/Goto theta=.. rho=..
```

---

## Speed

```bash
curl "$B/sand_feed?mm=500"                # base rate in mm/min (0..100000), works mid-pattern
curl "$B/command?plain=\$THR/Feed=2000"   # same base rate, but idle-only + NVS-persisted
curl "$B/sand_feed?pct=150"               # scale the base rate by an absolute % (10..200), mid-pattern
curl "$B/sand_feed?d=up"                  # override + (coarse, +10%)
curl "$B/sand_feed?d=down"                # override - (coarse, -10%)
curl "$B/sand_feed?d=reset"               # back to 100%
#   effective speed = base mm/min (feed) * feed_override% / 100; poll /sand_status for feed + feed_override.
#   ?mm during a run is in-memory and persists across the whole playlist/run (cleared when it ends);
#   ?mm while idle persists to $THR/Feed.
```

`/sand_status` reports `feed` (programmed) and `feed_override` (live %); effective
rate = `feed * feed_override / 100`.

---

## LEDs (only if `leds:` is configured; this table: 49 px on gpio.18, RGB)

```bash
curl "$B/command?plain=\$LED/Effect=fire"        # see effect list below
curl "$B/command?plain=\$LED/Palette=ocean"      # recolors the hue-cycling effects
curl "$B/command?plain=\$LED/Color=FF0000"       # primary color, RRGGBB hex
curl "$B/command?plain=\$LED/Color2=0040FF"      # secondary color (used by 'gradient')
curl "$B/command?plain=\$LED/Brightness=80"      # 0..255, master over every effect
curl "$B/command?plain=\$LED/Speed=50"           # animation speed 1..255
```

Palettes (`$LED/Palette=<name>`): `rainbow ocean lava forest party cloud heat sunset`.
`rainbow` is the classic wheel (default). The rest recolor every effect whose "Uses"
column below says **auto-hue** — e.g. `Palette=ocean` gives an ocean `colorloop`,
`sinelon`, `juggle`, `colorwaves`, etc.

Effects (`$LED/Effect=<name>`):

| Name | What it does | Uses |
|------|--------------|------|
| `off`       | all pixels dark                                         | — |
| `static`    | solid fill                                              | Color |
| `rainbow`   | full color wheel spread around the ring, rotating       | auto-hue |
| `breathe`   | solid color pulsing in brightness on a sine             | Color |
| `colorloop` | whole ring one hue, slowly cycling through the wheel    | auto-hue |
| `theater`   | every 3rd pixel lit, marching (theater chase)           | Color |
| `scan`      | dot sweeps back and forth with a short trail (Cylon)    | Color |
| `running`   | one sine peak of the color sliding around the ring      | Color |
| `sine`      | two faster sine bands of the color drifting             | Color |
| `gradient`  | scrolling blend between the two colors                  | Color + Color2 |
| `sinelon`   | moving dot with a fading trail and shifting hue         | auto-hue |
| `twinkle`   | random pixels light then fade out                       | auto-hue |
| `sparkle`   | dim base color with random white flashes                | Color |
| `fire`      | Fire2012 1D flame (heat diffusion + sparks)             | heat palette |
| `candle`    | warm global flicker                                     | Color |
| `meteor`    | bright head sweeping the ring, random-decay trail       | Color |
| `bouncing`  | a few balls bouncing under gravity                      | Color |
| `wipe`      | color-wipe from one end, alternating the two colors     | Color + Color2 |
| `dualscan`  | two dots sweeping the ring (one per color)              | Color + Color2 |
| `juggle`    | several sine dots of different colors, fading trails    | auto-hue |
| `multicomet`| several comets chasing at different speeds              | auto-hue |
| `glitter`   | rainbow base with random white sparks                   | auto-hue |
| `dissolve`  | pixels randomly flip color↔color2, then reverse         | Color + Color2 |
| `ripple`    | expanding rings from random centers, decaying           | auto-hue |
| `drip`      | droplets fall and splash                                | Color |
| `lightning` | random bright strikes over darkness                     | white |
| `fireworks` | random colored bursts that fade                         | auto-hue |
| `plasma`    | interfering sine waves through the color wheel          | auto-hue |
| `heartbeat` | double-pulse "lub-dub" brightness                       | Color |
| `strobe`    | brief full-strip flashes                                | Color |
| `police`    | alternating red / blue halves                           | red/blue |
| `chase`     | bright dot(s) running over a dim background             | Color + Color2 |
| `railway`   | alternating two colors with a moving shimmer            | Color + Color2 |
| `pacifica`  | layered scrolling ocean with whitecaps                  | built-in ocean |
| `aurora`    | drifting green/purple curtains                          | built-in aurora |
| `pride`     | flowing rainbow with brightness waves                   | rainbow |
| `colorwaves`| pride, but colored from the active palette              | auto-hue |
| `bpm`       | palette sweep pulsed by a beat                          | auto-hue |
| `ball`      | smooth blob tracking the sand ball's angle over a background | Color + Color2 + Size/Direction/Align |

`Speed` paces every effect (and gates spawn rates for `twinkle`/`bouncing`).
`Brightness` is a master scale applied over whatever the effect produces.

### Live control while a pattern is running

The `$LED/*` settings above are **idle-gated** — sending one while the table is
moving returns `Error: Command requires idle state` (FluidNC blocks NVS/flash
writes mid-motion). Use `/sand_led` (or `$Sand/Led`) for live control instead:
it applies in memory immediately (no flash write) and is persisted to NVS
automatically when the table next returns to idle.

```bash
# Works any time, including mid-pattern.
#   keys: effect palette color color2 brightness speed direction align size bg fgbright bgbright
curl "$B/sand_led?effect=fire&palette=ocean&brightness=120"
curl "$B/sand_led?color=FF0000&speed=80"
# command form (same behavior), space-separated key=value:
curl "$B/command?plain=\$Sand/Led=effect=plasma%20palette=lava"
```

The **`ball`** effect is a smooth glowing blob that follows the sand ball's angle
over a background colour. Tune it live while a pattern runs:
```bash
curl "$B/sand_led?effect=ball"
curl "$B/sand_led?color=FF0000&fgbright=255"   # blob colour + blob brightness (0..255)
curl "$B/sand_led?color2=000020&bgbright=60"   # solid background colour + its own brightness
curl "$B/sand_led?bg=plasma"                   # animated background sub-effect (or 'static'=solid color2, 'off'=black)
curl "$B/sand_led?size=6"                      # 1..200 - glow size (how big the follow blob is)
curl "$B/sand_led?speed=200"                   # tracking smoothness: low=smoother/laggier, high=snappier
curl "$B/sand_led?direction=ccw"               # cw|ccw — flip if the blob moves opposite the ball
curl "$B/sand_led?align=120"                   # 0..359 degrees — rotate the blob onto the ball
```
Motion is sub-pixel/anti-aliased. Background can be a solid colour (`bg=static` +
`color2`) or any effect (`bg=rainbow|fire|plasma|...`) rendered behind the blob.
Blob (`fgbright`) and background (`bgbright`) brightness are independent; master
`$LED/Brightness` scales the whole strip. All persist to NVS when set at idle
(`$LED/Color`/`Color2`/`BallSize`/`BallBg`/`BallBright`/`BallBgBright`/`Direction`/`Align`).

`/sand_status`'s `led` object reflects the live value during a run. A live
choice set mid-run is saved to the matching `$LED/*` setting on the return to
idle, so it survives the next reboot.

```bash
# Motion-reactive (override the manual effect by machine state; none = don't override)
curl "$B/command?plain=\$LED/RunEffect=fire"     # while Run/Jog/Home
curl "$B/command?plain=\$LED/IdleEffect=breathe" # while Idle/Hold
#   values: none | <any effect name from the table above>
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
curl "$B/command?plain=\$Playlist/AutoHome=10"              # home every n patterns (0=off; honors $Sand/HomingMode + ThetaOffset)
curl "$B/command?plain=\$Playlist/Autostart=evening"       # auto-run /playlists/evening.txt on boot
curl "$B/command?plain=\$Playlist/Autostart="             # (empty) disable auto-play on boot
#   Autostart fires once per boot, and only after a SUCCESSFUL home (Idle alone is not
#   enough: with must_home false the table boots Idle with its position unknown). The
#   boot home normally comes from startup_line0; if none arrives within ~5 s of unhomed
#   Idle, auto-play requests one itself (honoring $Sand/HomingMode). The boot run uses its
#   OWN params (independent of the manual-run Playlist/* above); defaults loop/OFF/0/OFF/none:
curl "$B/command?plain=\$Playlist/AutostartMode=loop"             # single | loop
curl "$B/command?plain=\$Playlist/AutostartShuffle=ON"           # ON | OFF
curl "$B/command?plain=\$Playlist/AutostartPause=60"             # seconds between patterns
curl "$B/command?plain=\$Playlist/AutostartPauseFromStart=OFF"   # ON | OFF
curl "$B/command?plain=\$Playlist/AutostartClear=adaptive"       # none|adaptive|in|out|sideway|random

# Still Sands quiet hours (needs a set clock / time: config section)
curl "$B/command?plain=\$Sands/Enabled=ON"
curl "$B/command?plain=\$Sands/Slots=21:00-08:00@daily"     # HH:MM-HH:MM@days,... (days: daily|weekdays|weekends|mon,tue,...)
curl "$B/command?plain=\$Sands/LedOff=ON"                   # turn LEDs off during quiet hours (works headless/idle)
curl "$B/command?plain=\$Sands/FinishPattern=OFF"           # ON=finish current pattern then pause (default); OFF=hold mid-pattern, resume after
```

## Clock (quiet hours need a set clock)

```bash
curl "$B/sand_time"                                # {epoch, synced, local, tz}; also in /sand_status.time
curl "$B/sand_time?epoch=$(date +%s)"              # app auto-sync: push the current unix time
curl "$B/sand_time?tz=ICT-7"                       # set + persist POSIX timezone (empty = config time:tz)
#   tz is safe mid-run: applied immediately, NVS write deferred to the return to idle.
#   config.yaml `time:` section sets ntp/server/tz; $Time/Show, $Time/Set=<epoch>, $Time/Zone=<POSIX> also work.
```

---

## Files

```bash
# Fetch a pattern/playlist (for in-app preview)
curl "$B/sd/patterns/star.thr"
curl "$B/sd/playlists/evening.txt"

# List one directory level (non-recursive; dirs have "size":-1).
# Optional pagination: <path>|<offset>|<limit> — "count" in the reply is the
# directory's total entry count, so |0|0 is a cheap count-only probe.
# Entries come in directory order. (Serial: $SD/ListJSON=..., $LocalFS/ListJSON=...)
curl "$B/command?commandText=%24SD/ListJSON=/patterns"
curl "$B/command?commandText=%24SD/ListJSON=/patterns%7C100%7C50"   # entries 100-149

# Upload to the SD card (multipart): field <name>S = size, field <name> = bytes
curl -F "path=/patterns" -F "my.thrS=$(wc -c < my.thr)" \
     -F "my.thr=@my.thr;filename=my.thr" "$B/upload"

# Upload to the on-board flash / littlefs (e.g. config.yaml)
curl -F "path=/" -F "config.yamlS=$(wc -c < config.yaml)" \
     -F "config.yaml=@config.yaml;filename=config.yaml" "$B/files"

# Generate previews + manifest + a zip (Dune-Weaver-style .webp thumbnails):
#   python3 tools/gen_previews.py /path/to/patterns --out export/
#   -> export/index.json, export/cached_images/<pattern>.thr.webp, export/previews.zip
#   (needs Pillow: pip install Pillow). Then upload export/index.json as below.

# Or just (re)generate + push the manifest (/patterns/index.json) that
# /sand_patterns serves.  Run from the patterns root (or the SD mounted on a Mac):
#   full recursive .thr list, paths relative to /patterns, as a JSON array.
python3 -c 'import os,json; \
  fs=[os.path.relpath(os.path.join(d,f),".").replace(os.sep,"/") \
      for d,ds,fns in os.walk(".") if (ds.__setitem__(slice(None),[x for x in ds if x!="cached_images" and not x.startswith(".")]) or True) \
      for f in fns if f.lower().endswith(".thr") and not f.startswith(".")]; \
  fs.sort(key=str.lower); open("/tmp/index.json","w").write(json.dumps(fs,ensure_ascii=False,separators=(",",":")))'
curl -F "path=/patterns" -F "/patterns/index.jsonS=$(wc -c < /tmp/index.json)" \
     -F "f=@/tmp/index.json;filename=/patterns/index.json;type=application/json" "$B/upload"
```

Upload/file-op failures return a real HTTP error (503 fs-inaccessible, 507 no space,
500 other) with `error:{code,message}` in the JSON — see API.md "File-op errors".
`fopen` won't create directories: `createdir` first when uploading into a new subfolder
(`curl "$B/upload?action=createdir&filename=sub&path=/patterns"`).

---

## System

```bash
# OTA firmware update (rejected with 409 while a pattern runs; works in Alarm)
curl "$B/updatefw"                        # probe: {"status":"ready"|"busy","fw":"<version>"}
curl -F "firmware.binS=$(wc -c < .pio/build/sandtable/firmware.bin)" \
     -F "firmware.bin=@.pio/build/sandtable/firmware.bin;filename=firmware.bin" "$B/updatefw"
#   "status":"ok" -> board reboots ~1s later; poll /sand_status until uptime resets,
#   then check its "fw" field shows the new version.

curl "$B/command?plain=\$Bye"             # reboot (board needs ~25-30s to rejoin WiFi)
curl "$B/command?plain=\$/macros/startup_line0"   # read a config value at runtime
#   NOTE: $/path=value runtime changes are NOT persisted; edit config.yaml to persist.
```

---

## WiFi setup / modes

Two modes, keyed off `$WiFi/Mode`: **STA>AP** (default: join home WiFi; on failure
fall back to the `DuneWeaver` hotspot, password `12345678`, which serves a **captive
setup portal** at `http://192.168.0.1/`) and **AP** ("standalone": the hotspot IS the
product — the app joins it and drives the API at `192.168.0.1`; captive probes answer
"online" so phones stay put). See API.md → "WiFi onboarding / setup portal".

```bash
curl "$B/wifi_status"        # {"mode":"sta"|"fallback"|"standalone","sta_ssid":..,"ap_ssid":..,
                             #  "fail":"<why this boot's join failed>"|""}
curl "$B/wifi_scan"          # {"status":"scanning"} -> poll ~1.5s -> {"status":"ok","aps":[
                             #  {"ssid":..,"rssi":..,"secure":0|1}]}; ?rescan=1 forces fresh
# Set home-WiFi credentials (POST body, so any password chars are safe) and reboot:
curl -X POST -d "ssid=MyNetwork" -d "password=hunter2秘密" "$B/wifi_save"
#   -> {"status":"ok","reboot":1}; join fails -> hotspot returns, /wifi_status shows why
# Switch to standalone hotspot mode (live from the AP; reboots if sent from the LAN):
curl -X POST "$B/wifi_standalone"
curl "$B/"                   # the portal page (root, every mode; also at /wifi)
curl "$B/help"               # plain-text API map (moved off root in v0.1.8)
# Old-school (also what the USB installer uses over serial):
curl "$B/command?plain=\$Sta/SSID=MyNetwork"      # then $Sta/Password=..., then $Bye
curl "$B/command?plain=\$WiFi/Mode=STA>AP"        # or =AP for standalone
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
