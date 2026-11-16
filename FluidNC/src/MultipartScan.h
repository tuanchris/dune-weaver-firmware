// Copyright (c) 2026 - Dune Weaver
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

// Copying one multipart/form-data file part to its handler in blocks: the web
// server's upload parser (libraries/WebServer/src/Parsing.cpp) uses copyPart().
// The stock parser read the body one client.read() per byte, with a delay(2)
// whenever the socket ran dry; reading in blocks took ~2.5 s off a 1 MB upload
// on a DW Gold (2026-11-15). Header-only and std-only, so FluidNC/tests covers
// the exact loop the firmware runs.

#include <cstddef>
#include <cstring>

namespace MultipartScan {
    // Offset of the first pat[0, plen) in buf[0, len), or -1.
    inline long find(const unsigned char* buf, size_t len, const char* pat, size_t plen) {
        if (plen == 0 || len < plen) {
            return -1;
        }
        for (size_t i = 0; i + plen <= len;) {
            auto hit = static_cast<const unsigned char*>(memchr(buf + i, static_cast<unsigned char>(pat[0]), len - plen + 1 - i));
            if (!hit) {
                return -1;
            }
            i = static_cast<size_t>(hit - buf);
            if (memcmp(hit, pat, plen) == 0) {
                return static_cast<long>(i);
            }
            ++i;
        }
        return -1;
    }

    // Copy a file part's data, from buf[0, have) (bytes already read) on, to
    // flush(n) (which consumes buf[0, n)) until the boundary pat[0, plen) (the
    // part's "\r\n--<boundary>"), reading more with read(dst, max) -> bytes
    // read (> 0), or 0 / < 0 when the client has gone. buf holds cap >= plen
    // bytes. flush only sees a full buffer's worth (less the last plen - 1
    // bytes, which may start the boundary) or the part's final bytes, so the
    // SD sees big writes; with `align`, a full buffer's flush is rounded down to
    // a multiple of it (the FileStream's 4096-byte stdio buffer: an 8176-byte
    // write became two 4 KB SD writes, an 8192-byte one goes out in one, which
    // was ~1.5 s per MB on a DW Gold). Returns how many bytes followed the boundary: they
    // are moved to buf[0, n) and belong to the rest of the form. -1 = the
    // client went away first.
    template <class Read, class Flush>
    long copyPart(unsigned char* buf, size_t cap, size_t have, const char* pat, size_t plen, Read read, Flush flush, size_t align = 1) {
        size_t scanned = 0;  // buf[0, scanned) holds no boundary start
        while (true) {
            // Search from where a boundary could still begin.
            size_t from = scanned > plen - 1 ? scanned - (plen - 1) : 0;
            long   at   = find(buf + from, have - from, pat, plen);
            if (at >= 0) {
                size_t end = from + static_cast<size_t>(at);
                if (end) {
                    flush(end);
                }
                size_t rest = have - (end + plen);
                memmove(buf, buf + end + plen, rest);
                return static_cast<long>(rest);
            }
            scanned = have;
            if (have == cap) {
                // Full and no boundary: hand on all but a possible boundary start.
                size_t out = cap - (plen - 1);
                if (align > 1 && out >= align) {
                    out -= out % align;
                }
                flush(out);
                memmove(buf, buf + out, cap - out);
                have = scanned = cap - out;
            }
            long got = read(buf + have, cap - have);
            if (got <= 0) {
                return -1;
            }
            have += static_cast<size_t>(got);
        }
    }
}
