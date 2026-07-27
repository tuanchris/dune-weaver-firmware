// Copyright (c) 2014 Luc Lebosse. All rights reserved.
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "src/Settings.h"
#include "src/Machine/MachineConfig.h"
#include <sstream>
#include <iomanip>

#include "src/Channel.h"     // Channel
#include "src/Error.h"       // Error
#include "src/Module.h"      // Module
#include "src/Job.h"         // Job::active() - pattern boundary for the STA link supervisor
#include "src/Types.h"       // State, state_is()
#include "Authentication.h"  // AuthenticationLevel

#include "src/Main.h"

#include "WebServer.h"             // Web_Server::port()
#include "WifiConfig.h"            // provisioning interface for the captive portal
#include "CaptiveDns.h"            // stop the responder when the recovery AP comes down
#include "TelnetServer.h"          // TelnetServer::port()
#include "NotificationsService.h"  // notificationsservice

#include <WiFi.h>
#include <esp_wifi.h>
#include "Driver/localfs.h"
#include <string>
#include <cstring>

#include <esp_ota_ops.h>

namespace WebUI {
    enum WiFiStartupMode {
        WiFiOff = 0,
        WiFiSTA,
        WiFiAP,
        WiFiFallback,  // Try STA and fall back to AP if STA fails
    };

    const enum_opt_t wifiModeOptions = {
        { "Off", WiFiOff },
        { "STA", WiFiSTA },
        { "AP", WiFiAP },
        { "STA>AP", WiFiFallback },
    };

    const enum_opt_t wifiPsModeOptions = {
        { "None", WIFI_PS_NONE },
        { "Min", WIFI_PS_MIN_MODEM },
        { "Max", WIFI_PS_MAX_MODEM },
    };

    enum WiFiCountry {
        WiFiCountry01 = 0,  // country "01" is the safest set of settings which complies with all regulatory domains
        WiFiCountryAT,
        WiFiCountryAU,
        WiFiCountryBE,
        WiFiCountryBG,
        WiFiCountryBR,
        WiFiCountryCA,
        WiFiCountryCH,
        WiFiCountryCN,
        WiFiCountryCY,
        WiFiCountryCZ,
        WiFiCountryDE,
        WiFiCountryDK,
        WiFiCountryEE,
        WiFiCountryES,
        WiFiCountryFI,
        WiFiCountryFR,
        WiFiCountryGB,
        WiFiCountryGR,
        WiFiCountryHK,
        WiFiCountryHR,
        WiFiCountryHU,
        WiFiCountryIE,
        WiFiCountryIN,
        WiFiCountryIS,
        WiFiCountryIT,
        WiFiCountryJP,
        WiFiCountryKR,
        WiFiCountryLI,
        WiFiCountryLT,
        WiFiCountryLU,
        WiFiCountryLV,
        WiFiCountryMT,
        WiFiCountryMX,
        WiFiCountryNL,
        WiFiCountryNO,
        WiFiCountryNZ,
        WiFiCountryPL,
        WiFiCountryPT,
        WiFiCountryRO,
        WiFiCountrySE,
        WiFiCountrySI,
        WiFiCountrySK,
        WiFiCountryTW,
        WiFiCountryUS,
    };

    const enum_opt_t wifiCountryOptions = {
        { "01", WiFiCountry01 }, { "AT", WiFiCountryAT }, { "AU", WiFiCountryAU }, { "BE", WiFiCountryBE }, { "BG", WiFiCountryBG },
        { "BR", WiFiCountryBR }, { "CA", WiFiCountryCA }, { "CH", WiFiCountryCH }, { "CN", WiFiCountryCN }, { "CY", WiFiCountryCY },
        { "CZ", WiFiCountryCZ }, { "DE", WiFiCountryDE }, { "DK", WiFiCountryDK }, { "EE", WiFiCountryEE }, { "ES", WiFiCountryES },
        { "FI", WiFiCountryFI }, { "FR", WiFiCountryFR }, { "GB", WiFiCountryGB }, { "GR", WiFiCountryGR }, { "HK", WiFiCountryHK },
        { "HR", WiFiCountryHR }, { "HU", WiFiCountryHU }, { "IE", WiFiCountryIE }, { "IN", WiFiCountryIN }, { "IS", WiFiCountryIS },
        { "IT", WiFiCountryIT }, { "JP", WiFiCountryJP }, { "KR", WiFiCountryKR }, { "LI", WiFiCountryLI }, { "LT", WiFiCountryLT },
        { "LU", WiFiCountryLU }, { "LV", WiFiCountryLV }, { "MT", WiFiCountryMT }, { "MX", WiFiCountryMX }, { "NL", WiFiCountryNL },
        { "NO", WiFiCountryNO }, { "NZ", WiFiCountryNZ }, { "PL", WiFiCountryPL }, { "PT", WiFiCountryPT }, { "RO", WiFiCountryRO },
        { "SE", WiFiCountrySE }, { "SI", WiFiCountrySI }, { "SK", WiFiCountrySK }, { "TW", WiFiCountryTW }, { "US", WiFiCountryUS },
    };

    static const char* NULL_IP = "0.0.0.0";

    //boundaries
    static constexpr int MAX_SSID_LENGTH     = 32;
    static constexpr int MIN_SSID_LENGTH     = 0;  // Allow null SSIDs as a way to disable
    static constexpr int MAX_PASSWORD_LENGTH = 64;
    //min size of password is 0 or upper than 8 char
    //so let set min is 8
    static constexpr int MIN_PASSWORD_LENGTH = 8;
    static constexpr int MAX_HOSTNAME_LENGTH = 32;
    static constexpr int MIN_HOSTNAME_LENGTH = 1;

    static constexpr int DHCP_MODE   = 0;
    static constexpr int STATIC_MODE = 1;

    static const enum_opt_t staModeOptions = {
        { "DHCP", DHCP_MODE },
        { "Static", STATIC_MODE },
    };

    static const enum_opt_t staSecurityOptions = {
        { "OPEN", WIFI_AUTH_OPEN },
        { "WEP", WIFI_AUTH_WEP },
        { "WPA-PSK", WIFI_AUTH_WPA_PSK },
        { "WPA2-PSK", WIFI_AUTH_WPA2_PSK },
        { "WPA-WPA2-PSK", WIFI_AUTH_WPA_WPA2_PSK },
        { "WPA2-ENTERPRISE", WIFI_AUTH_WPA2_ENTERPRISE },
    };

    class PasswordSetting : public StringSetting {
    public:
        PasswordSetting(const char* description, const char* grblName, const char* name, const char* defVal) :
            StringSetting(description, WEBSET, WA, grblName, name, defVal, MIN_PASSWORD_LENGTH, MAX_PASSWORD_LENGTH) {
            load();
        }
        const char* getDefaultString() { return "********"; }
        const char* getStringValue() { return "********"; }
    };

    class HostnameSetting : public StringSetting {
    public:
        HostnameSetting(const char* description, const char* grblName, const char* name, const char* defVal) :
            StringSetting(description, WEBSET, WA, grblName, name, defVal, MIN_HOSTNAME_LENGTH, MAX_HOSTNAME_LENGTH) {
            load();
        }
        Error setStringValue(std::string_view s) {
            // Hostname strings may contain only letters, digits and -
            for (auto const& c : s) {
                if (c == ' ' || !(isdigit(c) || isalpha(c) || c == '-')) {
                    return Error::InvalidValue;
                }
            }
            return StringSetting::setStringValue(s);
        }
    };

    static EnumSetting*     _mode;
    static StringSetting*   _sta_ssid;
    static HostnameSetting* _hostname;
    static IntSetting*      _ap_channel;
    static IPaddrSetting*   _ap_ip;
    static PasswordSetting* _ap_password;
    static StringSetting*   _ap_ssid;
    static EnumSetting*     _ap_country;
    static IPaddrSetting*   _sta_netmask;
    static IPaddrSetting*   _sta_gateway;
    static IPaddrSetting*   _sta_ip;
    static EnumSetting*     _sta_mode;
    static EnumSetting*     _fast_scan;
    static EnumSetting*     _sta_min_security;
    static PasswordSetting* _sta_password;
    static EnumSetting*     _wifi_ps_mode;
    static IntSetting*      _ap_fallback_min;

    // Captive-portal state (see WifiConfig.h): why the AP is up, and why the
    // last STA join failed.  Both are per-boot; a failed join after /wifi_save
    // happens on the NEXT boot, which is also the boot whose fallback AP
    // serves the portal, so RAM is the right lifetime.
    static bool        _ap_is_fallback = false;
    static std::string _sta_fail_reason;

    bool wifi_ap_is_fallback() {
        return _ap_is_fallback;
    }
    const char* wifi_sta_fail_reason() {
        return _sta_fail_reason.c_str();
    }
    const char* wifi_sta_ssid() {
        return _sta_ssid ? _sta_ssid->get() : "";
    }
    const char* wifi_ap_ssid() {
        return _ap_ssid ? _ap_ssid->get() : "";
    }

    Error wifi_save_sta_credentials(const std::string& ssid, const std::string& password) {
        Error err = _sta_ssid->setStringValue(ssid);
        if (err != Error::Ok) {
            return err;
        }
        err = _sta_password->setStringValue(password);
        if (err != Error::Ok) {
            return err;
        }
        return _mode->setStringValue("STA>AP");
    }

    Error wifi_set_standalone() {
        Error err = _mode->setStringValue("AP");
        if (err == Error::Ok) {
            _ap_is_fallback = false;  // live flip: captive probes now answer "online"
        }
        return err;
    }

    // --- STA link supervisor state (see pollStaLink) ---------------------
    // Wall-clock bookkeeping only; the policy lives in pollStaLink().
    static uint32_t _sta_down_since   = 0;      // millis of the first poll that saw the link down; 0 = up
    static uint32_t _sta_next_try     = 0;      // millis before which we do not retry
    static uint32_t _sta_tries        = 0;      // consecutive retries since the link went down
    static uint32_t _sta_gate_checked = 0;      // millis of the last Job::active() sample
    static bool     _sta_job_active   = false;  // last sampled Job::active(), for the falling edge
    static bool     _sta_pattern_done = false;  // sticky: a job finished, a retry window is owed
    static bool     _ap_raised        = false;  // WE raised the setup hotspot alongside the station
    static bool     _ap_probing       = false;  // a bounded home-network probe is in flight
    static uint32_t _ap_probe_until   = 0;      // millis the current probe gives up at

    class WiFiConfig : public Module {
    private:
        static void print_mac(Channel& out, const char* prefix, const char* mac) { log_stream(out, prefix << " (" << mac << ")"); }

        static Error showIP(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP111
            log_stream(out, parameter << IP_string(WiFi.getMode() == WIFI_STA ? WiFi.localIP() : WiFi.softAPIP()));
            return Error::Ok;
        }

        static Error showSetStaParams(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP103
            if (*parameter == '\0') {
                log_stream(out,
                           "IP:" << _sta_ip->getStringValue() << " GW:" << _sta_gateway->getStringValue()
                                 << " MSK:" << _sta_netmask->getStringValue());
                return Error::Ok;
            }
            std::string gateway, netmask, ip;
            if (!(get_param(parameter, "GW", gateway) && get_param(parameter, "MSK", netmask) && get_param(parameter, "IP", ip))) {
                return Error::InvalidValue;
            }

            Error err = _sta_ip->setStringValue(ip);
            if (err == Error::Ok) {
                err = _sta_netmask->setStringValue(netmask);
            }
            if (err == Error::Ok) {
                err = _sta_gateway->setStringValue(gateway);
            }
            return err;
        }

        void wifi_stats(JSONencoder& j) {
            j.id_value_object("Sleep mode", WiFi.getSleep() ? "Modem" : "None");
            int mode = WiFi.getMode();
            if (mode != WIFI_OFF) {
                //Is OTA available ?
                size_t flashsize = 0;
                if (esp_ota_get_running_partition()) {
                    const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
                    if (partition) {
                        flashsize = partition->size;
                    }
                }
                j.id_value_object("Available Size for update", formatBytes(flashsize));
                j.id_value_object("Available Size for LocalFS", formatBytes(localfs_size()));
                j.id_value_object("Web port", Web_Server::port());
                j.id_value_object("Data port", TelnetServer::port());
                j.id_value_object("Hostname", WiFi.getHostname());
            }

            switch (mode) {
                case WIFI_STA:

                    j.id_value_object("Current WiFi Mode", std::string("STA (") + WiFi.macAddress().c_str() + ")");

                    if (WiFi.isConnected()) {  //in theory no need but ...
                        j.id_value_object("Connected to", WiFi.SSID().c_str());
                        j.id_value_object("Signal", std::string("") + std::to_string(getSignal(WiFi.RSSI())) + "%");

                        uint8_t PhyMode;
                        esp_wifi_get_protocol(WIFI_IF_STA, &PhyMode);
                        const char* modeName;
                        switch (PhyMode) {
                            case WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N:
                                modeName = "11n";
                                break;
                            case WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G:
                                modeName = "11g";
                                break;
                            case WIFI_PROTOCOL_11B:
                                modeName = "11b";
                                break;
                            default:
                                modeName = "???";
                        }

                        j.id_value_object("Phy Mode", modeName);
                        j.id_value_object("Channel", WiFi.channel());

                        tcpip_adapter_dhcp_status_t dhcp_status;
                        tcpip_adapter_dhcpc_get_status(TCPIP_ADAPTER_IF_STA, &dhcp_status);
                        j.id_value_object("IP Mode", (dhcp_status == TCPIP_ADAPTER_DHCP_STARTED ? "DHCP" : "Static"));
                        j.id_value_object("IP", IP_string(WiFi.localIP()));
                        j.id_value_object("Gateway", IP_string(WiFi.gatewayIP()));
                        j.id_value_object("Mask", IP_string(WiFi.subnetMask()));
                        j.id_value_object("DNS", IP_string(WiFi.dnsIP()));

                    }  //this is web command so connection => no command
                    j.id_value_object("Disabled Mode", std::string("AP (") + WiFi.softAPmacAddress().c_str() + ")");
                    break;
                case WIFI_AP:
                    j.id_value_object("Current WiFi Mode", std::string("AP (") + WiFi.softAPmacAddress().c_str() + ")");
                    wifi_config_t  conf;
                    wifi_country_t country;
                    esp_wifi_get_config(WIFI_IF_AP, &conf);
                    esp_wifi_get_country(&country);
                    j.id_value_object("SSID", (const char*)conf.ap.ssid);
                    j.id_value_object("Visible", (conf.ap.ssid_hidden == 0 ? "Yes" : "No"));
                    j.id_value_object("Radio country set",
                                      std::string("") + country.cc[0] + country.cc[1] + " (channels " + std::to_string(country.schan) +
                                          "-" + std::to_string((country.schan + country.nchan - 1)) + ", max power " +
                                          std::to_string(country.max_tx_power) + "dBm)");

                    const char* mode;
                    switch (conf.ap.authmode) {
                        case WIFI_AUTH_OPEN:
                            mode = "None";
                            break;
                        case WIFI_AUTH_WEP:
                            mode = "WEP";
                            break;
                        case WIFI_AUTH_WPA_PSK:
                            mode = "WPA-PSK";
                            break;
                        case WIFI_AUTH_WPA2_PSK:
                            mode = "WPA2-PSK";
                            break;
                        case WIFI_AUTH_WPA_WPA2_PSK:
                            mode = "WPA-WPA2-PSK";
                            break;
                        default:
                            mode = "WPA/WPA2";
                    }

                    j.id_value_object("Authentication", mode);
                    j.id_value_object("Max Connections", conf.ap.max_connection);

                    tcpip_adapter_dhcp_status_t dhcp_status;
                    tcpip_adapter_dhcps_get_status(TCPIP_ADAPTER_IF_AP, &dhcp_status);
                    j.id_value_object("DHCP Server", (dhcp_status == TCPIP_ADAPTER_DHCP_STARTED ? "Started" : "Stopped"));

                    j.id_value_object("IP", IP_string(WiFi.softAPIP()));

                    tcpip_adapter_ip_info_t ip_AP;
                    tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip_AP);
                    j.id_value_object("Gateway", IP_string(IPAddress(ip_AP.gw.addr)));
                    j.id_value_object("Mask", IP_string(IPAddress(ip_AP.netmask.addr)));

                    wifi_sta_list_t          station;
                    tcpip_adapter_sta_list_t tcpip_sta_list;
                    esp_wifi_ap_get_sta_list(&station);
                    tcpip_adapter_get_sta_list(&station, &tcpip_sta_list);
                    j.id_value_object("Connected channels", station.num);

                    for (int i = 0; i < station.num; i++) {
                        j.id_value_object("",
                                          std::string("") + mac2str(tcpip_sta_list.sta[i].mac) + " " +
                                              IP_string(IPAddress(tcpip_sta_list.sta[i].ip.addr)));
                    }
                    j.id_value_object("Disabled Mode", std::string("STA (") + WiFi.macAddress().c_str() + ")");
                    break;
                case WIFI_AP_STA:  //we should not be in this state but just in case ....
                    j.id_value_object("Mixed", std::string("STA (") + WiFi.macAddress().c_str() + ")");
                    j.id_value_object("Mixed", std::string("AP (") + WiFi.softAPmacAddress().c_str() + ")");
                    break;
                default:  //we should not be there if no wifi ....

                    j.id_value_object("Current WiFi Mode", "Off");
                    break;
            }
        }

        void status_report(Channel& out) {
            log_stream(out, "Sleep mode: " << (WiFi.getSleep() ? "Modem" : "None"));
            int mode = WiFi.getMode();
            if (mode != WIFI_OFF) {
                //Is OTA available ?
                size_t flashsize = 0;
                if (esp_ota_get_running_partition()) {
                    const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
                    if (partition) {
                        flashsize = partition->size;
                    }
                }
                log_stream(out, "Available Size for update: " << formatBytes(flashsize));
                log_stream(out, "Available Size for LocalFS: " << formatBytes(localfs_size()));
                log_stream(out, "Web port: " << Web_Server::port());
                log_stream(out, "Hostname: " << WiFi.getHostname());
            }

            switch (mode) {
                case WIFI_STA:
                    print_mac(out, "Current WiFi Mode: STA", WiFi.macAddress().c_str());

                    if (WiFi.isConnected()) {  //in theory no need but ...
                        log_stream(out, "Connected to: " << WiFi.SSID().c_str());
                        log_stream(out, "Signal: " << getSignal(WiFi.RSSI()) << "%");

                        uint8_t PhyMode;
                        esp_wifi_get_protocol(WIFI_IF_STA, &PhyMode);
                        const char* phyModeName;
                        switch (PhyMode) {
                            case WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N:
                                phyModeName = "11n";
                                break;
                            case WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G:
                                phyModeName = "11g";
                                break;
                            case WIFI_PROTOCOL_11B:
                                phyModeName = "11b";
                                break;
                            default:
                                phyModeName = "???";
                        }
                        log_stream(out, "Phy Mode: " << phyModeName);
                        log_stream(out, "Channel: " << WiFi.channel());

                        tcpip_adapter_dhcp_status_t dhcp_status;
                        tcpip_adapter_dhcpc_get_status(TCPIP_ADAPTER_IF_STA, &dhcp_status);
                        log_stream(out, "IP Mode: " << (dhcp_status == TCPIP_ADAPTER_DHCP_STARTED ? "DHCP" : "Static"));
                        log_stream(out, "IP: " << IP_string(WiFi.localIP()));
                        log_stream(out, "Gateway: " << IP_string(WiFi.gatewayIP()));
                        log_stream(out, "Mask: " << IP_string(WiFi.subnetMask()));
                        log_stream(out, "DNS: " << IP_string(WiFi.dnsIP()));

                    }  //this is web command so connection => no command
                    print_mac(out, "Disabled Mode: AP", WiFi.softAPmacAddress().c_str());
                    break;
                case WIFI_AP:
                    print_mac(out, "Current WiFi Mode: AP", WiFi.softAPmacAddress().c_str());

                    wifi_config_t  conf;
                    wifi_country_t country;
                    esp_wifi_get_config(WIFI_IF_AP, &conf);
                    esp_wifi_get_country(&country);
                    log_stream(out, "SSID: " << (const char*)conf.ap.ssid);
                    log_stream(out, "Visible: " << (conf.ap.ssid_hidden == 0 ? "Yes" : "No"));
                    log_stream(out,
                               "Radio country set: " << country.cc[0] << country.cc[1] << " (channels " << country.schan << "-"
                                                     << (country.schan + country.nchan - 1) << ", max power " << country.max_tx_power
                                                     << "dBm)");

                    const char* mode;
                    switch (conf.ap.authmode) {
                        case WIFI_AUTH_OPEN:
                            mode = "None";
                            break;
                        case WIFI_AUTH_WEP:
                            mode = "WEP";
                            break;
                        case WIFI_AUTH_WPA_PSK:
                            mode = "WPA-PSK";
                            break;
                        case WIFI_AUTH_WPA2_PSK:
                            mode = "WPA2-PSK";
                            break;
                        case WIFI_AUTH_WPA_WPA2_PSK:
                            mode = "WPA-WPA2-PSK";
                            break;
                        default:
                            mode = "WPA/WPA2";
                    }

                    log_stream(out, "Authentication: " << mode);
                    log_stream(out, "Max Connections: " << conf.ap.max_connection);

                    tcpip_adapter_dhcp_status_t dhcp_status;
                    tcpip_adapter_dhcps_get_status(TCPIP_ADAPTER_IF_AP, &dhcp_status);
                    log_stream(out, "DHCP Server: " << (dhcp_status == TCPIP_ADAPTER_DHCP_STARTED ? "Started" : "Stopped"));

                    log_stream(out, "IP: " << IP_string(WiFi.softAPIP()));

                    tcpip_adapter_ip_info_t ip_AP;
                    tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip_AP);
                    log_stream(out, "Gateway: " << IP_string(IPAddress(ip_AP.gw.addr)));
                    log_stream(out, "Mask: " << IP_string(IPAddress(ip_AP.netmask.addr)));

                    wifi_sta_list_t          station;
                    tcpip_adapter_sta_list_t tcpip_sta_list;
                    esp_wifi_ap_get_sta_list(&station);
                    tcpip_adapter_get_sta_list(&station, &tcpip_sta_list);
                    log_stream(out, "Connected channels: " << station.num);

                    for (int i = 0; i < station.num; i++) {
                        log_stream(out, mac2str(tcpip_sta_list.sta[i].mac) << " " << IP_string(IPAddress(tcpip_sta_list.sta[i].ip.addr)));
                    }
                    print_mac(out, "Disabled Mode: STA", WiFi.macAddress().c_str());
                    break;
                case WIFI_AP_STA:  //we should not be in this state but just in case ....
                    log_string(out, "");

                    print_mac(out, "Mixed: STA", WiFi.macAddress().c_str());
                    print_mac(out, "Mixed: AP", WiFi.softAPmacAddress().c_str());
                    break;
                default:  //we should not be there if no wifi ....

                    log_string(out, "Current WiFi Mode: Off");
                    break;
            }

            LogStream s(out, "Notifications: ");
            s << (NotificationsService::started() ? "Enabled" : "Disabled");
            if (NotificationsService::started()) {
                s << "(" << NotificationsService::getTypeString() << ")";
            }
        }

        static const char* modeName() {
            switch (WiFi.getMode()) {
                case WIFI_OFF:
                    return "None";
                case WIFI_STA:
                    return "STA";
                case WIFI_AP:
                    return "AP";
                default:
                    return "?";
            }
        }

        bool _events_registered = false;

        static Error showFwInfoJSON(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP800
            if (strstr(parameter, "json=yes") != NULL) {
                JSONencoder j(true, &out);
                j.begin();
                j.member("cmd", "800");
                j.member("status", "ok");
                j.begin_member_object("data");
                j.member("FWVersion", git_info);
                j.member("FWTarget", "FluidNC");
                j.member("FWTargetId", "60");
                j.member("WebUpdate", "Enabled");

                j.member("Setup", "Disabled");
                j.member("SDConnection", "direct");
                j.member("SerialProtocol", "Socket");
#ifdef ENABLE_AUTHENTICATION
                j.member("Authentication", "Enabled");
#else
                j.member("Authentication", "Disabled");
#endif
                j.member("WebCommunication", "Synchronous");

                switch (WiFi.getMode()) {
                  case WIFI_AP:
                    j.member("WebSocketIP", IP_string(WiFi.softAPIP()));
                    break;
                  case WIFI_STA:
                    j.member("WebSocketIP", IP_string(WiFi.localIP()));
                    break;
                  case WIFI_AP_STA:
                    j.member("WebSocketIP", IP_string(WiFi.softAPIP()));
                    break;
                  default:
                    j.member("WebSocketIP", "0.0.0.0");
                    break;
                }

                j.member("WebSocketPort", std::to_string(Web_Server::port() + 2));
                j.member("HostName", WiFi.getHostname());
                j.member("WiFiMode", modeName());
                j.member("FlashFileSystem", "LittleFS");
                j.member("HostPath", "/");
                j.member("Time", "none");
                j.member("Axisletters", Axes::_names);
                j.end_object();
                j.end();
                return Error::Ok;
            }

            return Error::InvalidStatement;
        }

        static Error showFwInfo(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP800
            if (parameter != NULL && paramIsJSON(parameter)) {
                return showFwInfoJSON(parameter, auth_level, out);
            }

            LogStream s(out, "FW version: dune-weaver-firmware ");
            s << git_info;
            // TODO: change grbl-embedded to FluidNC after fixing WebUI
            s << " # FW target:grbl-embedded  # FW HW:";

            // std::error_code ec;
            // FluidPath { "/sd", ec };
            // s << (ec ? "No SD" : "Direct SD");

            // We do not check the SD presence here because if the SD card is out,
            // WebUI will switch to M20 for SD access, which is wrong for FluidNC
            s << "Direct SD";

            s << "  # primary sd:";

            (config->_sdCard->config_ok) ? s << "/sd" : s << "none";

            s << " # secondary sd:none ";

            s << " # authentication:";
#ifdef ENABLE_AUTHENTICATION
            s << "yes";
#else
            s << "no";
#endif
            s << " # webcommunication: Sync: ";
            s << std::to_string(Web_Server::port() + 1);
#if 0
            // If we omit the explicit IP address for the websocket,
            // WebUI will use the same IP address that it uses for
            // HTTP, with the port number as above.  That is better
            // than providing an explicit address, because if the WiFi
            // drops and comes back up again, DHCP might assign a
            // different IP address so the one provided below would no
            // longer work.  But if we are using an MDNS address like
            // fluidnc.local, a websocket reconnection will succeed
            // because MDNS will offer the new IP address.
            s << ":";
            switch (WiFi.getMode()) {
                case WIFI_AP:
                    s << IP_string(WiFi.softAPIP());
                    break;
                case WIFI_STA:
                    s << IP_string(WiFi.localIP());
                    break;
                case WIFI_AP_STA:
                    s << IP_string(WiFi.softAPIP());
                    break;
                default:
                    s << "0.0.0.0";
                    break;
            }
#endif
            s << " # hostname:";
            s << WiFi.getHostname();
            if (WiFi.getMode() == WIFI_AP) {
                s << "(AP mode)";
            }

            //to save time in decoding `?`
            s << " # axis:" << Axes::_numberAxis;
            return Error::Ok;
        }

        /**
     * WiFi events
     * SYSTEM_EVENT_WIFI_READY               < ESP32 WiFi ready
     * SYSTEM_EVENT_SCAN_DONE                < ESP32 finish scanning AP
     * SYSTEM_EVENT_STA_START                < ESP32 station start
     * SYSTEM_EVENT_STA_STOP                 < ESP32 station stop
     * SYSTEM_EVENT_STA_CONNECTED            < ESP32 station connected to AP
     * SYSTEM_EVENT_STA_DISCONNECTED         < ESP32 station disconnected from AP
     * SYSTEM_EVENT_STA_AUTHMODE_CHANGE      < the auth mode of AP connected by ESP32 station changed
     * SYSTEM_EVENT_STA_GOT_IP               < ESP32 station got IP from connected AP
     * SYSTEM_EVENT_STA_LOST_IP              < ESP32 station lost IP and the IP is reset to 0
     * SYSTEM_EVENT_STA_WPS_ER_SUCCESS       < ESP32 station wps succeeds in enrollee mode
     * SYSTEM_EVENT_STA_WPS_ER_FAILED        < ESP32 station wps fails in enrollee mode
     * SYSTEM_EVENT_STA_WPS_ER_TIMEOUT       < ESP32 station wps timeout in enrollee mode
     * SYSTEM_EVENT_STA_WPS_ER_PIN           < ESP32 station wps pin code in enrollee mode
     * SYSTEM_EVENT_AP_START                 < ESP32 soft-AP start
     * SYSTEM_EVENT_AP_STOP                  < ESP32 soft-AP stop
     * SYSTEM_EVENT_AP_STACONNECTED          < a station connected to ESP32 soft-AP
     * SYSTEM_EVENT_AP_STADISCONNECTED       < a station disconnected from ESP32 soft-AP
     * SYSTEM_EVENT_AP_PROBEREQRECVED        < Receive probe request packet in soft-AP interface
     * SYSTEM_EVENT_GOT_IP6                  < ESP32 station or ap or ethernet interface v6IP addr is preferred
     * SYSTEM_EVENT_ETH_START                < ESP32 ethernet start
     * SYSTEM_EVENT_ETH_STOP                 < ESP32 ethernet stop
     * SYSTEM_EVENT_ETH_CONNECTED            < ESP32 ethernet phy link up
     * SYSTEM_EVENT_ETH_DISCONNECTED         < ESP32 ethernet phy link down
     * SYSTEM_EVENT_ETH_GOT_IP               < ESP32 ethernet got IP from connected AP
     * SYSTEM_EVENT_MAX
     */

        // Registered as a WiFiEventSysCb (pointer to the event) rather than the
        // (event, info) form: that one is a std::function taking the ~140-byte
        // arduino_event_info_t union BY VALUE, copied onto the arduino_events
        // task's 4 KB stack on every WiFi event.  The pointer form copies nothing.
        static void WiFiEvent(arduino_event_t* sys_event) {
            static bool disconnect_seen = false;
            switch (sys_event->event_id) {
                case SYSTEM_EVENT_STA_GOT_IP:
                    break;
                case SYSTEM_EVENT_STA_DISCONNECTED:
                    if (!disconnect_seen) {
                        // The reason code is the whole diagnosis for a station that
                        // never comes back: the arduino core refuses to auto-retry
                        // ASSOC_LEAVE (8, sent by many APs as they reboot) and
                        // AUTH_FAIL (202) after the first join, so those are the
                        // codes that leave the link down until we retry it
                        // ourselves in pollStaLink().
                        uint8_t reason = sys_event->event_info.wifi_sta_disconnected.reason;
                        log_info_to(Uart0,
                                    "WiFi Disconnected, reason " << (int)reason << " ("
                                                                 << WiFi.disconnectReasonName((wifi_err_reason_t)reason) << ")");
                        disconnect_seen = true;
                    }
                    break;
                case SYSTEM_EVENT_STA_START:
                    break;
                case SYSTEM_EVENT_STA_STOP:
                    break;
                case SYSTEM_EVENT_STA_CONNECTED:
                    disconnect_seen = false;
                    log_info_to(Uart0, "WiFi STA Connected");
                    break;
                default:
                    log_debug_to(Uart0, "WiFi event: " << (int)sys_event->event_id);
                    break;
            }
        }

        static int32_t getSignal(int32_t RSSI) {
            if (RSSI <= -100) {
                return 0;
            }
            if (RSSI >= -50) {
                return 100;
            }
            return 2 * (RSSI + 100);
        }

        static bool ConnectSTA2AP() {
            std::string msg, msg_out;
            uint8_t     dot = 0;
            for (size_t i = 0; i < 10; ++i) {
                switch (WiFi.status()) {
                    case WL_NO_SSID_AVAIL:
                        log_info("No SSID");
                        _sta_fail_reason = "network not found - check the name, and note only 2.4 GHz networks work";
                        return false;
                    case WL_CONNECT_FAILED:
                        log_info("Connection failed");
                        _sta_fail_reason = "the network refused the connection - usually a wrong password";
                        return false;
                    case WL_CONNECTED:
                        log_info("Connected - IP is " << IP_string(WiFi.localIP()));
                        _sta_fail_reason.clear();
                        return true;
                    default:
                        if ((dot > 3) || (dot == 0)) {
                            dot     = 0;
                            msg_out = "Connecting";
                        }
                        msg_out += ".";
                        msg = msg_out;
                        dot++;
                        break;
                }
                log_info(msg);
                delay_ms(2000);  // Give it some time to connect
            }
            // A wrong password on many routers shows up as an endless
            // disconnect/retry loop rather than WL_CONNECT_FAILED, so the
            // timeout hint mentions it too.
            _sta_fail_reason = "timed out - check the password and that the table is in Wi-Fi range";
            return false;
        }

        static bool StartSTA() {
            //Sanity check
            auto mode = WiFi.getMode();
            if (mode == WIFI_STA || mode == WIFI_AP_STA) {
                WiFi.disconnect();
            }

            if (mode == WIFI_AP || mode == WIFI_AP_STA) {
                WiFi.softAPdisconnect();
            }

            WiFi.enableAP(false);

            //SSID
            _sta_fail_reason.clear();
            const char* SSID = _sta_ssid->get();
            if (strlen(SSID) == 0) {
                // Unconfigured, not an error: the portal shows no failure
                // banner on a fresh table.
                log_info("STA SSID is not set");
                return false;
            }
            //Hostname needs to be set before mode to take effect.
            //A hostname in config.yaml overrides the $Hostname NVS setting.
            WiFi.setHostname(config->_hostname.empty() ? _hostname->get() : config->_hostname.c_str());
            WiFi.mode(WIFI_STA);
            WiFi.setMinSecurity(static_cast<wifi_auth_mode_t>(_sta_min_security->get()));
            WiFi.setScanMethod(_fast_scan->get() ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN);
            WiFi.setAutoReconnect(true);
            //Get parameters for STA
            //password
            const char* password = _sta_password->get();
            int8_t      IP_mode  = _sta_mode->get();
            int32_t     IP       = _sta_ip->get();
            int32_t     GW       = _sta_gateway->get();
            int32_t     MK       = _sta_netmask->get();
            //if not DHCP
            if (IP_mode != DHCP_MODE) {
                IPAddress ip(IP), mask(MK), gateway(GW);
                WiFi.config(ip, gateway, mask);
            }
            if (WiFi.begin(SSID, (strlen(password) > 0) ? password : NULL)) {
                log_info("Connecting to STA SSID:" << SSID);
                return ConnectSTA2AP();
            } else {
                log_info("Starting client failed");
                _sta_fail_reason = "Wi-Fi radio failed to start";
                return false;
            }
        }

        static bool StartAP() {
            //Sanity check
            if ((WiFi.getMode() == WIFI_STA) || (WiFi.getMode() == WIFI_AP_STA)) {
                WiFi.disconnect();
            }
            if ((WiFi.getMode() == WIFI_AP) || (WiFi.getMode() == WIFI_AP_STA)) {
                WiFi.softAPdisconnect();
            }

            WiFi.enableSTA(false);
            // Set the hostname before the mode, same as StartSTA -- WiFi
            // getHostname()/setHostname() share one mode-independent buffer, so
            // in pure AP mode nothing else sets it and it would default to
            // esp32-XXXXXX.  This name is what /sand_status reports and what
            // mDNS advertises (<hostname>.local), so the table stays identity-
            // stable across home-network and hotspot connections.
            WiFi.setHostname(config->_hostname.empty() ? _hostname->get() : config->_hostname.c_str());
            WiFi.mode(WIFI_AP);

            const char* country = _ap_country->getStringValue();
            if (ESP_OK != esp_wifi_set_country_code(country, true)) {
                log_error("failed to set Wifi regulatory domain to " << country);
            }

            //Get parameters for AP
            const char* SSID = _ap_ssid->get();

            const char* password = _ap_password->get();

            int8_t channel = int8_t(_ap_channel->get());

            IPAddress ip(_ap_ip->get());
            IPAddress mask;
            mask.fromString("255.255.255.0");

            log_info("AP SSID " << SSID << " IP " << IP_string(ip) << " mask " << IP_string(mask) << " channel " << channel);

            //Set static IP
            WiFi.softAPConfig(ip, ip, mask);

            //Start AP
            if (WiFi.softAP(SSID, (strlen(password) > 0) ? password : NULL, channel)) {
                log_info("AP started");
                return true;
            }

            log_info("AP did not start");
            return false;
        }

        static void reset() {
            WiFi.persistent(false);
            WiFi.disconnect(true);
            WiFi.enableSTA(false);
            WiFi.enableAP(false);
            WiFi.mode(WIFI_OFF);
        }

        static void StopWiFi() {
            if (WiFi.getMode() != WIFI_OFF) {
                if ((WiFi.getMode() == WIFI_STA) || (WiFi.getMode() == WIFI_AP_STA)) {
                    WiFi.disconnect(true);
                }
                if ((WiFi.getMode() == WIFI_AP) || (WiFi.getMode() == WIFI_AP_STA)) {
                    WiFi.softAPdisconnect(true);
                }
                // wifi_services.end();
                WiFi.enableSTA(false);
                WiFi.enableAP(false);
                WiFi.mode(WIFI_OFF);
            }
            log_info("WiFi Off");
        }

        static char* mac2str(uint8_t mac[8]) {
            static char macstr[18];
            if (0 > sprintf(macstr, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5])) {
                strcpy(macstr, "00:00:00:00:00:00");
            }
            return macstr;
        }

        static std::string station_info() {
            std::string result;

            auto mode = WiFi.getMode();
            if (mode == WIFI_STA || mode == WIFI_AP_STA) {
                result += "Mode=STA:SSID=";
                result += WiFi.SSID().c_str();
                result += ":Status=";
                result += (WiFi.status() == WL_CONNECTED) ? "Connected" : "Not connected";
                result += ":IP=";
                result += IP_string(WiFi.localIP());
                result += ":MAC=";
                std::string mac(WiFi.macAddress().c_str());
                std::replace(mac.begin(), mac.end(), ':', '-');
                result += mac;
            }
            return result;
        }

        static std::string ap_info() {
            std::string result;

            auto mode = WiFi.getMode();
            if (mode == WIFI_AP || mode == WIFI_AP_STA) {
                if (WiFi.getMode() == WIFI_AP_STA) {
                    result += "]\n[MSG:";
                }
                result += "Mode=AP:SSID=";
                wifi_config_t conf;
                esp_wifi_get_config(WIFI_IF_AP, &conf);
                result += (const char*)conf.ap.ssid;
                result += ":IP=";
                result += IP_string(WiFi.softAPIP());
                result += ":MAC=";
                std::string mac(WiFi.softAPmacAddress().c_str());
                std::replace(mac.begin(), mac.end(), ':', '-');
                result += mac;
            }
            return result;
        }

        static bool isOn() {
            return !(WiFi.getMode() == WIFI_OFF);
        }

        // Used by js/scanwifidlg.js

        static Error listAPs(const char* parameter, AuthenticationLevel auth_level, Channel& out) {  // ESP410
            JSONencoder j(false, &out);
            j.begin();

            if (parameter != NULL && (strstr(parameter, "json=yes")) != NULL) {
                j.member("cmd", "410");
                j.member("status", "ok");
                j.begin_array("data");
            } else {
                j.begin_array("AP_LIST");
            }

            // An initial async scanNetworks was issued at startup, so there
            // is a good chance that scan information is already available.
            int n;
            while (true) {
                n = WiFi.scanComplete();
                if (n >= 0) {  // Scan completed with n results
                    break;
                }
                if (n == WIFI_SCAN_FAILED) {  // Begin async scan
                    //                async hidden passive ms_per_chan
                    WiFi.scanNetworks(true, false, false, 1000);
                }
                // Else WIFI_SCAN_RUNNING
                delay(1000);
            }

            for (int i = 0; i < n; ++i) {
                j.begin_object();
                j.member("SSID", WiFi.SSID(i).c_str());
                j.member("SIGNAL", getSignal(WiFi.RSSI(i)));
                j.member("IS_PROTECTED", WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
                //            j->member("IS_PROTECTED", WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "0" : "1");
                j.end_object();
            }
            WiFi.scanDelete();
            // Restart the scan in async mode so new data will be available
            // when we ask again.
            WiFi.scanNetworks(true);
            j.end_array();
            j.end();
            return Error::Ok;
        }

    public:
        WiFiConfig(const char* name) : Module(name) {}

        void init() {
            _sta_ssid    = new StringSetting("Station SSID", WEBSET, WA, "ESP100", "Sta/SSID", "", MIN_SSID_LENGTH, MAX_SSID_LENGTH);
            _hostname    = new HostnameSetting("Hostname", "ESP112", "Hostname", "duneweaver");
            _ap_channel  = new IntSetting("AP Channel", WEBSET, WA, "ESP108", "AP/Channel", 1, 1, 14);
            _ap_ip       = new IPaddrSetting("AP Static IP", WEBSET, WA, "ESP107", "AP/IP", "192.168.0.1");
            _ap_password = new PasswordSetting("AP Password", "ESP106", "AP/Password", "12345678");
            _ap_ssid     = new StringSetting("AP SSID", WEBSET, WA, "ESP105", "AP/SSID", "DuneWeaver", MIN_SSID_LENGTH, MAX_SSID_LENGTH);
            _ap_country  = new EnumSetting("AP regulatory domain", WEBSET, WA, NULL, "AP/Country", WiFiCountry01, &wifiCountryOptions);
            // Minutes the home network may be unreachable before the setup
            // hotspot joins the still-retrying station.  0 = never raise it.
            _ap_fallback_min = new IntSetting("AP fallback delay (min)", WEBSET, WA, NULL, "WiFi/ApFallbackMin", 10, 0, 1440);
            _sta_netmask = new IPaddrSetting("Station Static Mask", WEBSET, WA, NULL, "Sta/Netmask", NULL_IP);
            _sta_gateway = new IPaddrSetting("Station Static Gateway", WEBSET, WA, NULL, "Sta/Gateway", NULL_IP);
            _sta_ip      = new IPaddrSetting("Station Static IP", WEBSET, WA, NULL, "Sta/IP", NULL_IP);
            _sta_mode    = new EnumSetting("Station IP Mode", WEBSET, WA, "ESP102", "Sta/IPMode", DHCP_MODE, &staModeOptions);
            _fast_scan   = new EnumSetting("WiFi Fast Scan", WEBSET, WA, NULL, "WiFi/FastScan", 0, &onoffOptions);
            _sta_min_security =
                new EnumSetting("Station IP Mode", WEBSET, WA, NULL, "Sta/MinSecurity", WIFI_AUTH_WPA2_PSK, &staSecurityOptions);
            _sta_password = new PasswordSetting("Station Password", "ESP101", "Sta/Password", "");

            _mode         = new EnumSetting("WiFi mode", WEBSET, WA, "ESP116", "WiFi/Mode", WiFiFallback, &wifiModeOptions);
            _wifi_ps_mode = new EnumSetting("WiFi power saving mode", WEBSET, WA, NULL, "WiFi/PsMode", WIFI_PS_NONE, &wifiPsModeOptions);

            new WebCommand(NULL, WEBCMD, WU, "ESP410", "WiFi/ListAPs", listAPs);
            new WebCommand(NULL, WEBCMD, WG, "ESP800", "Firmware/Info", showFwInfo, anyState);

            new WebCommand(NULL, WEBCMD, WG, "ESP111", "System/IP", showIP);
            new WebCommand("IP=ipaddress MSK=netmask GW=gateway", WEBCMD, WA, "ESP103", "Sta/Setup", showSetStaParams);

            //stop active services
            // wifi_services.end();

            switch (_mode->get()) {
                case WiFiOff:
                    log_info("WiFi is disabled");
                    return;
                case WiFiSTA:
                    if (StartSTA()) {
                        goto wifi_on;
                    }
                    goto wifi_off;
                case WiFiFallback:
                    if (StartSTA()) {
                        goto wifi_on;
                    } else {  // STA failed, reset
                        _ap_is_fallback = true;  // captive portal serves the setup page
                        WiFi.mode(WIFI_OFF);
                        esp_wifi_restore();
                        delay_ms(100);
                    }
                    // fall through to fallback to AP mode
                case WiFiAP:
                    if (StartAP()) {
                        goto wifi_on;
                    }
                    goto wifi_off;
            }

        wifi_off:
            log_info("WiFi off");
            WiFi.mode(WIFI_OFF);
            return;

        wifi_on:
            //setup events
            if (!_events_registered) {
                //cumulative function and no remove so only do once
                WiFi.onEvent(WiFiEvent);
                _events_registered = true;
            }
            esp_wifi_set_ps(WIFI_PS_NONE);
            esp_wifi_set_ps(static_cast<wifi_ps_type_t>(_wifi_ps_mode->get()));
            log_info("WiFi on");
            //        wifi_services.begin();
        }

        void deinit() override {
            StopWiFi();
        }

        void build_info(Channel& channel) {
            std::string sti = station_info();
            if (sti.length()) {
                log_msg_to(channel, sti);
            }
            std::string api = ap_info();
            if (api.length()) {
                log_msg_to(channel, api);
            }
            if (!sti.length() && !api.length()) {
                log_msg_to(channel, "No Wifi");
            }
        }

        // How long the link may be down before we take over from the core's own
        // auto-reconnect, and how long between our attempts.  Attempts are cheap
        // but they are still radio work, so they slow down once it is clear the
        // AP is gone for a while rather than blipping.
        static const uint32_t STA_GRACE_MS   = 20000;
        static const uint32_t STA_RETRY_MS   = 30000;
        static const uint32_t STA_SLOW_MS    = 120000;
        static const uint32_t STA_SLOW_AFTER = 5;  // retries before backing off to STA_SLOW_MS

        // While the setup hotspot is up the station is PARKED, because a
        // scanning station drags the shared-radio AP off its channel and makes
        // it unjoinable.  It wakes only for a bounded probe of the home network,
        // so the AP is stable for all but ~25s out of every 5 minutes.
        static const uint32_t AP_PROBE_SETTLE_MS = 30000;
        static const uint32_t AP_PROBE_WINDOW_MS = 25000;
        static const uint32_t AP_PROBE_EVERY_MS  = 300000;

        // True when a reconnect attempt cannot disturb work in progress.
        //
        // Idle alone is not enough: a playlist looping indefinitely with a short
        // (or zero) PauseTime may never be observed at Idle, which is exactly
        // the table that would sit off-network for days.  So a finished job --
        // the moment a pattern or clear ends and before the next is injected --
        // opens a window of its own.  The flag is sticky so a window earned
        // mid-homing is spent afterwards rather than lost.
        static bool staRetryWindowOpen() {
            const uint32_t now = millis();
            // Sample the job stack at 4 Hz, not every poll: Job::active() takes
            // the shared job mutex, and this loop also feeds segment prep.
            if ((uint32_t)(now - _sta_gate_checked) >= 250) {
                _sta_gate_checked = now;
                bool active       = Job::active();
                if (_sta_job_active && !active) {
                    _sta_pattern_done = true;
                }
                _sta_job_active = active;
            }
            // Homing is our most timing-fragile motion; never touch the radio
            // during it.  A pending window survives to the other side.
            if (state_is(State::Homing)) {
                return false;
            }
            if (_sta_pattern_done) {
                _sta_pattern_done = false;
                return true;
            }
            return state_is(State::Idle);
        }

        // How long the station must stay down before the setup hotspot joins it.
        // 0 disables the hotspot entirely (retry-only, the v0.1.16 behavior).
        static uint32_t apFallbackMs() {
            int m = _ap_fallback_min ? _ap_fallback_min->get() : 0;
            return m > 0 ? (uint32_t)m * 60000u : 0;
        }

        // Bring up the setup hotspot WITHOUT tearing down the station.
        // AP_STA, not AP: the whole point is that reconnect attempts continue
        // underneath, so a table whose router was merely rebooting rejoins on
        // its own and the hotspot goes away again -- the owner never has to
        // touch it.  StartAP() is not reusable here; it kills the station.
        static void raiseRecoveryAp(uint32_t now) {
            // PARK THE STATION FIRST.  The softAP shares one radio with the
            // station and has to follow its channel, so a station left hunting
            // for a missing SSID keeps yanking the channel and the AP never
            // beacons long enough to be joinable -- measured: the AP reported
            // itself up at 192.168.0.1 while a laptop could not see the SSID at
            // all.  Stop the core's own reconnect loop, park the station, and
            // probe the home network on a slow cadence instead (below).
            WiFi.setAutoReconnect(false);
            esp_wifi_disconnect();
            _ap_probing = false;

            WiFi.mode(WIFI_AP_STA);

            const char* country = _ap_country->getStringValue();
            if (ESP_OK != esp_wifi_set_country_code(country, true)) {
                log_error("failed to set Wifi regulatory domain to " << country);
            }

            IPAddress ip(_ap_ip->get());
            IPAddress mask;
            mask.fromString("255.255.255.0");
            WiFi.softAPConfig(ip, ip, mask);

            const char* ssid     = _ap_ssid->get();
            const char* password = _ap_password->get();
            if (WiFi.softAP(ssid, (strlen(password) > 0) ? password : NULL, int8_t(_ap_channel->get()))) {
                _ap_raised = true;
                // Fallback semantics: the captive DNS resolves every name to the
                // table so the phone pops the setup sheet, which is exactly the
                // "I need to re-point this table at a network" case.
                _ap_is_fallback  = true;
                _sta_fail_reason = "home network unreachable for " + std::to_string((now - _sta_down_since) / 60000) + " min";
                // Let the AP settle before the first probe steals the channel.
                _sta_next_try = now + AP_PROBE_SETTLE_MS;
                log_info("WiFi down " << (now - _sta_down_since) / 1000 << "s; setup hotspot " << ssid << " raised at "
                                      << IP_string(ip) << " (home network probed every "
                                      << AP_PROBE_EVERY_MS / 60000 << " min)");
            } else {
                // Leave _ap_raised false so a later attempt can try again.
                WiFi.setAutoReconnect(true);
                WiFi.mode(WIFI_STA);
                esp_wifi_connect();
                log_error("could not raise the setup hotspot; staying station-only");
            }
        }

        // Home network is back (or the mode changed): return to a pure station.
        static void dropRecoveryAp() {
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            WiFi.setAutoReconnect(true);  // hand routine retries back to the core
            CaptiveDns::stop();
            _ap_raised      = false;
            _ap_probing     = false;
            _ap_is_fallback = false;
            _sta_fail_reason.clear();
            log_info("Setup hotspot lowered; back to station only");
        }

        // Retry a dropped station link ourselves.
        //
        // The arduino core gives up permanently on WIFI_REASON_ASSOC_LEAVE (8) --
        // "Voluntarily disconnected. Don't reconnect!" in WiFiGeneric.cpp -- and on
        // AUTH_FAIL (202), neither of which is in _isReconnectableReason().  An AP
        // rebooting on a schedule commonly deauths with 8, so without this the
        // table stays dark until someone power-cycles it.
        static void pollStaLink() {
            // Supervise a real station link.  WIFI_AP_STA counts only when it is
            // OUR recovery AP (the station is still there, retrying underneath
            // it); otherwise AP_STA is the transient scan mode handled above and
            // AP-only has no station to recover.
            auto mode = WiFi.getMode();
            if (mode != WIFI_STA && !(mode == WIFI_AP_STA && _ap_raised)) {
                return;
            }
            if (!_sta_ssid || strlen(_sta_ssid->get()) == 0) {
                return;
            }

            const uint32_t now = millis();

            if (WiFi.status() == WL_CONNECTED) {
                if (_sta_down_since) {
                    log_info("WiFi reconnected after " << (now - _sta_down_since) / 1000 << "s, IP " << IP_string(WiFi.localIP()));
                }
                _sta_down_since   = 0;
                _sta_tries        = 0;
                _sta_pattern_done = false;
                if (_ap_raised) {
                    // Home network is back: the table belongs on the LAN, so
                    // drop the recovery hotspot (any phone on it gets kicked --
                    // correct, the thing it was there to fix is fixed).
                    dropRecoveryAp();
                }
                return;
            }

            // Associated to the AP but no IP yet (DHCP in flight, or a DHCP
            // server that never answers).  esp_wifi_connect() would only knock a
            // live association back down, and it cannot fix DHCP, so leave it be.
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                return;
            }

            // Hotspot is up: the station is parked and only wakes for a bounded
            // probe, so the AP keeps a stable channel and stays joinable.
            if (_ap_raised) {
                if (_ap_probing) {
                    if ((int32_t)(now - _ap_probe_until) >= 0) {
                        _ap_probing = false;
                        esp_wifi_disconnect();  // re-park: stop scanning, give the AP its channel back
                        _sta_next_try = now + AP_PROBE_EVERY_MS;
                    }
                    return;
                }
                if ((int32_t)(now - _sta_next_try) < 0) {
                    return;
                }
                if (!staRetryWindowOpen()) {
                    return;
                }
                ++_sta_tries;
                _ap_probing     = true;
                _ap_probe_until = now + AP_PROBE_WINDOW_MS;
                esp_wifi_connect();
                log_info("WiFi down " << (now - _sta_down_since) / 1000 << "s; probing home network (attempt " << _sta_tries
                                      << ") while the hotspot stays up");
                return;
            }

            if (!_sta_down_since) {
                // First poll that sees the link down: give the core's own retry
                // (and a brief AP reboot) a grace period before we step in.
                _sta_down_since = now ? now : 1;
                _sta_next_try   = now + STA_GRACE_MS;
                return;
            }
            if ((int32_t)(now - _sta_next_try) < 0) {
                return;
            }
            if (!staRetryWindowOpen()) {
                return;
            }

            // A home network that has stayed down this long may not be coming
            // back as it was (moved house, renamed SSID, changed password), and
            // a headless table with no radio the owner can reach is a brick.
            // Raise the setup hotspot alongside the station -- the retries below
            // keep running underneath it, so a router that WAS just rebooting
            // still gets picked up automatically and the AP disappears again.
            if (!_ap_raised && _mode->get() == WiFiFallback && apFallbackMs() &&
                (uint32_t)(now - _sta_down_since) >= apFallbackMs()) {
                raiseRecoveryAp(now);
            }

            ++_sta_tries;
            _sta_next_try = now + (_sta_tries >= STA_SLOW_AFTER ? STA_SLOW_MS : STA_RETRY_MS);

            // esp_wifi_connect(), not WiFi.begin(): we never call
            // WiFi.persistent(false), so the core leaves storage at
            // WIFI_STORAGE_FLASH and begin()'s esp_wifi_set_config() can write
            // NVS on every attempt.  connect() reuses the config already in the
            // driver, writes nothing, and returns immediately -- no scanNetworks()
            // (an async scan racing the status poller is what wedges the web
            // server) and nothing that blocks this task.
            esp_err_t err = esp_wifi_connect();
            if (err == ESP_OK || err == ESP_ERR_WIFI_CONN) {
                log_info("WiFi down " << (now - _sta_down_since) / 1000 << "s; reconnect attempt " << _sta_tries);
            } else {
                log_warn("WiFi reconnect attempt " << _sta_tries << " failed to start: " << esp_err_to_name(err));
            }
        }

        void poll() {
            //to avoid mixed mode due to scan network
            // Skipped while we hold a recovery AP: there AP_STA is deliberate and
            // the station is the thing being recovered, so a completed scan must
            // not disable it.
            if (WiFi.getMode() == WIFI_AP_STA && !_ap_raised) {
                // In principle it should be sufficient to check for != WIFI_SCAN_RUNNING,
                // but that does not work well.  Doing so makes scans in AP mode unreliable.
                // Sometimes the first try works, but subsequent scans fail.
                if (WiFi.scanComplete() >= 0) {
                    WiFi.enableSTA(false);
                }
            }
            pollStaLink();
        }

        bool is_radio() override {
            return true;
        }

        ~WiFiConfig() {
            deinit();
        }
    };

    ModuleFactory::InstanceBuilder<WiFiConfig> __attribute__((init_priority(105))) wifi_module("wifi", true);
}
