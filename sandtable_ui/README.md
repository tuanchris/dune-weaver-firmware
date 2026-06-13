# Sand-table UI (flash-hosted SPA)

A self-contained single-file web UI for the Dune Weaver / FluidNC sand
table. No build toolchain — `index.html` is hand-written HTML/CSS/JS,
gzipped to `../FluidNC/data/index.html.gz` and served from littlefs
(flash) exactly like FluidNC's stock WebUI. 7.5 KB gzipped (~6 % of the
192 KB littlefs partition).

It talks to the firmware over the existing command dispatch — no custom
HTTP routes (see `PORTING.md` "JSON API"):

| UI | Firmware |
|---|---|
| live status (1 Hz poll) | `$Sand/Status` |
| pattern / playlist lists | `$Sand/Patterns`, `$Sand/Playlists` |
| run pattern | `$SD/Run=/patterns/<file>` |
| run playlist | `$Playlist/Run=<name>` |
| pause / resume / stop | `/feedhold_reload` / `/cyclestart_reload` / `/restart_reload` |
| skip | `$Playlist/Skip` |
| speed | `$THR/Feed=<n>` |
| lights | `$LED/Effect|Color|Brightness|Speed|RunEffect|IdleEffect` |
| quiet hours | `$Sands/Enabled`, `$Sands/Slots` |
| home / reboot | `$H` / reset |

Thumbnails are static files on the SD card at `/sd/thumbs/<file>.png`
(served by FluidNC's catch-all, which resolves `/sd/...`). Patterns
without a thumbnail show a placeholder — the table still runs them.

## Preview it without hardware

```sh
python3 gen_mock_thumbs.py   # one-time: render sample thumbnails (needs the dune-weaver patterns)
python3 mock.py              # serves the UI + a simulated firmware on :8088
# open http://localhost:8088
```

The mock simulates `$Sand/Status` with advancing progress, the
pattern/playlist lists, settings round-trips, LED, and the realtime
pause/resume/stop routes, so the whole UI is interactive.

`preview_idle.png` / `preview_running.png` are headless-Chrome captures
(their right edge is cropped by the capture tool, not the layout —
`scrollWidth == innerWidth`, no overflow).

## Edit / rebuild

```sh
# edit index.html, then:
python3 build.py             # regenerates ../FluidNC/data/index.html.gz + checks budget
```

## Put it on the table

The UI lives in littlefs as `index.html.gz`. Upload **only** that file
over serial (XModem) — this does NOT touch the board's `config.yaml`:

```sh
python3 ../dwg_configs/upload_config.py \
    ../FluidNC/data/index.html.gz /dev/cu.usbserial-8310 /littlefs/index.html.gz
```

Then browse to the table (`http://192.168.0.1` in AP mode, or
`http://dwg.local` on the LAN). It replaces the stock FluidNC WebUI;
the original is recoverable from git history.

A full `pio run -e sandtable -t uploadfs` would also work but would
overwrite `config.yaml` on littlefs with the repo's generic one — don't
use it unless `FluidNC/data/config.yaml` holds your real config.
