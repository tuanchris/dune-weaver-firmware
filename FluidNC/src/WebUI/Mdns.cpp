// Copyright (c) 2024 Mitch Bradley All rights reserved.
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "src/Module.h"
#include "Mdns.h"
#include <WiFi.h>
#include <esp_heap_caps.h>

namespace WebUI {
    EnumSetting* Mdns::_enable;

    std::vector<Mdns::Service> Mdns::_services;
    std::vector<Mdns::TxtItem> Mdns::_txt;

    bool     Mdns::_running        = false;
    bool     Mdns::_shed           = false;
    bool     Mdns::_off_by_setting = false;
    uint32_t Mdns::_shed_ms    = 0;
    uint32_t Mdns::_shed_count = 0;
    uint32_t Mdns::_up_since   = 0;
    uint32_t Mdns::_next_check = 0;

    // Free-heap floor at which the responder is torn down.  It sits ABOVE
    // every web-server floor (15 KB heapWarnThreshold, the 10 KB "busy: low
    // memory" 503 guard, the 6 KB RST-at-accept hard floor) on purpose:
    // discovery is a convenience, serving the app is not, so mDNS is the first
    // thing to give way and the web server never has to start refusing clients
    // over memory mDNS was holding.  It also sits below the free heap of a
    // healthy loaded run so an ordinary pattern never trips it.
    static const uint32_t MDNS_SHED_FLOOR = 24 * 1024;

    // Restore well clear of the floor so a table hovering near it does not flap
    // the responder up and down.  The ceiling on this is what a table has free
    // WHILE DRAWING with the responder already shed, because a looping playlist
    // may not go idle for days: measured on DWMP that oscillates 48.8..50.9 KB
    // (a draw holds ~42.7 KB with mDNS up, ~49-51 KB without, both flat over a
    // 7 minute run).  48 KB did restore mid-pattern, but it sat INSIDE that
    // band and so depended on catching a favourable sample; 40 KB puts the
    // threshold clear of it for tables that idle a little lower (more LEDs, a
    // bigger playlist) while still leaving 16 KB of hysteresis over the shed
    // floor, so the ~42.7 KB the responder settles back to cannot re-shed.
    static const uint32_t MDNS_RESTORE_FLOOR = 40 * 1024;

    // Wait this long after a shed before trying again, doubling per consecutive
    // shed.  A one-off burst gets the responder straight back; a network that
    // keeps hammering us backs off instead of re-arming into the same flood.
    static const uint32_t MDNS_COOLDOWN_MS     = 60 * 1000;
    static const uint32_t MDNS_COOLDOWN_MAX_MS = 30 * 60 * 1000;

    // Responder up this long without trouble = the burst is over; forget the
    // backoff so the next unrelated event starts from a fast retry again.
    static const uint32_t MDNS_SETTLED_MS = 60 * 60 * 1000;

    // How often poll() samples the heap.  poll() runs off the hot poller loop
    // and summing the heap regions is not free, but the sample interval is
    // what the floor overshoots by: the drain runs at ~19 KB/s under a 150
    // queries/sec flood, and at 250 ms that measured a 17700-byte low against
    // a 24 KB floor (~5 KB of sampling lag plus ~2 KB to tear the responder
    // down).  10 samples/sec is still nothing next to the poller's rate and it
    // keeps the overshoot near 4 KB, so an even heavier flood than we can
    // generate still sheds above the web server's 10 KB guard.
    static const uint32_t MDNS_CHECK_EVERY_MS = 100;

    bool Mdns::active() {
        auto mode = WiFi.getMode();
        return _enable && _enable->get() && (mode == WIFI_STA || mode == WIFI_AP || mode == WIFI_AP_STA);
    }

    uint32_t Mdns::cooldownMs() {
        uint32_t ms = MDNS_COOLDOWN_MS;
        for (uint32_t i = 1; i < _shed_count && ms < MDNS_COOLDOWN_MAX_MS; ++i) {
            ms *= 2;
        }
        return ms > MDNS_COOLDOWN_MAX_MS ? MDNS_COOLDOWN_MAX_MS : ms;
    }

    // Bring the responder up and replay everything we advertise.
    bool Mdns::startResponder() {
        if (_running) {
            return true;
        }
        if (mdns_init()) {
            log_error("Cannot start mDNS");
            return false;
        }
        _running = true;

        const char* h = WiFi.getHostname();
        if (mdns_hostname_set(h)) {
            log_error("Cannot set mDNS hostname to " << h);
            stopResponder();
            return false;
        }
        for (const auto& s : _services) {
            mdns_service_add(NULL, s.service.c_str(), s.proto.c_str(), s.port, NULL, 0);
        }
        for (const auto& t : _txt) {
            mdns_service_txt_item_set(t.service.c_str(), t.proto.c_str(), t.key.c_str(), t.value.c_str());
        }
        _up_since = millis();
        return true;
    }

    void Mdns::stopResponder() {
        if (!_running) {
            return;
        }
        mdns_free();
        _running = false;
    }

    void Mdns::init() {
        _enable = new EnumSetting("mDNS enable", WEBSET, WA, NULL, "MDNS/Enable", true, &onoffOptions);

        // Record when the SETTING -- rather than a down radio -- is why we are
        // not running, so a table booted with mDNS off can be switched back on
        // from the app without a reboot.
        _off_by_setting = !_enable->get();

        if (active() && startResponder()) {
            log_info("Start mDNS with hostname:http://" << WiFi.getHostname() << ".local/");
        }
    }

    void Mdns::deinit() {
        stopResponder();
    }

    // Every mDNS query the table can answer costs heap that is not returned
    // until the response is actually transmitted -- and IDF transmits at most
    // ONE queued response per 100 ms timer tick (_mdns_scheduler_run() picks a
    // single packet and returns), while _mdns_server->tx_queue_head that they
    // pile up on is an unbounded linked list.  So above roughly 10 answered
    // queries/sec the queue grows without limit.  Measured on a bench table:
    // 10/s holds flat, 15/s loses 30 KB in 20 s, 20/s loses 61 KB, 30/s panics
    // the board outright.  Raising the mDNS task priority does NOT help --
    // the scheduler runs on the esp_timer task and the one-per-tick dispatch is
    // structural, not starvation (measured: no change whatsoever).
    //
    // Death is unpleasant and indirect: the heap hits zero, some unrelated
    // allocation fails, and if the failure happens to be IDF's mDNS receive
    // path its HOOK_MALLOC_FAILED diagnostic calls ESP_LOGE -- the board's
    // first stdio write, since the IDF log level is ERROR and nothing else ever
    // logs -- so uart_write lazily creates its VFS mutex, xQueueCreateMutex
    // fails too, and lock_init_generic answers with abort().  The reboot then
    // skips config.yaml ("due to panic") and the table comes up as Test Drive
    // with no SD.
    //
    // We cannot bound IDF's queue from here (mdns ships as a precompiled
    // libmdns.a inside the framework), so we bound the damage instead: watch
    // the heap and tear the responder down before it can take the board with
    // it.  mdns_free() releases the whole backlog at once, which is exactly the
    // memory we are trying to reclaim.
    void Mdns::poll() {
        if (!_enable) {
            return;
        }

        // $MDNS/Enable=OFF has to take the responder DOWN, not merely stop
        // watching it.  Returning early here left an already-running responder
        // answering queries with the heap guard below switched off -- strictly
        // more dangerous than leaving mDNS enabled -- and the setting looked
        // like it worked only because it was always followed by a reboot, where
        // init() never starts the responder in the first place.
        if (!_enable->get()) {
            if (_running) {
                stopResponder();
                _off_by_setting = true;
                _shed           = false;  // a deliberate stop cancels any owed shed-restore
                log_info("mDNS off ($MDNS/Enable=OFF); the table stays reachable by IP");
            }
            return;
        }

        // ...and back ON at runtime restarts it, so the switch is symmetric.
        // Deliberately scoped to a responder WE stopped for this reason: a
        // responder that never started (radio down at boot) stays that way.
        if (_off_by_setting && !_running) {
            if (!active() || !startResponder()) {
                return;
            }
            _off_by_setting = false;
            log_info("mDNS back on with hostname:http://" << WiFi.getHostname() << ".local/");
        }

        const uint32_t now = millis();
        if ((int32_t)(now - _next_check) < 0) {
            return;
        }
        _next_check = now + MDNS_CHECK_EVERY_MS;

        const uint32_t free_heap = xPortGetFreeHeapSize();

        if (_running) {
            if (free_heap < MDNS_SHED_FLOOR) {
                stopResponder();
                _shed    = true;
                _shed_ms = now;
                ++_shed_count;
                log_warn("mDNS off: only " << free_heap << " bytes free; discovery pauses for "
                                           << cooldownMs() / 1000 << "s (the table stays reachable by IP)");
            } else if (_shed_count && (uint32_t)(now - _up_since) >= MDNS_SETTLED_MS) {
                _shed_count = 0;
            }
            return;
        }

        // Only restore what we took down ourselves; a responder that never
        // started (mDNS disabled, radio down at boot) is not ours to start.
        if (!_shed) {
            return;
        }
        if (free_heap < MDNS_RESTORE_FLOOR || (uint32_t)(now - _shed_ms) < cooldownMs()) {
            return;
        }
        if (!active()) {
            return;
        }
        if (startResponder()) {
            _shed = false;
            log_info("mDNS back on at " << free_heap << " bytes free");
        } else {
            _shed_ms = now;  // failed to restart; wait out another cooldown
        }
    }

    void Mdns::setHostname(const char* hostname) {
        if (!active() || !_running) {
            return;
        }
        // The registered services were added with a NULL instance name, so they
        // follow the host record and keep advertising under the new name.
        if (mdns_hostname_set(hostname)) {
            log_error("Cannot set mDNS hostname to " << hostname);
            return;
        }
        log_info("mDNS hostname is now http://" << hostname << ".local/");
    }
    void Mdns::add(const char* service, const char* proto, int port) {
        if (!active()) {
            return;
        }
        _services.push_back({ service, proto, port });
        if (_running) {
            mdns_service_add(NULL, service, proto, port, NULL, 0);
        }
    }
    void Mdns::addTxt(const char* service, const char* proto, const char* key, const char* value) {
        if (!active()) {
            return;
        }
        _txt.push_back({ service, proto, key, value });
        if (_running) {
            mdns_service_txt_item_set(service, proto, key, value);
        }
    }
    void Mdns::remove(const char* service, const char* proto) {
        if (!active()) {
            return;
        }
        for (auto it = _services.begin(); it != _services.end();) {
            it = (it->service == service && it->proto == proto) ? _services.erase(it) : it + 1;
        }
        for (auto it = _txt.begin(); it != _txt.end();) {
            it = (it->service == service && it->proto == proto) ? _txt.erase(it) : it + 1;
        }
        if (_running) {
            mdns_service_remove(service, proto);
        }
    }

    ModuleFactory::InstanceBuilder<Mdns> __attribute__((init_priority(107))) mdns_module("mdns", true);
}
