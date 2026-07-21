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
    $Playlist/ClearIn=<sd path>    clear-from-in pattern file (empty = config default)
    $Playlist/ClearOut=<sd path>   clear-from-out pattern file (empty = config default)
    $Playlist/ClearSpeed=<mm/min>  feed for clear moves; 0 = use $THR/Feed
    $Playlist/AutoHome=<n>         home every n patterns; 0 disables
                                   (honors $Sand/HomingMode + ThetaOffset)

  Mechanics: like Status_Outputs/Leds this is a Channel registered with
  allChannels, so the polling task ticks the state machine via
  pollLine().  Actions that start work are returned as command lines
  ("$SD/Run=...") for the protocol task to execute with stock
  semantics; homes are requested via protocol_do_start_home() (the same
  flag /sand_home uses) so they honor $Sand/HomingMode and run in the
  main task.  The polling task itself only calls Job::abort(), which is
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
#include "FluidPath.h"
#include "PlaylistParse.h"  // clean_line, CLEAR_*

#include <string>
#include <vector>
#include <cstdint>  // SIZE_MAX

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

    // Output to this channel is discarded, which made injected-command
    // failures invisible ("file did not start (see log)" with nothing in the
    // log).  Capture error lines (e.g. "Failed to open file" from $SD/Run)
    // so the inject-timeout path can log WHY.
    void sendLine(MsgLevel level, const char* line) override;
    void sendLine(MsgLevel level, const std::string* line) override;
    void sendLine(MsgLevel level, const std::string& line) override;
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
        // Resolved upcoming pattern (the shuffle permutation is internal, so
        // this is the only truthful "up next").  "" = unknown: last pattern
        // of a pass (next pass not yet shuffled) or a single run.
        char next[160] = {};
        // Most recently COMPLETED pattern of this run -- i.e. what is currently
        // drawn on the table.  During the between-patterns pause this names the
        // just-finished pattern so a client can show its preview.  "" until the
        // first pattern of the run completes; cleared when the run ends.
        char last[160] = {};
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

    // Effective run parameters: a per-run override (>=0, set by the boot
    // auto-play from the Autostart* settings) wins over the global $Playlist/*
    // setting (used by manual runs, override = -1).
    int runMode();
    int runShuffle();
    int runPause();
    int runPauseFromStart();
    void  shuffleOrder();
    void  publish();  // copy current state into the cross-task snapshot
    float firstRho(const std::string& sdpath);
    // chooses the clear file for the upcoming pattern; empty = none
    std::string clearFileFor(const std::string& patternPath);
    void        finish(const char* why);

    // Read the pattern path for the CURRENT logical position (_order[_index])
    // back off the SD playlist file into _current_path.  We keep only the
    // shuffled index array in RAM, not the paths, so this is where the one
    // line we need is fetched -- called once per pattern advance (between
    // patterns, machine idle).  false on a file/read error.  Single runs
    // ($Sand/Run) hold their one path in _current_path already, so it's a
    // no-op for those.
    bool resolveCurrent();

    // Advance to the next slot, wrapping (and reshuffling) in loop mode.
    // Returns false when the run is over (finish("complete") already called).
    bool advanceIndex();

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
    // App-configurable clear pattern files (override the config.yaml
    // clear_from_in/clear_from_out; default to those when unset/empty) and a
    // dedicated feed for clear moves ($Playlist/ClearSpeed; 0 = same as $THR/Feed).
    StringSetting* _clear_in_set   = nullptr;
    StringSetting* _clear_out_set  = nullptr;
    IntSetting*    _clear_speed    = nullptr;
    IntSetting*  _auto_home        = nullptr;
    EnumSetting*   _sands_enabled  = nullptr;
    StringSetting* _sands_slots    = nullptr;
    EnumSetting*   _sands_led_off  = nullptr;  // turn LEDs off during quiet hours
    EnumSetting*   _sands_finish   = nullptr;  // finish current pattern before pausing (else hold mid-run)
    StringSetting* _autostart      = nullptr;  // playlist to auto-run on boot ("" = off)
    // Per-boot-run overrides so auto-play can use different mode/pause/shuffle/
    // clear than manual runs (applied only to the autostart run).
    EnumSetting* _autostart_mode    = nullptr;
    EnumSetting* _autostart_shuffle = nullptr;
    IntSetting*  _autostart_pause   = nullptr;
    EnumSetting* _autostart_pfs     = nullptr;  // pause-from-start
    EnumSetting* _autostart_clear   = nullptr;

    // True from boot until the auto-play playlist has been kicked off (on the
    // first Idle AFTER a successful home -- Homing::homed_since_boot(); Idle
    // alone doesn't imply a known position with must_home false).  One-shot
    // per boot.
    bool _autostart_pending = false;
    // Fallback boot home: if the machine sits Idle-unhomed past a grace
    // period (no startup_line0 home arrived), auto-play requests one itself.
    bool     _autostart_home_sent  = false;  // one-shot per boot
    bool     _autostart_idle_seen  = false;  // an unhomed-Idle streak is being timed
    uint32_t _autostart_idle_ms    = 0;      // when that streak started

    bool _led_off_active = false;  // Still Sands has forced the LEDs off
    bool _quiet_held     = false;  // Still Sands has feed-held a pattern mid-run (FinishPattern=OFF)

    // Last error line delivered to this channel (see sendLine above); cleared
    // when a command is injected.  Written by the task running the injected
    // line, read by the poller -- a fixed char buffer keeps the unsynchronized
    // cross-task access harmless (worst case a garbled log string).
    void captureError(const char* line);
    char _last_err[96] = { 0 };

    // Active per-run overrides (-1 = inherit the global $Playlist/* setting),
    // copied from the staged _req_ov_* when a run starts.
    int _ov_mode = -1;
    int _ov_shuffle = -1;
    int _ov_pause = -1;
    int _ov_pfs   = -1;
    volatile int _req_ov_mode    = -1;
    volatile int _req_ov_shuffle = -1;
    volatile int _req_ov_pause   = -1;
    volatile int _req_ov_pfs     = -1;

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
    // Staged before _req_run: don't stopJobEvent an active job when starting
    // this run.  Set only by the boot autostart, whose trigger can race the
    // after_homing recenter macro being nested as a Job by $H -- aborting it
    // skipped the X0 Y0 recenter and patterns started from the switch.
    volatile bool _req_no_abort       = false;

    // State machine (polling task only)
    Phase                 _phase = Phase::Off;
    // The playlist's pattern paths are NOT held in RAM -- only this shuffled
    // permutation of valid-line indices [0.._order.size()).  The path for the
    // current position is read back off the SD file on demand (resolveCurrent
    // -> _current_path).  This is what keeps a 188-entry playlist to a few
    // hundred bytes instead of a multi-KB buffer the fragmented heap couldn't
    // allocate.
    std::vector<uint16_t>    _order;
    std::string              _playlist_path;   // SD path of the playlist .txt (for resolveCurrent); "" for single runs
    std::string              _current_path;    // resolved pattern path for _index (kept in sync by resolveCurrent)
    std::string              _next_path;       // resolved path for _index+1 ("up next"); "" = end of pass / single run
    std::string              _last_pattern;    // most recently completed pattern = what's on the table; "" until one finishes
    size_t                   _resolved_index = SIZE_MAX;  // _index that _current_path currently reflects
    std::string              _playlist_name;
    std::string              _pending_clear;  // chosen clear file for current item
    // Holds the SD mounted for the whole run.  Otherwise the mount refcount
    // hits zero between patterns and FATFS + card structs are freed and
    // re-malloc'd 2-4x per pattern - a heap-fragmentation driver and extra
    // exposure to remount failures on a marginal card.
    FluidPath _sd_hold;
    size_t                   _index            = 0;
    bool                     _single           = false;  // one-shot pattern run, not a playlist
    int                      _clear_override   = -1;     // CLEAR_* for this run, or -1 = use setting
    bool                     _clear_done       = false;  // clear already ran for current item
    int                      _consec_fail      = 0;      // unplayable patterns in a row (skip-vs-cancel; see RunPattern)
    // A manual $Playlist/Skip is in flight: set when the skip aborts the running
    // clear/pattern, consumed at the deferred completion.  It makes a skip (a) jump
    // the WHOLE item even from a clear (not just drop the clear) and (b) bypass the
    // between-patterns pause.  Distinguishes a skip-triggered completion from a
    // natural one, which look identical once the job goes Idle.
    bool                     _skip_active      = false;
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
    char          _pub_next[160]    = {};
    char          _pub_last[160]    = {};
};
