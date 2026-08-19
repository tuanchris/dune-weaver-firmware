// Copyright (c) 2021 -  Stefan de Bruijn
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "Config.h"
#include "Protocol.h"
#include "Serial.h"
#include "SettingsDefinitions.h"
#include "Channel.h"

const EnumItem messageLevels2[] = { { MsgLevelNone, "None" }, { MsgLevelError, "Error" }, { MsgLevelWarning, "Warn" },
                                    { MsgLevelInfo, "Info" }, { MsgLevelDebug, "Debug" }, { MsgLevelVerbose, "Verbose" },
                                    EnumItem(MsgLevelNone) };

bool atMsgLevel(MsgLevel level) {
    return message_level == nullptr || message_level->get() >= level;
}

LogStream::LogStream(Channel& channel, MsgLevel level) : _channel(channel), _level(level) {
    // nothrow: logging must never be the thing that panics the board.  Under a
    // heap crater the old throwing `new` turned a caught std::bad_alloc back
    // into an uncaught one the instant any catch handler tried to log it (the
    // Playlist load handlers did exactly that).  A null _line makes write() and
    // the dtor no-ops, so the message is dropped instead of aborting.
    _line = new (std::nothrow) std::string();
}

LogStream::LogStream(Channel& channel, MsgLevel level, const char* name) : LogStream(channel, level) {
    print(name);
}

LogStream::LogStream(Channel& channel, const char* name) : LogStream(channel, MsgLevelNone, name) {}
LogStream::LogStream(MsgLevel level, const char* name) : LogStream(allChannels, level, name) {}

size_t LogStream::write(uint8_t c) {
    if (_line) {
        // The append itself can throw bad_alloc while growing under low heap.
        // Swallow it, drop the rest of the message, and never propagate -- a
        // dropped log line is always preferable to abort().
        try {
            *_line += (char)c;
        } catch (...) {
            delete _line;
            _line = nullptr;
        }
    }
    return 1;
}

LogStream::~LogStream() {
    if (!_line) {
        return;  // message was dropped under low heap; see the ctor and write()
    }
    if (_line->length() && (*_line)[0] == '[') {
        try {
            *_line += ']';
        } catch (...) {}
    }
    _channel.sendLine(_level, _line);  // takes ownership of _line
}
