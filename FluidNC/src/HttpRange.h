// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

#include <cstddef>

/*
  Pure parsing of the HTTP Range request header, separated from the web
  server so it can be unit-tested in the native test environment
  (HttpRangeTest.cpp).

  Ranged GETs exist so a client syncing a multi-MB file off the SD card
  (the app's preview-bundle shards) can pull it in bounded chunks instead
  of one monolithic response: the single-threaded web server is deaf for
  the whole duration of a transfer, so every bounded chunk is a gap where
  queued clients (status pollers, HA) get served instead of stacking up
  ~2 KB of lwIP buffers each.

  Only the single-range "bytes=" forms are honored ("a-b", "a-", "-n").
  Anything else -- absent header, malformed value, multi-range -- yields
  None, i.e. "serve the whole file", which is always a correct response
  to a Range request.
*/
namespace HttpRange {
    enum class Result {
        None,           // serve the whole file with 200
        Partial,        // serve [start, start+len) with 206
        Unsatisfiable,  // answer 416 with "Content-Range: bytes */size"
    };

    // value is the raw header value (e.g. "bytes=0-1023"); may be null/empty.
    // On Partial, start/len describe the byte window to serve.
    Result parse(const char* value, size_t fileSize, size_t& start, size_t& len);
}
