#!/usr/bin/env python3
"""HIL check (HTTP, moves the table): /sand_status `elapsed` stands still while
a pattern is paused and leaves the pause out after it resumes.

    python3 hil/elapsed_hold.py <table-ip>

Before v0.2.9 `elapsed` kept counting through a hold, so a client's ETA
(elapsed * (1 - progress) / progress) came out long after every pause.
Exit 0 = pass. Test tables only (DWMP, DWMP2, DWMAX): it starts, pauses and
stops a pattern.
"""
import json
import sys
import time
import urllib.parse
import urllib.request

BASE = "http://" + sys.argv[1]
HOLD_S = 15


def get(path, timeout=10):
    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return r.read().decode()


def status():
    return json.loads(get("/sand_status"))


def wait(pred, what, limit=120):
    end = time.time() + limit
    while True:
        st = status()
        if pred(st):
            return st
        assert time.time() < end, what
        time.sleep(1)


def main():
    st = status()
    print(f"table {st['hostname']} {st['mac']} fw={st['fw']} state={st['state']}")
    assert st["state"] == "Idle", "start from Idle"

    pattern = next(p for p in json.loads(get("/sand_patterns", timeout=30)) if p.endswith(".thr"))
    get("/command?plain=" + urllib.parse.quote(f"$Sand/Run=/patterns/{pattern}", safe=""))
    try:
        wait(lambda s: s["running"] and s["elapsed"] >= 5, "pattern never started")

        get("/sand_pause")
        held = wait(lambda s: s["state"].startswith("Hold:0"), "hold never completed", 30)
        time.sleep(HOLD_S)
        still = status()
        print(f"held {HOLD_S}s: elapsed {held['elapsed']} -> {still['elapsed']}")
        assert still["state"].startswith("Hold"), "the pattern left the hold"
        assert still["elapsed"] - held["elapsed"] <= 1, "elapsed kept counting through the hold"

        get("/sand_resume")
        resumed = wait(lambda s: s["state"] == "Run", "never resumed", 30)
        time.sleep(10)
        after = status()
        ran = after["elapsed"] - resumed["elapsed"]
        print(f"10s after resume: elapsed {resumed['elapsed']} -> {after['elapsed']} (+{ran})")
        assert 8 <= ran <= 12, "elapsed must run again after the resume"
        assert after["elapsed"] - still["elapsed"] <= 14, "the pause was counted after all"
    finally:
        get("/sand_stop")
    print("PASS")


if __name__ == "__main__":
    main()
