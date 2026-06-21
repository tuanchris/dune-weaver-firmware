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
#include "Leds.h"                  // live LED control during a run

#include "Settings.h"
#include "Report.h"               // state_name()
#include "System.h"               // get_mpos()
#include "Machine/MachineConfig.h"
#include "Kinematics/Kinematics.h"
#include "Kinematics/ThetaRho.h"  // live base-feed override for status + /sand_feed?mm
#include "Job.h"
#include "FluidPath.h"
#include "FileStream.h"           // serve a prebuilt pattern manifest
#include "Protocol.h"            // protocol_request_goto (deferred jog)

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

    // Depth-first walk of an SD folder, emitting files as JSON-array elements
    // ("<subdir>/<file>") so patterns organised in subfolders are listed too.
    //
    // CRASH SAFETY: the ESP32 heap is small and fragmented (WiFi + web server),
    // and the real library is large (1000+ .thr).  Building the whole ~50 KB
    // list as one string risks a bad_alloc -> std::terminate -> panic, so we
    // stream instead: append into a small buffer and flush it through `emit`
    // (chunked HTTP / channel write) whenever it crosses kFlushAt, keeping RAM
    // bounded regardless of file count.  Depth is capped; hidden / macOS-metadata
    // names (".DS_Store", "._foo.thr") and hidden subdirs (".Trashes") are
    // skipped.
    constexpr size_t kBufReserve   = 2048;
    constexpr size_t kFlushAt      = 1536;  // < reserve, so the buffer never reallocs

    // Optional prebuilt catalog (full recursive .thr list as a JSON array); the
    // host regenerates + uploads it when patterns change.  Served verbatim by
    // /sand_patterns when present.
    constexpr const char* kPatternManifest = "/patterns/index.json";

    // Case-insensitive "does name end with ext" (ext like ".thr"); a null/empty
    // ext matches everything.
    bool hasExt(const std::string& name, const char* ext) {
        if (!ext || !*ext) {
            return true;
        }
        size_t el = strlen(ext);
        return name.size() >= el && strcasecmp(name.c_str() + name.size() - el, ext) == 0;
    }

    // Emits the matching files in ONE folder (non-recursive) as JSON-array
    // elements, keeping only names ending in `ext` (e.g. ".thr"), flushing
    // through `emit` as the buffer fills.
    //
    // Deliberately NOT recursive: the real library is ~1000 .thr nested several
    // levels deep on a slow 8 MHz SD, and a full recursive walk froze the
    // single-threaded web server for minutes.  The app owns the full catalog
    // and runs patterns by full path (e.g. $Sand/Run=/patterns/sub/x.thr), so
    // this endpoint is just a fast top-level convenience / fallback.
    void appendFilesJson(const stdfs::path& dir, const char* ext, std::string& buf, bool& first, const SandApi::JsonSink& emit) {
        for (auto const& entry : stdfs::directory_iterator { dir }) {
            std::string fname = entry.path().filename().c_str();
            if (fname.empty() || fname[0] == '.') {
                continue;
            }
            bool isdir = false;
            try {
                isdir = entry.is_directory();
            } catch (...) {
                continue;  // unstattable entry -> skip, don't abort the whole list
            }
            if (isdir || !hasExt(fname, ext)) {
                continue;  // subdirs are the app's job; skip non-matching files
            }
            {
                if (!first) {
                    buf += ',';
                }
                first = false;
                SandStatus::append_escaped(buf, fname.c_str());
                if (buf.size() >= kFlushAt) {
                    emit(buf.data(), buf.size());
                    buf.clear();
                }
            }
        }
    }

    Error sandStatus(const char* value, AuthenticationLevel auth_level, Channel& out) {
        writeJson(out, SandApi::statusJson());
        return Error::Ok;
    }

    // Stream the JSON array straight to the channel so a 1000+ pattern list is
    // never assembled as one big string (heap-safe).
    void streamToChannel(const char* folder, const char* ext, Channel& out) {
        SandApi::streamDirJson(folder, ext, [&out](const char* data, size_t len) {
            out.write(reinterpret_cast<const uint8_t*>(data), len);
        });
        out.print("\n");
    }

    Error sandPatterns(const char* value, AuthenticationLevel auth_level, Channel& out) {
        SandApi::streamPatterns([&out](const char* data, size_t len) {
            out.write(reinterpret_cast<const uint8_t*>(data), len);
        });
        out.print("\n");
        return Error::Ok;
    }

    Error sandPlaylists(const char* value, AuthenticationLevel auth_level, Channel& out) {
        streamToChannel("/playlists", ".txt", out);
        return Error::Ok;
    }

    // $Sand/Run=<file> [clear=<mode>] -- run one pattern, optionally preceded by
    // a clear (the host's "pre_execution").  The clear is sequenced by the
    // Playlist state machine so the firmware is the single source of truth for
    // the adaptive/random selection.  Filenames have no spaces, so the path is
    // the first token and "clear=<mode>" is an optional trailing token.
    Error sandRun(const char* value, AuthenticationLevel auth_level, Channel& out) {
        std::string v = value ? value : "";
        size_t      b = v.find_first_not_of(" \t");
        if (b == std::string::npos) {
            log_error_to(out, "Usage: $Sand/Run=<file> [clear=none|adaptive|in|out|sideway|random]");
            return Error::InvalidValue;
        }
        size_t      sp   = v.find_first_of(" \t", b);
        std::string path = v.substr(b, sp == std::string::npos ? std::string::npos : sp - b);

        std::string clearMode;
        if (sp != std::string::npos) {
            size_t cp = v.find("clear=", sp);
            if (cp != std::string::npos) {
                cp += 6;  // past "clear="
                size_t end = v.find_first_of(" \t", cp);
                clearMode  = v.substr(cp, end == std::string::npos ? std::string::npos : end - cp);
            }
        }
        return Playlist::runSingle(path.c_str(), clearMode.c_str(), out);
    }

    // $Sand/Led=<key>=<val> [<key>=<val> ...] -- live LED control that works
    // while a pattern is running (the $LED/* settings are idle-gated).
    Error sandLed(const char* value, AuthenticationLevel auth_level, Channel& out) {
        if (!value || !*value) {
            log_error_to(out, "Usage: $Sand/Led=effect=fire palette=ocean brightness=120");
            return Error::InvalidValue;
        }
        return static_cast<Error>(SandApi::applyLed(value, out));
    }

    // $Sand/Goto theta=<rad> rho=<0..1> -- jog to an absolute theta and/or rho
    // for manual positioning between patterns (at least one axis).
    Error sandGoto(const char* value, AuthenticationLevel auth_level, Channel& out) {
        std::string v = value ? value : "";
        bool        hasTheta = false, hasRho = false;
        float       theta = 0.0f, rho = 0.0f;
        size_t      i = 0;
        while (i < v.size()) {
            size_t b = v.find_first_not_of(" \t,&", i);
            if (b == std::string::npos) {
                break;
            }
            size_t      e     = v.find_first_of(" \t,&", b);
            std::string token = v.substr(b, e == std::string::npos ? std::string::npos : e - b);
            i                 = e == std::string::npos ? v.size() : e + 1;
            size_t eq = token.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            std::string key = token.substr(0, eq);
            float       val = strtof(token.c_str() + eq + 1, nullptr);
            if (strcasecmp(key.c_str(), "theta") == 0) {
                theta    = val;
                hasTheta = true;
            } else if (strcasecmp(key.c_str(), "rho") == 0) {
                rho    = val;
                hasRho = true;
            }
        }
        Error err = SandApi::goTo(hasTheta, theta, hasRho, rho);
        if (err == Error::InvalidValue) {
            log_error_to(out, "Usage: $Sand/Goto theta=<rad> rho=<0..1> (at least one)");
        } else if (err == Error::IdleError) {
            log_error_to(out, "goto requires idle (home first / stop the pattern)");
        }
        return err;
    }

    // Asynchronous so $Sand/Status reports while a pattern is running.
    UserCommand sandStatusCmd(NULL, "Sand/Status", sandStatus, nullptr, WG, false);
    UserCommand sandPatternsCmd(NULL, "Sand/Patterns", sandPatterns, nullptr, WG, false);
    UserCommand sandPlaylistsCmd(NULL, "Sand/Playlists", sandPlaylists, nullptr, WG, false);
    UserCommand sandRunCmd(NULL, "Sand/Run", sandRun, nullptr, WG, false);
    // cmdChecker = nullptr, so this is NOT idle-gated -- usable mid-pattern.
    UserCommand sandLedCmd(NULL, "Sand/Led", sandLed, nullptr, WG, false);
    // goTo() checks Idle itself (so it can report a clear error), so leave this
    // un-gated rather than letting the generic idle gate reject it first.
    UserCommand sandGotoCmd(NULL, "Sand/Goto", sandGoto, nullptr, WG, false);
}

// Shared status builder.  Defined outside the anonymous namespace so the web
// server's /sand_status route can reuse it; it still sees the file-local
// settingValue() helper above (internal linkage, visible for the rest of the TU).
std::string SandApi::statusJson() {
    SandStatus::Data d;
    d.state = state_name();

    // get_mpos() already returns the cartesian (theta, rho) machine position
    // (it runs motors_to_cartesian internally, see motor_steps_to_mpos), so use
    // it directly.  Re-applying motors_to_cartesian here double-transformed it
    // (theta came out ~7.96x too small and rho mis-coupled).
    if (float* mpos = get_mpos()) {
        d.theta = mpos[X_AXIS];
        d.rho   = mpos[Y_AXIS];
    }

    if (const char* feed = settingValue("THR/Feed")) {
        d.feed = strtof(feed, nullptr);
    }
    // Reflect a live /sand_feed?mm base-feed override active during a run.
    if (int live = Kinematics::ThetaRho::effectiveFeed(); live >= 0) {
        d.feed = static_cast<float>(live);
    }
    // Live override (set by /sand_feed) so the app can read back the effective
    // rate: effective mm/min = feed * feed_override / 100.
    d.feed_override = sys.f_override;

    // Single locked fetch of the job source (runs in the poller task, which is
    // also the only task that pops, so the returned pointer stays valid here).
    if (JobSource* src = Job::source()) {
        Channel* ch = src->channel();
        d.running   = true;
        d.file      = ch->name();
        d.progress  = SandStatus::parse_sd_percent(ch->_progress);
    }

    Playlist::RuntimeStatus rs;
    if (Playlist::runtimeStatus(rs)) {
        d.playlist_active            = rs.active;
        d.playlist_index             = rs.index;
        d.playlist_total             = rs.total;
        d.playlist_pause_remaining   = rs.pause_remaining;
        d.playlist_pause_total       = rs.pause_total;
        d.playlist_name              = rs.name;
        d.playlist_clearing          = rs.clearing;
        d.quiet                      = rs.quiet;
    }

    // During a pre-execution clear the running file is the CLEAR pattern, not the
    // one the user picked; we report the clear file's own progress (file stays the
    // clear file and the "clearing" flag marks the phase, so clients can show e.g.
    // a separate "clearing" bar that resets when the real pattern starts).

    // Report progress as a 0..1 fraction; keep the <0 "unknown" sentinel as-is.
    // (executed_percent / parse_sd_percent stay in percent internally so their
    // unit tests are unaffected; we scale only at the reporting boundary.)
    if (d.progress >= 0.0f) {
        d.progress /= 100.0f;
    }

    if (const char* effect = settingValue("LED/Effect")) {
        d.has_led    = true;
        d.led_effect = effect;
        if (const char* b = settingValue("LED/Brightness")) {
            d.led_brightness = atoi(b);
        }
        // Reflect any live override active during a run.
        if (Leds* leds = Leds::instance()) {
            if (const char* le = leds->liveEffect()) {
                d.led_effect = le;
            }
            int lb = leds->liveBrightness();
            if (lb >= 0) {
                d.led_brightness = lb;
            }
        }
    }

    return SandStatus::encode(d);
}

void SandApi::streamPatterns(const JsonSink& emit) {
    // Prefer a prebuilt manifest (/patterns/index.json): the full library is
    // ~1000 .thr nested deep on a slow SD, so an on-device recursive walk froze
    // the single-threaded server for minutes.  The app/host generates the
    // manifest offline and pushes it; here we just stream that one file (a fast
    // sequential read).  If it's absent (or the SD is down), fall back to a live
    // top-level listing so the endpoint still works.
    try {
        FileStream f(kPatternManifest, "r", "sd");
        char       chunk[1024];
        size_t     n;
        while ((n = f.read(chunk, sizeof(chunk))) > 0) {
            emit(chunk, n);
        }
        return;
    } catch (...) {
        // no manifest / SD unavailable -> live listing below
    }
    streamDirJson("/patterns", ".thr", emit);
}

void SandApi::streamDirJson(const char* folder, const char* ext, const JsonSink& emit) {
    std::string buf;
    bool        first = true;
    buf.reserve(kBufReserve);
    buf.push_back('[');
    try {
        // Lists one folder, non-recursive (see appendFilesJson).  Streamed via
        // `emit` so RAM stays bounded.
        FluidPath dir { folder, "sd" };
        appendFilesJson(dir, ext, buf, first, emit);
    } catch (...) {
        // Folder missing or SD not mounted -> emit the (possibly empty) array.
    }
    buf.push_back(']');
    emit(buf.data(), buf.size());
}

std::string SandApi::settingsJson() {
    // App-relevant settings, returned as strings (the app casts numeric ones).
    static const char* const keys[] = {
        "THR/Feed",
        "LED/Effect",      "LED/Palette",      "LED/Color",             "LED/Color2",
        "LED/Brightness",  "LED/Speed",        "LED/RunEffect",         "LED/IdleEffect",
        "LED/Direction",   "LED/Align",
        "Playlist/Mode",   "Playlist/Shuffle", "Playlist/PauseTime",    "Playlist/PauseFromStart",
        "Playlist/ClearPattern", "Playlist/AutoHome", "Playlist/Autostart",
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

Error SandApi::goTo(bool hasTheta, float theta, bool hasRho, float rho) {
    if (!hasTheta && !hasRho) {
        return Error::InvalidValue;  // nothing to move
    }
    // Only when no pattern is running and the machine is homed.  (Alarm = unhomed
    // -> position is garbage; Run/Hold/Jog = busy.)
    if (!state_is(State::Idle)) {
        return Error::IdleError;
    }
    if (hasRho) {
        rho = rho < 0.0f ? 0.0f : (rho > 1.0f ? 1.0f : rho);  // rho is 0..1
    }
    float feed = static_cast<float>(Kinematics::ThetaRho::effectiveFeed());
    if (feed <= 0.0f) {
        feed = 1000.0f;  // fallback if $THR/Feed unset
    }
    // Deferred to the main loop so the jog is planned in the main task, never
    // concurrently with it from the web/command task.
    protocol_request_goto(hasTheta, theta, hasRho, rho, feed);
    return Error::Ok;
}

int SandApi::applyLed(const std::string& kv, Channel& out) {
    Leds* leds = Leds::instance();
    if (!leds) {
        log_error_to(out, "leds not configured");
        return static_cast<int>(Error::InvalidStatement);
    }
    Error first = Error::Ok;
    size_t i    = 0;
    while (i < kv.size()) {
        // tokens separated by space, tab, comma or &
        size_t b = kv.find_first_not_of(" \t,&", i);
        if (b == std::string::npos) {
            break;
        }
        size_t e     = kv.find_first_of(" \t,&", b);
        std::string token = kv.substr(b, e == std::string::npos ? std::string::npos : e - b);
        i             = e == std::string::npos ? kv.size() : e + 1;

        size_t eq = token.find('=');
        if (eq == std::string::npos) {
            log_error_to(out, "bad token (need key=value): " << token);
            if (first == Error::Ok) {
                first = Error::InvalidValue;
            }
            continue;
        }
        std::string key = token.substr(0, eq);
        std::string val = token.substr(eq + 1);
        Error err       = leds->setLive(key, val);
        if (err != Error::Ok && first == Error::Ok) {
            first = err;
            log_error_to(out, "rejected " << key << "=" << val);
        }
    }
    return static_cast<int>(first);
}
