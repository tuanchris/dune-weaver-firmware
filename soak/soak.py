#!/usr/bin/env python3
"""Soak-test harness: hammer a table with concurrent load and watch for the
failures that short test suites can't see - panics, silent reboots, heap
decay, and server wedges.

    python3 soak/soak.py --profile torture                 # 10-min release gate
    python3 soak/soak.py --profile household --hours 24    # optional long soak

The torture profile is THE pre-release integration test: 10 minutes, exit
code 0/1. It compresses ~a day of household events into the window - feed
override 200% so several patterns COMPLETE naturally (a different job-stack
transition than an abort), alternating with mid-pattern stops:
  - 5 status pollers at 2-3 Hz         (the app's steady state, multiplied)
  - pattern cycling: odd runs stopped after 20 s, even runs allowed to finish
    (bounded 110 s); 25% start with clear=adaptive
  - /wifi_scan every 45 s, /sand_led write every 12 s, pause/resume every 45 s
  - a 20 s client-abort storm every 2.5 min
  - verdict: any reboot/panic/wedge alert, plus start-vs-end heap_largest
    decay >25% (catches per-operation leaks without wall-clock time)
Feed override and motion are restored/stopped when the gate ends.

The household profile is a gentle 1x load for optional long qualification
runs (new subsystem, framework bump) - not part of the release path.

Telemetry (once a minute -> <outdir>/telemetry.jsonl) and events
(<outdir>/events.log). ALERT lines mean act: a reboot was observed
(bootlog + coredump snapshots are saved next to the log), last_reset went
panic/task_wdt/brownout, heap decayed past thresholds, or the server
stopped answering for >90 s. Exit code 1 if any ALERT fired.

Stdlib only; stop with Ctrl-C / SIGTERM (writes a final summary).
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

# Panic classes are mostly event-coupled (races per job transition, leaks per
# operation), not time-coupled - so the release gate compresses a day of
# household events into ~15 min instead of running for a day.  "household" is
# the long qualification profile for structural changes (new subsystem,
# framework bump); "torture" is the pre-release gate.
PROFILES = {
    "household": dict(pollers=(0.9, 1.0, 1.1), scan_s=300, led_s=120,
                      pattern_hold_s=0,     # patterns run to completion
                      complete_wait_s=0, feed_pct=0,
                      chaos_s=600, storm_s=0, telemetry_s=60),
    # The pre-release gate: 10 minutes, feed override 200% so several patterns
    # COMPLETE inside the window (natural pattern->next is a different
    # job-stack path than an abort), alternating with mid-pattern stops.
    "torture":   dict(pollers=(0.3, 0.35, 0.4, 0.5, 0.6), scan_s=45, led_s=12,
                      pattern_hold_s=20,    # abort cycles: stop after 20s
                      complete_wait_s=110,  # completion cycles: bounded wait
                      feed_pct=200,
                      chaos_s=45, storm_s=150, telemetry_s=20),
}


def now_iso():
    return time.strftime("%Y-%m-%d %H:%M:%S")


class Soak:
    def __init__(self, base, outdir, profile):
        self.base = base.rstrip("/")
        self.p = PROFILES[profile]
        self.profile = profile
        self.outdir = outdir
        os.makedirs(outdir, exist_ok=True)
        self.events = open(os.path.join(outdir, "events.log"), "a", 1)
        self.telemetry = open(os.path.join(outdir, "telemetry.jsonl"), "a", 1)
        self.last_ok = time.time()  # any successful request, any thread
        self.counters = {"requests": 0, "failures": 0, "patterns": 0,
                         "scans": 0, "led": 0, "pauses": 0, "reboots": 0,
                         "storm_aborts": 0}
        self.lock = threading.Lock()
        self.patterns = []
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

    # ---- load threads ---------------------------------------------------

    def poller(self, period):
        while not STOP.wait(period):
            self.get("/sand_status")

    def scanner(self):
        while not STOP.wait(self.p["scan_s"]):
            self.get("/wifi_scan?rescan=1")
            for _ in range(30):
                if STOP.wait(1.5):
                    return
                r = self.get_json("/wifi_scan")
                if r and r.get("status") == "ok":
                    self.counters["scans"] += 1
                    self.log("INFO", f"scan ok: {len(r['aps'])} networks")
                    break

    def led(self):
        effects = ["rainbow", "breathe", "plasma", "fire", "pacifica", "aurora", "static"]
        while not STOP.wait(self.p["led_s"]):
            e = random.choice(effects)
            b = random.randint(30, 180)
            if self.get(f"/sand_led?effect={e}&brightness={b}") is not None:
                self.counters["led"] += 1

    def driver(self):
        # keep a pattern playing at all times; occasionally with a clear
        while not STOP.wait(5):
            st = self.get_json("/sand_status")
            if not st or st.get("running") or st.get("state") not in ("Idle",):
                continue
            p = random.choice(self.patterns)
            clear = " clear=adaptive" if random.random() < 0.25 else ""
            self.command(f"$Sand/Run=/patterns/{p}{clear}")
            # confirm it took; if not, log and let the next cycle try another
            if STOP.wait(15):
                return
            st = self.get_json("/sand_status")
            if st and st.get("running"):
                n = self.counters["patterns"] = self.counters["patterns"] + 1
                self.log("INFO", f"pattern {n}: {p}{clear}")
                hold = self.p["pattern_hold_s"]
                if not hold:
                    continue  # household: let it run; next Idle starts the next
                if n % 2:
                    # abort path: stop mid-pattern -> next (job-stack abort)
                    if STOP.wait(hold):
                        return
                    self.get("/sand_stop")
                else:
                    # completion path: at 200% feed short patterns finish here;
                    # natural end -> Idle is a different transition than abort.
                    # Bounded so one long pattern can't eat the gate.
                    end = time.time() + self.p["complete_wait_s"]
                    while time.time() < end and not STOP.is_set():
                        if STOP.wait(3):
                            return
                        st = self.get_json("/sand_status")
                        if st and not st.get("running"):
                            self.log("INFO", f"pattern {n} completed naturally")
                            break
                    else:
                        self.get("/sand_stop")
            else:
                self.log("WARN", f"pattern did not start: {p}{clear}")

    def chaos(self):
        while not STOP.wait(self.p["chaos_s"]):
            st = self.get_json("/sand_status")
            if st and st.get("running") and not st.get("paused"):
                self.get("/sand_pause")
                if STOP.wait(10):
                    return
                self.get("/sand_resume")
                self.counters["pauses"] += 1

    def storm(self):
        # burst of client-aborted requests (the app's fetch timeouts): 20s of
        # 0.25s-timeout hammering.  Counted separately - these failures are
        # the load, not a finding.
        while not STOP.wait(self.p["storm_s"]):
            self.log("INFO", "abort-storm burst (20s)")
            end = time.time() + 20
            while time.time() < end and not STOP.is_set():
                try:
                    urllib.request.urlopen(self.base + "/sand_status", timeout=0.25).read()
                except Exception:
                    self.counters["storm_aborts"] += 1

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
        while not STOP.wait(self.p["telemetry_s"]):
            if deadline and time.time() > deadline:
                self.log("INFO", "soak duration reached")
                STOP.set()
                break
            st = self.get_json("/sand_status", timeout=10)
            if st is None:
                if time.time() - self.last_ok > 90:
                    self.log("ALERT", "server unresponsive >90s (wedge?)")
                    self.last_ok = time.time()  # don't re-fire every minute
                continue
            rec = {k: st.get(k) for k in ("uptime", "state", "running", "file",
                                          "progress", "heap", "heap_min",
                                          "heap_largest", "sd_ok", "last_reset")}
            rec["ts"] = now_iso()
            rec.update(self.counters)
            self.telemetry.write(json.dumps(rec) + "\n")

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

            hl = st.get("heap_largest")
            if isinstance(hl, int):
                self.heap_largest_samples.append(hl)
                if hl < 20000:
                    self.log("ALERT" if hl < 12000 else "WARN",
                             f"heap_largest low: {hl} (heap={st.get('heap')}, min={st.get('heap_min')})")

    def heap_trend_verdict(self):
        # Per-event leaks / fragmentation show as decay between the first and
        # last samples; median-of-3 smooths run-vs-idle variance.
        s = self.heap_largest_samples
        if len(s) < 8:
            return
        first, last = statistics.median(s[:3]), statistics.median(s[-3:])
        if last < first * 0.75:
            self.log("ALERT", f"heap_largest decayed {first} -> {last} over the run (leak/fragmentation?)")
        else:
            self.log("INFO", f"heap trend ok: heap_largest {first} -> {last}")

    def run(self, hours):
        pats = self.get_json("/sand_patterns", timeout=15)
        if not pats:
            self.log("ALERT", "could not fetch pattern manifest; aborting")
            return 1
        self.patterns = [p for p in pats if p.endswith(".thr")]
        st = self.get_json("/sand_status")
        self.log("INFO", f"soak start [{self.profile}]: base={self.base} "
                         f"fw={st.get('fw') if st else '?'} "
                         f"uptime={st.get('uptime') if st else '?'} patterns={len(self.patterns)} "
                         f"hours={hours or 'unbounded'}")

        if self.p["feed_pct"]:
            # The gate owns the table: whatever was playing (a leftover
            # pattern would hog the whole window) is stopped so the driver
            # can cycle patterns from the first minute.
            self.get("/sand_stop")
            self.get(f"/sand_feed?pct={self.p['feed_pct']}")
            self.log("INFO", f"feed override {self.p['feed_pct']}% for the gate")

        threads = [threading.Thread(target=self.poller, args=(p,), daemon=True)
                   for p in self.p["pollers"]]
        threads += [threading.Thread(target=f, daemon=True)
                    for f in (self.scanner, self.led, self.driver, self.chaos)]
        if self.p["storm_s"]:
            threads.append(threading.Thread(target=self.storm, daemon=True))
        for t in threads:
            t.start()

        try:
            self.telemetry_loop(hours)
        except KeyboardInterrupt:
            pass
        STOP.set()
        if self.p["feed_pct"]:  # leave the table idle at normal speed
            self.get("/sand_stop")
            self.get("/sand_feed?d=reset")
        self.heap_trend_verdict()
        c = self.counters
        verdict = "FAIL" if ALERTS else "CLEAN"
        self.log("INFO", f"soak end [{verdict}] [{self.profile}]: {c['requests']} requests "
                         f"({c['failures']} failed), {c['patterns']} patterns, "
                         f"{c['scans']} scans, {c['led']} led, {c['pauses']} pauses, "
                         f"{c['storm_aborts']} storm aborts, "
                         f"{c['reboots']} reboots, {len(ALERTS)} alerts")
        return 1 if ALERTS else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", default="http://192.168.68.125")
    ap.add_argument("--profile", choices=sorted(PROFILES), default="household",
                    help="household = long qualification soak; torture = 15-min release gate")
    ap.add_argument("--hours", type=float, default=0, help="0 = run until stopped")
    ap.add_argument("--minutes", type=float, default=0, help="shorthand for sub-hour runs")
    ap.add_argument("--outdir", default=None)
    args = ap.parse_args()
    hours = args.hours or args.minutes / 60
    if args.profile == "torture" and not hours:
        hours = 10 / 60  # the release gate defaults to 10 minutes
    outdir = args.outdir or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "logs",
        time.strftime(f"soak_{args.profile}_%Y%m%d_%H%M"))
    signal.signal(signal.SIGTERM, lambda *_: STOP.set())
    sys.exit(Soak(args.base, outdir, args.profile).run(hours))


if __name__ == "__main__":
    main()
