#!/usr/bin/env python3
"""HIL check (HTTP, moves the table): an upload while a pattern runs is refused
fast and the ball keeps drawing; an upload at Idle still works.

    python3 hil/upload_gate.py <table-ip>

On v0.2.7 and older the multipart body was read in the task that feeds the
running pattern, so the ball stopped until the upload ended. Exit 0 = pass.
Test tables only (DWMP, DWMP2, DWMAX): it starts and stops a pattern and writes
/hil_upload_gate.thr at the SD root (deleted again).
"""
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = "http://" + sys.argv[1]
NAME = "/hil_upload_gate.thr"


def get(path, timeout=10):
    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return r.read().decode()


def status():
    return json.loads(get("/sand_status"))


def command(line):
    return get("/command?plain=" + urllib.parse.quote(line, safe=""))


def upload(nbytes):
    """POST one multipart file; returns (http status or exception name, seconds)."""
    b = "hilgate"
    body = (f'--{b}\r\nContent-Disposition: form-data; name="{NAME}"; filename="{NAME}"\r\n'
            "Content-Type: application/octet-stream\r\n\r\n").encode() + b"0 0\n" * (nbytes // 4) + f"\r\n--{b}--\r\n".encode()
    req = urllib.request.Request(BASE + "/upload?dontlist=yes", data=body, method="POST",
                                 headers={"Content-Type": f"multipart/form-data; boundary={b}"})
    t = time.time()
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.status, time.time() - t
    except urllib.error.HTTPError as e:
        return e.code, time.time() - t
    except OSError as e:  # the board resets a refused upload it never read
        return type(e).__name__, time.time() - t


def main():
    st = status()
    print(f"table {st['hostname']} {st['mac']} fw={st['fw']} state={st['state']}")
    assert st["state"] == "Idle", "start from Idle"

    code, secs = upload(4096)
    print(f"idle upload: {code} in {secs:.1f}s")
    assert code == 200, "an upload at Idle must still work"
    get(f"/upload?path=/&action=delete&filename={NAME[1:]}")

    pattern = next(p for p in json.loads(get("/sand_patterns", timeout=30)) if p.endswith(".thr"))
    print("running /patterns/" + pattern, command(f"$Sand/Run=/patterns/{pattern}").strip())
    try:
        end = time.time() + 120
        while not status()["running"]:
            assert time.time() < end, "pattern never started"
            time.sleep(1)
        time.sleep(3)
        before = status()
        code, secs = upload(3_000_000)  # ~25 s if the board took it
        after = status()
        time.sleep(2)
        moved = status()
        # theta per second across the upload vs right after it: on v0.2.7 the ball
        # stood still for most of the upload, so the first rate collapses.
        print(f"running upload: {code} in {secs:.1f}s; theta/s during {abs(after['theta'] - before['theta']) / secs:.3f}"
              f", after {abs(moved['theta'] - after['theta']) / 2:.3f}")
        assert code != 200 and secs < 5, "an upload mid-pattern must be refused before its body is read"
        assert after["running"], "the pattern must still be running"
        assert (moved["theta"], moved["rho"]) != (after["theta"], after["rho"]), "the ball stopped"
        assert NAME[1:] not in get("/upload?path=/&list=yes"), "the refused file must not exist"
    finally:
        get("/sand_stop")
        try:
            get(f"/upload?path=/&action=delete&filename={NAME[1:]}")
        except urllib.error.HTTPError:
            pass  # nothing to delete: the refused file never existed
    print("PASS")


if __name__ == "__main__":
    main()
