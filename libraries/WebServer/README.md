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

When bumping the Arduino framework, re-vendor from the new version and re-apply the
`DW fork:` blocks.
