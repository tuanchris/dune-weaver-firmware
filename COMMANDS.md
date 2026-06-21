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
```

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
curl "$B/sand_home"                       # sensor homing ($H) via the main loop (safe)
curl "$B/command?plain=\$H"               # same, raw
curl "$B/command?plain=\$X"               # unlock from Alarm without homing
```

- Auto-homes on every boot (config `macros: startup_line0: $H`; `after_homing`
  re-centers to 0,0).
- Sensor homing uses the limit switches (gpio.36 theta, gpio.35 rho).
- Crash (sensorless) homing is **not implemented** in firmware.

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
| `ball`      | a glowing dot that tracks the sand ball's angle         | Color + Direction/Align |

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
#   keys: effect palette color color2 brightness speed direction align
curl "$B/sand_led?effect=fire&palette=ocean&brightness=120"
curl "$B/sand_led?color=FF0000&speed=80"
# command form (same behavior), space-separated key=value:
curl "$B/command?plain=\$Sand/Led=effect=plasma%20palette=lava"
```

The **`ball`** effect makes a glowing dot follow the sand ball's angle. Calibrate
it live while a pattern runs:
```bash
curl "$B/sand_led?effect=ball"
curl "$B/sand_led?direction=ccw"     # cw|ccw — flip if the dot moves opposite the ball
curl "$B/sand_led?align=120"         # 0..359 degrees — rotate the dot onto the ball
```
`direction`/`align` are also `$LED/Direction` (cw|ccw) and `$LED/Align` (degrees)
settings; like the rest, they persist to NVS when set at idle.

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
curl "$B/command?plain=\$Playlist/AutoHome=10"              # home every n patterns (0=off)
curl "$B/command?plain=\$Playlist/Autostart=evening"       # auto-run /playlists/evening.txt on boot
curl "$B/command?plain=\$Playlist/Autostart="             # (empty) disable auto-play on boot
#   Autostart fires once per boot at the first Idle (after homing), using the
#   Mode/PauseTime/Shuffle/ClearPattern settings above.

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

# Regenerate + push the pattern manifest (/patterns/index.json) that
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
