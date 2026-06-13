# Settings round-trips through the real NVS/settings machinery.
import re


def _setting_value(board, name):
    text, status = board.cmd(name)
    assert status == "ok", f"{name} failed: {text}"
    m = re.search(re.escape(name) + r"=(\S+)", text)
    assert m, f"no value in response to {name}: {text}"
    return m.group(1)


def test_led_brightness_roundtrip(board, leds_configured):
    orig = _setting_value(board, "$LED/Brightness")
    try:
        _, status = board.cmd("$LED/Brightness=77")
        assert status == "ok"
        assert _setting_value(board, "$LED/Brightness") == "77"
    finally:
        board.cmd(f"$LED/Brightness={orig}")


def test_led_effect_enum(board, leds_configured):
    orig = _setting_value(board, "$LED/Effect")
    try:
        for value in ("off", "static", "rainbow"):
            _, status = board.cmd(f"$LED/Effect={value}")
            assert status == "ok"
            assert _setting_value(board, "$LED/Effect") == value
        _, status = board.cmd("$LED/Effect=nonsense")
        assert status != "ok", "invalid enum value was accepted"
    finally:
        board.cmd(f"$LED/Effect={orig}")


def test_playlist_mode_enum(board, playlist_configured):
    orig = _setting_value(board, "$Playlist/Mode")
    try:
        for value in ("single", "loop"):
            _, status = board.cmd(f"$Playlist/Mode={value}")
            assert status == "ok"
            assert _setting_value(board, "$Playlist/Mode") == value
    finally:
        board.cmd(f"$Playlist/Mode={orig}")


def test_playlist_skip_without_playlist(board, playlist_configured):
    text, status = board.cmd("$Playlist/Skip")
    assert status == "ok"
    assert "No playlist active" in text


def test_thr_feed_roundtrip(board):
    # $THR/Feed exists whenever the ThetaRho kinematics is configured
    import pytest

    text, status = board.cmd("$THR/Feed")
    if status != "ok":
        pytest.skip("kinematics is not ThetaRho")
    orig = _setting_value(board, "$THR/Feed")
    try:
        _, status = board.cmd("$THR/Feed=150")
        assert status == "ok"
        assert _setting_value(board, "$THR/Feed") == "150"
    finally:
        board.cmd(f"$THR/Feed={orig}")


def test_led_state_hook_enums(board, leds_configured):
    orig = _setting_value(board, "$LED/RunEffect")
    try:
        for value in ("off", "static", "rainbow", "none"):
            _, status = board.cmd(f"$LED/RunEffect={value}")
            assert status == "ok"
            assert _setting_value(board, "$LED/RunEffect") == value
    finally:
        board.cmd(f"$LED/RunEffect={orig}")


def test_time_show_and_set(board):
    # $Time/* exists when the time: config section is present
    import pytest

    text, status = board.cmd("$Time/Show")
    if status != "ok":
        pytest.skip("board config has no time: section")
    # Setting a known epoch reflects in $Time/Show output (UTC default)
    _, status = board.cmd("$Time/Set=1750000000")
    assert status == "ok"
    text, status = board.cmd("$Time/Show")
    assert status == "ok"
    assert "2025-06-" in text, f"unexpected time: {text}"
    assert "NOT SET" not in text


def test_sands_settings_roundtrip(board, playlist_configured):
    orig = _setting_value(board, "$Sands/Slots")
    try:
        _, status = board.cmd("$Sands/Slots=21:00-08:00@weekdays")
        assert status == "ok"
        assert _setting_value(board, "$Sands/Slots") == "21:00-08:00@weekdays"
    finally:
        board.cmd(f"$Sands/Slots={orig}")
