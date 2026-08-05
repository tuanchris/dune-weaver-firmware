// Copyright (c) 2024 Mitch Bradley All rights reserved.
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once
#include "src/Settings.h"
#include "esp_wifi.h"
#include <mdns.h>
#include <string>
#include <vector>

namespace WebUI {
    class Mdns : public Module {
        static EnumSetting* _enable;

        // mDNS runs whenever WiFi is up -- STA (home network) or AP (hotspot).
        // Upstream gated this to STA; the sand table's hotspot is a primary
        // mode, so the table must stay discoverable + identity-taggable there.
        static bool active();

        // What we advertise.  Remembered because the low-heap shed in poll()
        // tears the responder all the way down with mdns_free(), which drops
        // every registration with it -- the modules that called add()/addTxt()
        // (WebServer, TelnetServer) run their init() once at boot and have no
        // reason to call again, so rebuilding is our job.
        struct Service {
            std::string service;
            std::string proto;
            int         port;
        };
        struct TxtItem {
            std::string service;
            std::string proto;
            std::string key;
            std::string value;
        };
        static std::vector<Service> _services;
        static std::vector<TxtItem> _txt;

        // Responder lifecycle.  _running tracks whether mdns_init() is in
        // effect; _shed says we took it down ourselves and owe a restore.
        static bool     _running;
        static bool     _shed;
        static uint32_t _shed_ms;     // millis() when we last shed
        static uint32_t _shed_count;  // consecutive sheds, drives the backoff
        static uint32_t _up_since;    // millis() when the responder last came up
        static uint32_t _next_check;  // millis() of the next heap sample

        static bool startResponder();
        static void stopResponder();
        static uint32_t cooldownMs();

    public:
        Mdns(const char* name) : Module(name) {}

        void init() override;
        void deinit() override;

        // Watches free heap and drops the responder when it gets dangerous.
        // See the comment block in poll() for why this is necessary.
        void poll() override;

        // Re-advertise under a new <hostname>.local without a reboot, so a
        // table renamed with $Hostname is discoverable under the new name
        // right away.  No-op when mDNS is off or WiFi is down.
        static void setHostname(const char* hostname);

        static void add(const char* service, const char* proto, int port);
        static void addTxt(const char* service, const char* proto, const char* key, const char* value);
        static void remove(const char* service, const char* proto);
        ~Mdns() {}
    };
}
