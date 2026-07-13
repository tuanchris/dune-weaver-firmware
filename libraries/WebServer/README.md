# WebServer (DW fork)

Vendored from `framework-arduinoespressif32@3.20017.241212` (arduino-esp32 v2.0.17)
`libraries/WebServer`, examples dropped. Lives here so it shadows the framework copy
via `lib_extra_dirs = libraries`.

Local changes (all tagged `DW fork:` in the sources):

- `WebServer.h`: `HTTP_MAX_DATA_WAIT` 5000 → 1000 ms, `HTTP_MAX_CLOSE_WAIT` 2000 →
  250 ms. The server handles one client at a time, so these waits are head-of-line
  blocking — under an aborted-request storm (e.g. a status poller racing an async
  WiFi scan) the 5 s waits serialize into minutes of outage.
- `WebServer.cpp`: ABORTED clients get `SO_LINGER {1,0}` (`_lingerAbort()` — close
  sends RST, no TIME_WAIT), so abort storms can't exhaust the lwIP PCB pool. Only on
  the no-request-completed paths (data-wait expiry, unparseable request, watchdog
  drain) — a served response keeps the orderly FIN close, because linger-0 close is
  `tcp_abort` and would discard response bytes still in the send buffer.
- `WebServer.{h,cpp}`: `lastHandledMillis()` + `hasPendingClient()` liveness
  accessors for the self-heal watchdog in `Web_Server::poll()`
  (`FluidNC/src/WebUI/WebServer.cpp`).
- `WebServer.{h,cpp}`: `setLowHeapGuard(floorBytes, exempt)` — when free heap is
  under the floor, `_handleRequest()` answers **503 "busy: low memory"** before
  the route handler runs. A handler whose allocations fail mid-flight stalls the
  single-threaded server and the queued clients pin even more memory (observed
  as a total HTTP outage at ~7 KB free while pings still answered). The exempt
  callback keeps must-work routes live; registration (floor 10 KB, exempting
  `/sand_status` + stop/pause/resume) is in `Web_Server::begin()`.
- `WebServer.{h,cpp}`: `_currentClientWrite{,_P}()` check the write result and
  RST+stop the client on a short write (`_abortDeadClient()`). A client that
  stalls or vanishes mid-response (the app cancelling a preview download)
  otherwise costs a full `HTTP_MAX_SEND_WAIT` per REMAINING chunk of the
  response — a 50 KB chunked reply = minutes of poller blockage (observed as
  repeating 30 s HTTP deaf spells via the stall watchdog, and one 120 s
  task-WDT panic with the heap staircasing down as blocked clients piled up).
  RST mid-response is safe: the response is already broken.
- `WebServer.{h,cpp}`: `onRequestTrace(fn)` — optional per-request hook (uri)
  called at the top of `_handleRequest()`, before the low-heap guard. Used for
  heap-drain diagnosis from the FluidNC side; no-op unless registered.

When bumping the Arduino framework, re-vendor from the new version and re-apply the
`DW fork:` blocks.
