// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

// Stub for the sandtable build, which excludes WebUI/NotificationsService.cpp.
// The notification service (Pushover/email/Line/Telegram) is the only user of
// WiFiClientSecure and therefore the only thing that links the ~160KB mbedTLS
// stack into the image.  Report.cpp's notify() is WEAK_LINK with a no-op
// default; these two statics are the only other external references.
// This directory is outside the default build_src_filter, so only the
// sandtable environment (which adds +<sandtable/>) compiles it.

#include "src/WebUI/NotificationsService.h"

namespace WebUI {
    bool NotificationsService::started() {
        return false;
    }
    const char* NotificationsService::getTypeString() {
        return "None";
    }
}
