# JSON API: $Sand/Status, $Sand/Patterns, $Sand/Playlists ride the
# command channel, so they are reachable over serial here and over the
# /command HTTP endpoint from the UI.
import json


def _extract(buf, opener):
    for line in buf.splitlines():
        s = line.strip()
        if s.startswith(opener):
            return json.loads(s)
    raise AssertionError(f"no JSON {opener!r} in response:\n{buf}")


def test_sand_status_is_valid_json(board):
    text, status = board.cmd("$Sand/Status")
    assert status == "ok", f"command failed: {text}"
    doc = _extract(text, "{")
    # Core fields the UI depends on
    for key in ("state", "theta", "rho", "feed", "running", "file", "progress", "playlist"):
        assert key in doc, f"missing {key}: {doc}"
    assert isinstance(doc["playlist"], dict)
    for key in ("active", "index", "total", "name", "clearing", "quiet"):
        assert key in doc["playlist"], f"missing playlist.{key}: {doc}"
    # Idle and not running with no job
    assert isinstance(doc["running"], bool)


def test_sand_patterns_json_or_clean_error(board):
    # With an SD card -> JSON array of filenames; without -> clean
    # error:60, never a crash (guards the filesystem_error fix).
    text, status = board.cmd("$Sand/Patterns", timeout=10.0)
    if status == "ok":
        arr = _extract(text, "[")
        assert isinstance(arr, list)
    else:
        assert status.startswith("error"), f"unexpected: {text}"
        assert "Grbl" not in text, f"controller rebooted:\n{text}"
    # Controller still responsive either way
    _, status = board.cmd("$I")
    assert status == "ok"


def test_sand_playlists_json_or_clean_error(board):
    text, status = board.cmd("$Sand/Playlists", timeout=10.0)
    if status == "ok":
        assert isinstance(_extract(text, "["), list)
    else:
        assert status.startswith("error")
        assert "Grbl" not in text
