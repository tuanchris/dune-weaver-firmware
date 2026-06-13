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

        writeJson(out, SandStatus::encode(d));
        return Error::Ok;
    }

    Error listDir(const char* folder, Channel& out) {
        std::vector<std::string> names;
        try {
            FluidPath dir { folder, "sd" };
            for (auto const& entry : stdfs::directory_iterator { dir }) {
                if (!entry.is_directory()) {
                    names.push_back(entry.path().filename().c_str());
                }
            }
        } catch (std::filesystem::filesystem_error const& ex) {
            log_error_to(out, ex.what());
            return Error::FsFailedMount;
        } catch (const Error err) {
            return err;
        }
        writeJson(out, SandStatus::encode_array(names));
        return Error::Ok;
    }

    Error sandPatterns(const char* value, AuthenticationLevel auth_level, Channel& out) {
        return listDir("/patterns", out);
    }

    Error sandPlaylists(const char* value, AuthenticationLevel auth_level, Channel& out) {
        return listDir("/playlists", out);
    }

    // Asynchronous so $Sand/Status reports while a pattern is running.
    UserCommand sandStatusCmd(NULL, "Sand/Status", sandStatus, nullptr, WG, false);
    UserCommand sandPatternsCmd(NULL, "Sand/Patterns", sandPatterns, nullptr, WG, false);
    UserCommand sandPlaylistsCmd(NULL, "Sand/Playlists", sandPlaylists, nullptr, WG, false);
}
