// Copyright (c) 2024 Mitch Bradley All rights reserved.
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once
#include "src/Settings.h"
#include "esp_wifi.h"
#include <mdns.h>

namespace WebUI {
    class Mdns : public Module {
        static EnumSetting* _enable;

        // mDNS runs whenever WiFi is up -- STA (home network) or AP (hotspot).
        // Upstream gated this to STA; the sand table's hotspot is a primary
        // mode, so the table must stay discoverable + identity-taggable there.
        static bool active();

    public:
        Mdns(const char* name) : Module(name) {}

        void        init() override;
        void        deinit() override;
        static void add(const char* service, const char* proto, int port);
        static void addTxt(const char* service, const char* proto, const char* key, const char* value);
        static void remove(const char* service, const char* proto);
        ~Mdns() {}
    };
}
