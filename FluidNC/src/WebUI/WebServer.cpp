// Copyright (c) 2014 Luc Lebosse. All rights reserved.
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "src/Machine/MachineConfig.h"
#include "src/Serial.h"    // is_realtime_command()
#include "src/Settings.h"  // settings_execute_line()

#include "WebServer.h"

#include "Mdns.h"

#include <WebSocketsServer.h>
#include <WiFi.h>
#include <WebServer.h>
#include <StreamString.h>
#include <Update.h>
#include <esp_wifi_types.h>
// #include <ESP32SSDP.h>
#include <DNSServer.h>

#include "WSChannel.h"

#include "WebClient.h"

#include "src/Protocol.h"  // protocol_send_event
#include "src/SandApi.h"   // SandApi::statusJson() for /sand_status
#include "src/Leds.h"      // Leds::instance() for /sand_led
#include "src/Playlist.h"  // Playlist::stopActive() for /sand_stop
#include "src/Kinematics/ThetaRho.h"  // ThetaRho::setFeedLive() for /sand_feed?mm
#include "src/TimeKeeper.h"            // Clock:: for /sand_time
#include "src/FluidPath.h"
#include "src/JSONEncoder.h"
#include "src/StartupLog.h"  // StartupLog::dump() for /sand_bootlog
#include "src/RingLog.h"     // RingLog::dump() for /sand_log
#include "src/Job.h"         // Job::active() to gate OTA while a pattern runs
#include "src/Report.h"      // git_info for the /updatefw response

#include "src/HashFS.h"
#include <list>

namespace WebUI {
    const byte DNS_PORT = 53;
    DNSServer  dnsServer;
}

#include <esp_ota_ops.h>

//embedded response file if no files on LocalFS
#include "NoFile.h"

namespace WebUI {
    // Error codes for upload
    const int ESP_ERROR_AUTHENTICATION   = 1;
    const int ESP_ERROR_FILE_CREATION    = 2;
    const int ESP_ERROR_FILE_WRITE       = 3;
    const int ESP_ERROR_UPLOAD           = 4;
    const int ESP_ERROR_NOT_ENOUGH_SPACE = 5;
    const int ESP_ERROR_UPLOAD_CANCELLED = 6;
    const int ESP_ERROR_FILE_CLOSE       = 7;
    const int ESP_ERROR_FILE_OP          = 8;

    // Last upload failure detail, reported in the /files HTTP response by
    // handleFileOps.  pushError only reaches the WebSocket, which is disabled
    // on this headless build, so without this the app just sees "Upload failed".
    static int         _upload_error_code = 0;
    static std::string _upload_error_msg;

    static void recordUploadError(int code, const std::string& msg) {
        _upload_error_code = code;
        _upload_error_msg  = msg;
    }

    static const char LOCATION_HEADER[] = "Location";

    bool     Web_Server::_setupdone = false;
    uint16_t Web_Server::_port      = 0;

    UploadStatus      Web_Server::_upload_status   = UploadStatus::NONE;
    WebServer*        Web_Server::_webserver       = NULL;
    WebSocketsServer* Web_Server::_socket_server   = NULL;
    WebSocketsServer* Web_Server::_socket_serverv3 = NULL;
#ifdef ENABLE_AUTHENTICATION
    AuthenticationIP* Web_Server::_head  = NULL;
    uint8_t           Web_Server::_nb_ip = 0;
    const int         MAX_AUTH_IP        = 10;
#endif
    FileStream* Web_Server::_uploadFile = nullptr;

    EnumSetting *http_enable, *http_block_during_motion;
    IntSetting*  http_port;

    Web_Server::~Web_Server() {
        deinit();
    }

    void Web_Server::init() {
        http_port   = new IntSetting("HTTP Port", WEBSET, WA, "ESP121", "HTTP/Port", DEFAULT_HTTP_PORT, MIN_HTTP_PORT, MAX_HTTP_PORT);
        http_enable = new EnumSetting("HTTP Enable", WEBSET, WA, "ESP120", "HTTP/Enable", DEFAULT_HTTP_STATE, &onoffOptions);
        http_block_during_motion = new EnumSetting("Block serving HTTP content during motion",
                                                   WEBSET,
                                                   WA,
                                                   "",
                                                   "HTTP/BlockDuringMotion",
                                                   DEFAULT_HTTP_BLOCKED_DURING_MOTION,
                                                   &onoffOptions);

        _setupdone = false;

        if (WiFi.getMode() == WIFI_OFF || !http_enable->get()) {
            return;
        }

        _port = http_port->get();

        //create instance
        _webserver = new WebServer(_port);
#ifdef ENABLE_AUTHENTICATION
        //here the list of headers to be recorded
        const char* headerkeys[]   = { "Cookie" };
        size_t      headerkeyssize = sizeof(headerkeys) / sizeof(char*);
        //ask server to track these headers
        _webserver->collectHeaders(headerkeys, headerkeyssize);
#endif

        //here the list of headers to be recorded
        const char* headerkeys[]   = { "If-None-Match" };
        size_t      headerkeyssize = sizeof(headerkeys) / sizeof(char*);
        _webserver->collectHeaders(headerkeys, headerkeyssize);

        // Sand-table fork: the WebUI WebSockets are intentionally NOT started.
        // They stream status / read machine, Job and report state from the
        // poller task concurrently with motion, which races the running pattern
        // and panics the board (StoreProhibited).  This headless build is driven
        // entirely by the stateless HTTP API (/sand_* + /command), which any
        // number of clients can poll safely.  Every use of _socket_server /
        // _socket_serverv3 below is null-guarded, so leaving them NULL turns the
        // whole WebSocket path into a no-op.
        _socket_server   = NULL;
        _socket_serverv3 = NULL;

        //events functions
        //_web_events->onConnect(handle_onevent_connect);
        //events management
        // _webserver->addHandler(_web_events);

        //Web server handlers
        //trick to catch command line on "/" before file being processed
        _webserver->on("/", HTTP_ANY, handle_root);

        //Page not found handler
        _webserver->onNotFound(handle_not_found);

        //need to be there even no authentication to say to UI no authentication
        _webserver->on("/login", HTTP_ANY, handle_login);

        //web commands
        _webserver->on("/command", HTTP_ANY, handle_web_command);
        _webserver->on("/command_silent", HTTP_ANY, handle_web_command_silent);
        _webserver->on("/feedhold_reload", HTTP_ANY, handleFeedholdReload);
        _webserver->on("/cyclestart_reload", HTTP_ANY, handleCyclestartReload);
        _webserver->on("/restart_reload", HTTP_ANY, handleRestartReload);
        _webserver->on("/did_restart", HTTP_ANY, handleDidRestart);
        _webserver->on("/sand_stop", HTTP_ANY, handleSandStop);
        _webserver->on("/sand_home", HTTP_ANY, handleSandHome);
        _webserver->on("/sand_goto", HTTP_ANY, handleSandGoto);
        _webserver->on("/sand_pause", HTTP_ANY, handleSandPause);
        _webserver->on("/sand_resume", HTTP_ANY, handleSandResume);
        _webserver->on("/sand_status", HTTP_ANY, handleSandStatus);
        _webserver->on("/sand_patterns", HTTP_ANY, handleSandPatterns);
        _webserver->on("/sand_bootlog", HTTP_ANY, handleSandBootlog);
        _webserver->on("/sand_log", HTTP_ANY, handleSandLog);
        _webserver->on("/sand_playlists", HTTP_ANY, handleSandPlaylists);
        _webserver->on("/sand_settings", HTTP_ANY, handleSandSettings);
        _webserver->on("/sand_time", HTTP_ANY, handleSandTime);
        _webserver->on("/sand_feed", HTTP_ANY, handleSandFeed);
        _webserver->on("/sand_led", HTTP_ANY, handleSandLed);

        //LocalFS
        _webserver->on("/files", HTTP_ANY, handleFileList, LocalFSFileupload);

        //web update
        _webserver->on("/updatefw", HTTP_ANY, handleUpdate, WebUpdateUpload);

        //Direct SD management
        _webserver->on("/upload", HTTP_ANY, handle_direct_SDFileList, SDFileUpload);
        //_webserver->on("/SD", HTTP_ANY, handle_SDCARD);

        if (WiFi.getMode() == WIFI_AP) {
            // if DNSServer is started with "*" for domain name, it will reply with
            // provided IP to all DNS request
            dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
            log_info("Captive Portal Started");
            _webserver->on("/generate_204", HTTP_ANY, handle_root);
            _webserver->on("/gconnectivitycheck.gstatic.com", HTTP_ANY, handle_root);
            //do not forget the / at the end
            _webserver->on("/fwlink/", HTTP_ANY, handle_root);
        }

#if 0
        //SSDP service presentation
        if (WiFi.getMode() == WIFI_STA && WebUI::mdns_enable->get()) {
            _webserver->on("/description.xml", HTTP_GET, handle_SSDP);
            //Add specific for SSDP
            SSDP.setSchemaURL("description.xml");
            SSDP.setHTTPPort(_port);
            SSDP.setName(WiFi.getHostname());
            SSDP.setURL("/");
            SSDP.setDeviceType("upnp:rootdevice");
            /*Any customization could be here
        SSDP.setModelName (ESP32_MODEL_NAME);
        SSDP.setModelURL (ESP32_MODEL_URL);
        SSDP.setModelNumber (ESP_MODEL_NUMBER);
        SSDP.setManufacturer (ESP_MANUFACTURER_NAME);
        SSDP.setManufacturerURL (ESP_MANUFACTURER_URL);
        */

            //Start SSDP
            log_info("SSDP Started");
            SSDP.begin();
        }
#endif

        log_info("HTTP started on port " << WebUI::http_port->get());
        //start webserver
        _webserver->begin();

        Mdns::add("_http", "_tcp", _port);
        // Tag this service so a native app can identify a sand table among the
        // _http._tcp services. The table is headless and driven over stateless
        // HTTP (WebSockets are disabled), so no socket port is advertised.
        Mdns::addTxt("_http", "_tcp", "model", "dune-weaver");
        Mdns::addTxt("_http", "_tcp", "api", "sandtable/1");

        HashFS::hash_all();

        _setupdone = true;
    }

    void Web_Server::deinit() {
        _setupdone = false;

        //        SSDP.end();

        Mdns::remove("_http", "_tcp");

        if (_socket_server) {
            delete _socket_server;
            _socket_server = NULL;
        }

        if (_socket_serverv3) {
            delete _socket_serverv3;
            _socket_serverv3 = NULL;
        }

        if (_webserver) {
            delete _webserver;
            _webserver = NULL;
        }

#ifdef ENABLE_AUTHENTICATION
        while (_head) {
            AuthenticationIP* current = _head;
            _head                     = _head->_next;
            delete current;
        }
        _nb_ip = 0;
#endif
    }

    // Send a file, either the specified path or path.gz
    bool Web_Server::myStreamFile(const char* path, bool download) {
        std::error_code ec;
        FluidPath       fpath { path, localfsName, ec };
        if (ec) {
            return false;
        }

        std::string hash;

        // If you load or reload WebUI while a program is running, there is a high
        // risk of stalling the motion because serving a file from
        // the local FLASH filesystem takes away a lot of CPU cycles.  If we get
        // a request for a file when running, reject it to preserve the motion
        // integrity.
        // This can make it hard to debug ISR IRAM problems, because the easiest
        // way to trigger such problems is to refresh WebUI during motion.
        if (http_block_during_motion->get() && inMotionState()) {
            // Check to see if we have a cached hash of the file that can be retrieved without accessing FLASH
            hash = HashFS::hash(fpath, true);
            if (!hash.length()) {
                std::filesystem::path gzpath(fpath);
                gzpath += ".gz";
                hash = HashFS::hash(gzpath, true);
            }

            if (hash.length() && std::string(_webserver->header("If-None-Match").c_str()) == hash) {
                _webserver->send(304);
                return true;
            }

            Web_Server::handleReloadBlocked();
            return true;
        }

        // Check for brower cache match
        hash = HashFS::hash(fpath);
        if (!hash.length()) {
            std::filesystem::path gzpath(fpath);
            gzpath += ".gz";
            hash = HashFS::hash(gzpath);
        }

        if (hash.length() && std::string(_webserver->header("If-None-Match").c_str()) == hash) {
            _webserver->send(304);
            return true;
        }

        bool        isGzip = false;
        FileStream* file;
        // catch (...) not just Error: FluidPath inside the FileStream ctor
        // throws filesystem_error on an SD mount/stat hiccup, and uncaught it
        // aborts the board (this runs in the web-poller task).
        try {
            file = new FileStream(path, "r", "");
        } catch (...) {
            try {
                std::filesystem::path gzpath(fpath);
                gzpath += ".gz";
                file   = new FileStream(gzpath, "r", "");
                isGzip = true;
            } catch (...) {
                log_debug(path << " not found");
                return false;
            }
        }
        if (download) {
            _webserver->sendHeader("Content-Disposition", "attachment");
        }
        if (hash.length()) {
            _webserver->sendHeader("ETag", hash.c_str());
        }
        _webserver->setContentLength(file->size());
        if (isGzip) {
            _webserver->sendHeader("Content-Encoding", "gzip");
        }
        _webserver->send(200, getContentType(path), "");

        // This depends on the fact that FileStream inherits from Stream
        // The Arduino implementation of WiFiClient::write(Stream*) just
        // reads repetitively from the stream in 1360-byte chunks and
        // sends the data over the TCP socket. so nothing special.
        _webserver->client().write(*file);

        delete file;
        return true;
    }
    void Web_Server::sendWithOurAddress(const char* content, int code) {
        auto        ip    = WiFi.getMode() == WIFI_STA ? WiFi.localIP() : WiFi.softAPIP();
        std::string ipstr = IP_string(ip);
        if (_port != 80) {
            ipstr += ":";
            ipstr += std::to_string(_port);
        }

        std::string scontent(content);
        replace_string_in_place(scontent, "$WEB_ADDRESS$", ipstr);
        replace_string_in_place(scontent, "$QUERY$", _webserver->uri().c_str());
        _webserver->send(code, "text/html", scontent.c_str());
    }

    // Captive Portal Page for use in AP mode
    const char PAGE_CAPTIVE[] =
        "<HTML>\n<HEAD>\n<title>Captive Portal</title> \n</HEAD>\n<BODY>\n<CENTER>Captive Portal page : $QUERY$- you will be "
        "redirected...\n<BR><BR>\nif not redirected, <a href='http://$WEB_ADDRESS$'>click here</a>\n<BR><BR>\n<PROGRESS name='prg' "
        "id='prg'></PROGRESS>\n\n<script>\nvar i = 0; \nvar x = document.getElementById(\"prg\"); \nx.max=5; \nvar "
        "interval=setInterval(function(){\ni=i+1; \nvar x = document.getElementById(\"prg\"); \nx.value=i; \nif (i>5) "
        "\n{\nclearInterval(interval);\nwindow.location.href='/';\n}\n},1000);\n</script>\n</CENTER>\n</BODY>\n</HTML>\n\n";

    void Web_Server::sendCaptivePortal() {
        sendWithOurAddress(PAGE_CAPTIVE, 200);
    }

    //Default 404 page that is sent when a request cannot be satisfied
    const char PAGE_404[] =
        "<HTML>\n<HEAD>\n<title>Redirecting...</title> \n</HEAD>\n<BODY>\n<CENTER>Unknown page : $QUERY$- you will be "
        "redirected...\n<BR><BR>\nif not redirected, <a href='http://$WEB_ADDRESS$'>click here</a>\n<BR><BR>\n<PROGRESS name='prg' "
        "id='prg'></PROGRESS>\n\n<script>\nvar i = 0; \nvar x = document.getElementById(\"prg\"); \nx.max=5; \nvar "
        "interval=setInterval(function(){\ni=i+1; \nvar x = document.getElementById(\"prg\"); \nx.value=i; \nif (i>5) "
        "\n{\nclearInterval(interval);\nwindow.location.href='/';\n}\n},1000);\n</script>\n</CENTER>\n</BODY>\n</HTML>\n\n";

    void Web_Server::send404Page() {
        sendWithOurAddress(PAGE_404, 404);
    }

    void Web_Server::handle_root() {
        // Headless sand-table build: no web UI is served.  The board is driven
        // entirely by the stateless HTTP API (and runs playlists autonomously
        // once triggered), so a browser hitting the IP just gets this plain map
        // of the API instead of a single-page app or the WebUI file manager.
        _webserver->sendHeader("Cache-Control", "no-store");
        _webserver->send(200,
                         "text/plain",
                         "FluidNC sand-table headless API\n"
                         "\n"
                         "Status     GET /sand_status      (poll ~1s; JSON)\n"
                         "Patterns   GET /sand_patterns\n"
                         "Playlists  GET /sand_playlists\n"
                         "Settings   GET /sand_settings\n"
                         "Boot log   GET /sand_bootlog     (text; survives a panic reset)\n"
                         "Log        GET /sand_log         (text; last ~8KB of runtime log)\n"
                         "\n"
                         "Home       GET /sand_home\n"
                         "Goto       GET /sand_goto?theta=<rad>&rho=<0..1>  (either/both; idle only)\n"
                         "Stop       GET /sand_stop\n"
                         "Pause      GET /sand_pause\n"
                         "Resume     GET /sand_resume\n"
                         "Speed      GET /sand_feed?mm=<0..100000> | pct=<10..200> | d=up|down|reset\n"
                         "Time       GET /sand_time [?epoch=<unix>] [?tz=<POSIX>]   (read / app-sync clock)\n"
                         "LEDs       GET /sand_led?effect=|palette=|color=|color2=|brightness=|speed=\n"
                         "\n"
                         "Run/playlist, LED & everything else: GET /command?plain=$...\n"
                         "  $SD/Run=/patterns/star.thr | $Playlist/Run=<name>\n"
                         "  $Sand/Run=/patterns/star.thr clear=adaptive  (clear then run)\n"
                         "  $LED/Effect=rainbow | $LED/Brightness=80   (needs leds: in config)\n");
    }

    // Handle filenames and other things that are not explicitly registered
    void Web_Server::handle_not_found() {
        if (is_authenticated() == AuthenticationLevel::LEVEL_GUEST) {
            _webserver->sendHeader(LOCATION_HEADER, "/");
            _webserver->send(302);

            //_webserver->client().stop();
            return;
        }

        std::string path(_webserver->urlDecode(_webserver->uri()).c_str());

        if (path.rfind("/api/", 0) == 0) {
            _webserver->send(404);
            return;
        }

        // Download a file.  The true forces a download instead of displaying the file
        if (myStreamFile(path.c_str(), true)) {
            return;
        }

        if (WiFi.getMode() == WIFI_AP) {
            sendCaptivePortal();
            return;
        }

        // This lets the user customize the not-found page by
        // putting a "404.htm" file on the local filesystem
        if (myStreamFile("404.htm")) {
            return;
        }

        send404Page();
    }

#if 0
    //http SSDP xml presentation
    void Web_Server::handle_SSDP() {
        StreamString sschema;
        if (!sschema.reserve(1024)) {
            _webserver->send(500);
            return;
        }
        const char*       templ = "<?xml version=\"1.0\"?>"
                                  "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
                                  "<specVersion>"
                                  "<major>1</major>"
                                  "<minor>0</minor>"
                                  "</specVersion>"
                                  "<URLBase>http://%s:%u/</URLBase>"
                                  "<device>"
                                  "<deviceType>upnp:rootdevice</deviceType>"
                                  "<friendlyName>%s</friendlyName>"
                                  "<presentationURL>/</presentationURL>"
                                  "<serialNumber>%u</serialNumber>"
                                  "<modelName>ESP32</modelName>"
                                  "<modelNumber>Marlin</modelNumber>"
                                  "<modelURL>http://espressif.com/en/products/hardware/esp-wroom-32/overview</modelURL>"
                                  "<manufacturer>Espressif Systems</manufacturer>"
                                  "<manufacturerURL>http://espressif.com</manufacturerURL>"
                                  "<UDN>uuid:%s</UDN>"
                                  "</device>"
                                  "</root>\r\n"
                                  "\r\n";
        char              uuid[37];
        const std::string sip    = IP_string(WiFi.localIP());
        uint32_t          chipId = (uint16_t)(ESP.getEfuseMac() >> 32);
        sprintf(uuid,
                "38323636-4558-4dda-9188-cda0e6%02x%02x%02x",
                (uint16_t)((chipId >> 16) & 0xff),
                (uint16_t)((chipId >> 8) & 0xff),
                (uint16_t)chipId & 0xff);
        sschema.printf(templ, sip.c_str(), _port, WiFi.getHostname(), chipId, uuid);
        _webserver->send(200, "text/xml", sschema);
    }
#endif

    // WebUI sends a PAGEID arg to identify the websocket it is using
    int Web_Server::getPageid() {
        if (_webserver->hasArg("PAGEID")) {
            return _webserver->arg("PAGEID").toInt();
        }
        return -1;
    }
    void Web_Server::synchronousCommand(const char* cmd, bool silent, AuthenticationLevel auth_level) {
        if (http_block_during_motion->get() && inMotionState()) {
            _webserver->send(503, "text/plain", "Try again when not moving\n");
            return;
        }
        char line[256];
        strncpy(line, cmd, 255);
        webClient.attachWS(_webserver, silent);
        Error err = settings_execute_line(line, webClient, auth_level);
        if (err != Error::Ok) {
            std::string answer = "Error: ";
            const char* msg    = errorString(err);
            if (msg) {
                answer += msg;
            } else {
                answer += std::to_string(static_cast<int>(err));
            }
            answer += "\n";
            webClient.sendError(500, answer);
        } else {
            // This will send a 200 if it hasn't already been sent
            webClient.write(nullptr, 0);
        }
        webClient.detachWS();
    }
    void Web_Server::websocketCommand(const char* cmd, int pageid, AuthenticationLevel auth_level) {
        if (auth_level == AuthenticationLevel::LEVEL_GUEST) {
            _webserver->send(401, "text/plain", "Authentication failed\n");
            return;
        }

        bool hasError = WSChannels::runGCode(pageid, cmd);
        _webserver->send(hasError ? 500 : 200, "text/plain", hasError ? "WebSocket dead" : "");
    }
    void Web_Server::_handle_web_command(bool silent) {
        AuthenticationLevel auth_level = is_authenticated();
        if (_webserver->hasArg("cmd")) {  // WebUI3

            auto cmd = _webserver->arg("cmd");
            // [ESPXXX] commands expect data in the HTTP response
            if (cmd.startsWith("[ESP") || cmd.startsWith("$/")) {
                synchronousCommand(cmd.c_str(), silent, auth_level);
            } else {
                websocketCommand(cmd.c_str(), -1, auth_level);  // WebUI3 does not support PAGEID
            }
            return;
        }
        if (_webserver->hasArg("plain")) {
            synchronousCommand(_webserver->arg("plain").c_str(), silent, auth_level);
            return;
        }
        if (_webserver->hasArg("commandText")) {
            auto cmd = _webserver->arg("commandText");
            if (cmd.startsWith("[ESP")) {
                // [ESPXXX] commands expect data in the HTTP response
                // Only the fallback web page uses commandText with [ESPxxx]
                synchronousCommand(cmd.c_str(), silent, auth_level);
            } else {
                websocketCommand(_webserver->arg("commandText").c_str(), getPageid(), auth_level);
            }
            return;
        }
        _webserver->send(500, "text/plain", "Invalid command");
    }

    //login status check
    void Web_Server::handle_login() {
#ifdef ENABLE_AUTHENTICATION
        const char* smsg;
        std::string sUser, sPassword;
        const char* auths;
        int         code            = 200;
        bool        msg_alert_error = false;
        //disconnect can be done anytime no need to check credential
        if (_webserver->hasArg("DISCONNECT")) {
            std::string cookie(_webserver->header("Cookie").c_str());
            int         pos = cookie.find("ESPSESSIONID=");
            std::string sessionID;
            if (pos != std::string::npos) {
                int pos2  = cookie.find(";", pos);
                sessionID = cookie.substr(pos + strlen("ESPSESSIONID="), pos2);
            }
            ClearAuthIP(_webserver->client().remoteIP(), sessionID);
            _webserver->sendHeader("Set-Cookie", "ESPSESSIONID=0");
            _webserver->sendHeader("Cache-Control", "no-cache");
            sendAuth("Ok", "guest", "");
            //_webserver->client().stop();
            return;
        }

        AuthenticationLevel auth_level = is_authenticated();
        if (auth_level == AuthenticationLevel::LEVEL_GUEST) {
            auths = "guest";
        } else if (auth_level == AuthenticationLevel::LEVEL_USER) {
            auths = "user";
        } else if (auth_level == AuthenticationLevel::LEVEL_ADMIN) {
            auths = "admin";
        } else {
            auths = "???";
        }

        //check is it is a submission or a query
        if (_webserver->hasArg("SUBMIT")) {
            //is there a correct list of query?
            if (_webserver->hasArg("PASSWORD") && _webserver->hasArg("USER")) {
                //USER
                sUser = _webserver->arg("USER").c_str();
                if (!((sUser == DEFAULT_ADMIN_LOGIN) || (sUser == DEFAULT_USER_LOGIN))) {
                    msg_alert_error = true;
                    smsg            = "Error : Incorrect User";
                    code            = 401;
                }

                if (msg_alert_error == false) {
                    //Password
                    sPassword = _webserver->arg("PASSWORD").c_str();
                    std::string sadminPassword(admin_password->get());
                    std::string suserPassword(user_password->get());

                    if (!(sUser == DEFAULT_ADMIN_LOGIN && sPassword == sadminPassword) ||
                        (sUser == DEFAULT_USER_LOGIN && sPassword == suserPassword)) {
                        msg_alert_error = true;
                        smsg            = "Error: Incorrect password";
                        code            = 401;
                    }
                }
            } else {
                msg_alert_error = true;
                smsg            = "Error: Missing data";
                code            = 500;
            }
            //change password
            if (_webserver->hasArg("PASSWORD") && _webserver->hasArg("USER") && _webserver->hasArg("NEWPASSWORD") &&
                (msg_alert_error == false)) {
                std::string newpassword(_webserver->arg("NEWPASSWORD").c_str());

                char pwdbuf[MAX_LOCAL_PASSWORD_LENGTH + 1];
                newpassword.toCharArray(pwdbuf, MAX_LOCAL_PASSWORD_LENGTH + 1);

                Error err;

                if (sUser == DEFAULT_ADMIN_LOGIN) {
                    err = admin_password->setStringValue(pwdbuf);
                } else {
                    err = user_password->setStringValue(pwdbuf);
                }
                if (err != Error::Ok) {
                    msg_alert_error = true;
                    smsg            = "Error: Password cannot contain spaces";
                    code            = 500;
                }
            }
            if ((code == 200) || (code == 500)) {
                AuthenticationLevel current_auth_level;
                if (sUser == DEFAULT_ADMIN_LOGIN) {
                    current_auth_level = AuthenticationLevel::LEVEL_ADMIN;
                } else if (sUser == DEFAULT_USER_LOGIN) {
                    current_auth_level = AuthenticationLevel::LEVEL_USER;
                } else {
                    current_auth_level = AuthenticationLevel::LEVEL_GUEST;
                }
                //create Session
                if ((current_auth_level != auth_level) || (auth_level == AuthenticationLevel::LEVEL_GUEST)) {
                    AuthenticationIP* current_auth = new AuthenticationIP;
                    current_auth->level            = current_auth_level;
                    current_auth->ip               = _webserver->client().remoteIP();
                    strcpy(current_auth->sessionID, create_session_ID());
                    strcpy(current_auth->userID, sUser.c_str());
                    current_auth->last_time = millis();
                    if (AddAuthIP(current_auth)) {
                        std::string tmps = "ESPSESSIONID=";
                        tmps += current_auth->sessionID.c_str();
                        _webserver->sendHeader("Set-Cookie", tmps);
                        _webserver->sendHeader("Cache-Control", "no-cache");
                        switch (current_auth->level) {
                            case AuthenticationLevel::LEVEL_ADMIN:
                                auths = "admin";
                                break;
                            case AuthenticationLevel::LEVEL_USER:
                                auths = "user";
                                break;
                            default:
                                auths = "guest";
                                break;
                        }
                    } else {
                        delete current_auth;
                        msg_alert_error = true;
                        code            = 500;
                        smsg            = "Error: Too many connections";
                    }
                }
            }
            if (code == 200) {
                smsg = "Ok";
            }

            sendAuth("Ok", "guest", "");
        } else {
            if (auth_level != AuthenticationLevel::LEVEL_GUEST) {
                std::string cookie(_webserver->header("Cookie").c_str());
                int         pos = cookie.find("ESPSESSIONID=");
                std::string sessionID;
                if (pos != std::string::npos) {
                    int pos2                            = cookie.find(";", pos);
                    sessionID                           = cookie.substr(pos + strlen("ESPSESSIONID="), pos2);
                    AuthenticationIP* current_auth_info = GetAuth(_webserver->client().remoteIP(), sessionID.c_str());
                    if (current_auth_info != NULL) {
                        sUser = current_auth_info->userID;
                    }
                }
            }
            sendAuth(smsg, auths, "");
        }
#else
        sendAuth("Ok", "admin", "");
#endif
    }

    // This page is used when you try to reload WebUI during motion,
    // to avoid interrupting that motion.  It lets you wait until
    // motion is finished.
    void Web_Server::handleReloadBlocked() {
        _webserver->send(503,
                         "text/html",
                         "<!DOCTYPE html><html><body>"
                         "<h3>Cannot load WebUI while GCode Program is Running</h3>"

                         "<button onclick='window.location.replace(\"/feedhold_reload\")'>Pause</button>"
                         "&nbsp;Pause the GCode program with feedhold<br><br>"

                         "<button onclick='window.location.replace(\"/restart_reload\")'>Stop</button>"
                         "&nbsp;Stop the GCode Program with reset<br><br>"

                         "<button onclick='window.location.reload()'>Reload WebUI</button>"
                         "&nbsp;(You must first stop the GCode program or wait for it to finish)<br><br>"

                         "</body></html>");
    }
    void Web_Server::handleDidRestart() {
        _webserver->send(503,
                         "text/html",
                         "<!DOCTYPE html><html><body>"
                         "<h3>GCode Program has been stopped</h3>"
                         "<button onclick='window.location.replace(\"/\")'>Reload WebUI</button>"
                         "</body></html>");
    }
    // This page issues a feedhold to pause the motion then retries the WebUI reload
    void Web_Server::handleFeedholdReload() {
        protocol_send_event(&feedHoldEvent);
        //        delay(100);
        //        delay(100);
        // Go to the main page
        _webserver->sendHeader(LOCATION_HEADER, "/");
        _webserver->send(302);
    }
    // This page issues a feedhold to pause the motion then retries the WebUI reload
    void Web_Server::handleCyclestartReload() {
        protocol_send_event(&cycleStartEvent);
        //        delay(100);
        //        delay(100);
        // Go to the main page
        _webserver->sendHeader(LOCATION_HEADER, "/");
        _webserver->send(302);
    }
    // This page issues a feedhold to pause the motion then retries the WebUI reload
    void Web_Server::handleRestartReload() {
        protocol_send_event(&rtResetEvent);
        //        delay(100);
        //        delay(100);
        // Go to the main page
        _webserver->sendHeader(LOCATION_HEADER, "/did_restart");
        _webserver->send(302);
    }

    // Sand-table UI: clean stop (keeps position, no re-home) - see Cmd::StopJob.
    void Web_Server::handleSandStop() {
        // Stop the clear->pattern / playlist sequence first, so the state
        // machine goes Off instead of treating the aborted job's return to
        // Idle as normal completion and advancing to the pattern / next item.
        Playlist::stopActive();
        protocol_send_event(&stopJobEvent);
        _webserver->send(200, "text/plain", "ok");
    }

    // Sand-table UI: home.  Flags the main loop to run $H in the main task -
    // running $H here (the polling_loop task, via handleClient) makes homing
    // crawl, because two tasks then pump Stepper::prep_buffer().
    // Jog to an absolute theta (radians) and/or rho (0..1) for manual
    // positioning between patterns: /sand_goto?theta=<rad>&rho=<0..1> (either or
    // both).  Requires idle (no pattern running); the jog runs in the main loop.
    void Web_Server::handleSandGoto() {
        bool  hasTheta = _webserver->hasArg("theta");
        bool  hasRho   = _webserver->hasArg("rho");
        float theta    = hasTheta ? _webserver->arg("theta").toFloat() : 0.0f;
        float rho      = hasRho ? _webserver->arg("rho").toFloat() : 0.0f;
        Error err      = SandApi::goTo(hasTheta, theta, hasRho, rho);
        if (err == Error::Ok) {
            _webserver->send(200, "text/plain", "ok");
        } else if (err == Error::IdleError) {
            _webserver->send(409, "text/plain", "busy: goto requires idle (home first / stop the pattern)");
        } else {
            _webserver->send(400, "text/plain", "need theta=<rad> and/or rho=<0..1>");
        }
    }
    void Web_Server::handleSandHome() {
        protocol_send_event(&startHomeEvent);
        _webserver->send(200, "text/plain", "ok");
    }

    // Sand-table API: pause / resume a running pattern.  Both post a realtime
    // event to the main loop (feed hold / cycle start), so they work mid-motion
    // and never touch the block-during-motion gate or any WebSocket.
    void Web_Server::handleSandPause() {
        protocol_send_event(&feedHoldEvent);
        _webserver->send(200, "text/plain", "ok");
    }
    void Web_Server::handleSandResume() {
        protocol_send_event(&cycleStartEvent);
        _webserver->send(200, "text/plain", "ok");
    }

    // Sand-table UI: status snapshot returned directly in the HTTP body.  Unlike
    // $Sand/Status over /command (whose output goes to the WebSocket channel),
    // this lets any number of app clients poll status over plain HTTP - the
    // webui-v3 WebSocket is single-client.  Read-only and fast, and it skips the
    // block-during-motion gate so status is available while a pattern runs.
    void Web_Server::handleSandStatus() {
        _webserver->sendHeader("Cache-Control", "no-store");
        _webserver->send(200, "application/json", SandApi::statusJson().c_str());
    }

    // Multi-client HTTP reads (the $Sand/* command equivalents emit asynchronously
    // and only reach the single-client WebSocket).  All read-only and motion-safe.
    // Chunked-stream the listing so a 1000+ pattern library (~50 KB JSON) is
    // never built as one string -> no heap exhaustion / panic.
    void Web_Server::streamSandList(const char* folder, const char* ext) {
        _webserver->sendHeader("Cache-Control", "no-store");
        _webserver->setContentLength(CONTENT_LENGTH_UNKNOWN);
        _webserver->send(200, "application/json", "");
        SandApi::streamDirJson(folder, ext, [](const char* data, size_t len) {
            _webserver->sendContent(data, len);  // _webserver is static
        });
        _webserver->sendContent("", 0);  // terminate the chunked response
    }
    void Web_Server::handleSandPatterns() {
        // Serves the prebuilt /patterns/index.json manifest if present, else a
        // live top-level listing (see SandApi::streamPatterns).
        _webserver->sendHeader("Cache-Control", "no-store");
        _webserver->setContentLength(CONTENT_LENGTH_UNKNOWN);
        _webserver->send(200, "application/json", "");
        SandApi::streamPatterns([](const char* data, size_t len) {
            _webserver->sendContent(data, len);  // _webserver is static
        });
        _webserver->sendContent("", 0);  // terminate the chunked response
    }
    void Web_Server::handleSandPlaylists() {
        streamSandList("/playlists", ".txt");
    }

    // Boot log over HTTP.  StartupLog lives in RTC RAM that survives resets:
    // after a panic it still holds the previous boot's log (and dump() says
    // so), which is the only on-device crash breadcrumb on this headless
    // table.  Lines are streamed to the client as they arrive - accumulating
    // the ~7 KB dump in a string and then send()'s copy of it cost ~15 KB of
    // transient heap, a real dent in the ~35 KB free of a mid-pattern table.
    // The capture is synchronous (sendLine overridden): the default
    // Channel::sendLine queues lines to the output task, which would race
    // (and outlive) this HTTP response.
    namespace {
        class StreamOutChannel : public Channel {
        public:
            StreamOutChannel() : Channel("capture") {}
            size_t write(uint8_t c) override {
                char ch = (char)c;
                Web_Server::sendChunk(&ch, 1);
                return 1;
            }
            void sendLine(MsgLevel level, const char* line) override { addLine(line); }
            void sendLine(MsgLevel level, const std::string* line) override {
                addLine(line->c_str());
                delete line;  // this overload passes ownership (see Channel.cpp)
            }
            void sendLine(MsgLevel level, const std::string& line) override { addLine(line.c_str()); }

        private:
            void addLine(const char* line) {
                Web_Server::sendChunk(line, strlen(line));
                Web_Server::sendChunk("\n", 1);
            }
        };
    }
    void Web_Server::sendChunk(const char* data, size_t len) {
        _webserver->sendContent(data, len);
    }

    void Web_Server::handleSandBootlog() {
        _webserver->sendHeader("Cache-Control", "no-store");
        _webserver->setContentLength(CONTENT_LENGTH_UNKNOWN);
        _webserver->send(200, "text/plain", "");
        StreamOutChannel out;
        StartupLog::dump(out);
        _webserver->sendContent("", 0);  // terminate the chunked response
    }

    // Rolling session log (RingLog): the last ~8 KB of runtime log lines,
    // each prefixed with uptime seconds.  This is where playlist finish
    // reasons and SD errors end up on a headless table.  Snapshot into a
    // static buffer (no heap - see RingLog::snapshot) and stream it out in
    // bounded chunks.
    void Web_Server::handleSandLog() {
        static char snap[RingLog::kCapacity];
        size_t      n = RingLog::snapshot(snap, sizeof(snap));
        _webserver->sendHeader("Cache-Control", "no-store");
        _webserver->setContentLength(CONTENT_LENGTH_UNKNOWN);
        _webserver->send(200, "text/plain", "");
        for (size_t off = 0; off < n; off += 1024) {
            _webserver->sendContent(snap + off, n - off < 1024 ? n - off : 1024);
        }
        _webserver->sendContent("", 0);  // terminate the chunked response
    }
    void Web_Server::handleSandSettings() {
        _webserver->sendHeader("Cache-Control", "no-store");
        _webserver->send(200, "application/json", SandApi::settingsJson().c_str());
    }

    // Wall clock: read state, and let the app auto-sync the clock / timezone.
    //   GET /sand_time                       -> {epoch, synced, local, tz}
    //   GET /sand_time?epoch=<unix>          -> set the clock (e.g. app sync)
    //   GET /sand_time?tz=<POSIX>            -> set + persist the timezone
    void Web_Server::handleSandTime() {
        _webserver->sendHeader("Cache-Control", "no-store");
        if (_webserver->hasArg("tz")) {
            Clock::setTz(_webserver->arg("tz").c_str());
        }
        if (_webserver->hasArg("epoch")) {
            if (!Clock::setEpoch(static_cast<time_t>(_webserver->arg("epoch").toInt()))) {
                _webserver->send(400, "text/plain", "epoch must be a unix time after 2023-01-01");
                return;
            }
        }
        _webserver->send(200, "application/json", SandApi::timeJson().c_str());
    }

    // Sand-table UI: live feed control (works mid-pattern, no flash write).
    //   /sand_feed?mm=<0..100000>  set the base feed rate in motor mm/min
    //   /sand_feed?pct=<10..200>   scale it by an absolute override percentage
    //   /sand_feed?d=up|down|reset coarse override step / reset to 100%
    // mm sets the actual speed ($THR/Feed) directly; pct scales it. Both are
    // reported back by /sand_status (feed, feed_override).
    void Web_Server::handleSandFeed() {
        if (_webserver->hasArg("mm")) {
            if (Kinematics::ThetaRho::setFeedLive(_webserver->arg("mm").toInt()) != Error::Ok) {
                _webserver->send(400, "text/plain", "mm out of range (0..100000)");
            } else {
                _webserver->send(200, "text/plain", "ok");
            }
            return;
        }
        if (_webserver->hasArg("pct")) {
            protocol_send_event(&feedOverrideSetEvent, _webserver->arg("pct").toInt());
            _webserver->send(200, "text/plain", "ok");
            return;
        }
        std::string d = _webserver->hasArg("d") ? _webserver->arg("d").c_str() : "";
        if (d == "up") {
            protocol_send_event(&feedOverrideEvent, FeedOverride::CoarseIncrement);
        } else if (d == "down") {
            protocol_send_event(&feedOverrideEvent, -FeedOverride::CoarseIncrement);
        } else if (d == "reset") {
            protocol_send_event(&feedOverrideEvent, FeedOverride::Default);
        }
        _webserver->send(200, "text/plain", "ok");
    }

    // Sand-table UI: live LED control (works mid-pattern, no flash write).
    //   /sand_led?effect=fire&palette=ocean&brightness=120&color=FF0000&speed=80
    void Web_Server::handleSandLed() {
        _webserver->sendHeader("Cache-Control", "no-store");
        Leds* leds = Leds::instance();
        if (!leds) {
            _webserver->send(503, "text/plain", "leds not configured");
            return;
        }
        static const char* const keys[] = { "effect", "palette",  "color",  "color2",   "brightness", "speed",
                                            "direction", "align",  "size",   "bg",       "fgbright",   "bgbright" };
        std::string              rejected;
        int                      applied = 0;
        for (const char* k : keys) {
            if (_webserver->hasArg(k)) {
                if (leds->setLive(k, _webserver->arg(k).c_str()) == Error::Ok) {
                    ++applied;
                } else {
                    rejected += k;
                    rejected += ' ';
                }
            }
        }
        if (!rejected.empty()) {
            _webserver->send(400, "text/plain", ("rejected: " + rejected).c_str());
        } else if (applied == 0) {
            _webserver->send(400, "text/plain", "no LED args (effect|palette|color|color2|brightness|speed)");
        } else {
            _webserver->send(200, "text/plain", "ok");
        }
    }

    //push error code and message to websocket.  Used by upload code
    void Web_Server::pushError(int code, const char* st, bool web_error, uint16_t timeout) {
        if (_socket_server && st) {
            std::string s("ERROR:");
            s += std::to_string(code) + ":";
            s += st;

            WSChannels::sendError(getPageid(), st);

            if (web_error != 0 && _webserver && _webserver->client().available() > 0) {
                _webserver->send(web_error, "text/xml", st);
            }

            uint32_t start_time = millis();
            while ((millis() - start_time) < timeout) {
                _socket_server->loop();
                delay_ms(10);
            }

            if (_socket_serverv3) {
                start_time = millis();
                while ((millis() - start_time) < timeout) {
                    _socket_serverv3->loop();
                    delay_ms(10);
                }
            }
        }
    }

    //abort reception of packages
    void Web_Server::cancelUpload() {
        if (_webserver && _webserver->client().available() > 0) {
            HTTPUpload& upload = _webserver->upload();
            upload.status      = UPLOAD_FILE_ABORTED;
            errno              = ECONNABORTED;
            _webserver->client().stop();
            delay(100);
        }
    }

    //LocalFS files uploader handle
    void Web_Server::fileUpload(const char* fs) {
        HTTPUpload& upload = _webserver->upload();
        //this is only for admin and user
        if (is_authenticated() == AuthenticationLevel::LEVEL_GUEST) {
            _upload_status = UploadStatus::FAILED;
            log_info("Upload rejected");
            recordUploadError(ESP_ERROR_AUTHENTICATION, "authentication failed");
            sendJSON(401, "{\"status\":\"Authentication failed!\"}");
            pushError(ESP_ERROR_AUTHENTICATION, "Upload rejected", 401);
        } else {
            if ((_upload_status != UploadStatus::FAILED) || (upload.status == UPLOAD_FILE_START)) {
                if (upload.status == UPLOAD_FILE_START) {
                    std::string sizeargname(upload.filename.c_str());
                    sizeargname += "S";
                    size_t filesize = _webserver->hasArg(sizeargname.c_str()) ? _webserver->arg(sizeargname.c_str()).toInt() : 0;
                    uploadStart(upload.filename.c_str(), filesize, fs);
                } else if (upload.status == UPLOAD_FILE_WRITE) {
                    uploadWrite(upload.buf, upload.currentSize);
                } else if (upload.status == UPLOAD_FILE_END) {
                    std::string sizeargname(upload.filename.c_str());
                    sizeargname += "S";
                    size_t filesize = _webserver->hasArg(sizeargname.c_str()) ? _webserver->arg(sizeargname.c_str()).toInt() : 0;
                    uploadEnd(filesize);
                } else {  //Upload cancelled
                    uploadStop();
                    return;
                }
            }
        }
        uploadCheck();
    }

    void Web_Server::sendJSON(int code, const char* s) {
        _webserver->sendHeader("Cache-Control", "no-cache");
        _webserver->send(code, "application/json", s);
    }

    void Web_Server::sendAuth(const char* status, const char* level, const char* user) {
        std::string s;
        JSONencoder j(&s);
        j.begin();
        j.member("status", status);
        if (*level != '\0') {
            j.member("authentication_lvl", level);
        }
        if (*user != '\0') {
            j.member("user", user);
        }
        j.end();
        sendJSON(200, s);
    }

    void Web_Server::sendStatus(int code, const char* status) {
        std::string s;
        JSONencoder j(&s);
        j.begin();
        j.member("status", status);
        j.end();
        sendJSON(code, s);
    }

    void Web_Server::sendAuthFailed() {
        sendStatus(401, "Authentication failed");
    }

    void Web_Server::LocalFSFileupload() {
        fileUpload(localfsName);
    }
    void Web_Server::SDFileUpload() {
        fileUpload(sdName);
    }

    //Web Update handler
    // OTA is blocked while motion or a job is running (an app-triggered
    // auto-update must never kill a pattern mid-run), but stays available in
    // Idle and Alarm/ConfigAlarm - a wedged board must remain updatable.
    bool Web_Server::updateBlocked() {
        return Job::active() || state_is(State::Cycle) || state_is(State::Homing) || state_is(State::Hold) || state_is(State::Jog);
    }

    void Web_Server::handleUpdate() {
        AuthenticationLevel auth_level = is_authenticated();
        if (auth_level != AuthenticationLevel::LEVEL_ADMIN) {
            _upload_status = UploadStatus::NONE;
            _webserver->send(403, "text/plain", "Not allowed, log in first!\n");
            return;
        }

        // App contract ("status"): "ok" -> flashed, board reboots ~1s later
        // (poll /sand_status until uptime resets and "fw" shows the new
        // version); "ready"/"busy" -> probe result with no file attached
        // (GET before uploading); "failed" -> upload rejected or bad image.
        bool        ok    = _upload_status == UploadStatus::SUCCESSFUL;
        bool        probe = _upload_status == UploadStatus::NONE;
        bool        busy  = updateBlocked();
        std::string s;
        JSONencoder j(&s);
        j.begin();
        j.member("status", ok ? "ok" : probe ? (busy ? "busy" : "ready") : busy ? "busy" : "failed");
        j.member("code", std::to_string(int(_upload_status)));
        j.member("fw", git_info);
        j.end();
        sendJSON(ok ? 200 : busy ? 409 : probe ? 200 : 500, s);

        //if success restart
        if (ok) {
            delay_ms(1000);
            protocol_send_event(&fullResetEvent);
        } else {
            _upload_status = UploadStatus::NONE;
        }
    }

    //File upload for Web update
    void Web_Server::WebUpdateUpload() {
        static size_t   last_upload_update;
        static uint32_t maxSketchSpace = 0;

        //only admin can update FW
        if (is_authenticated() != AuthenticationLevel::LEVEL_ADMIN) {
            _upload_status = UploadStatus::FAILED;
            log_info("Upload rejected");
            sendAuthFailed();
            pushError(ESP_ERROR_AUTHENTICATION, "Upload rejected", 401);
        } else {
            //get current file ID
            HTTPUpload& upload = _webserver->upload();
            if ((_upload_status != UploadStatus::FAILED) || (upload.status == UPLOAD_FILE_START)) {
                //Upload start
                //**************
                if (upload.status == UPLOAD_FILE_START) {
                    if (updateBlocked()) {
                        _upload_status = UploadStatus::FAILED;
                        log_info("Update rejected - motion/job active");
                        pushError(ESP_ERROR_UPLOAD, "Update rejected, machine is running");
                        uploadCheck();
                        return;
                    }
                    log_info("Update Firmware");
                    _upload_status = UploadStatus::ONGOING;
                    std::string sizeargname(upload.filename.c_str());
                    sizeargname += "S";
                    if (_webserver->hasArg(sizeargname.c_str())) {
                        maxSketchSpace = _webserver->arg(sizeargname.c_str()).toInt();
                    }
                    //check space
                    size_t flashsize = 0;
                    if (esp_ota_get_running_partition()) {
                        const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
                        if (partition) {
                            flashsize = partition->size;
                        }
                    }
                    if (flashsize < maxSketchSpace) {
                        pushError(ESP_ERROR_NOT_ENOUGH_SPACE, "Upload rejected, not enough space");
                        _upload_status = UploadStatus::FAILED;
                        log_info("Update cancelled");
                    }
                    if (_upload_status != UploadStatus::FAILED) {
                        last_upload_update = 0;
                        if (!Update.begin()) {  //start with max available size
                            _upload_status = UploadStatus::FAILED;
                            log_info("Update cancelled");
                            pushError(ESP_ERROR_NOT_ENOUGH_SPACE, "Upload rejected, not enough space");
                        } else {
                            log_info("Update 0%");
                        }
                    }
                    //Upload write
                    //**************
                } else if (upload.status == UPLOAD_FILE_WRITE) {
                    delay_ms(1);
                    //check if no error
                    if (_upload_status == UploadStatus::ONGOING) {
                        if (((100 * upload.totalSize) / maxSketchSpace) != last_upload_update) {
                            if (maxSketchSpace > 0) {
                                last_upload_update = (100 * upload.totalSize) / maxSketchSpace;
                            } else {
                                last_upload_update = upload.totalSize;
                            }

                            log_info("Update " << last_upload_update << "%");
                        }
                        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                            _upload_status = UploadStatus::FAILED;
                            log_info("Update write failed");
                            pushError(ESP_ERROR_FILE_WRITE, "File write failed");
                        }
                    }
                    //Upload end
                    //**************
                } else if (upload.status == UPLOAD_FILE_END) {
                    if (Update.end(true)) {  //true to set the size to the current progress
                        //Now Reboot
                        log_info("Update 100%");
                        _upload_status = UploadStatus::SUCCESSFUL;
                    } else {
                        _upload_status = UploadStatus::FAILED;
                        log_info("Update failed");
                        pushError(ESP_ERROR_UPLOAD, "Update upload failed");
                    }
                } else if (upload.status == UPLOAD_FILE_ABORTED) {
                    log_info("Update failed");
                    _upload_status = UploadStatus::FAILED;
                    return;
                }
            }
        }

        if (_upload_status == UploadStatus::FAILED) {
            cancelUpload();
            Update.end();
        }
    }

    void Web_Server::handleFileOps(const char* fs) {
        //this is only for admin and user
        if (is_authenticated() == AuthenticationLevel::LEVEL_GUEST) {
            _upload_status = UploadStatus::NONE;
            sendAuthFailed();
            return;
        }

        std::error_code ec;

        std::string path("");
        std::string sstatus("Ok");
        // Machine-readable failure detail for the response: HTTP status != 200
        // plus an "error":{code,message} member whenever serror is non-empty.
        std::string serror;
        int         ecode = 0;
        if (_upload_status == UploadStatus::FAILED) {
            sstatus = "Upload failed";
            serror  = _upload_error_msg.empty() ? "upload failed" : _upload_error_msg;
            ecode   = _upload_error_code;
        }
        _upload_status     = UploadStatus::NONE;
        _upload_error_code = 0;
        _upload_error_msg.clear();
        bool     list_files = true;
        uint64_t totalspace = 0;
        uint64_t usedspace  = 0;

        //get current path
        if (_webserver->hasArg("path")) {
            path += _webserver->arg("path").c_str();
            // path.trim();
            replace_string_in_place(path, "//", "/");
            if (path[path.length() - 1] == '/') {
                path = path.substr(0, path.length() - 1);
            }
            if (path.length() & path[0] == '/') {
                path = path.substr(1);
            }
        }

        FluidPath fpath { path, fs, ec };
        if (ec) {
            std::string s;
            JSONencoder j(&s);
            j.begin();
            j.member("status", "No SD card");
            j.begin_member_object("error");
            j.member("code", ESP_ERROR_FILE_CREATION);
            j.member("message", std::string("filesystem inaccessible: ") + ec.message());
            j.end_object();
            j.end();
            sendJSON(503, s);
            return;
        }

        // Handle deletions and directory creation
        if (_webserver->hasArg("action") && _webserver->hasArg("filename")) {
            std::string action(_webserver->arg("action").c_str());
            std::string filename = std::string(_webserver->arg("filename").c_str());
            if (action == "delete") {
                if (stdfs::remove(fpath / filename, ec)) {
                    sstatus = filename + " deleted";
                    HashFS::delete_file(fpath / filename);
                } else {
                    sstatus = "Cannot delete ";
                    sstatus += filename + " " + ec.message();
                    serror  = sstatus;
                    ecode   = ESP_ERROR_FILE_OP;
                }
            } else if (action == "deletedir") {
                stdfs::path dirpath { fpath / filename };
                log_debug("Deleting directory " << dirpath);
                int count = stdfs::remove_all(dirpath, ec);
                if (count > 0) {
                    sstatus = filename + " deleted";
                    HashFS::report_change();
                } else {
                    log_debug("remove_all returned " << count);
                    sstatus = "Cannot delete ";
                    sstatus += filename + " " + ec.message();
                    serror  = sstatus;
                    ecode   = ESP_ERROR_FILE_OP;
                }
            } else if (action == "createdir") {
                if (stdfs::create_directory(fpath / filename, ec)) {
                    sstatus = filename + " created";
                    HashFS::report_change();
                } else {
                    sstatus = "Cannot create ";
                    sstatus += filename + " " + ec.message();
                    serror  = sstatus;
                    ecode   = ESP_ERROR_FILE_OP;
                }
            } else if (action == "rename") {
                if (!_webserver->hasArg("newname")) {
                    sstatus = "Missing new filename";
                    serror  = sstatus;
                    ecode   = ESP_ERROR_FILE_OP;
                } else {
                    std::string newname = std::string(_webserver->arg("newname").c_str());
                    std::filesystem::rename(fpath / filename, fpath / newname, ec);
                    if (ec) {
                        sstatus = "Cannot rename ";
                        sstatus += filename + " " + ec.message();
                        serror  = sstatus;
                        ecode   = ESP_ERROR_FILE_OP;
                    } else {
                        sstatus = filename + " renamed to " + newname;
                        HashFS::rename_file(fpath / filename, fpath / newname);
                    }
                }
            }
        }

        //check if no need build file list
        if (_webserver->hasArg("dontlist") && _webserver->arg("dontlist") == "yes") {
            list_files = false;
        }

        std::string s;
        JSONencoder j(&s);
        j.begin();

        if (list_files) {
            auto iter = stdfs::directory_iterator { fpath, ec };
            if (ec && serror.empty()) {
                serror = "cannot list " + path + ": " + ec.message();
                ecode  = ESP_ERROR_FILE_OP;
            }
            if (!ec) {
                j.begin_array("files");
                for (auto const& dir_entry : iter) {
                    j.begin_object();
                    j.member("name", dir_entry.path().filename());
                    j.member("shortname", dir_entry.path().filename());
                    j.member("size", dir_entry.is_directory() ? -1 : dir_entry.file_size());
                    j.member("datetime", "");
                    j.end_object();
                }
                j.end_array();
            }
        }

        auto space = stdfs::space(fpath, ec);
        totalspace = space.capacity;
        usedspace  = totalspace - space.available;

        j.member("path", path.c_str());
        j.member("total", formatBytes(totalspace));
        j.member("used", formatBytes(usedspace + 1));

        uint32_t percent = totalspace ? (usedspace * 100) / totalspace : 100;

        j.member("occupation", percent);
        j.member("status", sstatus);
        if (!serror.empty()) {
            j.begin_member_object("error");
            j.member("code", ecode);
            j.member("message", serror);
            j.end_object();
        }
        j.end();

        // Failures get a real HTTP error so a stateless client can react
        // without parsing the human status text (the WebSocket error channel
        // is disabled on this build).
        int http = 200;
        if (!serror.empty()) {
            http = ecode == ESP_ERROR_AUTHENTICATION ? 401 : ecode == ESP_ERROR_NOT_ENOUGH_SPACE ? 507 : 500;
        }
        sendJSON(http, s);
    }

    void Web_Server::handle_direct_SDFileList() {
        handleFileOps(sdName);
    }
    void Web_Server::handleFileList() {
        handleFileOps(localfsName);
    }

    // File upload
    void Web_Server::uploadStart(const char* filename, size_t filesize, const char* fs) {
        std::error_code ec;

        FluidPath fpath { filename, fs, ec };
        if (ec) {
            _upload_status = UploadStatus::FAILED;
            log_info("Upload filesystem inaccessible");
            recordUploadError(ESP_ERROR_FILE_CREATION, std::string("filesystem inaccessible: ") + ec.message());
            pushError(ESP_ERROR_FILE_CREATION, "Upload rejected, filesystem inaccessible");
            return;
        }

        // error_code overload: the throwing stdfs::space would abort the board
        // on a flaky-SD statvfs failure (uncaught filesystem_error).
        auto space = stdfs::space(fpath, ec);
        if (ec) {
            _upload_status = UploadStatus::FAILED;
            log_info("Upload filesystem inaccessible");
            recordUploadError(ESP_ERROR_FILE_CREATION, std::string("filesystem inaccessible: ") + ec.message());
            pushError(ESP_ERROR_FILE_CREATION, "Upload rejected, filesystem inaccessible");
            return;
        }
        if (filesize && filesize > space.available) {
            // If the file already exists, maybe there will be enough space
            // when we replace it.
            auto existing_size = stdfs::file_size(fpath, ec);
            if (ec || (filesize > (space.available + existing_size))) {
                _upload_status = UploadStatus::FAILED;
                log_info("Upload not enough space");
                recordUploadError(ESP_ERROR_NOT_ENOUGH_SPACE, "not enough space");
                pushError(ESP_ERROR_NOT_ENOUGH_SPACE, "Upload rejected, not enough space");
                return;
            }
        }

        if (_upload_status != UploadStatus::FAILED) {
            //Create file for writing
            try {
                _uploadFile    = new FileStream(fpath, "w");
                _upload_status = UploadStatus::ONGOING;
            } catch (...) {
                _uploadFile    = nullptr;
                _upload_status = UploadStatus::FAILED;
                log_info("Upload failed - cannot create file");
                recordUploadError(ESP_ERROR_FILE_CREATION, std::string("cannot create ") + filename + " (missing directory, bad name, or filesystem full)");
                pushError(ESP_ERROR_FILE_CREATION, "File creation failed");
            }
        }
    }

    void Web_Server::uploadWrite(uint8_t* buffer, size_t length) {
        delay_ms(1);
        if (_uploadFile && _upload_status == UploadStatus::ONGOING) {
            //no error write post data
            if (length != _uploadFile->write(buffer, length)) {
                _upload_status = UploadStatus::FAILED;
                log_info("Upload failed - file write failed");
                recordUploadError(ESP_ERROR_FILE_WRITE, "file write failed (filesystem full or failing?)");
                pushError(ESP_ERROR_FILE_WRITE, "File write failed");
            }
        } else {  //if error set flag UploadStatus::FAILED
            _upload_status = UploadStatus::FAILED;
            log_info("Upload failed - file not open");
            recordUploadError(ESP_ERROR_FILE_WRITE, "file not open");
            pushError(ESP_ERROR_FILE_WRITE, "File not open");
        }
    }

    void Web_Server::uploadEnd(size_t filesize) {
        //if file is open close it
        if (_uploadFile) {
            //            delete _uploadFile;
            // _uploadFile = nullptr;

            std::string pathname = _uploadFile->fpath();
            delete _uploadFile;
            _uploadFile = nullptr;
            log_debug("pathname " << pathname);

            // The delete above dropped the SD refcount, so this FluidPath
            // re-mounts the card and can throw filesystem_error on a flaky
            // card; bare, it would abort the board on every marginal upload.
            std::error_code fp_ec;
            FluidPath       filepath { pathname, "", fp_ec };
            if (fp_ec) {
                _upload_status = UploadStatus::FAILED;
                log_info("Upload failed - filesystem inaccessible at close");
                recordUploadError(ESP_ERROR_FILE_CLOSE, std::string("filesystem inaccessible at close: ") + fp_ec.message());
                pushError(ESP_ERROR_FILE_CLOSE, "File close failed");
                return;
            }

            HashFS::rehash_file(filepath);

            // Check size
            if (filesize) {
                uint32_t        actual_size;
                std::error_code sz_ec;
                actual_size = stdfs::file_size(filepath, sz_ec);
                if (sz_ec) {
                    actual_size = 0;
                }

                if (filesize != actual_size) {
                    _upload_status = UploadStatus::FAILED;
                    recordUploadError(ESP_ERROR_UPLOAD,
                                      "size mismatch - expected " + std::to_string(filesize) + " got " + std::to_string(actual_size));
                    pushError(ESP_ERROR_UPLOAD, "File upload mismatch");
                    log_info("Upload failed - size mismatch - exp " << filesize << " got " << actual_size);
                }
            }
        } else {
            _upload_status = UploadStatus::FAILED;
            log_info("Upload failed - file not open");
            recordUploadError(ESP_ERROR_FILE_CLOSE, "file not open at close");
            pushError(ESP_ERROR_FILE_CLOSE, "File close failed");
        }
        if (_upload_status == UploadStatus::ONGOING) {
            _upload_status = UploadStatus::SUCCESSFUL;
        } else {
            _upload_status = UploadStatus::FAILED;
            pushError(ESP_ERROR_UPLOAD, "Upload error 8");
        }
    }
    void Web_Server::uploadStop() {
        _upload_status = UploadStatus::FAILED;
        recordUploadError(ESP_ERROR_UPLOAD_CANCELLED, "upload cancelled");
        log_info("Upload cancelled");
        if (_uploadFile) {
            std::filesystem::path filepath = _uploadFile->fpath();
            delete _uploadFile;
            _uploadFile = nullptr;
            HashFS::rehash_file(filepath);
        }
    }
    void Web_Server::uploadCheck() {
        std::error_code error_code;
        if (_upload_status == UploadStatus::FAILED) {
            cancelUpload();
            if (_uploadFile) {
                std::filesystem::path filepath = _uploadFile->fpath();
                delete _uploadFile;
                _uploadFile = nullptr;
                stdfs::remove(filepath, error_code);
                HashFS::rehash_file(filepath);
            }
        }
    }

    void Web_Server::poll() {
        static uint32_t start_time = millis();
        if (WiFi.getMode() == WIFI_AP) {
            dnsServer.processNextRequest();
        }
        if (_webserver) {
            _webserver->handleClient();
        }
        if (_socket_server && _setupdone) {
            _socket_server->loop();
        }
        if (_socket_serverv3 && _setupdone) {
            _socket_serverv3->loop();
        }
        if ((millis() - start_time) > 10000 && _socket_server) {
            WSChannels::sendPing();
            start_time = millis();
        }
    }

    void Web_Server::handle_Websocket_Event(uint8_t num, uint8_t type, uint8_t* payload, size_t length) {
        WSChannels::handleEvent(_socket_server, num, type, payload, length);
    }

    void Web_Server::handle_Websocketv3_Event(uint8_t num, uint8_t type, uint8_t* payload, size_t length) {
        WSChannels::handlev3Event(_socket_serverv3, num, type, payload, length);
    }

    //Convert file extension to content type
    struct mime_type {
        const char* suffix;
        const char* mime_type;
    } mime_types[] = {
        { ".htm", "text/html" },         { ".html", "text/html" },        { ".css", "text/css" },   { ".js", "application/javascript" },
        { ".htm", "text/html" },         { ".png", "image/png" },         { ".gif", "image/gif" },  { ".jpeg", "image/jpeg" },
        { ".jpg", "image/jpeg" },        { ".ico", "image/x-icon" },      { ".xml", "text/xml" },   { ".pdf", "application/x-pdf" },
        { ".zip", "application/x-zip" }, { ".gz", "application/x-gzip" }, { ".txt", "text/plain" }, { "", "application/octet-stream" }
    };
    static bool endsWithCI(const char* suffix, const char* test) {
        size_t slen = strlen(suffix);
        size_t tlen = strlen(test);
        if (slen > tlen) {
            return false;
        }
        const char* s = suffix + slen;
        const char* t = test + tlen;
        while (--s != s) {
            if (tolower(*s) != tolower(*--t)) {
                return false;
            }
        }
        return true;
    }
    const char* Web_Server::getContentType(const char* filename) {
        mime_type* m;
        for (m = mime_types; *(m->suffix) != '\0'; ++m) {
            if (endsWithCI(m->suffix, filename)) {
                return m->mime_type;
            }
        }
        return m->mime_type;
    }

    //check authentification
    AuthenticationLevel Web_Server::is_authenticated() {
#ifdef ENABLE_AUTHENTICATION
        if (_webserver->hasHeader("Cookie")) {
            std::string cookie(_webserver->header("Cookie").c_str());
            size_t      pos = cookie.find("ESPSESSIONID=");
            if (pos != std::string::npos) {
                size_t      pos2      = cookie.find(";", pos);
                std::string sessionID = cookie.substr(pos + strlen("ESPSESSIONID="), pos2);
                IPAddress   ip        = _webserver->client().remoteIP();
                //check if cookie can be reset and clean table in same time
                return ResetAuthIP(ip, sessionID.c_str());
            }
        }
        return AuthenticationLevel::LEVEL_GUEST;
#else
        return AuthenticationLevel::LEVEL_ADMIN;
#endif
    }

#ifdef ENABLE_AUTHENTICATION

    //add the information in the linked list if possible
    bool Web_Server::AddAuthIP(AuthenticationIP* item) {
        if (_nb_ip > MAX_AUTH_IP) {
            return false;
        }
        item->_next = _head;
        _head       = item;
        _nb_ip++;
        return true;
    }

    //Session ID based on IP and time using 16 char
    char* Web_Server::create_session_ID() {
        static char sessionID[17];
        //reset SESSIONID
        for (int i = 0; i < 17; i++) {
            sessionID[i] = '\0';
        }
        //get time
        uint32_t now = millis();
        //get remote IP
        IPAddress remoteIP = _webserver->client().remoteIP();
        //generate SESSIONID
        if (0 > sprintf(sessionID,
                        "%02X%02X%02X%02X%02X%02X%02X%02X",
                        remoteIP[0],
                        remoteIP[1],
                        remoteIP[2],
                        remoteIP[3],
                        (uint8_t)((now >> 0) & 0xff),
                        (uint8_t)((now >> 8) & 0xff),
                        (uint8_t)((now >> 16) & 0xff),
                        (uint8_t)((now >> 24) & 0xff))) {
            strcpy(sessionID, "NONE");
        }
        return sessionID;
    }

    bool Web_Server::ClearAuthIP(IPAddress ip, const char* sessionID) {
        AuthenticationIP* current  = _head;
        AuthenticationIP* previous = NULL;
        bool              done     = false;
        while (current) {
            if ((ip == current->ip) && (strcmp(sessionID, current->sessionID) == 0)) {
                //remove
                done = true;
                if (current == _head) {
                    _head = current->_next;
                    _nb_ip--;
                    delete current;
                    current = _head;
                } else {
                    previous->_next = current->_next;
                    _nb_ip--;
                    delete current;
                    current = previous->_next;
                }
            } else {
                previous = current;
                current  = current->_next;
            }
        }
        return done;
    }

    //Get info
    AuthenticationIP* Web_Server::GetAuth(IPAddress ip, const char* sessionID) {
        AuthenticationIP* current = _head;
        //AuthenticationIP * previous = NULL;
        while (current) {
            if (ip == current->ip) {
                if (strcmp(sessionID, current->sessionID) == 0) {
                    //found
                    return current;
                }
            }
            //previous = current;
            current = current->_next;
        }
        return NULL;
    }

    //Review all IP to reset timers
    AuthenticationLevel Web_Server::ResetAuthIP(IPAddress ip, const char* sessionID) {
        AuthenticationIP* current  = _head;
        AuthenticationIP* previous = NULL;
        while (current) {
            if ((millis() - current->last_time) > 360000) {
                //remove
                if (current == _head) {
                    _head = current->_next;
                    _nb_ip--;
                    delete current;
                    current = _head;
                } else {
                    previous->_next = current->_next;
                    _nb_ip--;
                    delete current;
                    current = previous->_next;
                }
            } else {
                if (ip == current->ip && strcmp(sessionID, current->sessionID) == 0) {
                    //reset time
                    current->last_time = millis();
                    return (AuthenticationLevel)current->level;
                }
                previous = current;
                current  = current->_next;
            }
        }
        return AuthenticationLevel::LEVEL_GUEST;
    }
#endif
    ModuleFactory::InstanceBuilder<Web_Server> __attribute__((init_priority(108))) web_server_module("wifi", true);
}
