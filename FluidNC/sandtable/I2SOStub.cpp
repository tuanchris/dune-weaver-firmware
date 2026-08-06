// I2S shift-register output stubs for boards that do not have one.
//
// The MKS DLC32 (ESP32) drives its steppers through a 74HC595 fed by the I2S
// peripheral, so i2so.N pins are real there. The MKS DLC32 MAX (ESP32-S3)
// wires step/dir directly to GPIOs and has no shift register, and the ESP32-S3
// I2S peripheral has a different register layout than esp32/i2s_engine.c
// expects - so [env:sandtable_s3] leaves that engine out of the build.
//
// Three call sites reference the I2SO machinery unconditionally (Main.cpp's
// I2SOBus::init, Pin.cpp's i2so pin parsing, MachineConfig.cpp's i2so config
// section), so dropping the engine alone does not link. These stubs satisfy
// them. i2s_out_init() returning failure is what makes an i2so config on such
// a board fail visibly rather than silently doing nothing.
//
// Only compiled into builds whose src filter excludes esp32/i2s_engine.c;
// on the ESP32 the real engine provides these symbols instead.
#include <sdkconfig.h>  // CONFIG_IDF_TARGET_*

#include "Driver/i2s_out.h"

#ifndef CONFIG_IDF_TARGET_ESP32

extern "C" {

int i2s_out_init(i2s_out_init_t* init_param) {
    return -1;
}

uint8_t i2s_out_read(pinnum_t pin) {
    return 0;
}

void i2s_out_write(pinnum_t pin, uint8_t val) {}

void i2s_out_delay() {}
}

#endif
