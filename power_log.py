#!/usr/bin/env python3
"""Measure whole-system energy for a command on Apple Silicon.

    ./power_log.py --idle 60                      # baseline
    ./power_log.py --records 5e8 -- ./merge_program 10
    ./power_log.py --loop 120 --records 5e8 -- ./merge_program 10
    ./power_log.py --csv run.csv -- ./split_program 5000000 8

Reads `macmon pipe` (brew install macmon), which exposes IOReport channels
directly and sudolessly at 1 Hz:

    sys_power    whole-system draw. The channel we want.
    all_power    SoC only (CPU+GPU+ANE+RAM) -- what powermetrics reports.

Measured response: unplugged with apps closed, idle is 3.54 W of which the SoC
is 0.20 W. Four busy `yes` loops took sys_power from ~15 W to ~30 W while
all_power went 1.5 -> 9 W, so sys_power rises by more than the SoC does. It
covers regulator losses, memory, storage and display, which is what a
total-system figure has to include.

WHY NOT THE BATTERY GAUGE. Four attempts, four failures, documented here so
nobody repeats them:

  * InstantAmperage refreshes every ~19s and is pre-filtered (FilteredCurrent
    sits beside it in ioreg). A 28s run gets 1-2 samples. Integrating it gave
    2 distinct values across 168 samples.
  * AppleRawCurrentCapacity is a state-of-charge *estimate*, not a coulomb
    ledger. Measured going UP 17 mAh while discharging. Unusable.
  * AccumulatedBatteryPower did not tick in 60s; interval unknown.
  * Averaging the slow readings over a 4-minute loop produced a 0.70 W sample
    during an active merge and a 54% coefficient of variation.

  The gauge is built to say "3:58 remaining" -- smooth and stable by design,
  which is the opposite of what measuring a 30-second event needs.

CROSS-CHECK. Measured in matched machine states, sys_power and the battery
gauge agree within ~10%: 9.23 vs ~8.4 W in one state, 3.54 vs 3.74 W in
another. An earlier note here claimed a 2x disagreement; that was an error
from comparing readings taken in different states.

STILL NOT AC-SIDE. sys_power is a DC rail figure, so it excludes charger
conversion loss -- expect a wall meter to read roughly 10% higher. Anything
published needs the wall reading.

CONDITIONS THAT CHANGED MEASUREMENTS HERE, all of which must be recorded:
power source (idle 7.97 W plugged vs 3.54 W unplugged), which apps are open
(11.61 / 8.39 / 3.74 W as they were closed), and disk utilisation (worth 2x on
merge throughput between ~50% and 75% full).
"""

import argparse
import json
import subprocess
import sys
import threading
import time


def start_macmon(interval_ms=500):
    try:
        p = subprocess.Popen(["macmon", "pipe", "-i", str(interval_ms)],
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                             text=True, bufsize=1)
    except FileNotFoundError:
        sys.exit("macmon not found. Install it:  brew install macmon")
    return p


class Monitor(threading.Thread):
    """Collect (monotonic, sys_power, all_power) from macmon until stopped."""

    def __init__(self, interval_ms=500):
        super().__init__(daemon=True)
        self.proc = start_macmon(interval_ms)
        self.rows = []
        self.stop = threading.Event()

    def run(self):
        for line in self.proc.stdout:
            if self.stop.is_set():
                break
            try:
                d = json.loads(line)
                self.rows.append((time.monotonic(),
                                  float(d["sys_power"]),
                                  float(d["all_power"])))
            except (json.JSONDecodeError, KeyError, ValueError):
                continue

    def close(self):
        self.stop.set()
        self.proc.terminate()
        try:
            self.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def slice(self, t0, t1):
        return [r for r in self.rows if t0 <= r[0] <= t1]


def integrate(rows, idx):
    """Trapezoid-integrate column idx over the row timestamps."""
    if len(rows) < 2:
        return 0.0, 0.0
    total = sum((rows[i + 1][0] - rows[i][0]) * (rows[i + 1][idx] + rows[i][idx]) / 2.0
                for i in range(len(rows) - 1))
    span = rows[-1][0] - rows[0][0]
    return total, span


def report(rows, label, records=0, iters=1, idle_w=None, ref=0.0):
    if len(rows) < 3:
        sys.exit(f"  only {len(rows)} samples -- window too short")
    sys_j, span = integrate(rows, 1)
    soc_j, _ = integrate(rows, 2)
    sysw = [r[1] for r in rows]
    mean = sys_j / span

    print(f"\n  {label}")
    print(f"  samples          {len(rows):8d} over {span:.1f}s")
    print(f"  sys_power        {mean:8.2f} W  (min {min(sysw):.2f}, "
          f"max {max(sysw):.2f})")
    print(f"  SoC only         {soc_j/span:8.2f} W  "
          f"({100*soc_j/sys_j:.0f}% of system)")
    print(f"  energy           {sys_j:8.0f} J")

    if idle_w is not None:
        marginal = sys_j - idle_w * span
        print(f"  minus {idle_w:.2f} W idle  {marginal:8.0f} J   "
              f"({100*idle_w*span/sys_j:.0f}% of the total was idle draw)")

    if records:
        per = sys_j / iters
        if iters > 1:
            print(f"  per iteration    {per:8.0f} J  ({iters} runs)")
        print(f"  records/joule    {records/per:12,.0f}")
        proj = 1e10 / (records / per) / 1000
        print(f"  1e10 records     {proj:8.1f} kJ")
        if ref:
            print(f"  vs reference     {ref:8.1f} kJ   ->  {ref/proj:.2f}x")
    return sys_j, span


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--idle", type=float, metavar="SECONDS",
                    help="measure baseline draw, no command")
    ap.add_argument("--loop", type=float, default=0, metavar="SECONDS",
                    help="repeat the command until this long has passed")
    ap.add_argument("--records", type=float, default=0,
                    help="records per iteration, for records/joule")
    ap.add_argument("--idle-watts", type=float, metavar="W",
                    help="baseline from an --idle run, to report marginal energy")
    ap.add_argument("--interval", type=int, default=500,
                    help="macmon sample interval in ms (default 500)")
    ap.add_argument("--reference", type=float, default=0.0, metavar="KJ",
                    help="a published kJ figure to compare the 1e10 "
                         "extrapolation against")
    ap.add_argument("--csv", metavar="FILE")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    mon = Monitor(args.interval)
    mon.start()
    time.sleep(2.0)          # let macmon produce its first samples

    try:
        if args.idle:
            print(f"  measuring idle for {args.idle:.0f}s. Do not touch the "
                  "machine.")
            t0 = time.monotonic()
            time.sleep(args.idle)
            report(mon.slice(t0, time.monotonic()), "idle")
            return

        cmd = args.cmd[1:] if args.cmd and args.cmd[0] == "--" else args.cmd
        if not cmd:
            ap.error("give a command after -- , or use --idle")

        print(f"  running: {' '.join(cmd)}")
        t0, iters = time.monotonic(), 0
        while True:
            rc = subprocess.call(cmd, stdout=subprocess.DEVNULL)
            iters += 1
            if rc != 0:
                sys.exit(f"  command failed with {rc} on iteration {iters}")
            if time.monotonic() - t0 >= args.loop:
                break
        t1 = time.monotonic()

        # sys_power lags load by ~2s at both edges, so the rise and decay
        # roughly cancel over a run of tens of seconds. Not true for very
        # short commands.
        rows = mon.slice(t0, t1)
        if t1 - t0 < 10:
            print("  NOTE: run under 10s. sys_power lags load by ~2s, so the "
                  "edges do not\n  cancel and this figure is unreliable. Use "
                  "--loop.")
        report(rows, " ".join(cmd), args.records, iters, args.idle_watts,
               args.reference)

        if args.csv:
            with open(args.csv, "w") as f:
                f.write("seconds,sys_power,soc_power\n")
                for t, s, a in rows:
                    f.write(f"{t-t0:.3f},{s:.3f},{a:.3f}\n")
            print(f"  samples          {args.csv}")
    finally:
        mon.close()


if __name__ == "__main__":
    main()
