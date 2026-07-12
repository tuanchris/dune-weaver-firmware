// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

/*
  Pure parsing and policy helpers for the Playlist module, separated
  from file I/O and machine state so they can be unit-tested in the
  native test environment.
*/

#include <cstdint>
#include <string>
#include <vector>

namespace PlaylistParse {

    // Clear-pattern modes, the values of the $Playlist/ClearPattern setting
    constexpr int CLEAR_NONE     = 0;
    constexpr int CLEAR_ADAPTIVE = 1;
    constexpr int CLEAR_IN       = 2;
    constexpr int CLEAR_OUT      = 3;
    constexpr int CLEAR_SIDEWAY  = 4;
    constexpr int CLEAR_RANDOM   = 5;

    // Which clear file the policy chose
    enum class Clear : uint8_t {
        None,
        FromIn,
        FromOut,
        Sideway,
    };

    // Parse playlist file content: one SD-relative pattern path per line,
    // '#' starts a comment, surrounding whitespace is trimmed, a leading
    // '/' is added when missing.  At most max_items entries are returned.
    std::vector<std::string> parse_playlist(const std::string& content, size_t max_items);

    // First rho of a .thr pattern, skipping the up-to-two "0 0" preamble
    // pairs that many community patterns start with (the same convention
    // as the ThetaRho kinematics and dune-weaver).  Returns -1.0 when no
    // coordinates are found.
    float first_rho(const std::string& content);

    // Clear-pattern policy.  first_rho is the upcoming pattern's first
    // rho (or negative when unknown); rnd supplies the randomness for
    // CLEAR_RANDOM and the unknown-rho fallback.  A pattern starting
    // near the center is preceded by a clear that ends at the center
    // (clear-from-out), and vice versa.
    Clear choose_clear(int clear_mode, float first_rho, uint32_t rnd);

    // Map a clear-mode name ("none"|"adaptive"|"in"|"out"|"sideway"|"side"
    // |"random", case-insensitive; "side" is an alias for "sideway") to its
    // CLEAR_* value.  Returns false (leaving mode untouched) for an empty or
    // unrecognized name so callers can report a usage error.
    bool parse_clear_mode(const char* name, int& mode);

    // Split a $Sand/Run value into the pattern path and optional clear mode.
    // Filenames may contain spaces ("/patterns/13b Battlesbury (C C).thr"),
    // so the path is NOT the first token: the optional "clear=<mode>" must be
    // the LAST whitespace-delimited token, and everything before it, trimmed,
    // is the path.  Returns false when no path remains (empty value, or a
    // bare "clear=<mode>").  clear_mode is left empty when the token is
    // absent; its value is not validated here (see parse_clear_mode).
    bool parse_run_args(const std::string& value, std::string& path, std::string& clear_mode);
}
