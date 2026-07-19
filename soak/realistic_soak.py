#!/usr/bin/env python3
"""Realistic day-in-the-life soak (NOT the pre-merge release gate).

The FIRMWARE playlist engine drives a real curated playlist - looping at normal
speed, LEDs on - and this harness only plays the CLIENTS and watches for the
failures short tests miss: panics, silent reboots, heap decay, server wedges.
Complements soak/soak.py's 10-min torture gate; it does not replace it.

    python3 soak/realistic_soak.py --base http://192.168.68.128 \
        --playlist "beautiful patterns" --hours 8

Simulated clients (a normal household, not a stress test):
  - 3 app pollers at ~1 Hz          (the app's steady /sand_status poll)
  - 1 Home-Assistant REST poll at 30 s
  - an occasional app-style effect change (~3 min) and pause/resume (~4 min)
No pattern driver (the playlist engine owns the job stack - a driver would
hijack it during the inter-pattern pause), no wifi-scan / abort-storm, no feed
override.  Feed and LEDs are left as-is when the run ends; the playlist keeps
looping.

Watchdogs (same thresholds as the release gate):
  - REBOOT: uptime regression -> ALERT + bootlog/coredump snapshot
  - last_reset -> panic/task_wdt/int_wdt/brownout -> ALERT
  - heap_largest: <20k WARN; sustained <12k or free heap <12k ALERT; run-decay
    >25% ALERT
  - server unresponsive >90 s -> ALERT (wedge)
Exit code 1 if any ALERT fired.

Touches the SD only via /sand_status - NO per-file probing (bulk ranged GETs
panic the board and wedge the card until a power cycle).  Stdlib only; stop with
Ctrl-C / SIGTERM (writes a final summary).
"""

import argparse
import json
import os
import random
import signal
import statistics
import sys
import threading
import time
import urllib.parse
import urllib.request

STOP = threading.Event()
ALERTS = []

APP_POLLERS = (1.0, 1.0, 1.3)   # the app's steady ~1 Hz status poll, x3 clients
HA_POLL_S = 30.0                # Home-Assistant REST sensor scan_interval
LED_S = 180                     # app user tweaks the effect now and then
CHAOS_S = 240                   # app user pauses to look, then resumes
TELEMETRY_S = 60
LED_EFFECTS = ["rainbow", "aurora", "pacifica", "plasma", "breathe"]  # never off


def now_iso():
    return time.strftime("%Y-%m-%d %H:%M:%S")


class Soak:
    def __init__(self, base, outdir, playlist):
        self.base = base.rstrip("/")
        self.outdir = outdir
        self.playlist = playlist
        os.makedirs(outdir, exist_ok=True)
        self.events = open(os.path.join(outdir, "events.log"), "a", 1)
        self.telemetry = open(os.path.join(outdir, "telemetry.jsonl"), "a", 1)
        self.last_ok = time.time()
        self.counters = {"requests": 0, "failures": 0, "led": 0, "pauses": 0,
                         "reboots": 0}
        self.lock = threading.Lock()
        self.heap_largest_samples = []

    def log(self, level, msg):
        line = f"{now_iso()} {level} {msg}"
        print(line, flush=True)
        self.events.write(line + "\n")
        if level == "ALERT":
            ALERTS.append(msg)

    def get(self, path, timeout=5):
        with self.lock:
            self.counters["requests"] += 1
        try:
            body = urllib.request.urlopen(self.base + path, timeout=timeout).read()
            self.last_ok = time.time()
            return body
        except Exception:
            with self.lock:
                self.counters["failures"] += 1
            return None

    def get_json(self, path, timeout=5):
        body = self.get(path, timeout)
        if body is None:
            return None
        try:
            return json.loads(body)
        except ValueError:
            self.log("WARN", f"non-JSON response from {path}")
            return None

    def command(self, plain, timeout=8):
        return self.get("/command?plain=" + urllib.parse.quote(plain, safe=""), timeout)

    # ---- client threads --------------------------------------------------

    def poller(self, period):
        while not STOP.wait(period):
            self.get("/sand_status")

    def led(self):
        # an app user changing the effect occasionally; always leaves LEDs on
        while not STOP.wait(LED_S):
            e = random.choice(LED_EFFECTS)
            b = random.randint(90, 180)
            if self.get(f"/sand_led?effect={e}&brightness={b}") is not None:
                self.counters["led"] += 1

    def chaos(self):
        # an app user pausing to admire a pattern, then resuming
        while not STOP.wait(CHAOS_S):
            st = self.get_json("/sand_status")
            if st and st.get("running") and not st.get("paused"):
                self.get("/sand_pause")
                if STOP.wait(12):
                    return
                self.get("/sand_resume")
                self.counters["pauses"] += 1

    # ---- telemetry / verdict --------------------------------------------

    def snapshot_crash_evidence(self, tag):
        for path, name in (("/sand_bootlog", f"bootlog_{tag}.txt"),
                           ("/sand_coredump", f"coredump_{tag}.json")):
            body = self.get(path, timeout=10)
            if body is not None:
                with open(os.path.join(self.outdir, name), "wb") as f:
                    f.write(body)
                self.log("INFO", f"saved {name}")

    def telemetry_loop(self, hours):
        deadline = time.time() + hours * 3600 if hours else None
        prev_uptime = None
        baseline_reset = None
        while not STOP.wait(TELEMETRY_S):
            if deadline and time.time() > deadline:
                self.log("INFO", "soak duration reached")
                STOP.set()
                break
            st = self.get_json("/sand_status", timeout=10)
            if st is None:
                if time.time() - self.last_ok > 90:
                    self.log("ALERT", "server unresponsive >90s (wedge?)")
                    self.last_ok = time.time()
                continue
            pl = st.get("playlist", {}) or {}
            rec = {k: st.get(k) for k in ("uptime", "state", "running", "file",
                                          "progress", "heap", "heap_min",
                                          "heap_largest", "sd_ok", "last_reset")}
            rec["ts"] = now_iso()
            rec["pl_active"] = pl.get("active")
            rec["pl_index"] = pl.get("index")
            rec.update(self.counters)
            self.telemetry.write(json.dumps(rec) + "\n")

            # the playlist silently dying (SD hiccup, MAX_CONSEC_FAIL) turns the
            # whole run into a no-op - surface it, don't fail silently.
            if not pl.get("active"):
                self.log("WARN", f"playlist no longer active (state={st.get('state')})")

            up, reset = st.get("uptime"), st.get("last_reset")
            if baseline_reset is None:
                baseline_reset = reset
            if prev_uptime is not None and isinstance(up, (int, float)) and up < prev_uptime - 5:
                self.counters["reboots"] += 1
                self.log("ALERT", f"REBOOT detected (uptime {prev_uptime} -> {up}, last_reset={reset})")
                self.snapshot_crash_evidence(f"reboot{self.counters['reboots']}")
            elif reset in ("panic", "task_wdt", "int_wdt", "brownout") and reset != baseline_reset:
                self.log("ALERT", f"last_reset changed to {reset}")
            prev_uptime = up

            hl, hfree = st.get("heap_largest"), st.get("heap")
            if isinstance(hl, int):
                self.heap_largest_samples.append(hl)
                low_streak = (len(self.heap_largest_samples) >= 2
                              and self.heap_largest_samples[-1] < 12000
                              and self.heap_largest_samples[-2] < 12000)
                if isinstance(hfree, int) and hfree < 12000:
                    self.log("ALERT", f"free heap nearly exhausted: {hfree}")
                elif low_streak:
                    self.log("ALERT", f"heap_largest sustained low: {self.heap_largest_samples[-2:]}")
                elif hl < 20000:
                    self.log("WARN", f"heap_largest low: {hl} (heap={hfree}, min={st.get('heap_min')})")

    def heap_trend_verdict(self):
        s = self.heap_largest_samples
        if len(s) < 12:
            return
        first, last = statistics.median(s[4:7]), statistics.median(s[-3:])
        if last < first * 0.75:
            self.log("ALERT", f"heap_largest decayed {first} -> {last} over the run (leak/fragmentation?)")
        else:
            self.log("INFO", f"heap trend ok: heap_largest {first} -> {last} (loaded steady state)")

    def run(self, hours):
        st = self.get_json("/sand_status")
        pl = (st or {}).get("playlist", {})
        if self.playlist and not pl.get("active"):
            self.command("$Playlist/Mode=loop")
            self.command(f"$Playlist/Run={self.playlist}")
            for _ in range(30):
                if STOP.wait(2):
                    return 1
                st = self.get_json("/sand_status")
                pl = (st or {}).get("playlist", {})
                if pl.get("active"):
                    break
        if not pl.get("active"):
            self.log("ALERT", "no playlist active (pass --playlist NAME and make "
                              "sure its entries resolve); aborting")
            return 1
        self.log("INFO", f"realistic soak start: base={self.base} "
                         f"fw={st.get('fw') if st else '?'} "
                         f"uptime={st.get('uptime') if st else '?'} "
                         f"playlist='{pl.get('name')}' ({pl.get('total')} entries) "
                         f"hours={hours or 'unbounded'}")

        threads = [threading.Thread(target=self.poller, args=(p,), daemon=True)
                   for p in APP_POLLERS]
        threads.append(threading.Thread(target=self.poller, args=(HA_POLL_S,), daemon=True))
        threads += [threading.Thread(target=f, daemon=True) for f in (self.led, self.chaos)]
        for t in threads:
            t.start()

        try:
            self.telemetry_loop(hours)
        except KeyboardInterrupt:
            pass
        STOP.set()
        self.heap_trend_verdict()
        c = self.counters
        verdict = "FAIL" if ALERTS else "CLEAN"
        self.log("INFO", f"soak end [{verdict}]: {c['requests']} requests "
                         f"({c['failures']} failed), {c['led']} led, "
                         f"{c['pauses']} pauses, {c['reboots']} reboots, "
                         f"{len(ALERTS)} alerts (playlist left looping)")
        return 1 if ALERTS else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", default="http://192.168.68.128")
    ap.add_argument("--playlist", default=None,
                    help="firmware playlist to loop; if omitted, observes whatever is active")
    ap.add_argument("--hours", type=float, default=0, help="0 = run until stopped")
    ap.add_argument("--minutes", type=float, default=0, help="shorthand for sub-hour runs")
    ap.add_argument("--outdir", default=None)
    args = ap.parse_args()
    hours = args.hours or args.minutes / 60
    outdir = args.outdir or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "logs",
        time.strftime("realistic_%Y%m%d_%H%M"))
    signal.signal(signal.SIGTERM, lambda *_: STOP.set())
    sys.exit(Soak(args.base, outdir, args.playlist).run(hours))


if __name__ == "__main__":
    main()
