// Copyright (c) 2026 - FluidNC
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "ThetaRho.h"

#include "src/Machine/MachineConfig.h"
#include "src/System.h"    // get_mpos, set_motor_steps_from_mpos
#include "src/Planner.h"   // plan_sync_position
#include "src/GCode.h"     // gc_sync_position
#include "src/Protocol.h"  // protocol_buffer_synchronize
#include "src/Channel.h"
#include "src/Job.h"
#include "src/Settings.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Kinematics {
    namespace {
        constexpr float TWO_PI = 2.0f * float(M_PI);
    }

    void ThetaRho::group(Configuration::HandlerBase& handler) {
        handler.item("theta_mm_per_rev", _theta_mm_per_rev, 0.1f, 1000.0f);
        handler.item("rho_mm", _rho_mm, 0.1f, 1000.0f);
        handler.item("gear_ratio", _gear_ratio, 0.0f, 1000.0f);
        handler.item("invert_coupling", _invert_coupling);
        handler.item("default_feed_mm_per_min", _default_feed, 0.0f, 100000.0f);
    }

    void ThetaRho::init() {
        log_info("Kinematic system: " << name());

        _theta_scale = _theta_mm_per_rev / TWO_PI;

        _coupling = 0.0f;
        if (_gear_ratio != 0.0f) {
            auto  axes   = config->_axes;
            float xsteps = axes->_axis[X_AXIS]->_stepsPerMm;
            float ysteps = axes->_axis[Y_AXIS]->_stepsPerMm;
            if (ysteps != 0.0f) {
                _coupling = xsteps / (_gear_ratio * ysteps);
                if (_invert_coupling) {
                    _coupling = -_coupling;
                }
            }
        }

        log_info("  theta " << _theta_mm_per_rev << " mm/rev, rho " << _rho_mm << " mm, coupling " << _coupling);

        if (!_feed_setting) {
            _feed_setting = new IntSetting(
                "Feed for .thr jobs, motor mm/min", EXTENDED, WG, NULL, "THR/Feed", static_cast<int>(_default_feed), 0, 100000);
        }

        init_position();
    }

    bool ThetaRho::transform_cartesian_to_motors(float* motors, float* cartesian) {
        motors[X_AXIS] = cartesian[X_AXIS] * _theta_scale;
        motors[Y_AXIS] = cartesian[Y_AXIS] * _rho_mm + motors[X_AXIS] * _coupling;

        auto n_axis = Axes::_numberAxis;
        for (size_t axis = Z_AXIS; axis < n_axis; axis++) {
            motors[axis] = cartesian[axis];
        }
        return true;
    }

    void ThetaRho::motors_to_cartesian(float* cartesian, float* motors, int n_axis) {
        cartesian[X_AXIS] = motors[X_AXIS] / _theta_scale;
        cartesian[Y_AXIS] = (motors[Y_AXIS] - motors[X_AXIS] * _coupling) / _rho_mm;

        for (int axis = Z_AXIS; axis < n_axis; axis++) {
            cartesian[axis] = motors[axis];
        }
    }

    bool ThetaRho::cartesian_to_motors(float* target, plan_line_data_t* pl_data, float* position) {
        // The transform is linear, so a straight line in (theta, rho) space is
        // a straight line in motor space and needs no segmentation.  The feed
        // rate passes through unchanged: F is interpreted as motor-space
        // mm/min, the same meaning as the F values the dune-weaver host
        // software sends, so familiar speed numbers behave the same here.
        float motors[MAX_N_AXIS];
        transform_cartesian_to_motors(motors, target);
        return mc_move_motors(motors, pl_data);
    }

    // Relabel the motor counters so the current theta reads in [0, 2pi),
    // without moving the machine and without changing rho.  Keeps theta from
    // accumulating revolutions across patterns.
    void ThetaRho::normalize_theta() {
        protocol_buffer_synchronize();  // let any buffered motion finish first

        float mpos[MAX_N_AXIS];
        copyAxes(mpos, get_mpos());

        float theta = mpos[X_AXIS];
        float norm  = positive_mod_2pi(theta);
        if (fabsf(theta - norm) < 0.001f) {
            return;
        }

        mpos[X_AXIS] = norm;
        set_motor_steps_from_mpos(mpos);  // applies the coupling, so rho is preserved
        plan_sync_position();
        gc_sync_position();

        log_info("ThetaRho: theta " << theta << " rad relabeled to " << norm << " rad");
    }

    void ThetaRho::start_job(const std::string& name, const Channel* channel) {
        _job_name    = name;
        _job_channel = channel;
        _translator.start(_feed_setting ? static_cast<float>(_feed_setting->get()) : _default_feed);
        normalize_theta();
        log_info("ThetaRho: running theta-rho job " << name);
    }

    void ThetaRho::end_job() {
        _job_name.clear();
        _job_channel = nullptr;
        normalize_theta();
    }

    // Called for every command line before parsing.  Returns true when the
    // line was fully consumed.  For "theta rho" pairs the line is rewritten
    // in place (capacity maxlen) and false is returned so the G-code parser
    // executes the rewritten line.
    bool ThetaRho::translate_line(char* line, size_t maxlen, Channel& channel) {
        const std::string& source = channel.name();

        bool is_thr = source.size() > 4 && strcasecmp(source.c_str() + source.size() - 4, ".thr") == 0;

        if (!is_thr) {
            // The first command from another channel after a .thr job has
            // finished (e.g. the next $SD/Run) closes out the job.
            if (!_job_name.empty() && !(Job::active() && Job::channel() == _job_channel)) {
                end_job();
            }
            return false;
        }

        if (source != _job_name) {
            start_job(source, &channel);
        }

        bool was_locked = _translator.offset_locked();

        if (_feed_setting) {
            // Live feed: a changed $THR/Feed takes effect on the next move
            _translator.set_feed(static_cast<float>(_feed_setting->get()));
        }
        switch (_translator.translate(line, line, maxlen)) {
            case ThrLine::Skip:
                return true;
            case ThrLine::Invalid:
                log_warn("ThetaRho: skipping invalid .thr line: " << line);
                return true;
            case ThrLine::Oversize:
                log_warn("ThetaRho: skipping oversized .thr line: " << line);
                return true;
            case ThrLine::Move:
                break;
        }
        if (!was_locked && _translator.offset_locked() && _translator.theta_offset() != 0.0f) {
            log_info("ThetaRho: pattern theta normalized by " << _translator.theta_offset() << " rad");
        }
        return false;
    }

    // Configuration registration
    namespace {
        KinematicsFactory::InstanceBuilder<ThetaRho> registration("ThetaRho");
    }
}
