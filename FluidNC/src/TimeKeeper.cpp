// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

/*
  TimeKeeper gives the firmware wall-clock time for features like the
  Still Sands quiet hours.

  Config:
    time:
      ntp: true              # sync over SNTP once WiFi (STA) is up
      server: pool.ntp.org
      tz: UTC0               # POSIX TZ string, e.g. ICT-7 or CET-1CEST,M3.5.0,M10.5.0/3

  Commands:
    $Time/Show               # current local time and sync state
    $Time/Set=<unix epoch>   # set the clock manually (AP mode / testing)

  SNTP keeps retrying in the background, so starting it before the
  network is up is fine.  Consumers check validity with
  time(nullptr) > a 2023 epoch; the RTC starts at 1970 on power-up.
*/

#include "TimeKeeper.h"

#include "Config.h"
#include "Module.h"
#include "Settings.h"
#include "Machine/MachineConfig.h"

#include <esp_sntp.h>
#include <sys/time.h>
#include <ctime>
#include <cstdlib>
#include <string>

// Consumers treat the clock as "set" once it is past this epoch: the ESP32 RTC
// starts at 1970 on power-up, and NTP / $Time/Set jump it past 2023.
static constexpr time_t EPOCH_2023 = 1672531200;  // 2023-01-01 00:00:00 UTC

// Persisted runtime TZ override ($Time/Zone), shared with the Clock:: API.
// Non-empty wins over the config `time: tz:`.
static StringSetting* s_tz_setting = nullptr;
static std::string    s_config_tz  = "UTC0";

static void applyTz() {
    const char* tz = (s_tz_setting && *s_tz_setting->get()) ? s_tz_setting->get() : s_config_tz.c_str();
    setenv("TZ", tz, 1);
    tzset();
}

class TimeKeeper : public ConfigurableModule {
public:
    TimeKeeper(const char* name) : ConfigurableModule(name) {}

    TimeKeeper(const TimeKeeper&)            = delete;
    TimeKeeper(TimeKeeper&&)                 = delete;
    TimeKeeper& operator=(const TimeKeeper&) = delete;
    TimeKeeper& operator=(TimeKeeper&&)      = delete;

    virtual ~TimeKeeper() = default;

    static bool valid() { return Clock::isSet(); }

    void init() override {
        if (!s_tz_setting) {
            s_tz_setting = new StringSetting(
                "POSIX TZ override (empty = config time:tz)", EXTENDED, WG, NULL, "Time/Zone", "", 0, 64);
        }
        s_config_tz = _tz;
        applyTz();  // effective TZ = $Time/Zone if set, else config tz
        if (_ntp) {
            sntp_setoperatingmode(SNTP_OPMODE_POLL);
            sntp_setservername(0, _server.c_str());
            sntp_set_time_sync_notification_cb(synced);
            sntp_init();
        }
        log_info("time: tz " << getenv("TZ") << (_ntp ? " ntp " : " no ntp") << (_ntp ? _server : ""));

        if (!_commands_made) {
            _commands_made = true;
            new UserCommand(NULL, "Time/Show", showTime, nullptr, WG, false);
            new UserCommand(NULL, "Time/Set", setTime, nullptr, WG, false);
            new UserCommand(NULL, "Time/Zone", setZone, nullptr, WG, false);
        }
    }

    void deinit() override {
        if (_ntp) {
            sntp_stop();
        }
    }

    void validate() override {}
    void afterParse() override {}
    void group(Configuration::HandlerBase& handler) override {
        handler.item("ntp", _ntp);
        handler.item("server", _server);
        handler.item("tz", _tz);
    }

private:
    static void synced(struct timeval* tv) { log_info("time: synced via NTP"); }

    static Error showTime(const char* value, AuthenticationLevel auth_level, Channel& out) {
        time_t    t = time(nullptr);
        struct tm lt;
        localtime_r(&t, &lt);
        char buf[40];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %a", &lt);
        log_info_to(out, "Local time: " << buf << (valid() ? "" : " (NOT SET - no NTP sync or $Time/Set yet)"));
        return Error::Ok;
    }

    static Error setTime(const char* value, AuthenticationLevel auth_level, Channel& out) {
        if (!value || !*value) {
            log_error_to(out, "Usage: $Time/Set=<unix epoch seconds>");
            return Error::InvalidValue;
        }
        time_t t = static_cast<time_t>(strtoll(value, NULL, 10));
        if (t < EPOCH_2023) {
            log_error_to(out, "Epoch must be after 2023-01-01");
            return Error::InvalidValue;
        }
        struct timeval tv = { t, 0 };
        settimeofday(&tv, NULL);
        return showTime(NULL, auth_level, out);
    }

    static Error setZone(const char* value, AuthenticationLevel auth_level, Channel& out) {
        if (!value) {
            value = "";
        }
        s_tz_setting->setStringValue(value);  // "" clears the override -> config tz
        applyTz();
        log_info_to(out, "TZ now " << getenv("TZ"));
        return Error::Ok;
    }

    bool        _ntp    = true;
    std::string _server = "pool.ntp.org";
    std::string _tz     = "UTC0";

    bool _commands_made = false;
};

// ---- Clock:: wall-clock API (TimeKeeper.h) ----
namespace Clock {
    bool   isSet() { return time(nullptr) > EPOCH_2023; }
    time_t now() { return time(nullptr); }

    void localString(char* buf, size_t n) {
        time_t    t = time(nullptr);
        struct tm lt;
        localtime_r(&t, &lt);
        strftime(buf, n, "%Y-%m-%d %H:%M:%S", &lt);
    }

    const char* tz() {
        const char* z = getenv("TZ");
        return z ? z : "";
    }

    bool setEpoch(time_t t) {
        if (t < EPOCH_2023) {
            return false;
        }
        struct timeval tv = { t, 0 };
        settimeofday(&tv, NULL);
        return true;
    }

    bool setTz(const char* z) {
        if (s_tz_setting) {
            s_tz_setting->setStringValue(z ? z : "");  // persist (time: module present)
            applyTz();                                 // effective = setting else config tz
        } else {
            // No time: config section -> apply for this session only (the app
            // re-syncs tz/epoch on connect anyway).
            setenv("TZ", (z && *z) ? z : "UTC0", 1);
            tzset();
        }
        return true;
    }
}

namespace {
    ConfigurableModuleFactory::InstanceBuilder<TimeKeeper> registration("time");
}
