// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

/*
  SandApi exposes the sand-table state a custom UI or app needs as JSON,
  riding the existing command dispatch so it works over serial, the
  websocket, and the /command HTTP endpoint with no new web routing:

    $Sand/Status      one JSON object: state, theta/rho, feed, current
                      file + progress, playlist position, LED, quiet
    $Sand/Patterns    JSON array of .thr files in /patterns
    $Sand/Playlists   JSON array of playlist files in /playlists

  Actions stay as the existing commands the UI already drives:
  $SD/Run, $Playlist/Run|Stop|Skip, ! / ~ (pause/resume), $THR/Feed,
  $LED/*, $H.  All three commands are asynchronous so $Sand/Status can
  be polled while a pattern is running.

  See PORTING.md "JSON API" for the full UI<->firmware command map.
*/

#include "SandApi.h"
#include "SandStatus.h"
#include "Playlist.h"

#include "Settings.h"
#include "Report.h"               // state_name()
#include "System.h"               // get_mpos()
#include "Machine/MachineConfig.h"
#include "Kinematics/Kinematics.h"
#include "Job.h"
#include "FluidPath.h"

#include <cstring>
#include <cstdlib>
#include <vector>

namespace {
    const char* settingValue(const char* name) {
        for (Setting* s : Setting::List) {
            if (strcasecmp(s->getName(), name) == 0) {
                return s->getStringValue();
            }
        }
        return nullptr;
    }

    void writeJson(Channel& out, const std::string& json) {
        out.print(json.c_str());
        out.print("\n");
    }

    Error sandStatus(const char* value, AuthenticationLevel auth_level, Channel& out) {
        writeJson(out, SandApi::statusJson());
        return Error::Ok;
    }

    Error sandPatterns(const char* value, AuthenticationLevel auth_level, Channel& out) {
        writeJson(out, SandApi::listDirJson("/patterns"));
        return Error::Ok;
    }

    Error sandPlaylists(const char* value, AuthenticationLevel auth_level, Channel& out) {
        writeJson(out, SandApi::listDirJson("/playlists"));
        return Error::Ok;
    }

    // Asynchronous so $Sand/Status reports while a pattern is running.
    UserCommand sandStatusCmd(NULL, "Sand/Status", sandStatus, nullptr, WG, false);
    UserCommand sandPatternsCmd(NULL, "Sand/Patterns", sandPatterns, nullptr, WG, false);
    UserCommand sandPlaylistsCmd(NULL, "Sand/Playlists", sandPlaylists, nullptr, WG, false);
}

// Shared status builder.  Defined outside the anonymous namespace so the web
// server's /sand_status route can reuse it; it still sees the file-local
// settingValue() helper above (internal linkage, visible for the rest of the TU).
std::string SandApi::statusJson() {
    SandStatus::Data d;
    d.state = state_name();

    if (config && config->_kinematics) {
        float cartesian[MAX_N_AXIS] = {};
        config->_kinematics->motors_to_cartesian(cartesian, get_mpos(), Axes::_numberAxis);
        d.theta = cartesian[X_AXIS];
        d.rho   = cartesian[Y_AXIS];
    }

    if (const char* feed = settingValue("THR/Feed")) {
        d.feed = strtof(feed, nullptr);
    }

    if (Job::active()) {
        Channel* job = Job::channel();
        d.running    = true;
        d.file       = job->name();
        d.progress   = SandStatus::parse_sd_percent(job->_progress);
    }

    Playlist::RuntimeStatus rs;
    if (Playlist::runtimeStatus(rs)) {
        d.playlist_active   = rs.active;
        d.playlist_index    = rs.index;
        d.playlist_total    = rs.total;
        d.playlist_name     = rs.name;
        d.playlist_clearing = rs.clearing;
        d.quiet             = rs.quiet;
    }

    if (const char* effect = settingValue("LED/Effect")) {
        d.has_led    = true;
        d.led_effect = effect;
        if (const char* b = settingValue("LED/Brightness")) {
            d.led_brightness = atoi(b);
        }
    }

    return SandStatus::encode(d);
}

std::string SandApi::listDirJson(const char* folder) {
    std::vector<std::string> names;
    try {
        FluidPath dir { folder, "sd" };
        for (auto const& entry : stdfs::directory_iterator { dir }) {
            if (!entry.is_directory()) {
                names.push_back(entry.path().filename().c_str());
            }
        }
    } catch (...) {
        // Folder missing or SD not mounted -> empty list (HTTP route stays 200).
    }
    return SandStatus::encode_array(names);
}

std::string SandApi::settingsJson() {
    // App-relevant settings, returned as strings (the app casts numeric ones).
    static const char* const keys[] = {
        "THR/Feed",
        "LED/Effect",      "LED/Color",        "LED/Brightness",        "LED/Speed",
        "LED/RunEffect",   "LED/IdleEffect",
        "Playlist/Mode",   "Playlist/Shuffle", "Playlist/PauseTime",    "Playlist/PauseFromStart",
        "Playlist/ClearPattern", "Playlist/AutoHome",
        "Sands/Enabled",   "Sands/Slots",
    };
    std::vector<std::pair<std::string, std::string>> kv;
    for (const char* k : keys) {
        if (const char* v = settingValue(k)) {
            kv.emplace_back(k, v);
        }
    }
    return SandStatus::encode_object(kv);
}
