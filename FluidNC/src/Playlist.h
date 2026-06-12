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

private:
    enum class Phase : uint8_t {
        Off,         // no playlist active
        NextItem,    // decide what to do for the upcoming pattern
        Homing,      // $H injected, waiting for completion
        RunClear,    // clear pattern job
        RunPattern,  // main pattern job
        Pausing,     // waiting out PauseTime between patterns
    };

    static constexpr int CLEAR_NONE     = 0;
    static constexpr int CLEAR_ADAPTIVE = 1;
    static constexpr int CLEAR_IN       = 2;
    static constexpr int CLEAR_OUT      = 3;
    static constexpr int CLEAR_SIDEWAY  = 4;
    static constexpr int CLEAR_RANDOM   = 5;

    static constexpr int MODE_SINGLE = 0;
    static constexpr int MODE_LOOP   = 1;

    bool  loadPlaylist(const std::string& name);
    void  shuffleOrder();
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

    // Cross-task requests.  Handlers may run in another task, so the
    // name goes through a fixed buffer (a std::string assignment racing
    // a reader can corrupt the heap), written before the flag is set.
    volatile bool _req_stop = false;
    volatile bool _req_skip = false;
    volatile bool _req_run  = false;
    char          _req_name[128] = {};  // valid while _req_run is set

    // State machine (polling task only)
    Phase                 _phase = Phase::Off;
    std::vector<std::string> _items;
    std::vector<uint16_t>    _order;
    std::string              _playlist_name;
    std::string              _pending_clear;  // chosen clear file for current item
    size_t                   _index            = 0;
    bool                     _clear_done       = false;  // clear already ran for current item
    bool                     _job_seen         = false;  // injected job has been observed active
    uint32_t                 _inject_ms        = 0;      // when the last line was injected
    uint32_t                 _pattern_start_ms = 0;
    uint32_t                 _pause_until_ms   = 0;
    int                      _since_home       = 0;
    bool                     _registered       = false;
};
