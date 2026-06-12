# Restart the controller and verify it boots cleanly.  Runs first so
# the rest of the suite exercises a fresh boot.


def test_boot_log_is_clean(board):
    state = board.state()
    if state is None or not state.startswith("Idle"):
        import pytest

        pytest.skip(f"machine not idle ({state}); not restarting it")
    boot = board.restart()
    assert "Grbl 3.9" in boot, f"no FluidNC banner after $Bye:\n{boot}"
    errors = [l for l in boot.splitlines() if "[MSG:ERR" in l]
    assert not errors, f"boot errors: {errors}"


def test_identity(board):
    text, status = board.cmd("$I")
    assert status == "ok"
    assert "FluidNC v3.9" in text


def test_status_report_shape(board):
    s = board.status()
    assert s is not None, "no <...> status report"
    assert "MPos:" in s or "WPos:" in s
