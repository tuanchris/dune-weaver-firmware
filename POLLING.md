# Polling guidance for clients

The table is a **single-client HTTP server** on a fragmented-heap ESP32. Every
`GET /sand_status` is a full TCP connection (the firmware sends
`Connection: close`, so no client can reuse the socket — pooling/keep-alive is a
no-op against this server) plus a PCB that lingers in TIME_WAIT. The per-poll
cost is fixed and server-dictated; the only thing a client controls is **how
often it polls**.

## The one rule that matters: back off on the board's load signal

The board is **only ever load-stressed when heap is tight** — a big `/sd/`
transfer stacking connections behind the deaf single-threaded server, the app's
launch burst, fragmentation under sustained churn. When it's idle-and-quiet it
has ~full heap and absorbs polling for free; when it's *running* a pattern the
status route is cheap and exempt from load-shedding. So the poll rate should
track **board load, not machine motion state**:

> **Poll at the base rate normally. When the board signals heap pressure
> (`heap_largest` below the WARN floor, or a `503 busy: low memory`), back off
> hard until it recovers.**

`heap_largest` is in every `/sand_status` body. The WARN floor is **20000**
bytes (CLAUDE.md: WARN <20k, alert <12k). Under it, drop the poll interval to
~30 s so you stop competing for the last few KB against whatever is straining
the server — the failure mode that actually crashes the board. This is the
change that matters; implement it in every client.

Motion state (idle vs running) is a **poor proxy for load** and mostly optimizes
the regime that was never under pressure. Don't gate the poll rate on it for the
board's sake.

## Base cadence

| Client | Base rate | Rationale |
|---|---|---|
| App (mobile) | 1 s | the realtime driver — smooth progress/position/countdown |
| Touch panel | 1 s | same, at the table |
| HA integration | 5 s | background monitor, deliberately lighter |
| Pi backend relay | 1 s active / 2 s idle | relays to browsers over `/ws/status` |

All of them drop to **~30 s on the heap-pressure signal.**

## Optional: idle backoff (client-benefit only, not board-benefit)

Slowing the poll when the table is idle (nothing running/paused/homing, no
playlist in flight) does **not** meaningfully help the board — idle-and-quiet is
the one regime with spare heap. Its only upside is **client-side**: a
battery/radio saving on the mobile app while it's foregrounded but idle. So:

- **Mobile** may keep a modest idle backoff (e.g. 1 s → 8 s) purely for phone
  battery, snapping back to 1 s instantly on any activity or user action. Harmless.
- **Mains-powered clients** (touch panel, Pi backend, HA) get **no** benefit
  from idle backoff — don't add it there. It only adds state-tracking and
  snap-back complexity for a saving that doesn't exist.

## Non-negotiables (all clients)

1. **One poller per client.** A single process-wide status loop that every
   screen/widget *subscribes* to. Never one poller per component.
2. **Never stack polls.** Guard with an in-flight flag; a new tick must not fire
   while the previous `/sand_status` is outstanding. Anchor cadence to poll
   *start*, not completion.
3. **Back off when the table is unreachable.** On a failed/aborted poll, slow
   down (e.g. exponential to a ~5 s cap), don't keep hammering at the base rate.
   An offline single-client board polled-and-aborted every second is pure waste
   and the fleet's main abort source.
4. **Generous timeouts, not aggressive ones.** A busy board is not a dead board.
   Status timeout ≥ 5 s. A short abort timeout (≤ ~2 s) on a slow poll produces
   connect-then-abort churn that looks like an attack to a single-client server.
5. **Pause when truly unwatched** — mobile backgrounded (`AppState`), or during
   an upload/OTA (the single-client server can't serve polls and a flash upload
   at once). Resume with an immediate poll.
6. **Keep big payloads off the poll tick.** Patterns/playlists/settings load
   once on connect (or explicit refresh), never per status tick. Use the
   `/sand_patterns` ETag / `If-None-Match` path for cheap revalidation.

## Per-client status (2026-07-18)

- **Pi backend** (`dune-weaver-pi/modules/core/execution.py`) — 1 s/2 s
  active/idle base **+ heap-signal backoff to 30 s** (`POLL_LOWHEAP`). Browser
  UI is WebSocket push (no board load). Separately: move `adopt_board_playlists`
  off the status loop (event-driven) — it reads every playlist file periodically.
- **Touch panel** (`dune-weaver-pi/dune-weaver-touch`) — 1 s base **+ heap-signal
  backoff to 30 s** (`STATUS_POLL_LOWHEAP_MS`).
- **HA integration** (`dune-weaver-ha`) — 5 s base **+ heap-signal backoff to
  30 s** (already implemented; this is the reference).
- **Mobile** (`dune-weaver-mobile`) — 1 s base, **heap-signal backoff to 30 s**,
  unreachable backoff, background pause. Keeps a modest idle backoff (8 s) for
  phone battery only.

## What NOT to do

- **Don't add client-side keep-alive / connection pooling for status.** The
  firmware closes every connection; pooling buys nothing here.
- **Don't switch status to a long-lived transport** (WebSocket, SSE,
  long-poll). A held connection occupies the single client slot and starves
  every other client — this is exactly why firmware WebSockets were removed.
- **Don't add ETag/304 to `/sand_status`.** It changes every tick during
  motion; 304s only help while idle, which isn't the pressured regime.
- **Don't gate the poll rate on motion state for the board's benefit.** Idle ≠
  low-load. Gate on the heap signal instead.
