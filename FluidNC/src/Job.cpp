// Copyright (c) 2024 - Mitch Bradley
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "Job.h"
#include "Protocol.h"  // drain_messages()
#include <map>
#include <stack>
#include <mutex>

std::stack<JobSource*> job;

// The job stack is mutated by the main loop task ($SD/Run -> nest/save) and
// read+popped by the poller task (channel polling, EOF unnest/abort, and the
// /sand_status web read).  Without serialization a concurrent push vs. top/pop
// corrupts the underlying std::deque and panics (StoreProhibited).  Recursive
// because unnest()/abort() call pop()/active() while already holding the lock.
static std::recursive_mutex job_mtx;

Channel* Job::leader = nullptr;

bool Job::active() {
    std::lock_guard<std::recursive_mutex> lock(job_mtx);
    return !job.empty();
}

JobSource* Job::source() {
    std::lock_guard<std::recursive_mutex> lock(job_mtx);
    return job.empty() ? nullptr : job.top();
}

// save() and restore() are use to close/reopen an SD file atop the job stack
// before trying to open a nested SD file.  The reason for that is because
// the number of simultaneously-open SD files is limited to conserve RAM.
void Job::save() {
    std::lock_guard<std::recursive_mutex> lock(job_mtx);
    if (active()) {
        job.top()->save();
    }
}
void Job::restore() {
    std::lock_guard<std::recursive_mutex> lock(job_mtx);
    if (active()) {
        job.top()->restore();
    }
}
void Job::nest(Channel* in_channel, Channel* out_channel) {
    std::lock_guard<std::recursive_mutex> lock(job_mtx);
    auto source = new JobSource(in_channel);
    if (out_channel && job.empty()) {
        leader = out_channel;
    }
    job.push(source);
}
void Job::pop() {
    std::lock_guard<std::recursive_mutex> lock(job_mtx);
    auto source = job.top();
    job.pop();
    // The async log queue (drained by output_loop) holds raw Channel pointers.
    // A job's InputFile IS a Channel, so destroying it while a message that
    // references it is still queued makes output_loop call a method on freed
    // memory -> __cxa_pure_virtual -> abort() (seen on $Playlist/Stop during a
    // clear, where the abort path logs while tearing the job down).  Flush the
    // queue first so no pending message points at the channel we delete.
    drain_messages();
    delete source;
    if (!active()) {
        leader = nullptr;
    }
}
void Job::unnest() {
    std::lock_guard<std::recursive_mutex> lock(job_mtx);
    if (active()) {
        pop();
        restore();
    }
}

void Job::abort() {
    std::lock_guard<std::recursive_mutex> lock(job_mtx);
    // Kill all active jobs
    while (active()) {
        pop();
    }
}

bool Job::get_param(const std::string& name, float& value) {
    std::lock_guard<std::recursive_mutex> lock(job_mtx);
    return job.top()->get_param(name, value);
}
bool Job::set_param(const std::string& name, float value) {
    std::lock_guard<std::recursive_mutex> lock(job_mtx);
    return job.top()->set_param(name, value);
}
bool Job::param_exists(const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(job_mtx);
    return job.top()->param_exists(name);
}
Channel* Job::channel() {
    std::lock_guard<std::recursive_mutex> lock(job_mtx);
    return job.top()->channel();
}
