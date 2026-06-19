// Sand-table headless API helpers.

#pragma once

#include <string>

class Channel;

namespace SandApi {
    // Build the $Sand/Status JSON snapshot synchronously.  Used both by the
    // $Sand/Status command handler and by the multi-client /sand_status HTTP
    // route (which returns it directly in the HTTP body, so any number of app
    // clients can poll status without contending for the single-client
    // webui-v3 WebSocket).
    std::string statusJson();

    // JSON array of the files in an SD folder, e.g. "/patterns" or
    // "/playlists" (empty array on error).  Backs /sand_patterns and
    // /sand_playlists - multi-client HTTP reads, unlike the WS-only
    // $Sand/Patterns / $Sand/Playlists commands.
    std::string listDirJson(const char* folder);

    // JSON object of the app-relevant settings (speed, LED, playlist,
    // quiet hours) as strings.  Backs /sand_settings so the app can read
    // all settings in one multi-client HTTP request.
    std::string settingsJson();

    // Apply live LED parameters from a "key=value key=value" string (keys:
    // effect palette color color2 brightness speed).  Works while a pattern
    // is running (in-memory; persisted to NVS at idle).  Backs both the
    // $Sand/Led command and the /sand_led route.  Returns the first error.
    int applyLed(const std::string& kv, Channel& out);
}
