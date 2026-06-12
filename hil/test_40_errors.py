# Error paths must report errors, not crash the controller.


def test_file_errors_do_not_reboot(board):
    # Regression for the SD-mount abort fix (d44087f0): missing card or
    # missing file must produce a clean error.  A "Grbl" banner in the
    # response means the controller rebooted.
    text, status = board.cmd("$SD/Run=/no_such_dir/nope.thr", timeout=15.0)
    assert status is not None and status.startswith("error"), f"expected error, got: {text}"
    assert "Grbl" not in text, f"controller rebooted:\n{text}"
    text, status = board.cmd("$I")
    assert status == "ok", "controller unresponsive after file error"


def test_unknown_command_is_an_error(board):
    _, status = board.cmd("$No/Such/Command")
    assert status is not None and status.startswith("error")


def test_bad_playlist_name_fails_gracefully(board, playlist_configured):
    state = board.state()
    if state is None or not state.startswith("Idle"):
        import pytest

        pytest.skip(f"machine not idle ({state})")
    # The run is accepted (queued) ...
    text, status = board.cmd("$Playlist/Run=definitely_missing_playlist")
    assert status == "ok", f"queueing failed: {text}"
    # ... and the loader then reports failure into the log without
    # crashing; the controller stays responsive.
    import time

    time.sleep(2.0)
    board.drain(quiet=0.5, limit=3.0)
    text, status = board.cmd("$I")
    assert status == "ok"
