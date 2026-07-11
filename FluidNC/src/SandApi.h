// Sand-table headless API helpers.

#pragma once

#include <string>
#include <functional>
#include <cstddef>

#include "Error.h"

class Channel;

namespace SandApi {
    // Sink for streamed JSON output: receives the body in chunks (chunked HTTP
    // send, or a direct channel write) so a large listing is never assembled as
    // one string in RAM.
    using JsonSink = std::function<void(const char* data, size_t len)>;
    // Build the $Sand/Status JSON snapshot synchronously.  Used both by the
    // $Sand/Status command handler and by the multi-client /sand_status HTTP
    // route (which returns it directly in the HTTP body, so any number of app
    // clients can poll status without contending for the single-client
    // webui-v3 WebSocket).
    std::string statusJson();

    // Lowercase STA MAC "aa:bb:cc:dd:ee:ff" — the table's stable hardware
    // identity, so clients can dedupe a table across discovery paths (mDNS vs
    // manual IP) and DHCP changes.  Surfaced in /sand_status ("mac") and the
    // mDNS TXT record ("mac=").  Cached; always available (burned-in efuse).
    const char* macAddress();

    // Stream a JSON array of the files in ONE SD folder (non-recursive), e.g.
    // "/patterns" or "/playlists", to `emit` in bounded-size chunks.  If `ext`
    // is given (e.g. ".thr") only files with that extension are listed (skips
    // .webp thumbnails etc.); subfolders are NOT descended.  The full library
    // is ~1000 .thr nested deep on a slow SD - a recursive walk froze the
    // single-threaded server for minutes, so the app owns the full catalog and
    // runs patterns by full path; this endpoint is a fast top-level fallback.
    // Backs /sand_patterns and /sand_playlists, plus the $Sand/Patterns /
    // $Sand/Playlists commands.
    void streamDirJson(const char* folder, const char* ext, const JsonSink& emit);

    // Stream the pattern catalog to `emit`: serves the prebuilt manifest
    // /patterns/index.json verbatim when present (the host generates + uploads
    // it, avoiding the slow on-device recursive walk), else falls back to a
    // live top-level streamDirJson("/patterns", ".thr").  Backs /sand_patterns.
    void streamPatterns(const JsonSink& emit);

    // JSON object of the app-relevant settings (speed, LED, playlist,
    // quiet hours) as strings.  Backs /sand_settings so the app can read
    // all settings in one multi-client HTTP request.
    std::string settingsJson();

    // JSON object of the wall clock: {epoch, synced, local, tz}.  Backs the
    // /sand_status "time" block and the /sand_time route (which also sets the
    // clock/tz for app auto-sync).
    std::string timeJson();

    // SD readability probe: mounts the card and reads the root directory.
    // Called once at boot (the verdict lands in the startup log, hence in
    // /sand_bootlog) and cached; /sand_status reports it as "sd_ok" so the
    // app can flag a dead/unseated card without a request failing first.
    bool checkSd();
    bool sdOk();

    // Apply live LED parameters from a "key=value key=value" string (keys:
    // effect palette color color2 brightness speed).  Works while a pattern
    // is running (in-memory; persisted to NVS at idle).  Backs both the
    // $Sand/Led command and the /sand_led route.  Returns the first error.
    int applyLed(const std::string& kv, Channel& out);

    // Jog to an absolute theta (radians) and/or rho (0..1) for manual
    // positioning between patterns.  At least one axis must be present.  rho is
    // clamped to 0..1; theta is the absolute machine angle in radians.  Requires
    // Idle (returns Error::IdleError if a pattern is running or unhomed); the
    // jog itself is run by the main loop (see protocol_request_goto).  Backs
    // $Sand/Goto and /sand_goto.
    Error goTo(bool hasTheta, float theta, bool hasRho, float rho);
}
