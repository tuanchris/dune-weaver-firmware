// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "Playlist.h"
#include "PlaylistParse.h"
#include "QuietHours.h"

#include "Machine/MachineConfig.h"
#include "Settings.h"
#include "Serial.h"    // allChannels
#include "Job.h"
#include "System.h"    // state_is
#include "Protocol.h"  // LINE_BUFFER_SIZE
#include "FileStream.h"
#include "FluidPath.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>  // esp_random
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
    Playlist* playlistInstance = nullptr;

    uint32_t now_ms() {
        return xTaskGetTickCount() * portTICK_PERIOD_MS;
    }

    // How long to wait for an injected command to take effect before
    // declaring the playlist broken (missing file, alarm, etc.)
    constexpr uint32_t INJECT_TIMEOUT_MS = 8000;
    constexpr uint32_t HOMING_TIMEOUT_MS = 120000;
    constexpr size_t   MAX_ITEMS         = 1024;
    constexpr size_t   MAX_PLAYLIST_FILE = 64 * 1024;

    enum_opt_t modeOptions = {
        { "single", 0 },
        { "loop", 1 },
    };
    enum_opt_t onOffOptions = {
        { "OFF", 0 },
        { "ON", 1 },
    };
    // Values match PlaylistParse::CLEAR_*
    enum_opt_t clearOptions = {
        { "none", PlaylistParse::CLEAR_NONE },       { "adaptive", PlaylistParse::CLEAR_ADAPTIVE },
        { "in", PlaylistParse::CLEAR_IN },           { "out", PlaylistParse::CLEAR_OUT },
        { "sideway", PlaylistParse::CLEAR_SIDEWAY }, { "random", PlaylistParse::CLEAR_RANDOM },
    };

    Error cmd_run(const char* value, AuthenticationLevel auth_level, Channel& out) {
        if (!playlistInstance) {
            log_error_to(out, "No playlist: section in the config");
            return Error::InvalidStatement;
        }
        return playlistInstance->requestRun(value, out);
    }
    Error cmd_stop(const char* value, AuthenticationLevel auth_level, Channel& out) {
        if (!playlistInstance) {
            return Error::InvalidStatement;
        }
        return playlistInstance->requestStop(out);
    }
    Error cmd_skip(const char* value, AuthenticationLevel auth_level, Channel& out) {
        if (!playlistInstance) {
            return Error::InvalidStatement;
        }
        return playlistInstance->requestSkip(out);
    }
    Error cmd_list(const char* value, AuthenticationLevel auth_level, Channel& out) {
        if (!playlistInstance) {
            return Error::InvalidStatement;
        }
        return playlistInstance->showList(out);
    }
}

void Playlist::init() {
    if (!_mode) {
        _mode             = new EnumSetting("Playlist run mode", EXTENDED, WG, NULL, "Playlist/Mode", MODE_SINGLE, &modeOptions);
        _shuffle          = new EnumSetting("Playlist shuffle", EXTENDED, WG, NULL, "Playlist/Shuffle", 0, &onOffOptions);
        _pause_time       = new IntSetting("Seconds between patterns", EXTENDED, WG, NULL, "Playlist/PauseTime", 0, 0, 86400);
        _pause_from_start = new EnumSetting(
            "Measure pause from pattern start", EXTENDED, WG, NULL, "Playlist/PauseFromStart", 0, &onOffOptions);
        _clear_mode = new EnumSetting("Clear pattern mode", EXTENDED, WG, NULL, "Playlist/ClearPattern", PlaylistParse::CLEAR_NONE, &clearOptions);
        _auto_home  = new IntSetting("Home every n patterns", EXTENDED, WG, NULL, "Playlist/AutoHome", 0, 0, 1000);
        _sands_enabled = new EnumSetting("Still Sands quiet hours", EXTENDED, WG, NULL, "Sands/Enabled", 0, &onOffOptions);
        _sands_slots   = new StringSetting(
            "Quiet hour slots HH:MM-HH:MM@days,...", EXTENDED, WG, NULL, "Sands/Slots", "21:00-08:00@daily", 0, 100);

        // Handlers may run outside the protocol task and must not block
        // on motion, so synchronous=false and flag-setting only.
        new UserCommand(NULL, "Playlist/Run", cmd_run, nullptr, WG, false);
        new UserCommand(NULL, "Playlist/Stop", cmd_stop, nullptr, WG, false);
        new UserCommand(NULL, "Playlist/Skip", cmd_skip, nullptr, WG, false);
        new UserCommand(NULL, "Playlist/List", cmd_list, nullptr, WG, false);
    }
    playlistInstance = this;
    if (!_registered) {
        allChannels.registration(this);
        _registered = true;
    }
    log_info("playlist: folder " << _folder << " clear " << _clear_in << " | " << _clear_out << " | " << _clear_side);
}

void Playlist::deinit() {
    if (_registered) {
        allChannels.deregistration(this);
        _registered = false;
    }
    playlistInstance = nullptr;
}

// --- Command handlers: any task; flags only ---

Error Playlist::requestRun(const char* name, Channel& out) {
    if (!name || !*name) {
        log_error_to(out, "Usage: $Playlist/Run=<name>");
        return Error::InvalidValue;
    }
    if (state_is(State::Alarm) || state_is(State::ConfigAlarm) || state_is(State::Critical)) {
        log_error_to(out, "Machine is in alarm; home ($H) or unlock ($X) first");
        return Error::IdleError;
    }
    strlcpy(_req_name, name, sizeof(_req_name));
    _req_run = true;
    log_info_to(out, "Playlist queued: " << name);
    return Error::Ok;
}

Error Playlist::requestStop(Channel& out) {
    if (_phase == Phase::Off && !_req_run) {
        log_info_to(out, "No playlist active");
        return Error::Ok;
    }
    _req_run  = false;
    _req_stop = true;
    return Error::Ok;
}

Error Playlist::requestSkip(Channel& out) {
    if (_phase == Phase::Off) {
        log_info_to(out, "No playlist active");
        return Error::Ok;
    }
    _req_skip = true;
    return Error::Ok;
}

Error Playlist::showList(Channel& out) {
    try {
        FluidPath dir { _folder.c_str(), "sd" };
        for (auto const& entry : stdfs::directory_iterator { dir }) {
            if (!entry.is_directory()) {
                log_stream(out, "[FILE: " << entry.path().filename().c_str() << "]");
            }
        }
    } catch (std::filesystem::filesystem_error const& ex) {
        log_error_to(out, ex.what());
        return Error::FsFailedMount;
    } catch (Error err) {
        return err;
    }
    if (_phase != Phase::Off) {
        log_info_to(out,
                    "Playlist " << _playlist_name << " pattern " << (_index + 1) << "/" << _order.size() << " " << itemAt(_index));
    } else {
        log_info_to(out, "No playlist active");
    }
    return Error::Ok;
}

// --- Helpers: polling task only ---

// True while Still Sands quiet hours apply.  Rechecked at most every
// couple of seconds; requires a set clock (time: config section).
bool Playlist::quietNow(uint32_t now) {
    if (!_sands_enabled || !_sands_enabled->get()) {
        return false;
    }
    if (_last_quiet_check != 0 && now - _last_quiet_check < 2000) {
        return _quiet_cached;
    }
    _last_quiet_check = now;

    time_t t = time(nullptr);
    if (t < 1672531200) {  // clock still at the 1970 power-up default
        if (!_warned_no_time) {
            _warned_no_time = true;
            log_warn("playlist: $Sands/Enabled is ON but the clock is not set;"
                     " add a time: config section (NTP) or use $Time/Set");
        }
        _quiet_cached = false;
        return false;
    }
    _warned_no_time = false;

    std::string err;
    auto        slots = QuietHours::parse(_sands_slots->get(), &err);
    if (slots.empty()) {
        if (!err.empty() && !_warned_bad_slots) {
            _warned_bad_slots = true;
            log_warn("playlist: bad $Sands/Slots: " << err);
        }
        _quiet_cached = false;
        return false;
    }
    _warned_bad_slots = false;

    struct tm lt;
    localtime_r(&t, &lt);
    _quiet_cached = QuietHours::match(slots, lt.tm_wday, lt.tm_hour * 60 + lt.tm_min);
    return _quiet_cached;
}

void Playlist::finish(const char* why) {
    log_info("playlist: " << _playlist_name << " " << why);
    _phase = Phase::Off;
    _items.clear();
    _items.shrink_to_fit();
    _order.clear();
    _order.shrink_to_fit();
    _pending_clear.clear();
    _req_skip = false;
}

// Write the current state into the cross-task snapshot.  Runs in the
// polling task only.
void Playlist::publish() {
    bool active     = _phase != Phase::Off;
    _pub_active     = active;
    _pub_clearing   = _phase == Phase::RunClear;
    _pub_quiet      = _in_quiet;
    _pub_index      = active ? static_cast<int>(_index) : 0;
    _pub_total      = active ? static_cast<int>(_order.size()) : 0;
    if (active) {
        strlcpy(_pub_name, _playlist_name.c_str(), sizeof(_pub_name));
        strlcpy(_pub_current, _index < _order.size() ? itemAt(_index).c_str() : "", sizeof(_pub_current));
    } else {
        _pub_name[0]    = '\0';
        _pub_current[0] = '\0';
    }
}

bool Playlist::runtimeStatus(RuntimeStatus& out) {
    if (!playlistInstance) {
        return false;
    }
    Playlist* p  = playlistInstance;
    out.active   = p->_pub_active;
    out.clearing = p->_pub_clearing;
    out.quiet    = p->_pub_quiet;
    out.index    = p->_pub_index;
    out.total    = p->_pub_total;
    strlcpy(out.name, p->_pub_name, sizeof(out.name));
    strlcpy(out.current, p->_pub_current, sizeof(out.current));
    return true;
}

void Playlist::shuffleOrder() {
    for (size_t i = _order.size() - 1; i > 0; i--) {
        size_t j = esp_random() % (i + 1);
        std::swap(_order[i], _order[j]);
    }
}

// Runs in the polling task; errors go to the global log because the
// requesting channel is long gone by the time the load happens.
bool Playlist::loadPlaylist(const std::string& name) {
    std::string path = name;
    if (path.find('/') == std::string::npos) {
        path = _folder + "/" + path;
    }
    if (path.find('.') == std::string::npos) {
        path += ".txt";
    }
    if (path[0] != '/') {
        path = "/" + path;
    }

    std::string content;
    try {
        FileStream f(path.c_str(), "r", "sd");
        int        c;
        while ((c = f.read()) >= 0 && content.size() < MAX_PLAYLIST_FILE) {
            content += static_cast<char>(c);
        }
    } catch (std::filesystem::filesystem_error const& ex) {
        log_error("Playlist " << path << ": " << ex.what());
        return false;
    } catch (Error err) {
        log_error("Cannot open playlist " << path);
        return false;
    }

    _items = PlaylistParse::parse_playlist(content, MAX_ITEMS);

    if (_items.empty()) {
        log_error("Playlist " << path << " has no patterns");
        return false;
    }

    _playlist_name = name;
    _order.resize(_items.size());
    for (size_t i = 0; i < _order.size(); i++) {
        _order[i] = i;
    }
    if (_shuffle->get()) {
        shuffleOrder();
    }
    _index      = 0;
    _since_home = 0;
    _clear_done = false;
    _pending_clear.clear();
    log_info("playlist: " << name << " with " << _items.size() << " patterns, mode "
                          << (_mode->get() == MODE_LOOP ? "loop" : "single") << (_shuffle->get() ? ", shuffled" : ""));
    return true;
}

// Read the head of the pattern file and extract its first rho; -1 on failure
float Playlist::firstRho(const std::string& sdpath) {
    std::string content;
    try {
        FileStream f(sdpath.c_str(), "r", "sd");
        int        c;
        while ((c = f.read()) >= 0 && content.size() < 4096) {
            content += static_cast<char>(c);
        }
    } catch (std::filesystem::filesystem_error const&) { return -1.0f; } catch (Error) {
        return -1.0f;
    }
    return PlaylistParse::first_rho(content);
}

std::string Playlist::clearFileFor(const std::string& patternPath) {
    int   mode = _clear_mode->get();
    float rho  = mode == PlaylistParse::CLEAR_ADAPTIVE ? firstRho(patternPath) : -1.0f;

    std::string chosen;
    switch (PlaylistParse::choose_clear(mode, rho, esp_random())) {
        case PlaylistParse::Clear::FromIn:
            chosen = _clear_in;
            break;
        case PlaylistParse::Clear::FromOut:
            chosen = _clear_out;
            break;
        case PlaylistParse::Clear::Sideway:
            chosen = _clear_side;
            break;
        default:
            return "";
    }
    if (chosen.empty()) {
        return "";
    }
    try {
        FluidPath fpath { chosen.c_str(), "sd" };
        if (!stdfs::exists(fpath)) {
            log_warn("playlist: clear pattern " << chosen << " not found, skipping clear");
            return "";
        }
    } catch (...) { return ""; }
    return chosen;
}

// --- State machine: runs in the polling task via pollLine ---

Error Playlist::pollLine(char* line) {
    if (_phase == Phase::Off && !_req_run && !_req_stop) {
        return Error::NoData;
    }
    uint32_t now = now_ms();

    if (_req_stop) {
        _req_stop = false;
        _req_skip = false;
        if (_phase != Phase::Off) {
            finish("stopped");
            // Abort the running clear/pattern via the SAFE deferred path: the
            // main loop calls Job::abort() only once no line is in flight
            // (!activeChannel).  Calling Job::abort() directly here in the
            // poller deletes the job channel while the main loop / output_loop
            // still reference it -> use-after-free / pure-virtual -> abort().
            // This is the same path Cmd::StopJob (/sand_stop) already uses.
            if (Job::active()) {
                protocol_send_event(&stopJobEvent);
            }
        }
        publish();  // reflect the now-Off state in the status snapshot
        return Error::NoData;
    }

    if (_req_run) {
        // Starting (or restarting) a playlist; abort whatever is running via the
        // SAFE deferred path (not a direct Job::abort() in the poller -> UAF).
        // NextItem waits for Idle + !Job::active() before injecting the first
        // command, so the old job is fully torn down before the new one starts.
        _req_run = false;
        if (Job::active()) {
            protocol_send_event(&stopJobEvent);
        }
        if (loadPlaylist(std::string(_req_name))) {
            _phase = Phase::NextItem;
        } else {
            finish("failed to load");
        }
        return Error::NoData;
    }

    switch (_phase) {
        case Phase::Off:
            break;

        case Phase::NextItem: {
            if (state_is(State::Alarm) || state_is(State::ConfigAlarm) || state_is(State::Critical)) {
                finish("canceled by alarm");
                break;
            }
            if (quietNow(now)) {
                if (!_in_quiet) {
                    _in_quiet = true;
                    log_info("playlist: Still Sands quiet hours, pausing ($Playlist/Skip overrides for one pattern)");
                }
                if (_req_skip) {
                    _req_skip       = false;
                    _quiet_override = true;
                    log_info("playlist: quiet hours overridden for one pattern");
                }
                if (!_quiet_override) {
                    break;
                }
            } else if (_in_quiet) {
                _in_quiet       = false;
                _quiet_override = false;
                log_info("playlist: Still Sands ended, resuming");
            }
            if (_req_skip) {
                // Skip while deciding means skip this pattern entirely
                _req_skip = false;
                _since_home++;
                _index++;
                _clear_done = false;
                if (_index >= _order.size()) {
                    if (_mode->get() == MODE_LOOP) {
                        if (_shuffle->get()) {
                            shuffleOrder();
                        }
                        _index = 0;
                    } else {
                        finish("complete");
                        break;
                    }
                }
                break;
            }
            // Injecting a command needs a line slot and an idle machine, and no
            // job still tearing down (e.g. a deferred abort from skip/restart).
            if (!line || !state_is(State::Idle) || Job::active()) {
                break;
            }
            if (_auto_home->get() > 0 && _since_home >= _auto_home->get()) {
                log_info("playlist: homing after " << _since_home << " patterns");
                snprintf(line, LINE_BUFFER_SIZE, "$H");
                _phase     = Phase::Homing;
                _job_seen  = false;
                _inject_ms = now;
                return Error::Ok;
            }
            if (!_clear_done) {
                _pending_clear = clearFileFor(itemAt(_index));
                _clear_done    = true;  // decision made; clear runs (or not) exactly once
                if (!_pending_clear.empty()) {
                    log_info("playlist: clear pattern " << _pending_clear);
                    snprintf(line, LINE_BUFFER_SIZE, "$SD/Run=%s", _pending_clear.c_str());
                    _phase     = Phase::RunClear;
                    _job_seen  = false;
                    _inject_ms = now;
                    return Error::Ok;
                }
            }
            log_info("playlist: pattern " << (_index + 1) << "/" << _order.size() << ": " << itemAt(_index));
            snprintf(line, LINE_BUFFER_SIZE, "$SD/Run=%s", itemAt(_index).c_str());
            _phase            = Phase::RunPattern;
            _job_seen         = false;
            _inject_ms        = now;
            _pattern_start_ms = now;
            return Error::Ok;
        }

        case Phase::Homing: {
            if (state_is(State::Alarm) || state_is(State::Critical)) {
                finish("canceled: homing failed");
                break;
            }
            if (!_job_seen) {
                if (state_is(State::Homing)) {
                    _job_seen = true;
                } else if (now - _inject_ms > INJECT_TIMEOUT_MS) {
                    finish("canceled: homing did not start");
                }
                break;
            }
            if (state_is(State::Idle)) {
                _since_home = 0;
                _phase      = Phase::NextItem;
            } else if (now - _inject_ms > HOMING_TIMEOUT_MS) {
                finish("canceled: homing timed out");
            }
            break;
        }

        case Phase::RunClear:
        case Phase::RunPattern: {
            if (state_is(State::Alarm) || state_is(State::ConfigAlarm) || state_is(State::Critical)) {
                // Also reached when the user sends a realtime reset mid-move
                finish("canceled by alarm/reset");
                break;
            }
            if (_req_skip) {
                _req_skip = false;
                log_info("playlist: skipped");
                // Abort the running clear/pattern via the SAFE deferred path
                // (post stopJobEvent, as $Playlist/Stop does) rather than a
                // direct Job::abort() here in the poller, which frees the job
                // channel while the main loop / output_loop still use it ->
                // use-after-free -> abort().  Once the job goes Idle the
                // !Job::active() check below advances to the next item exactly
                // as a normal pattern completion would (RunClear -> NextItem,
                // RunPattern -> pattern_done).
                if (Job::active()) {
                    protocol_send_event(&stopJobEvent);
                }
                break;
            }
            if (!_job_seen) {
                if (Job::active()) {
                    _job_seen = true;
                } else if (now - _inject_ms > INJECT_TIMEOUT_MS) {
                    finish("canceled: file did not start (see log)");
                }
                break;
            }
            if (!Job::active() && state_is(State::Idle)) {
                if (_phase == Phase::RunClear) {
                    _phase = Phase::NextItem;
                    break;
                }
            pattern_done:
                _since_home++;
                _quiet_override = false;  // a Skip override is good for one pattern
                uint32_t pause_ms = static_cast<uint32_t>(_pause_time->get()) * 1000;
                if (_pause_from_start->get()) {
                    uint32_t elapsed = now - _pattern_start_ms;
                    pause_ms         = pause_ms > elapsed ? pause_ms - elapsed : 0;
                }
                _index++;
                _clear_done = false;
                if (_index >= _order.size()) {
                    if (_mode->get() == MODE_LOOP) {
                        if (_shuffle->get()) {
                            shuffleOrder();
                        }
                        _index = 0;
                    } else {
                        finish("complete");
                        break;
                    }
                }
                if (pause_ms > 0) {
                    log_info("playlist: pausing " << (pause_ms / 1000) << "s");
                    _pause_until_ms = now + pause_ms;
                    _phase          = Phase::Pausing;
                } else {
                    _phase = Phase::NextItem;
                }
            }
            break;
        }

        case Phase::Pausing: {
            if (_req_skip) {
                _req_skip = false;
                _phase    = Phase::NextItem;
                break;
            }
            if (static_cast<int32_t>(now - _pause_until_ms) >= 0) {
                _phase = Phase::NextItem;
            }
            break;
        }
    }
    publish();
    return Error::NoData;
}

// Configuration registration
namespace {
    ConfigurableModuleFactory::InstanceBuilder<Playlist> registration("playlist");
}
