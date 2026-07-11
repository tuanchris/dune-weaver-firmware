// Sand-table fork: WiFi provisioning interface for the captive setup portal
// (WebServer.cpp).  The portal needs to know WHY the AP is up and be able to
// store credentials / switch modes; everything else about WiFi stays private
// to WifiConfig.cpp.

#pragma once

#include "src/Error.h"

#include <string>

namespace WebUI {
    // True when the AP is a STA>AP fallback (failed or unconfigured home-WiFi
    // join): captive probes serve the setup portal so the phone pops the
    // setup sheet.  False when the AP is deliberate ($WiFi/Mode=AP,
    // "standalone" mode): captive probes answer what each OS expects from the
    // real internet, so phones treat the hotspot as online, stop nagging, and
    // keep app traffic on it.
    bool wifi_ap_is_fallback();

    // Human-readable reason this boot's STA join failed ("" if it didn't) -
    // surfaced on the portal so a wrong password or a 5 GHz-only SSID is a
    // self-serve fix instead of a support ticket.
    const char* wifi_sta_fail_reason();

    const char* wifi_sta_ssid();  // stored Sta/SSID ("" if unset)
    const char* wifi_ap_ssid();   // AP/SSID, e.g. "DuneWeaver"

    // Persist home-WiFi credentials and force $WiFi/Mode=STA>AP - never plain
    // STA, so a bad password lands back in the portal instead of a dead
    // board.  The caller is responsible for rebooting.
    Error wifi_save_sta_credentials(const std::string& ssid, const std::string& password);

    // Persist $WiFi/Mode=AP.  If the AP is already up (fallback), also flips
    // the captive behavior to "standalone" live - no reboot needed; from STA
    // mode the caller must reboot to switch the radio.
    Error wifi_set_standalone();
}
