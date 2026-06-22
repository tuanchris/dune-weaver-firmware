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
