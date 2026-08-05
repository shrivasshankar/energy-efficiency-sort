#!/usr/bin/env python3
"""Measure whole-system energy for a command, using the battery gauge.

Why the battery and not powermetrics: powermetrics reports SoC package power
(CPU/GPU/DRAM/ANE) and misses the SSD, display, and everything else on the
board. Total system energy is the number that matters. On a laptop on battery,
the battery's own current sensor measures exactly that -- every watt leaving
the pack is a watt the machine consumed. No sudo required.

Two independent numbers are reported:

  1. Integrated power   -- InstantAmperage * Voltage sampled over time and
                           trapezoid-integrated. Fine-grained, but only as
                           good as the gauge's own refresh rate.
  2. Coulomb counter    -- AppleRawCurrentCapacity delta (mAh) * mean voltage.
                           Coarse (1 mAh ~= 42 J) but it is the gauge's own
                           accounting, so it is an independent check. If these
                           two disagree by much, distrust both.

Usage:
    ./power_log.py -- ./split_program 5000000 8 2
    ./power_log.py --idle 20               # characterise baseline draw
    ./power_log.py --csv run.csv -- ./merge_program 10

Not a rigorous total-system figure on its own: a proper measurement is AC-side,
which includes the charger conversion loss this method excludes by construction.
Treat this as the development instrument -- it tells you whether a change moved
joules, which is what you need day to day.
"""

import argparse
import re
import subprocess
import sys
import threading
import time

IOREG = ["ioreg", "-rn", "AppleSmartBattery"]
RE_AMP = re.compile(r'"InstantAmperage" = (\d+)')
RE_VOLT = re.compile(r'"Voltage" = (\d+)')
RE_CAP = re.compile(r'"AppleRawCurrentCapacity" = (\d+)')
RE_EXT = re.compile(r'"ExternalConnected" = (Yes|No)')


def probe():
    """Return (watts, volts, capacity_mAh, on_ac). Raises on unparseable output."""
    out = subprocess.run(IOREG, capture_output=True, text=True).stdout
    m_a, m_v, m_c = RE_AMP.search(out), RE_VOLT.search(out), RE_CAP.search(out)
    if not (m_a and m_v and m_c):
        raise RuntimeError("could not read battery gauge from ioreg")

    amp = int(m_a.group(1))
    # ioreg prints the signed 64-bit amperage as unsigned. Discharge is
    # negative. Must be done in exact integers -- as a float this value
    # rounds to 2**64 and the subtraction yields zero.
    if amp >= 2**63:
        amp -= 2**64

    volts = int(m_v.group(1)) / 1000.0
    watts = (-amp / 1000.0) * volts           # positive while discharging
    m_e = RE_EXT.search(out)
    return watts, volts, int(m_c.group(1)), (m_e and m_e.group(1) == "Yes")


def preflight():
    try:
        watts, volts, cap, on_ac = probe()
    except Exception as e:
        sys.exit(f"cannot read battery: {e}")

    if on_ac:
        sys.exit("ERROR: on AC power. The battery current sensor only measures\n"
                 "system draw while discharging. Unplug and rerun.")

    lpm = subprocess.run(["pmset", "-g"], capture_output=True, text=True).stdout
    m = re.search(r"lowpowermode\s+(\d+)", lpm)
    if m and m.group(1) != "0":
        print("WARNING: Low Power Mode is ON -- it caps performance. "
              "Joules and seconds will both be wrong for comparison.\n")

    print(f"  baseline: {watts:.2f} W at {volts:.3f} V, {cap} mAh in pack")
    return volts, cap


class Sampler(threading.Thread):
    def __init__(self, hz):
        super().__init__(daemon=True)
        self.interval = 1.0 / hz
        self.stop = threading.Event()
        self.samples = []          # (monotonic_t, watts)

    def run(self):
        while not self.stop.is_set():
            try:
                w, _, _, _ = probe()
                self.samples.append((time.monotonic(), w))
            except Exception:
                pass                # a dropped sample beats a crashed run
            self.stop.wait(self.interval)

    def joules(self):
        """Trapezoid-integrate the power samples."""
        s = self.samples
        if len(s) < 2:
            return 0.0, 0.0, 0.0
        total = sum((s[i + 1][0] - s[i][0]) * (s[i + 1][1] + s[i][1]) / 2.0
                    for i in range(len(s) - 1))
        span = s[-1][0] - s[0][0]
        return total, (total / span if span else 0.0), max(w for _, w in s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hz", type=float, default=4.0,
                    help="sample rate; the gauge itself updates ~1Hz so more "
                         "than ~4 buys little (default 4)")
    ap.add_argument("--idle", type=float, metavar="SECONDS",
                    help="measure idle draw for N seconds instead of running a command")
    ap.add_argument("--csv", metavar="FILE", help="write raw samples here")
    ap.add_argument("cmd", nargs=argparse.REMAINDER,
                    help="-- followed by the command to measure")
    args = ap.parse_args()

    cmd = args.cmd[1:] if args.cmd and args.cmd[0] == "--" else args.cmd
    if not cmd and args.idle is None:
        ap.error("give a command after -- , or use --idle SECONDS")

    v0, cap0 = preflight()

    sampler = Sampler(args.hz)
    t0 = time.monotonic()
    sampler.start()

    rc = 0
    if args.idle is not None:
        print(f"  measuring idle for {args.idle}s ...")
        time.sleep(args.idle)
    else:
        print(f"  running: {' '.join(cmd)}\n")
        rc = subprocess.call(cmd)

    sampler.stop.set()
    sampler.join(timeout=2.0)
    elapsed = time.monotonic() - t0

    _, _, cap1, _ = probe()
    total_j, mean_w, peak_w = sampler.joules()

    # Independent cross-check: the gauge's own coulomb count.
    # mAh -> Wh -> J, using the mean of start and end voltage.
    v1 = probe()[1]
    coulomb_j = (cap0 - cap1) / 1000.0 * ((v0 + v1) / 2.0) * 3600.0

    print(f"\n  elapsed        {elapsed:8.2f} s")
    print(f"  mean power     {mean_w:8.2f} W   (peak {peak_w:.2f} W, "
          f"{len(sampler.samples)} samples)")
    print(f"  energy         {total_j:8.1f} J   <- integrated power")

    # The capacity field is quantised to 1 mAh (~42 J) and the gauge refreshes
    # on the order of tens of seconds, so over a short window it can even read
    # negative. Measured: -1 mAh over 12s when the true drain was ~3.2 mAh.
    # Only report it where it can actually mean something.
    if elapsed < 120:
        need = total_j / ((v0 + v1) / 2.0) / 3600.0 * 1000.0
        print(f"  cross-check         --     coulomb counter needs >2min "
              f"(window implies only {need:.0f} mAh, quantised to 1)")
    else:
        print(f"  cross-check    {coulomb_j:8.1f} J   <- coulomb counter "
              f"({cap0 - cap1} mAh)")
        if total_j > 0 and coulomb_j > 0:
            print(f"  agreement      {100.0 * min(total_j, coulomb_j) / max(total_j, coulomb_j):7.1f} %")
    print(f"  Wh             {total_j / 3600.0:8.4f} Wh")

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("seconds,watts\n")
            base = sampler.samples[0][0] if sampler.samples else 0
            for t, w in sampler.samples:
                f.write(f"{t - base:.3f},{w:.3f}\n")
        print(f"  samples        {args.csv}")

    sys.exit(rc)


if __name__ == "__main__":
    main()
