// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "Playlist.h"
#include "PlaylistParse.h"
#include "QuietHours.h"

#include "Machine/MachineConfig.h"
#include "Machine/Homing.h"  // homed_since_boot() gates auto-play
#include "Settings.h"
#include "Serial.h"    // allChannels
#include "Job.h"
#include "System.h"    // state_is
#include "Protocol.h"  // LINE_BUFFER_SIZE, protocol_do_start_home
#include "FileStream.h"
#include "Kinematics/ThetaRho.h"  // clearFeedLive() when a run ends
#include "Leds.h"                  // Still Sands LED-off during quiet hours
#include "FluidPath.h"
#include "TimeKeeper.h"            // Clock::isSet

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

    // True in any state where a job cannot be (re)started and a pending
    // request must be rejected. (The homing path at runStep() deliberately
    // uses a narrower check that omits ConfigAlarm.)
    bool inAlarmState() {
        return state_is(State::Alarm) || state_is(State::ConfigAlarm) || state_is(State::Critical);
    }

    // How long to wait for an injected command to take effect before
    // declaring the playlist broken (missing file, alarm, etc.)
    constexpr uint32_t INJECT_TIMEOUT_MS = 8000;
    // An unplayable pattern is skipped, but this many in a row means the SD
    // (not the file) is the problem -- cancel instead of retry-looping forever.
    constexpr int MAX_CONSEC_FAIL = 5;
    constexpr uint32_t HOMING_TIMEOUT_MS = 120000;
    // Auto-play fallback home: how long the machine may sit Idle-unhomed
    // before auto-play requests a home itself.  Long enough for the config
    // startup line ($H / $Sand/Home) to fire first, so tables that home at
    // boot don't home twice.
    constexpr uint32_t AUTOSTART_HOME_GRACE_MS = 5000;
    constexpr size_t   MAX_ITEMS         = 1024;
    constexpr size_t   MAX_PLAYLIST_FILE = 64 * 1024;
    // Longest credible playlist line (FAT path max is 255; leave room for a
    // trailing comment).  Anything longer is not a playlist line.
    constexpr size_t   MAX_LINE          = 512;

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
        // App-configurable clear pattern files: default to the config.yaml
        // clear_from_in/clear_from_out so an unset NVS value keeps today's
        // behavior; a non-empty setting overrides the file to use.
        _clear_in_set  = new StringSetting(
            "Clear-from-in pattern file", EXTENDED, WG, NULL, "Playlist/ClearIn", _clear_in.c_str(), 0, 128);
        _clear_out_set = new StringSetting(
            "Clear-from-out pattern file", EXTENDED, WG, NULL, "Playlist/ClearOut", _clear_out.c_str(), 0, 128);
        _clear_speed   = new IntSetting(
            "Clear pattern feed, motor mm/min (0 = use THR/Feed)", EXTENDED, WG, NULL, "Playlist/ClearSpeed", 0, 0, 100000);
        _auto_home  = new IntSetting("Home every n patterns", EXTENDED, WG, NULL, "Playlist/AutoHome", 0, 0, 1000);
        _sands_enabled = new EnumSetting("Still Sands quiet hours", EXTENDED, WG, NULL, "Sands/Enabled", 0, &onOffOptions);
        _sands_slots   = new StringSetting(
            "Quiet hour slots HH:MM-HH:MM@days,...", EXTENDED, WG, NULL, "Sands/Slots", "21:00-08:00@daily", 0, 100);
        _sands_led_off = new EnumSetting("Still Sands turn LEDs off", EXTENDED, WG, NULL, "Sands/LedOff", 0, &onOffOptions);
        _sands_finish  = new EnumSetting(
            "Still Sands finish current pattern before pausing", EXTENDED, WG, NULL, "Sands/FinishPattern", 1, &onOffOptions);
        _autostart = new StringSetting(
            "Playlist to auto-run on boot (empty = off)", EXTENDED, WG, NULL, "Playlist/Autostart", "", 0, 100);
        // Boot-run overrides: let auto-play use its own mode/pause/shuffle/clear,
        // independent of the manual-run $Playlist/* settings.
        _autostart_mode    = new EnumSetting("Auto-play run mode", EXTENDED, WG, NULL, "Playlist/AutostartMode", MODE_LOOP, &modeOptions);
        _autostart_shuffle = new EnumSetting("Auto-play shuffle", EXTENDED, WG, NULL, "Playlist/AutostartShuffle", 0, &onOffOptions);
        _autostart_pause   = new IntSetting("Auto-play seconds between patterns", EXTENDED, WG, NULL, "Playlist/AutostartPause", 0, 0, 86400);
        _autostart_pfs     = new EnumSetting(
            "Auto-play measure pause from start", EXTENDED, WG, NULL, "Playlist/AutostartPauseFromStart", 0, &onOffOptions);
        _autostart_clear   = new EnumSetting(
            "Auto-play clear mode", EXTENDED, WG, NULL, "Playlist/AutostartClear", PlaylistParse::CLEAR_NONE, &clearOptions);

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
    // Arm auto-play: pollLine starts the configured playlist on the first Idle
    // after boot (i.e. after homing).  Read in pollLine, not here, since the NVS
    // value may not be loaded yet at module init.
    _autostart_pending = true;
    log_info("playlist: folder " << _folder << " clear " << _clear_in << " | " << _clear_out << " | " << _clear_side);
}

void Playlist::deinit() {
    if (_registered) {
        allChannels.deregistration(this);
        _registered = false;
    }
    playlistInstance = nullptr;
}

// Effective run params: the active per-run override (set by auto-play) wins,
// else the global $Playlist/* setting (manual runs).
int Playlist::runMode() { return _ov_mode >= 0 ? _ov_mode : _mode->get(); }
int Playlist::runShuffle() { return _ov_shuffle >= 0 ? _ov_shuffle : _shuffle->get(); }
int Playlist::runPause() { return _ov_pause >= 0 ? _ov_pause : _pause_time->get(); }
int Playlist::runPauseFromStart() { return _ov_pfs >= 0 ? _ov_pfs : _pause_from_start->get(); }

// --- Command handlers: any task; flags only ---

Error Playlist::requestRun(const char* name, Channel& out) {
    if (!name || !*name) {
        log_error_to(out, "Usage: $Playlist/Run=<name>");
        return Error::InvalidValue;
    }
    if (inAlarmState()) {
        log_error_to(out, "Machine is in alarm; home ($H) or unlock ($X) first");
        return Error::IdleError;
    }
    strlcpy(_req_name, name, sizeof(_req_name));
    _req_single         = false;
    _req_clear_override = -1;  // playlist runs use the $Playlist/ClearPattern setting
    _req_ov_mode = _req_ov_shuffle = _req_ov_pause = _req_ov_pfs = -1;  // manual run: globals
    _req_no_abort       = false;
    _req_run            = true;
    log_info_to(out, "Playlist queued: " << name);
    return Error::Ok;
}

Error Playlist::runSingle(const char* patternPath, const char* clearMode, Channel& out) {
    if (!playlistInstance) {
        log_error_to(out, "No playlist: section in the config (needed to sequence the clear)");
        return Error::InvalidStatement;
    }
    return playlistInstance->requestSingle(patternPath, clearMode, out);
}

bool Playlist::stopActive() {
    if (!playlistInstance) {
        return false;
    }
    Playlist* p = playlistInstance;
    if (p->_phase == Phase::Off && !p->_req_run) {
        return false;  // nothing sequencing; the caller's job abort is enough
    }
    // Same request the $Playlist/Stop command sets; pollLine() will finish()
    // the sequence to Off (and abort the job) on its next tick, so it never
    // advances clear -> pattern or pattern -> next.
    p->_req_run  = false;
    p->_req_stop = true;
    return true;
}

Error Playlist::requestSingle(const char* patternPath, const char* clearMode, Channel& out) {
    if (!patternPath || !*patternPath) {
        log_error_to(out, "Usage: $Sand/Run=<file> [clear=none|adaptive|in|out|sideway|random]");
        return Error::InvalidValue;
    }
    if (inAlarmState()) {
        log_error_to(out, "Machine is in alarm; home ($H) or unlock ($X) first");
        return Error::IdleError;
    }
    int mode = PlaylistParse::CLEAR_NONE;
    if (clearMode && *clearMode && !PlaylistParse::parse_clear_mode(clearMode, mode)) {
        log_error_to(out, "Unknown clear mode; use none|adaptive|in|out|sideway|random");
        return Error::InvalidValue;
    }
    strlcpy(_req_name, patternPath, sizeof(_req_name));
    _req_single         = true;
    _req_clear_override = mode;
    _req_ov_mode = _req_ov_shuffle = _req_ov_pause = _req_ov_pfs = -1;  // single run: globals
    _req_no_abort       = false;
    _req_run            = true;
    log_info_to(out, "Pattern queued: " << patternPath << " clear=" << mode);
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
                    "Playlist " << _playlist_name << " pattern " << (_index + 1) << "/" << _order.size() << " " << _current_path);
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
    if (!Clock::isSet()) {  // clock still at the 1970 power-up default
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
    // Release a Still Sands mid-run hold so the machine doesn't stay in Hold
    // after the run ends (the job abort below then tears it down cleanly).
    if (_quiet_held) {
        protocol_send_event(&cycleStartEvent);
        _quiet_held = false;
    }
    // A live /sand_feed?mm override persists across the run's patterns; now the
    // run (playlist or single $Sand/Run) is over, flush it to $THR/Feed so a
    // speed set mid-run sticks as the new default (persists only when idle, the
    // natural completion path always is; a mid-move stop drops it instead).
    Kinematics::ThetaRho::flushFeedLive();
    // Drop any clear-pattern feed override too, in case the run ended (stop /
    // alarm / reset) while a clear was mid-flight.
    Kinematics::ThetaRho::setClearFeed(-1);
    _phase = Phase::Off;
    _order.clear();
    _order.shrink_to_fit();
    _playlist_path.clear();
    _playlist_path.shrink_to_fit();
    _current_path.clear();
    _current_path.shrink_to_fit();
    _next_path.clear();
    _next_path.shrink_to_fit();
    _last_pattern.clear();
    _last_pattern.shrink_to_fit();
    _resolved_index = SIZE_MAX;
    _pending_clear.clear();
    _req_skip = false;
    // Release the run-long SD hold (move-assign swaps _isSD into the
    // temporary, whose destructor drops the refcount).
    _sd_hold = FluidPath();
}

// Write the current state into the cross-task snapshot.  Runs in the
// polling task only.
void Playlist::publish() {
    // A single $Sand/Run uses this machine but is not a playlist, so it never
    // sets playlist.active (the app reads the running file from the job instead);
    // its clear phase still surfaces as "clearing" so the UI can show it.
    bool active         = _phase != Phase::Off && !_single;
    _pub_active         = active;
    _pub_clearing       = _phase == Phase::RunClear;
    _pub_quiet          = _in_quiet;
    _pub_pausing        = _phase == Phase::Pausing;
    _pub_pause_until_ms = _pause_until_ms;
    _pub_pause_total_ms = _pause_total_ms;
    _pub_index      = active ? static_cast<int>(_index) : 0;
    _pub_total      = active ? static_cast<int>(_order.size()) : 0;
    if (active) {
        strlcpy(_pub_name, _playlist_name.c_str(), sizeof(_pub_name));
        strlcpy(_pub_current, _index < _order.size() ? _current_path.c_str() : "", sizeof(_pub_current));
        strlcpy(_pub_next, _index < _order.size() ? _next_path.c_str() : "", sizeof(_pub_next));
        // What's on the table now = the last pattern that finished drawing;
        // "" until the first one completes.  Kept truthful through the pause.
        strlcpy(_pub_last, _last_pattern.c_str(), sizeof(_pub_last));
    } else {
        _pub_name[0]    = '\0';
        _pub_current[0] = '\0';
        _pub_next[0]    = '\0';
        _pub_last[0]    = '\0';
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
    // Seconds left in the between-patterns pause, computed live so it counts
    // down between status polls; -1 when not pausing.
    if (p->_pub_pausing) {
        int32_t left_ms      = static_cast<int32_t>(p->_pub_pause_until_ms - now_ms());
        out.pause_remaining  = left_ms > 0 ? (left_ms + 999) / 1000 : 0;
        out.pause_total      = (p->_pub_pause_total_ms + 999) / 1000;  // for a progress bar
    } else {
        out.pause_remaining = -1;
        out.pause_total     = -1;
    }
    strlcpy(out.name, p->_pub_name, sizeof(out.name));
    strlcpy(out.current, p->_pub_current, sizeof(out.current));
    strlcpy(out.next, p->_pub_next, sizeof(out.next));
    strlcpy(out.last, p->_pub_last, sizeof(out.last));
    return true;
}

void Playlist::shuffleOrder() {
    for (size_t i = _order.size() - 1; i > 0; i--) {
        size_t j = esp_random() % (i + 1);
        std::swap(_order[i], _order[j]);
    }
}

// Stream a playlist file line-by-line off the SD, invoking fn(validIndex,
// cleanedPath) for each non-blank/non-comment line.  fn returns false to stop
// early.  The whole file is never held in RAM (only one line at a time), and
// no pattern paths are retained -- callers decide what to keep.  Overlong
// lines (a stray binary in /playlists) are dropped whole; the byte cap and
// item cap bound the work.  Throws (FileStream) if the file can't be opened.
template <typename Fn>
static void scanValidLines(const std::string& path, Fn&& fn) {
    FileStream  f(path.c_str(), "r", "sd");
    std::string line;
    line.reserve(96);
    int    c;
    size_t consumed = 0;
    size_t valid    = 0;
    bool   overlong = false;
    auto   flush    = [&](const std::string& raw) -> bool {
        std::string p = PlaylistParse::clean_line(raw);
        if (p.empty()) {
            return true;
        }
        bool cont = fn(valid, p);
        valid++;
        return cont;
    };
    while ((c = f.read()) >= 0 && consumed < MAX_PLAYLIST_FILE && valid < MAX_ITEMS) {
        consumed++;
        if (c == '\n') {
            if (!overlong && !flush(line)) {
                return;
            }
            line.clear();
            overlong = false;
        } else if (line.size() < MAX_LINE) {
            line += static_cast<char>(c);
        } else {
            overlong = true;
        }
    }
    if (!overlong && valid < MAX_ITEMS) {
        flush(line);  // last line without a trailing newline
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

    // Count valid lines only -- the paths themselves are read back on demand
    // (resolveCurrent), so nothing but the count and the shuffle order lands
    // in RAM.  std::exception guards against a low-heap std::bad_alloc during
    // the scan (must degrade to "playlist didn't load", never abort()).
    size_t count = 0;
    try {
        scanValidLines(path, [&](size_t, const std::string&) {
            count++;
            return true;
        });
    } catch (std::filesystem::filesystem_error const& ex) {
        log_error("Playlist " << path << ": " << ex.what());
        return false;
    } catch (Error err) {
        log_error("Cannot open playlist " << path);
        return false;
    } catch (std::exception const& ex) {
        log_error("Playlist " << path << ": " << ex.what());
        return false;
    }

    if (count == 0) {
        log_error("Playlist " << path << " has no patterns");
        return false;
    }

    _playlist_path = path;
    _playlist_name = name;
    _order.resize(count);
    for (size_t i = 0; i < count; i++) {
        _order[i] = static_cast<uint16_t>(i);
    }
    if (runShuffle()) {
        shuffleOrder();
    }
    _index          = 0;
    _resolved_index = SIZE_MAX;
    _since_home     = 0;
    _clear_done     = false;
    _pending_clear.clear();
    // Prime _current_path so status is correct before the first advance.
    if (!resolveCurrent()) {
        log_error("Playlist " << path << ": first pattern unreadable");
        return false;
    }
    _resolved_index = _index;
    log_info("playlist: " << name << " with " << count << " patterns, mode "
                          << (runMode() == MODE_LOOP ? "loop" : "single") << (runShuffle() ? ", shuffled" : ""));
    return true;
}

// Read the pattern paths for the current position (_order[_index]) AND the
// upcoming one (_order[_index+1]) back off the SD playlist file in a single
// scan.  The next path only exists for the app's "up next" display -- the
// shuffle permutation is otherwise invisible outside this module, and the
// app used to guess "next" from the unshuffled file order (always showing
// line 2).  Empty _next_path = unknown (last pattern of a pass: the next
// pass's shuffle hasn't happened yet).  Single runs already hold their path
// in _current_path and have no next.
bool Playlist::resolveCurrent() {
    if (_playlist_path.empty()) {  // single $Sand/Run: _current_path is set directly
        _next_path.clear();
        return true;
    }
    if (_index >= _order.size()) {
        return false;
    }
    size_t      target      = _order[_index];
    bool        want_next   = _index + 1 < _order.size();
    size_t      next_target = want_next ? _order[_index + 1] : 0;
    std::string found;
    std::string found_next;
    try {
        // The shuffled next line can sit BEFORE the current one in the file,
        // so capture both in one pass and stop once everything wanted is in.
        scanValidLines(_playlist_path, [&](size_t vi, const std::string& p) {
            if (vi == target) {
                found = p;
            } else if (want_next && vi == next_target) {
                found_next = p;
            }
            return found.empty() || (want_next && found_next.empty());
        });
    } catch (std::exception const& ex) {
        log_error("Playlist " << _playlist_path << ": read error resolving pattern " << (_index + 1) << ": " << ex.what());
        return false;
    } catch (Error err) {
        log_error("Playlist " << _playlist_path << ": read error resolving pattern " << (_index + 1));
        return false;
    }
    if (found.empty()) {
        return false;  // line vanished (file changed under us)
    }
    _current_path = std::move(found);
    _next_path    = std::move(found_next);  // may be empty (end of pass)
    return true;
}

bool Playlist::advanceIndex() {
    _index++;
    _clear_done = false;
    if (_index >= _order.size()) {
        if (!_single && runMode() == MODE_LOOP) {
            if (runShuffle()) {
                shuffleOrder();
            }
            _index = 0;
        } else {
            finish("complete");
            return false;
        }
    }
    return true;
}

// Build a synthetic one-item "playlist" so a single $Sand/Run rides the same
// clear -> pattern -> stop state machine (and the same safe job injection) as a
// real playlist, without touching SD.  The path is normalized like a playlist
// line (leading '/').  _single makes the machine finish after one pattern and
// keeps it out of the playlist status snapshot.
void Playlist::loadSingle(const std::string& path) {
    // Verbatim except a leading '/' -- single-run paths may legitimately
    // contain '#', so NO comment stripping (unlike playlist lines).
    _current_path  = path.empty() || path[0] != '/' ? "/" + path : path;
    _next_path.clear();           // single run: nothing up next
    _playlist_path.clear();       // marks single mode: resolveCurrent is a no-op
    _order.assign(1, 0);
    _playlist_name  = _current_path;  // not surfaced (active=false) but useful in the log
    _index          = 0;
    _resolved_index = 0;              // _current_path already reflects _index
    _since_home     = 0;
    _clear_done     = false;
    _pending_clear.clear();
    log_info("playlist: single pattern " << _current_path << (_clear_override > 0 ? " (with clear)" : ""));
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
    } catch (std::filesystem::filesystem_error const&) {
        return -1.0f;
    } catch (Error) {
        return -1.0f;
    } catch (std::exception const&) {
        return -1.0f;  // low-heap std::bad_alloc etc. -> no adaptive clue, not a crash
    }
    return PlaylistParse::first_rho(content);
}

std::string Playlist::clearFileFor(const std::string& patternPath) {
    // A single $Sand/Run carries its own clear mode (the host's pre_execution);
    // a playlist run uses the persisted $Playlist/ClearPattern setting.
    int   mode = _clear_override >= 0 ? _clear_override : _clear_mode->get();
    float rho  = mode == PlaylistParse::CLEAR_ADAPTIVE ? firstRho(patternPath) : -1.0f;

    // A non-empty $Playlist/ClearIn|ClearOut overrides the config.yaml file;
    // otherwise fall back to the configured default path.
    auto pick = [](StringSetting* s, const std::string& fallback) {
        const char* v = s ? s->get() : nullptr;
        return (v && *v) ? std::string(v) : fallback;
    };
    std::string chosen;
    switch (PlaylistParse::choose_clear(mode, rho, esp_random())) {
        case PlaylistParse::Clear::FromIn:
            chosen = pick(_clear_in_set, _clear_in);
            break;
        case PlaylistParse::Clear::FromOut:
            chosen = pick(_clear_out_set, _clear_out);
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

// Error capture: this channel discards its output, so error replies to
// injected commands ("Failed to open file" from $SD/Run, "error:66") would
// otherwise vanish.  Stash the most recent one for the inject-timeout log.
void Playlist::captureError(const char* line) {
    if (!line) {
        return;
    }
    // log_error lines arrive as "[MSG:ERR: text]"; store just the text.
    if (strncmp(line, "[MSG:ERR: ", 10) == 0) {
        line += 10;
    }
    strlcpy(_last_err, line, sizeof(_last_err));
    size_t n = strlen(_last_err);
    if (n && _last_err[n - 1] == ']') {
        _last_err[n - 1] = '\0';
    }
}

void Playlist::sendLine(MsgLevel level, const char* line) {
    if (line && (level == MsgLevelError || strncmp(line, "error:", 6) == 0)) {
        captureError(line);
    }
    Channel::sendLine(level, line);
}
void Playlist::sendLine(MsgLevel level, const std::string* line) {
    if (line && (level == MsgLevelError || strncmp(line->c_str(), "error:", 6) == 0)) {
        captureError(line->c_str());
    }
    Channel::sendLine(level, line);  // base takes ownership of the pointer
}
void Playlist::sendLine(MsgLevel level, const std::string& line) {
    if (level == MsgLevelError || strncmp(line.c_str(), "error:", 6) == 0) {
        captureError(line.c_str());
    }
    Channel::sendLine(level, line);
}

// --- State machine: runs in the polling task via pollLine ---

Error Playlist::pollLine(char* line) {
    uint32_t now = now_ms();

    // Still Sands LED-off: force the strip off during quiet hours, restore on
    // exit.  Independent of the playlist phase, so it works headless and while
    // idle.  In-memory only (Leds::setQuietOff), no NVS write.
    {
        bool want_off = _sands_led_off && _sands_led_off->get() && quietNow(now);
        if (want_off != _led_off_active) {
            _led_off_active = want_off;
            if (Leds* leds = Leds::instance()) {
                leds->setQuietOff(want_off);
            }
        }
    }

    // Still Sands FinishPattern=OFF: feed-hold a pattern that is running when
    // quiet starts, and resume it when quiet ends.  (FinishPattern=ON, the
    // default, keeps the between-patterns gate in NextItem instead.)
    {
        bool finish  = !_sands_finish || _sands_finish->get();  // default ON
        bool enabled = _sands_enabled && _sands_enabled->get();
        bool q       = enabled && !finish && quietNow(now);
        if (q && !_quiet_held && _phase == Phase::RunPattern && state_is(State::Cycle)) {
            protocol_send_event(&feedHoldEvent);
            _quiet_held = true;
            log_info("playlist: Still Sands - holding pattern mid-run");
        } else if (_quiet_held && !q) {
            protocol_send_event(&cycleStartEvent);
            _quiet_held = false;
            log_info("playlist: Still Sands - resuming pattern");
        }
    }

    // Auto-play on boot: kick off the configured playlist on the first Idle
    // after a SUCCESSFUL home.  Idle alone is not enough: with must_home
    // false (all shipped configs) the machine boots straight to Idle with
    // its position unknown, and the first pattern would run from wherever
    // the ball happens to sit.  The boot home normally comes from the config
    // startup line ($H / $Sand/Home); if none arrives within a grace period
    // of continuous unhomed Idle, request one ourselves via
    // protocol_do_start_home() (honors $Sand/HomingMode).  One-shot per boot.
    if (_autostart_pending && _phase == Phase::Off && !_req_run && !_req_stop && state_is(State::Idle)) {
        const char* name = _autostart ? _autostart->get() : "";
        if (!name || !*name) {
            _autostart_pending = false;  // nothing configured; disarm
        } else if (!Machine::Homing::homed_since_boot()) {
            if (!_autostart_idle_seen) {
                _autostart_idle_seen = true;
                _autostart_idle_ms   = now;
            }
            if (!_autostart_home_sent && now - _autostart_idle_ms >= AUTOSTART_HOME_GRACE_MS) {
                _autostart_home_sent = true;
                log_info("playlist: auto-play needs a homed table; requesting home");
                protocol_do_start_home();
            }
        } else if (Job::active()) {
            // Homed, briefly Idle, but a job is still draining -- the
            // after_homing recenter macro ($H nests "G1 X0 Y0" as a Job, and
            // the state sits Idle until its move enters a Cycle).  Firing now
            // would stopJobEvent that job and the first pattern would start
            // from the switch position without the recenter.  Wait it out.
        } else {
            _autostart_pending = false;
            strlcpy(_req_name, name, sizeof(_req_name));
            _req_single = false;
            // Auto-play uses its own Autostart* settings, independent of the
            // manual-run $Playlist/* settings.
            _req_clear_override = _autostart_clear->get();
            _req_ov_mode        = _autostart_mode->get();
            _req_ov_shuffle     = _autostart_shuffle->get();
            _req_ov_pause       = _autostart_pause->get();
            _req_ov_pfs         = _autostart_pfs->get();
            _req_no_abort       = true;
            _req_run            = true;
            log_info("playlist: auto-play on boot -> " << name);
        }
    } else if (_autostart_pending && !state_is(State::Idle)) {
        // The Idle streak broke (boot home running, alarm, user motion);
        // restart the grace clock on the next Idle.
        _autostart_idle_seen = false;
    }

    if (_phase == Phase::Off && !_req_run && !_req_stop) {
        // A stop caught the pattern mid-move: finish() couldn't write $THR/Feed
        // (flash is idle-gated) so a live /sand_feed override is still pending.
        // Retry now the run is Off; it lands the moment the drain reaches Idle.
        Kinematics::ThetaRho::flushFeedLive();
        return Error::NoData;
    }

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
        _req_run        = false;
        _single         = _req_single;          // staged before _req_run was set
        _clear_override = _req_clear_override;
        _ov_mode        = _req_ov_mode;         // per-run overrides (auto-play sets these)
        _ov_shuffle     = _req_ov_shuffle;
        _ov_pause       = _req_ov_pause;
        _ov_pfs         = _req_ov_pfs;
        bool no_abort   = _req_no_abort;
        _req_no_abort   = false;
        _consec_fail    = 0;
        // no_abort (boot auto-play only): the only job that can be active at
        // boot is the after_homing recenter macro, nested by $H after the
        // autostart trigger's own Job::active() check -- let it finish;
        // NextItem waits for !Job::active() anyway.  Manual runs keep the
        // abort so "run this now" overrides whatever is playing.
        if (Job::active() && !no_abort) {
            protocol_send_event(&stopJobEvent);
        }
        // Keep the SD mounted for the whole run (released in finish()); see
        // the _sd_hold comment in Playlist.h.  A failed mount is not fatal
        // here - the pattern open will surface the error to the normal path.
        {
            std::error_code ec;
            FluidPath       hold { "", "sd", ec };
            if (!ec) {
                _sd_hold = std::move(hold);
            }
        }
        if (_single) {
            loadSingle(std::string(_req_name));  // synthetic one-item run, never fails
            _phase = Phase::NextItem;
        } else if (loadPlaylist(std::string(_req_name))) {
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
            if (inAlarmState()) {
                finish("canceled by alarm");
                break;
            }
            // Still Sands quiet hours gate playlists between patterns, but an
            // explicit single $Sand/Run is a deliberate user action and runs now.
            if (!_single && quietNow(now)) {
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
                advanceIndex();
                break;
            }
            // Injecting a command needs a line slot and an idle machine, and no
            // job still tearing down (e.g. a deferred abort from skip/restart).
            if (!line || !state_is(State::Idle) || Job::active()) {
                break;
            }
            if (!_single && _auto_home->get() > 0 && _since_home >= _auto_home->get()) {
                log_info("playlist: homing after " << _since_home << " patterns");
                // Flag the home for the main loop rather than injecting a
                // literal $H: this honors $Sand/HomingMode (crash tables have
                // no switches for $H to find) and applies $Sand/ThetaOffset,
                // exactly like /sand_home.
                protocol_do_start_home();
                _phase     = Phase::Homing;
                _job_seen  = false;
                _inject_ms = now;
                break;
            }
            // Fetch this item's path off the SD (once per item -- _order[_index]
            // is only read back here).  A read failure skips the slot rather
            // than cancelling the run (or injecting a garbage path) -- a
            // transient SD hiccup shouldn't kill an overnight playlist.  A
            // dead SD fails every slot, so the consecutive cap still cancels
            // within a few polls.
            if (_resolved_index != _index) {
                if (!resolveCurrent()) {
                    if (++_consec_fail >= MAX_CONSEC_FAIL) {
                        finish("canceled: playlist read error (see log)");
                        break;
                    }
                    log_warn("playlist: slot " << (_index + 1) << "/" << _order.size() << " unreadable, skipping");
                    advanceIndex();
                    break;
                }
                _resolved_index = _index;
            }
            if (!_clear_done) {
                _pending_clear = clearFileFor(_current_path);
                _clear_done    = true;  // decision made; clear runs (or not) exactly once
                if (!_pending_clear.empty()) {
                    log_info("playlist: clear pattern " << _pending_clear);
                    // Run the clear at its own feed if configured; disarmed when
                    // the clear finishes (RunClear -> NextItem) or the run ends.
                    int clear_speed = _clear_speed ? _clear_speed->get() : 0;
                    Kinematics::ThetaRho::setClearFeed(clear_speed > 0 ? clear_speed : -1);
                    snprintf(line, LINE_BUFFER_SIZE, "$SD/Run=%s", _pending_clear.c_str());
                    _phase       = Phase::RunClear;
                    _job_seen    = false;
                    _inject_ms   = now;
                    _last_err[0] = '\0';  // capture only errors from THIS command
                    return Error::Ok;
                }
            }
            log_info("playlist: pattern " << (_index + 1) << "/" << _order.size() << ": " << _current_path);
            snprintf(line, LINE_BUFFER_SIZE, "$SD/Run=%s", _current_path.c_str());
            _phase            = Phase::RunPattern;
            _job_seen         = false;
            _inject_ms        = now;
            _pattern_start_ms = now;
            _last_err[0]      = '\0';  // capture only errors from THIS command
            return Error::Ok;
        }

        case Phase::Homing: {
            if (state_is(State::Alarm) || state_is(State::Critical)) {
                finish("canceled: homing failed");
                break;
            }
            if (!_job_seen) {
                // Sensor homing ($H) shows State::Homing; a crash home drives
                // the rho stop as a jog and shows State::Jog.
                if (state_is(State::Homing) || state_is(State::Jog)) {
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
            if (inAlarmState()) {
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
                    if (_phase == Phase::RunPattern) {
                        _consec_fail = 0;  // a pattern started; the SD is alive
                    }
                } else if (now - _inject_ms > INJECT_TIMEOUT_MS) {
                    // Say WHICH file and WHY: the $SD/Run error reply went to
                    // this output-discarding channel, so without re-logging it
                    // here the serial log shows no reason at all.
                    const char* fname = _phase == Phase::RunClear ? _pending_clear.c_str() : _current_path.c_str();
                    log_error("playlist: " << fname << " did not start: " << (_last_err[0] ? _last_err : "no error reported"));
                    if (_single) {
                        // A deliberate one-shot $Sand/Run has nothing to skip
                        // to; fail loudly so the app surfaces it.
                        finish("canceled: file did not start (see log)");
                        break;
                    }
                    if (_phase == Phase::RunClear) {
                        // The pattern itself may be fine; drop the clear and
                        // try it (_clear_done is already true for this item).
                        Kinematics::ThetaRho::setClearFeed(-1);
                        _phase = Phase::NextItem;
                        break;
                    }
                    // Skip the unplayable pattern instead of cancelling the
                    // whole run.  A wholesale SD failure would turn skipping
                    // into an endless retry carousel, so a streak of failures
                    // (never interrupted by a pattern that starts) cancels.
                    if (++_consec_fail >= MAX_CONSEC_FAIL) {
                        finish("canceled: too many unplayable patterns in a row (see log)");
                        break;
                    }
                    log_warn("playlist: skipping " << _current_path);
                    _since_home++;
                    if (advanceIndex()) {
                        _phase = Phase::NextItem;
                    }
                }
                break;
            }
            if (!Job::active() && state_is(State::Idle)) {
                if (_phase == Phase::RunClear) {
                    // Clear done -- drop its feed override so the pattern runs
                    // at the normal $THR/Feed (or /sand_feed) speed.
                    Kinematics::ThetaRho::setClearFeed(-1);
                    _phase = Phase::NextItem;
                    break;
                }
            pattern_done:
                // Remember what just finished drawing: it's what's on the table
                // now, so a client can preview it during the upcoming pause.
                // (advanceIndex below moves _index on; _current_path isn't
                // re-resolved until NextItem, but capture here to be explicit.)
                _last_pattern = _current_path;
                _since_home++;
                _quiet_override = false;  // a Skip override is good for one pattern
                uint32_t pause_ms = static_cast<uint32_t>(runPause()) * 1000;
                if (runPauseFromStart()) {
                    uint32_t elapsed = now - _pattern_start_ms;
                    pause_ms         = pause_ms > elapsed ? pause_ms - elapsed : 0;
                }
                if (!advanceIndex()) {
                    break;
                }
                if (pause_ms > 0) {
                    log_info("playlist: pausing " << (pause_ms / 1000) << "s");
                    _pause_until_ms = now + pause_ms;
                    _pause_total_ms = pause_ms;  // full duration, for the status progress bar
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
