// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#pragma once

/*
  ThetaRho is a kinematic system for polar sand tables such as the
  Dune Weaver, where one motor spins an arm (theta) and a second,
  mechanically coupled motor moves the ball along the arm (rho).

  The G-code coordinate space is (theta, rho):
    X = theta in radians, unbounded; it grows with each revolution
    Y = rho, normalized 0 (center) .. 1 (perimeter)

  Motor space:
    X motor mm = theta * theta_mm_per_rev / 2pi
    Y motor mm = rho * rho_mm + coupling * X motor mm

  The coupling term compensates for the rho carriage being referenced
  to the rotating arm: turning theta drags rho along by 1/gear_ratio
  of the theta motor's rotation.  In motor-mm terms,
    coupling = x_steps_per_mm / (gear_ratio * y_steps_per_mm)

  Lines arriving from a file whose name ends in .thr are translated on
  the fly:
    "# comment"      -> ignored
    "<theta> <rho>"  -> G90 G1 X<theta> Y<rho>
  so theta-rho pattern files can be run directly with $SD/Run or
  $LocalFS/Run, with no host-side conversion.  At the start of each
  .thr job the machine theta is relabeled into [0, 2pi) and the
  pattern's leading whole revolutions are removed, so a pattern never
  starts by unwinding accumulated turns.
*/

#include "Cartesian.h"
#include "ThrTranslator.h"

#include <string>

class IntSetting;

namespace Kinematics {
    class ThetaRho : public Cartesian {
    public:
        ThetaRho(const char* name) : Cartesian(name) {}

        ThetaRho(const ThetaRho&)            = delete;
        ThetaRho(ThetaRho&&)                 = delete;
        ThetaRho& operator=(const ThetaRho&) = delete;
        ThetaRho& operator=(ThetaRho&&)      = delete;

        // Kinematic Interface
        void init() override;
        bool cartesian_to_motors(float* target, plan_line_data_t* pl_data, float* position) override;
        void motors_to_cartesian(float* cartesian, float* motors, int n_axis) override;
        bool transform_cartesian_to_motors(float* motors, float* cartesian) override;

        bool translate_line(char* line, size_t maxlen, Channel& channel) override;
        void stop() override;

        // Configuration handlers
        void group(Configuration::HandlerBase& handler) override;

        // Live base feed (mm/min) for the running pattern, usable mid-motion
        // (unlike the idle-gated $THR/Feed setting).  When idle this persists
        // to $THR/Feed; while moving it is an in-memory override applied on the
        // next move.  It persists across patterns (so a speed set mid-playlist
        // holds for the whole playlist) and is cleared by clearFeedLive() when
        // the run ends.  Backs /sand_feed?mm=.  Static + null-safe.
        static Error setFeedLive(int mm_per_min);
        static void  clearFeedLive();  // drop the override back to $THR/Feed (run ended)
        static int   effectiveFeed();  // live override or the persisted setting; -1 if no module

        // Current ball angle as a fraction of a full turn, [0,1).  Lets the
        // LED "ball" effect track the ball's angular position.  -1 if no
        // ThetaRho kinematics is configured.
        static float ballAngle();

        ~ThetaRho() {}

    private:
        void start_job(const std::string& name, const Channel* channel);
        void end_job();
        void normalize_theta();

        // Configuration
        float _theta_mm_per_rev = 50.0f;   // X motor mm per full theta revolution
        float _rho_mm           = 20.0f;   // Y motor mm for rho 0 -> 1
        float _gear_ratio       = 10.0f;   // theta:rho mechanical coupling ratio; 0 disables
        bool  _invert_coupling  = false;
        float _default_feed     = 100.0f;  // initial value for the $THR/Feed setting; 0 = use modal F

        // Derived
        float _theta_scale = 0.0f;  // X motor mm per radian
        float _coupling    = 0.0f;  // Y motor mm per X motor mm, sign included

        // $THR/Feed: live, NVS-persisted feed for .thr jobs (mm/min in
        // motor space); read per line so changes apply mid-pattern
        IntSetting* _feed_setting = nullptr;

        // In-memory feed override set via /sand_feed?mm while running (no flash
        // write mid-motion); -1 = unset.  Cleared in end_job().
        static ThetaRho* _instance;
        static int       _live_feed;

        // Per-job translation state
        std::string    _job_name;                // channel name of the active .thr job
        const Channel* _job_channel = nullptr;   // identity of the job's channel, never dereferenced
        ThrTranslator  _translator;              // line translation (preamble, offset, feed injection)
    };
}
