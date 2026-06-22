# dune-weaver-firmware

Firmware that turns a **single MKS-DLC32 (ESP32) board** into the whole brain of a
[Dune Weaver](https://github.com/tuanchris/dune-weaver) kinetic sand table — no
Raspberry Pi, no separate WLED controller. It is a fork of
[**FluidNC**](https://github.com/bdring/FluidNC) **v3.9.5** with the sand-table host's
features ported down into the firmware itself.

## What it does

A Dune Weaver normally splits its brain across a host (Raspberry Pi running the
ThetaRho / `.thr` logic, playlists, scheduler, LEDs) and a motion controller. This
firmware folds all of that onto the ESP32:

- **ThetaRho kinematics** and `.thr` pattern playback
- **Playlists** with shuffle, pauses, adaptive clear-between-patterns
- **WS2812 LED** effects driven directly from the board (no WLED)
- **Scheduler / quiet hours** ("Still Sands")
- **Selectable homing** — limit-switch (sensor) or blind crash-into-stop, plus a
  configurable theta zero offset
- A stateless **HTTP/JSON API** for clients to drive the table

## Headless by design

The table holds **no web UI**. It exposes an HTTP API and is driven by clients (a
phone/desktop/web app, Home Assistant, scripts). Motion is controlled over stateless
HTTP — `POST`/`GET` to `/command` and the `/sand_*` action routes — and status is
polled from `/sand_status` (~1 Hz). The WebUI WebSocket is disabled (it raced motion
on this hardware).

## Documentation

- [`COMMANDS.md`](COMMANDS.md) — copy-paste command / HTTP cheatsheet
- [`API.md`](API.md) — the stable HTTP contract and `$Sand/Status` JSON schema
- [`PORTING.md`](PORTING.md) — what was ported from the Dune Weaver host and how
- Upstream FluidNC config reference: <http://wiki.fluidnc.com>

## Build & flash

Uses [PlatformIO](https://platformio.org/).

```bash
pio test -e tests                 # fast native unit tests (googletest + ASan/UBSan)
pio run  -e sandtable             # build the firmware
pio run  -e sandtable -t upload --upload-port /dev/cu.usbserial-XXXX   # flash over USB
```

The machine is described by a `config.yaml` on the board's littlefs (theta/rho axes,
homing, LEDs, etc.) — see the upstream FluidNC [config docs](http://wiki.fluidnc.com)
and the examples in this repo.

## Credits

Built on the work of others, and distributed under the same license (GPLv3):

- [**FluidNC**](https://github.com/bdring/FluidNC) by Bart Dring and contributors —
  the CNC firmware this is forked from.
- [**Grbl**](https://github.com/gnea/grbl) by Sungeun (Sonny) Jeon and Simen Svale
  Skogsrud — the motion-control foundation FluidNC itself builds on.
- The Wi-Fi / WebUI plumbing derives from [ESP3D-WebUI](https://github.com/luc-github/ESP3D-WEBUI).
- The [Dune Weaver](https://github.com/tuanchris/dune-weaver) project — the sand table
  this firmware is built to drive.

## License

GPLv3 — see [`LICENSE`](LICENSE). As a fork of FluidNC, all upstream copyrights and
the GPLv3 terms are preserved; see [`AUTHORS`](AUTHORS).
