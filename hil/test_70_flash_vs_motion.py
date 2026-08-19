# Regression for the 2026-08-19 cache panic: an internal-flash (NVS) write
# concurrent with stepping used to kill the board with "Guru Meditation ...
# Cache disabled but cached memory region accessed" (PC 0xbad00bad, backtrace
# CORRUPTED).  The step ISRs stay enabled during flash writes
# (ESP_INTR_FLAG_IRAM) and Stepper::pulse_func reached flash-resident memory
# (the spindle vtable, state_is), so any settings write that landed while the
# motors still stepped was fatal.  Two vectors, both exercised here:
#
#  * holdOk settings during the feed-hold DECEL RAMP: sys.state reads Hold
#    immediately, but the motors are still decelerating.  check_state now
#    also requires !Stepper::isStepping(), so the write is either accepted
#    (hold complete) or cleanly rejected (error:8) -- never a panic.
#  * the Run->Idle blip when a new $Sand/Run aborts the current one:
#    Leds::flushLive / TimePersist fire their deferred NVS writes on the
#    poller the instant state touches Idle, racing the next job's motion
#    start.  With the ISR chain IRAM/DRAM-clean this is harmless.
#
# Writes MUST alternate values: a same-value write skips NVS entirely and
# proves nothing.  Needs HIL_MOTION=1 and patterns on the SD card.
import json
import time

import pytest

from conftest import motion

PANIC_MARKS = ("Guru Meditation", "MSG:RST", "Skipping configuration")


def _first_patterns(board, n=2):
    text, status = board.cmd("$Sand/Patterns", timeout=10.0)
    if status != "ok":
        pytest.skip("no pattern manifest (SD missing?)")
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("["):
            names = json.loads(s)
            if len(names) >= n:
                return [f"/patterns/{x}" for x in names[:n]]
    pytest.skip("fewer than 2 patterns on SD")


def _read_setting(board, name):
    text, status = board.cmd(f"${name}", timeout=5.0)
    assert status == "ok", f"cannot read {name}: {text}"
    for line in text.splitlines():
        if "=" in line and name.split("/")[-1] in line:
            return line.rsplit("=", 1)[1].strip()
    return None


def _assert_alive(board, seen_text):
    for mark in PANIC_MARKS:
        assert mark not in seen_text, f"board panicked/rebooted:\n{seen_text[-2000:]}"
    _, status = board.cmd("$I", timeout=5.0)
    assert status == "ok", "board unresponsive after flash-vs-motion stress"


def _abort_job(board):
    board.ser.write(b"\x18")  # realtime reset: cancel any job
    time.sleep(1.5)
    board.drain(quiet=0.5, limit=5.0)


@motion
def test_holdok_write_into_decel_ramp(board):
    if board.state() != "Idle":
        pytest.skip("machine not idle; home or unlock first")
    pattern = _first_patterns(board, 1)[0]
    prev_pause = _read_setting(board, "Playlist/PauseTime")
    prev_bright = _read_setting(board, "LED/Brightness")

    seen = ""
    try:
        text, status = board.cmd(f"$Sand/Run={pattern}", timeout=10.0)
        assert status == "ok", f"pattern did not start: {text}"
        assert board.wait_state("Run", 60), "pattern never reached Run"
        for i in range(12):
            time.sleep(1.0 + (i % 3) * 0.4)
            if board.state() == "Idle":  # pattern finished early; restart
                board.cmd(f"$Sand/Run={pattern}", timeout=10.0)
                board.wait_state("Run", 60)
            board.ser.write(b"!")  # feed hold; decel ramp begins
            # Alternating values so every accepted write really hits NVS.
            t1, s1 = board.cmd(f"$Playlist/PauseTime={1799 + i % 2}", timeout=5.0)
            t2, s2 = board.cmd(f"$LED/Brightness={40 + i % 2}", timeout=5.0)
            seen += t1 + t2
            # ok (hold complete) or a clean idle-gate rejection -- never death
            assert s1 in ("ok", "error:8"), f"unexpected: {s1}\n{t1}"
            assert s2 in ("ok", "error:8"), f"unexpected: {s2}\n{t2}"
            board.ser.write(b"~")  # resume
            time.sleep(0.6)
        _assert_alive(board, seen)
    finally:
        _abort_job(board)
        if prev_pause is not None:
            board.cmd(f"$Playlist/PauseTime={prev_pause}")
        if prev_bright is not None:
            board.cmd(f"$LED/Brightness={prev_bright}")


@motion
def test_deferred_persists_race_next_job_start(board):
    if board.state() != "Idle":
        pytest.skip("machine not idle; home or unlock first")
    patterns = _first_patterns(board, 2)
    prev_effect = _read_setting(board, "LED/Effect")
    prev_bright = _read_setting(board, "LED/Brightness")

    seen = ""
    try:
        board.cmd(f"$Sand/Run={patterns[0]}", timeout=10.0)
        board.wait_state("Run", 60)
        for i in range(8):
            time.sleep(1.2)
            if board.state() == "Idle":
                board.cmd(f"$Sand/Run={patterns[i % 2]}", timeout=10.0)
                board.wait_state("Run", 60)
            # Park live LED overrides mid-run (in-memory only)...
            effect = ("rainbow", "fire")[i % 2]
            t1, _ = board.cmd(f"$Sand/Led=effect={effect} brightness={40 + i % 2}", timeout=5.0)
            seen += t1
            time.sleep(0.3)
            # ...then abort into a new run: the Idle blip flushes them to NVS
            # on the poller while the new job's motion starts.
            t2, status = board.cmd(f"$Sand/Run={patterns[(i + 1) % 2]}", timeout=10.0)
            seen += t2
            assert status == "ok", f"restart failed: {t2}"
            time.sleep(1.0)
        _assert_alive(board, seen)
    finally:
        _abort_job(board)
        if prev_effect is not None:
            board.cmd(f"$Sand/Led=effect={prev_effect}")
            board.cmd(f"$LED/Effect={prev_effect}")
        if prev_bright is not None:
            board.cmd(f"$LED/Brightness={prev_bright}")
