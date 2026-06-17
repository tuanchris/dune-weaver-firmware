// Copyright (c) 2021 -	Mitch Bradley
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "InputFile.h"

#include "Report.h"
#include "Planner.h"                 // plan_get_block_buffer_available()
#include "Machine/MachineConfig.h"   // config->_planner_blocks
#include "SandStatus.h"              // executed_percent()

InputFile::InputFile(const char* defaultFs, const char* path) : FileStream(path, "r", defaultFs) {}
/*
  Read a line from the file
  Returns Error::Ok if a line was read, even if the line was empty.
  Returns Error::EOF on end of file.
  Returns other Error code on error, after displaying a message.
*/
Error InputFile::readLine(char* line, int maxlen) {
    int len = 0;
    int c;
    while ((c = read()) >= 0) {
        if (len >= maxlen) {
            return Error::LineLengthExceeded;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            ++_line_number;
            if (len == 0) {
                ++_blank_lines;
            }
            break;
        }
        line[len++] = c;
    }
    line[len] = '\0';
    return len || c >= 0 ? Error::Ok : Error::Eof;
}

void InputFile::ack(Error status) {
    if (status != Error::Ok) {
        log_error(static_cast<int>(status) << " (" << errorString(status) << ") in " << name() << " at line " << lineNumber());
        if (status != Error::GcodeUnsupportedCommand) {
            // Do not stop on unsupported commands because most senders do not stop.
            // Stop the file job on other errors
            notifyf("File job error", "Error:%d in %s at line: %d", status, name(), lineNumber());
            _pending_error == status;
        }
    }
}

#include <sstream>
#include <iomanip>

void InputFile::end_message() {
    _progress = "SD: ";
    _progress += name();
    _progress += ": Sent";
}

Error InputFile::pollLine(char* line) {
    // File input never returns realtime characters, so we do nothing
    // if line is null.
    if (!line) {
        return Error::NoData;
    }
    if (_pending_error != Error::Ok) {
        return _pending_error;
    }
    if (_percent) {
        _percent = false;
        // If the first non-blank line in the file is a % line, it denotes start-of-file.
        // Otherwise a % line causes the rest of the file to be skipped, per
        // https://linuxcnc.org/docs/html/gcode/overview.html#gcode:file-requirements
        // The line with % is not blank, so if it is the first non-blank line
        // _line_number will be one more than _blank_lines
        if (_line_number != (_blank_lines + 1)) {
            _ended = true;
        }
    }
    if (_ended) {
        end_message();
        return Error::Eof;
    }
    switch (auto err = readLine(line, Channel::maxLine)) {
        case Error::Ok: {
            // Report progress against the move being EXECUTED, not the one just
            // read: the reader runs ahead of the table by the moves still queued
            // in the planner.  plan_get_block_buffer_available() returns the free
            // slots; capacity is planner_blocks-1 (one slot is always reserved,
            // see Planner.cpp), so queued = (planner_blocks-1) - free.
            size_t cap     = config && config->_planner_blocks ? config->_planner_blocks - 1 : 0;
            size_t avail   = plan_get_block_buffer_available();
            size_t queued  = cap > avail ? cap - avail : 0;
            float percent_complete = SandStatus::executed_percent(position(), size(), _line_number, queued);

            std::ostringstream s;
            s << "SD:" << std::fixed << std::setprecision(2) << percent_complete << "," << path().c_str();
            _progress = s.str();
        }
            return Error::Ok;
        case Error::Eof:
            end_message();
            return Error::Eof;
        default:
            _progress = "";
            return err;
    }
}

InputFile::~InputFile() {}
