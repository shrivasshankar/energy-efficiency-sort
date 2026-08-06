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

SAMPLE RATE. The IOReport channel behind sys_power updates at 1 Hz. Polling
faster does not get more information -- it re-reads the same value, and every
second sample comes back byte-identical. Measured at -i 500: 6 of 13
consecutive pairs identical, alternating. So --interval defaults to 1000.
Integration is unaffected either way (the timestamps stay correct), but the
sample COUNT and the standard deviation are not, which is why the spread below
is computed over distinct readings only.

THE TAIL. sys_power reports a load change about 2s late. Ending the window at
the command's exit therefore drops the tail of its own power curve -- roughly
10% of a 20s run, worse on anything shorter. --settle keeps sampling past the
exit and nets out the idle draw over those seconds, which recovers it: a pure
sensor delay preserves the integral, so any window containing the whole
excursion gives the right total without needing to know the exact lag. Looping
used to hide this by amortising both edges over 120s, at 6x the disk writes.

SPREAD. A single mean is not enough to A/B a change worth a few percent, so
report() prints the sample standard deviation and, for --loop runs, the
per-iteration energy. Judge a change against the per-iteration CV: if the two
configurations' spreads overlap, the difference is not measured, it is noise.

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


def distinct(rows, idx=1):
    """Drop consecutive re-reads of the same value, keeping the first.

    macmon updates at 1 Hz, so polling faster repeats values. Two genuinely
    equal floats in a row are effectively impossible, so equality means a
    re-read. Used for counting and spread only, never for integration.
    """
    out = []
    for r in rows:
        if not out or r[idx] != out[-1][idx]:
            out.append(r)
    return out


def stats(xs):
    """mean, sample standard deviation, coefficient of variation in percent."""
    n = len(xs)
    if not n:
        return 0.0, 0.0, 0.0
    m = sum(xs) / n
    if n < 2 or not m:
        return m, 0.0, 0.0
    sd = (sum((x - m) ** 2 for x in xs) / (n - 1)) ** 0.5
    return m, sd, 100 * sd / m


def energy_between(rows, a, b, idx=1):
    """Trapezoid-integrate column idx over exactly [a, b].

    Both endpoints are interpolated. Summing only the samples that happen to
    fall inside the interval instead shortens it by up to one sample period at
    each end -- at 1 Hz that is a systematic undercount, ~4% on a 25s run and
    far worse on a short one.
    """
    total = 0.0
    for i in range(len(rows) - 1):
        t0, v0 = rows[i][0], rows[i][idx]
        t1, v1 = rows[i + 1][0], rows[i + 1][idx]
        if t1 <= a or t0 >= b or t1 == t0:
            continue
        lo, hi = max(t0, a), min(t1, b)
        vlo = v0 + (v1 - v0) * (lo - t0) / (t1 - t0)
        vhi = v0 + (v1 - v0) * (hi - t0) / (t1 - t0)
        total += (hi - lo) * (vlo + vhi) / 2.0
    return total


def integrate(rows, idx):
    """Trapezoid-integrate column idx over the row timestamps."""
    if len(rows) < 2:
        return 0.0, 0.0
    total = sum((rows[i + 1][0] - rows[i][0]) * (rows[i + 1][idx] + rows[i][idx]) / 2.0
                for i in range(len(rows) - 1))
    span = rows[-1][0] - rows[0][0]
    return total, span


def report(rows, label, records=0, iters=1, idle_w=None, ref=0.0, bounds=None,
           run_end=None):
    if len(rows) < 3:
        sys.exit(f"  only {len(rows)} samples -- window too short")
    sys_j, span = integrate(rows, 1)
    soc_j, _ = integrate(rows, 2)

    # sys_power reports a load change about 2s late, so a cold run's power
    # tail arrives after the command has already exited. Ending the window at
    # the exit drops that energy -- on a 20s run it is a ~10% undercount, and
    # the shorter the run the worse it gets. --settle keeps sampling past the
    # exit to catch the tail; the idle draw across those extra seconds is then
    # subtracted back out, since the tail is real run energy but the waiting
    # around it is not. A pure sensor delay preserves the integral, so a window
    # that contains the whole excursion gets the right answer without needing
    # to know the exact lag.
    tail = max(0.0, rows[-1][0] - run_end) if run_end else 0.0
    if tail > 0:
        sys_j -= (idle_w if idle_w is not None else min(r[1] for r in rows)) * tail
        soc_j -= min(r[2] for r in rows) * tail
        span -= tail
        if bounds:
            a, b = bounds[-1]
            bounds = bounds[:-1] + [(a, b + tail)]

    # Integration keeps every row -- the timestamps are right either way. The
    # count and the spread must not, or re-reads would look like agreement.
    uniq = distinct(rows)
    sysw = [r[1] for r in uniq]
    _, sd, cv = stats(sysw)
    dup = len(rows) - len(uniq)

    print(f"\n  {label}")
    print(f"  samples          {len(uniq):8d} over {span:.1f}s" +
          (f"   ({dup} duplicate re-reads dropped)" if dup else ""))
    print(f"  sys_power        {sys_j/span:8.2f} W  (min {min(sysw):.2f}, "
          f"max {max(sysw):.2f}, sd {sd:.2f}, CV {cv:.1f}%)")
    print(f"  SoC only         {soc_j/span:8.2f} W  "
          f"({100*soc_j/sys_j:.0f}% of system)")
    print(f"  energy           {sys_j:8.0f} J")

    if idle_w is not None:
        marginal = sys_j - idle_w * span
        print(f"  minus {idle_w:.2f} W idle  {marginal:8.0f} J   "
              f"({100*idle_w*span/sys_j:.0f}% of the total was idle draw)")

    # The number to A/B on. A configuration change only counts as measured if
    # it moves this by more than one configuration's own run-to-run spread.
    if bounds and len(bounds) > 1:
        each = [energy_between(rows, a, b) for a, b in bounds]
        if tail > 0 and each:   # the last window carries the settle period
            each[-1] -= (idle_w if idle_w is not None
                         else min(r[1] for r in rows)) * tail
        each = [j for j in each if j > 0]
        if len(each) > 1:
            m_i, sd_i, cv_i = stats(each)
            print(f"  per-run energy   {m_i:8.0f} J  +/- {sd_i:.0f}  "
                  f"(CV {cv_i:.1f}%, n={len(each)})")
            print("                   " +
                  "  ".join(f"{j:.0f}" for j in each))
            if cv_i > 5:
                print(f"  NOTE: run-to-run spread is {cv_i:.1f}%. A change "
                      "smaller than that is not\n        distinguishable from "
                      "noise at this sample count.")

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
    ap.add_argument("--settle", type=float, default=5.0, metavar="SECONDS",
                    help="keep sampling this long after the command exits, to "
                         "catch the power tail the sensor reports late. Idle "
                         "draw over the extra span is netted back out "
                         "(default 5; 0 restores the old exit-bounded window)")
    ap.add_argument("--interval", type=int, default=1000,
                    help="macmon sample interval in ms (default 1000, which "
                         "is the channel's own update rate; faster only "
                         "re-reads the same value)")
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
        t0, iters, bounds = time.monotonic(), 0, []
        while True:
            a = time.monotonic()
            rc = subprocess.call(cmd, stdout=subprocess.DEVNULL)
            bounds.append((a, time.monotonic()))
            iters += 1
            if rc != 0:
                sys.exit(f"  command failed with {rc} on iteration {iters}")
            if time.monotonic() - t0 >= args.loop:
                break
        t1 = time.monotonic()

        # Keep sampling after the exit so the sensor's late-arriving power
        # tail lands inside the window. report() nets the idle draw back out.
        if args.settle > 0:
            time.sleep(args.settle)
        t_end = time.monotonic()

        rows = mon.slice(t0, t_end)
        if t1 - t0 < 10:
            print("  NOTE: run under 10s. sys_power lags load by ~2s, so even "
                  "with --settle\n  the rise is compressed and this figure is "
                  "shaky. Prefer a longer run.")
        report(rows, " ".join(cmd), args.records, iters, args.idle_watts,
               args.reference, bounds, t1)

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
