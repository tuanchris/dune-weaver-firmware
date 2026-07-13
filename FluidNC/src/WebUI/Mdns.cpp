// Copyright (c) 2024 Mitch Bradley All rights reserved.
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "src/Module.h"
#include "Mdns.h"
#include <WiFi.h>

namespace WebUI {
    EnumSetting* Mdns::_enable;

    bool Mdns::active() {
        auto mode = WiFi.getMode();
        return _enable && _enable->get() && (mode == WIFI_STA || mode == WIFI_AP || mode == WIFI_AP_STA);
    }

    void Mdns::init() {
        _enable = new EnumSetting("mDNS enable", WEBSET, WA, NULL, "MDNS/Enable", true, &onoffOptions);

        if (active()) {
            if (mdns_init()) {
                log_error("Cannot start mDNS");
                return;
            }
            const char* h = WiFi.getHostname();
            if (mdns_hostname_set(h)) {
                log_error("Cannot set mDNS hostname to " << h);
                return;
            }
            log_info("Start mDNS with hostname:http://" << h << ".local/");
        }
    }

    void Mdns::deinit() {
        mdns_free();
    }
    void Mdns::add(const char* service, const char* proto, int port) {
        if (active()) {
            mdns_service_add(NULL, service, proto, port, NULL, 0);
        }
    }
    void Mdns::addTxt(const char* service, const char* proto, const char* key, const char* value) {
        if (active()) {
            mdns_service_txt_item_set(service, proto, key, value);
        }
    }
    void Mdns::remove(const char* service, const char* proto) {
        if (active()) {
            mdns_service_remove(service, proto);
        }
    }

    ModuleFactory::InstanceBuilder<Mdns> __attribute__((init_priority(107))) mdns_module("mdns", true);
}
