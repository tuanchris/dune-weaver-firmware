#pragma once

#include "Settings.h"

extern StringSetting* config_filename;

extern StringSetting* build_info;

extern StringSetting* start_message;

extern IntSetting* status_mask;

extern IntSetting* sd_fallback_cs;

extern EnumSetting* message_level;

extern EnumSetting* gcode_echo;

// Sand-table homing mode (see Protocol.cpp).  "sensor" = the normal FluidNC
// limit-switch $H cycle; "crash" = drive the rho (Y) carriage blindly into its
// physical centre stop (no switch, no stall detection) then zero theta + rho.
enum HomingMode {
    HomingSensor = 0,
    HomingCrash  = 1,
};
extern EnumSetting* homing_mode;

// Theta zero offset in DEGREES (UI: "Sensor Offset").  After homing, pattern
// theta=0 is placed this many degrees from the home reference (the limit switch
// in sensor mode, the crash position in crash mode).  Applied by both modes.
extern IntSetting* theta_offset;

// Sand API password.  Empty (default) = everything open, as before.  Non-empty
// locks the network control surfaces: HTTP control routes require it
// (?key=<pw> or X-Sand-Key header -> else 401), telnet refuses clients, and
// ArduinoOTA (port 3232) requires it.  Reads (/sand_status, /sand_patterns,
// logs) stay open.  Serial is never gated (physical access = trusted), so a
// lost password is cleared over USB with $Sand/Password=.
extern StringSetting* sand_password;
