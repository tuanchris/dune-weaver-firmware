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
