// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

/*
  Playlist sequences .thr pattern files from SD so a sand table runs
  standalone: clear pattern -> pattern -> pause -> next, with optional
  shuffle, looping, and homing every N patterns.

  Playlists are plain text files on SD (default folder /playlists),
  one SD-relative pattern path per line; blank lines and lines starting
  with # are ignored.

  Config:
    playlist:
      folder: /playlists
      clear_from_in: /patterns/clear_from_in.thr
      clear_from_out: /patterns/clear_from_out.thr
      clear_sideway: /patterns/clear_sideway.thr

  Commands:
    $Playlist/Run=<name>   start (name resolves to <folder>/<name>.txt)
    $Playlist/Stop         stop after aborting the current pattern
    $Playlist/Skip         abort current pattern / end pause, go to next
    $Playlist/List         show folder contents and current status

  Still Sands quiet hours pause the playlist between patterns while
  the local time (see the time: config section) is inside a slot:
    $Sands/Enabled=ON|OFF
    $Sands/Slots=21:00-08:00@daily   (see QuietHours.h for the syntax)
  $Playlist/Skip during quiet hours overrides them for one pattern.

  Settings (NVS-persisted):
    $Playlist/Mode=single|loop
    $Playlist/Shuffle=ON|OFF       reshuffled on every loop pass
    $Playlist/PauseTime=<sec>      wait between patterns
    $Playlist/PauseFromStart=ON|OFF  measure the cadence from pattern start
    $Playlist/ClearPattern=none|adaptive|in|out|sideway|random
    $Playlist/AutoHome=<n>         home every n patterns; 0 disables

  Mechanics: like Status_Outputs/Leds this is a Channel registered with
  allChannels, so the polling task ticks the state machine via
  pollLine().  Actions that start work are returned as command lines
  ("$SD/Run=...", "$H") for the protocol task to execute with stock
  semantics; the polling task itself only calls Job::abort(), which is
  the same task that does so for EOF/alarm unwinding in Protocol.cpp.
  Command handlers may run in other tasks and therefore only set
  request flags.

  While a pattern is running, line-oriented input is consumed only from
  the job channel, so $Playlist/Stop typed over serial executes when
  the pattern ends.  A realtime reset (or any alarm) mid-pattern
  cancels the playlist.  Stop/Skip act immediately during the pause
  between patterns, and via channels that execute commands outside the
  protocol task (e.g. the web API).
*/

#include "Config.h"
#include "Module.h"
#include "Channel.h"

#include <string>
#include <vector>

class IntSetting;
class EnumSetting;
class StringSetting;

class Playlist : public Channel, public ConfigurableModule {
public:
    Playlist(const char* name) : Channel(name), ConfigurableModule(name) {}

    Playlist(const Playlist&)            = delete;
    Playlist(Playlist&&)                 = delete;
    Playlist& operator=(const Playlist&) = delete;
    Playlist& operator=(Playlist&&)      = delete;

    virtual ~Playlist() = default;

    void init() override;
    void deinit() override;

    // Channel interface; produces command lines, discards output
    size_t write(uint8_t data) override { return 1; }
    Error  pollLine(char* line) override;
    void   flushRx() override {}
    bool   lineComplete(char*, char) override { return false; }
    size_t timedReadBytes(char* buffer, size_t length, TickType_t timeout) override { return 0; }

    // Configuration handlers
    void validate() override {}
    void afterParse() override {}
    void group(Configuration::HandlerBase& handler) override {
        handler.item("folder", _folder);
        handler.item("clear_from_in", _clear_in);
        handler.item("clear_from_out", _clear_out);
        handler.item("clear_sideway", _clear_side);
    }

    // Command handlers (may be called from any task; set flags only)
    Error requestRun(const char* name, Channel& out);
    Error requestStop(Channel& out);
    Error requestSkip(Channel& out);
    Error showList(Channel& out);

    // Run a single pattern, optionally preceded by a clear (the host's
    // "pre_execution").  clearMode is a PlaylistParse clear-mode name
    // ("none"|"adaptive"|"in"|"out"|"sideway"|"random"); nullptr/empty == none.
    // This reuses the playlist state machine (clear -> pattern, then stop) so
    // the clear is sequenced safely, but it is NOT reported as an active
    // playlist.  Returns InvalidStatement if no playlist: section is configured
    // (the clear file paths live there).  Static so SandApi can call it.
    static Error runSingle(const char* patternPath, const char* clearMode, Channel& out);

    // Stop the playlist / single-run (clear->pattern) sequence if one is
    // active, so a global stop (/sand_stop) halts the whole sequence instead
    // of letting the state machine advance to the next item (the aborted job
    // going Idle otherwise looks like normal completion).  Returns true if a
    // sequence was active.  Static + safe when no playlist module exists.
    static bool stopActive();

    // Cross-task status snapshot for the JSON API.  Uses fixed buffers
    // (not std::string) so a reader in another task never races a heap
    // free.  Returns false if no playlist module is configured.
    struct RuntimeStatus {
        bool active   = false;
        bool clearing = false;
        bool quiet    = false;
        int  index    = 0;
        int  total    = 0;
        int  pause_remaining = -1;  // seconds left in the between-patterns pause; -1 if not pausing
        int  pause_total     = -1;  // full duration of that pause, seconds; -1 if not pausing (for a progress bar)
        char name[64]     = {};
        char current[160] = {};
    };
    static bool runtimeStatus(RuntimeStatus& out);

private:
    enum class Phase : uint8_t {
        Off,         // no playlist active
        NextItem,    // decide what to do for the upcoming pattern
        Homing,      // $H injected, waiting for completion
        RunClear,    // clear pattern job
        RunPattern,  // main pattern job
        Pausing,     // waiting out PauseTime between patterns
    };

    static constexpr int MODE_SINGLE = 0;
    static constexpr int MODE_LOOP   = 1;

    Error requestSingle(const char* patternPath, const char* clearMode, Channel& out);

    bool  loadPlaylist(const std::string& name);
    void  loadSingle(const std::string& path);  // synthetic one-item run
    bool  quietNow(uint32_t now);
    void  shuffleOrder();
    void  publish();  // copy current state into the cross-task snapshot
    float firstRho(const std::string& sdpath);
    // chooses the clear file for the upcoming pattern; empty = none
    std::string clearFileFor(const std::string& patternPath);
    void        finish(const char* why);

    const std::string& itemAt(size_t idx) { return _items[_order[idx]]; }

    // Configuration
    std::string _folder     = "/playlists";
    std::string _clear_in   = "/patterns/clear_from_in.thr";
    std::string _clear_out  = "/patterns/clear_from_out.thr";
    std::string _clear_side = "/patterns/clear_sideway.thr";

    // Settings
    EnumSetting* _mode             = nullptr;
    EnumSetting* _shuffle          = nullptr;
    IntSetting*  _pause_time       = nullptr;
    EnumSetting* _pause_from_start = nullptr;
    EnumSetting* _clear_mode       = nullptr;
    IntSetting*  _auto_home        = nullptr;
    EnumSetting*   _sands_enabled  = nullptr;
    StringSetting* _sands_slots    = nullptr;

    // Cross-task requests.  Handlers may run in another task, so the
    // name goes through a fixed buffer (a std::string assignment racing
    // a reader can corrupt the heap), written before the flag is set.
    volatile bool _req_stop = false;
    volatile bool _req_skip = false;
    volatile bool _req_run  = false;
    char          _req_name[128] = {};  // valid while _req_run is set
    // Staged with _req_name before _req_run is set: whether the queued run is a
    // single pattern (vs a playlist) and, for single runs, the clear-mode
    // override (a PlaylistParse CLEAR_* value, or -1 to use the NVS setting).
    volatile bool _req_single         = false;
    volatile int  _req_clear_override = -1;

    // State machine (polling task only)
    Phase                 _phase = Phase::Off;
    std::vector<std::string> _items;
    std::vector<uint16_t>    _order;
    std::string              _playlist_name;
    std::string              _pending_clear;  // chosen clear file for current item
    size_t                   _index            = 0;
    bool                     _single           = false;  // one-shot pattern run, not a playlist
    int                      _clear_override   = -1;     // CLEAR_* for this run, or -1 = use setting
    bool                     _clear_done       = false;  // clear already ran for current item
    bool                     _job_seen         = false;  // injected job has been observed active
    uint32_t                 _inject_ms        = 0;      // when the last line was injected
    uint32_t                 _pattern_start_ms = 0;
    uint32_t                 _pause_until_ms   = 0;
    uint32_t                 _pause_total_ms   = 0;
    int                      _since_home       = 0;
    bool                     _registered       = false;

    // Still Sands (polling task only)
    uint32_t _last_quiet_check = 0;
    bool     _quiet_cached     = false;
    bool     _in_quiet         = false;  // currently inside quiet hours
    bool     _quiet_override   = false;  // Skip pressed: run one pattern anyway
    bool     _warned_no_time   = false;
    bool     _warned_bad_slots = false;

    // Published snapshot (written by the polling task, read by the API
    // command handler in another task).  POD only - no std::string.
    volatile bool     _pub_active         = false;
    volatile bool     _pub_clearing       = false;
    volatile bool     _pub_quiet          = false;
    volatile bool     _pub_pausing        = false;  // in the between-patterns pause
    volatile int      _pub_index          = 0;
    volatile int      _pub_total          = 0;
    volatile uint32_t _pub_pause_until_ms = 0;  // deadline; remaining computed live at read
    volatile uint32_t _pub_pause_total_ms = 0;  // full pause duration, for the status progress bar
    char          _pub_name[64]     = {};
    char          _pub_current[160] = {};
};
