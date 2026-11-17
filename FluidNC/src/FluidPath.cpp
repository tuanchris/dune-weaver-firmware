// Copyright (c) 2022 Mitch Bradley
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "FluidPath.h"
#include "Driver/sdspi.h"
#include "Config.h"
#include "Error.h"
#include "Machine/MachineConfig.h"
#include "FluidError.hpp"
#include "HashFS.h"

int  FluidPath::_refcnt = 0;
bool FluidPath::_pinned = false;

void FluidPath::unpinSd() {
    if (_pinned) {
        _pinned = false;
        log_warn("SD: I/O error; the card is remounted on next use");
        if (_refcnt && --_refcnt == 0) {
            sd_unmount();
        }
    }
}

FluidPath::FluidPath(const char* name, const char* fs, std::error_code* ecptr) : std::filesystem::path(canonicalPath(name, fs)) {
    // canonicalPath() can return an empty or root-only path -- e.g. it refuses
    // an over-long input (a scanner's long-URL probe), returning "".  Such a
    // path has no mount component, so *(++begin()) below would dereference the
    // path iterator PAST end() -> LoadProhibited (a hardware fault, NOT a C++
    // exception, so no try/catch upstream can save it: this is a real reboot).
    // Reject it as an invalid, non-SD, non-openable path instead.
    auto it = begin();
    if (it == end() || ++it == end()) {
        if (ecptr) {
            *ecptr = std::make_error_code(std::errc::invalid_argument);
        }
        return;  // _isSD stays false; ~FluidPath() and callers treat this as "not found"
    }
    auto mount = *it;  // the first component after the leading '/' is the mount name
    _isSD      = mount == "sd";

    if (_isSD) {
        if (!config->_sdCard->config_ok) {
            std::error_code ec = FluidError::SDNotConfigured;
            if (ecptr) {
                *ecptr = ec;
                return;
            }
            throw stdfs::filesystem_error { "SD card is inaccessible", name, ec };
        }
        if (_refcnt == 0) {
            // Allow several concurrently-open SD files.  The default of 1 means a
            // running .thr job holds the only descriptor, so any other SD open
            // (serving the /patterns/index.json manifest, a /sd file fetch, the
            // adaptive-clear firstRho peek) fails with "no free file descriptors"
            // and the retry storm wedges the board.  Headroom: job + manifest +
            // a file fetch + spare.
            auto ec = sd_mount(5);
            if (ec) {
                if (ecptr) {
                    *ecptr = ec;
                    return;
                }
                throw stdfs::filesystem_error { "SD card is inaccessible", name, ec };
            }
            // DW fork: keep the card mounted from here on.  A mount allocates
            // ~27 KB in ONE block (5 FILs x 4 KB sector buffers + the FATFS
            // window), and unmounting after every job meant remounting on a
            // fragmented heap: once the largest free block fell under that, no
            // pattern could start until a reboot, with 70 KB free.  The first
            // mount happens at boot (the SD check), on a clean heap.
            // FileStream::setup() calls unpinSd() on an I/O error, so a
            // glitched or swapped card is still re-probed on the next use.
            _pinned = true;
            ++_refcnt;
        }
        ++_refcnt;
    }
    // log_debug("construct " << _isSD << " " << _refcnt);
}

FluidPath::FluidPath(const FluidPath& o) : path(o), _isSD(o._isSD) {
    if (this != &o && _isSD) {
        ++_refcnt;
    }
    // log_debug("path construct " << _isSD << " " << _refcnt);
}

FluidPath::FluidPath(FluidPath&& o) : path(std::move(o)), _isSD(o._isSD) {
    if (this != &o) {
        // After a move, the other object is dead so we do not want
        // to decrement the refcount on destruction
        o._isSD = false;
    }
    // log_debug(" move construct " << _isSD << " " << _refcnt);
}

FluidPath& FluidPath::operator=(const FluidPath& o) {
    stdfs::path::operator=(o);

    _isSD = o._isSD;
    if (&o != this && _isSD) {
        ++_refcnt;
    }
    // log_debug(" copy assign " << _isSD << " " << _refcnt);
    return *this;
}

FluidPath& FluidPath::operator=(FluidPath&& o) {
    std::swap(_isSD, o._isSD);
    stdfs::path::operator=(std::move(o));
    // log_debug(" move assign " << _isSD << " " << _refcnt);
    return *this;
}

FluidPath::~FluidPath() {
    // log_debug("~ refcnt " << _isSD << " " << _refcnt);
    if (_isSD && (_refcnt && --_refcnt == 0)) {
        sd_unmount();
    }
}
