# energy-efficiency-sort

An external merge sort optimized for **joules per record**, not seconds.

Forked from [mergesort](https://github.com/shrivasshankar/mergesort), which chased
wall clock and got 500,000,000 records (47 GB) down to 48.5s on a MacBook Pro —
5.9× faster than GNU `sort` using 5.0× less CPU. That work is in
[docs/PERFORMANCE.md](docs/PERFORMANCE.md); the sort itself is unchanged so far.

Energy is a different optimization target than time, and mostly an unexplored one
here. Wall clock and joules only correlate while power draw is constant, and on an
I/O-bound workload it isn't.

## Measuring energy

`powermetrics` reports SoC package power only — CPU, GPU, DRAM, ANE. It misses the
SSD, the display, and everything else on the board, so it cannot produce a
total-system number.

The battery gauge can, and needs no `sudo`. While discharging, `InstantAmperage ×
Voltage` from `ioreg -rn AppleSmartBattery` is every watt leaving the pack, which
is every watt the machine consumed.

```bash
./power_log.py -- ./merge_program 10        # energy for one command
./power_log.py --idle 20                    # baseline draw
./power_log.py --csv run.csv -- ./split_program 5000000 8
```

Two things the tool gets right that a naive version would not. `InstantAmperage`
is printed as an unsigned 64-bit value, so it needs exact integer conversion — as
a double it rounds to 2⁶⁴ and the sign is lost. And it reports the coulomb counter
as a cross-check only over windows long enough to mean something.

## Findings so far

**Idle draw is 44% of the joules.** The merge measured 26.56 W mean against an
11.61 W idle floor. Nearly half the energy of a sort on this machine is the machine
existing, not the sort — which means running headless is a real optimization, and
that any comparison against a machine with no display attached is not like-for-like.

**The coulomb counter is useless below ~2 minutes.** It read −1 mAh over 12
seconds when the true drain was 3.2 mAh. Quantised to 1 mAh (~42 J) and refreshed
on the order of tens of seconds, so short runs have to rely on integrated power.

**Battery-side measurement is not wall power.** It excludes charger conversion
loss by construction, so it is a development instrument rather than a rigorous
one. A laptop on AC draws system power *plus* battery charging, which corrupts a
wall reading unless the pack is full and idle.

## Open

- **Compliant measurement.** See above. Needs a wall meter and a protocol for the
  charging problem.
- **Scratch space.** Sorting well above RAM needs roughly 2× the dataset on disk,
  since run files cannot be freed incrementally — the merge reads from all of them
  until nearly the end. That bounds how large a run this machine can host.
- **Energy-specific tuning, none of which exists yet.** Display off during the
  run. Thread count swept for joules rather than seconds — on an I/O-bound
  workload, fewer threads may finish in the same wall time at lower power, which is
  invisible to every measurement taken so far. Lower `BUFRECS` to cut the merge's
  35s of kernel time, since kernel time is watts too.

## Scaling

At 20× the current dataset the split produces 2000 runs, and a single-pass merge
would need 2000 × 800 KB × 10 threads = 16.4 GB of read buffers against 36 GiB of
RAM. It fits, which matters: a second merge pass would double the I/O and roughly
halve records-per-joule.

## Protocol

Energy measurements inherit the wall-clock protocol from the parent repo, which is
not optional — machine state was worth 53s on a 100s run there, more than every
code change combined.

```bash
rm -f run*.dat output.dat && sync && sleep 120 && sudo purge
```

`purge` drops clean pages but cannot drop dirty ones. Benchmarking shortly after
writing tens of GB means competing with your own writeback. Keep the volume under
~50% full; at 75% the merge ran half as fast for the same code.
