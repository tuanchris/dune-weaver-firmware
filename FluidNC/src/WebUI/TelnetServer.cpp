// Copyright (c) 2014 Luc Lebosse. All rights reserved.
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.
// #include <ESPmDNS.h>
#include "src/Machine/MachineConfig.h"
#include "TelnetClient.h"
#include "TelnetServer.h"

#include "Mdns.h"
#include "src/Report.h"               // report_init_message()
#include "src/Protocol.h"             // drain_messages()
#include "src/SettingsDefinitions.h"  // sand_password ($Sand/Password API lock)

#include <WiFi.h>

namespace WebUI {

    EnumSetting* telnet_enable;
    IntSetting*  telnet_port;

    uint16_t TelnetServer::_port = 0;

    std::queue<TelnetClient*> TelnetServer::_disconnected;
    std::mutex                TelnetServer::_disconnected_mtx;
    int                       TelnetServer::_client_count = 0;

    void TelnetServer::init() {
        if (WiFi.getMode() == WIFI_OFF) {
            return;
        }

        deinit();

        telnet_port =
            new IntSetting("Telnet Port", WEBSET, WA, "ESP131", "Telnet/Port", DEFAULT_TELNETSERVER_PORT, MIN_TELNET_PORT, MAX_TELNET_PORT);

        telnet_enable = new EnumSetting("Telnet Enable", WEBSET, WA, "ESP130", "Telnet/Enable", DEFAULT_TELNET_STATE, &onoffOptions);

        if (!WebUI::telnet_enable->get()) {
            return;
        }
        _port = WebUI::telnet_port->get();

        //create instance
        _wifiServer = new WiFiServer(_port, MAX_TLNT_CLIENTS);
        _wifiServer->setNoDelay(true);
        log_info("Telnet started on port " << _port);
        //start telnet server
        _wifiServer->begin();
        _setupdone = true;

        Mdns::add("_telnet", "_tcp", _port);
    }

    void TelnetServer::deinit() {
        _setupdone = false;
        if (_wifiServer) {
            // delete _wifiServer;
            _wifiServer = NULL;
        }

        //remove mDNS
        Mdns::remove("_telnet", "_tcp");
    }

    void TelnetServer::poll() {
        if (!_setupdone || _wifiServer == NULL) {
            return;
        }

        while (true) {
            TelnetClient* client = nullptr;
            {
                // Pop under the lock so we never race a concurrent push from
                // TelnetClient::closeOnDisconnect() (output task).
                std::lock_guard<std::mutex> lock(_disconnected_mtx);
                if (_disconnected.empty()) {
                    break;
                }
                client = _disconnected.front();
                _disconnected.pop();
            }
            log_debug("Telnet client disconnected");
            // Flush the async output queue before deleting: it holds raw
            // Channel pointers, and this client IS a Channel, so a still-queued
            // log message would make output_loop call a method on freed memory
            // (__cxa_pure_virtual -> abort()).  Same fix as Job::pop().
            drain_messages();
            allChannels.deregistration(client);
            delete client;
            if (_client_count > 0) {
                --_client_count;
            }
        }

        //check if there are any new clients
        if (_wifiServer->hasClient()) {
            // Refuse under low heap or at the client cap, BEFORE allocating the
            // ~1.5 KB client.  Accepting-then-stopping drops the pending socket
            // without a heap-heavy TelnetClient.  (The old `new` had no floor
            // and its `if (!tcpClient)` guard was dead code -- a throwing new
            // never returns null.)
            if (ESP.getMaxAllocHeap() < ACCEPT_HEAP_FLOOR || _client_count >= MAX_TLNT_CLIENTS) {
                WiFiClient reject = _wifiServer->available();
                reject.stop();
                return;
            }
            WiFiClient* tcpClient = new (std::nothrow) WiFiClient(_wifiServer->available());
            if (!tcpClient) {
                return;  // out of heap: drop the connection rather than abort
            }
            // $Sand/Password: telnet has no key mechanism and would bypass
            // the HTTP lock entirely, so a locked table refuses telnet
            // clients.  USB serial stays open for recovery.
            if (sand_password && *sand_password->get()) {
                log_info("Telnet from " << tcpClient->remoteIP() << " refused ($Sand/Password is set)");
                tcpClient->print("access denied: $Sand/Password is set; use HTTP with the key, or USB serial\r\n");
                tcpClient->stop();
                delete tcpClient;
                return;
            }
            TelnetClient* tnc = new (std::nothrow) TelnetClient(tcpClient);
            if (!tnc) {
                tcpClient->stop();
                delete tcpClient;
                return;
            }
            log_debug("Telnet from " << tcpClient->remoteIP());
            allChannels.registration(tnc);
            ++_client_count;
        }
    }

    void TelnetServer::status_report(Channel& out) {
        log_stream(out, "Data port: " << port());
    }

    TelnetServer::~TelnetServer() {
        deinit();
    }

    ModuleFactory::InstanceBuilder<TelnetServer> __attribute__((init_priority(109))) telnet_module("telnet_server", true);
}
