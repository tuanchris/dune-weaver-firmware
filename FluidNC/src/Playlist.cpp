// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "Playlist.h"

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
    enum_opt_t clearOptions = {
        { "none", 0 }, { "adaptive", 1 }, { "in", 2 }, { "out", 3 }, { "sideway", 4 }, { "random", 5 },
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
        _clear_mode = new EnumSetting("Clear pattern mode", EXTENDED, WG, NULL, "Playlist/ClearPattern", CLEAR_NONE, &clearOptions);
        _auto_home  = new IntSetting("Home every n patterns", EXTENDED, WG, NULL, "Playlist/AutoHome", 0, 0, 1000);

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

    _items.clear();
    size_t pos = 0;
    while (pos < content.size() && _items.size() < MAX_ITEMS) {
        size_t      eol  = content.find('\n', pos);
        std::string line = content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos              = eol == std::string::npos ? content.size() : eol + 1;

        size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }
        size_t b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos) {
            continue;
        }
        size_t e = line.find_last_not_of(" \t\r");
        line     = line.substr(b, e - b + 1);
        if (line[0] != '/') {
            line = "/" + line;
        }
        _items.push_back(line);
    }

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

// First rho of the pattern, skipping the up-to-two "0 0" preamble
// pairs that many community patterns start with (mirrors both the
// ThetaRho kinematics preamble handling and dune-weaver's logic).
float Playlist::firstRho(const std::string& sdpath) {
    float coords[3][2];
    int   found = 0;
    try {
        FileStream  f(sdpath.c_str(), "r", "sd");
        std::string line;
        int         c;
        size_t      seen = 0;
        while (found < 3 && (c = f.read()) >= 0 && ++seen < 4096) {
            if (c != '\n') {
                line += static_cast<char>(c);
                continue;
            }
            if (!line.empty() && line[0] != '#') {
                float t, r;
                if (sscanf(line.c_str(), "%f %f", &t, &r) == 2) {
                    coords[found][0] = t;
                    coords[found][1] = r;
                    found++;
                }
            }
            line.clear();
        }
    } catch (std::filesystem::filesystem_error const&) { return -1.0f; } catch (Error) {
        return -1.0f;
    }
    if (found == 0) {
        return -1.0f;
    }
    auto isZero = [](float* c) { return c[0] > -1e-9f && c[0] < 1e-9f && c[1] > -1e-9f && c[1] < 1e-9f; };
    if (found >= 3 && isZero(coords[0]) && isZero(coords[1])) {
        return coords[2][1];
    }
    return coords[0][1];
}

std::string Playlist::clearFileFor(const std::string& patternPath) {
    std::string chosen;
    switch (_clear_mode->get()) {
        case CLEAR_IN:
            chosen = _clear_in;
            break;
        case CLEAR_OUT:
            chosen = _clear_out;
            break;
        case CLEAR_SIDEWAY:
            chosen = _clear_side;
            break;
        case CLEAR_RANDOM: {
            const std::string* options[3] = { &_clear_in, &_clear_out, &_clear_side };
            chosen                        = *options[esp_random() % 3];
            break;
        }
        case CLEAR_ADAPTIVE: {
            float rho = firstRho(patternPath);
            if (rho < 0.0f) {
                // Unknown start; match dune-weaver's fallback of picking randomly
                const std::string* options[3] = { &_clear_in, &_clear_out, &_clear_side };
                chosen                        = *options[esp_random() % 3];
            } else {
                // Pattern starting near center is preceded by a clear that
                // ends at the center, and vice versa
                chosen = rho < 0.5f ? _clear_out : _clear_in;
            }
            break;
        }
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
            if (Job::active()) {
                Job::abort();
            }
            finish("stopped");
        }
        return Error::NoData;
    }

    if (_req_run) {
        // Starting (or restarting) a playlist; abort whatever is running
        _req_run = false;
        if (Job::active()) {
            Job::abort();
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
            // Injecting a command needs a line slot and an idle machine
            if (!line || !state_is(State::Idle)) {
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
                if (Job::active()) {
                    Job::abort();
                }
                log_info("playlist: skipped");
                if (_phase == Phase::RunClear) {
                    _phase = Phase::NextItem;  // skip the clear, go to the pattern
                } else {
                    goto pattern_done;
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
    return Error::NoData;
}

// Configuration registration
namespace {
    ConfigurableModuleFactory::InstanceBuilder<Playlist> registration("playlist");
}
