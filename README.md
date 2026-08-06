# energy-efficiency-sort

An external merge sort measured in **joules per record** rather than seconds.

Forked from [mergesort](https://github.com/shrivasshankar/mergesort), which chased
wall clock and got 500,000,000 records (47 GB) to 48.5s on a MacBook Pro — 5.9×
faster than GNU `sort` using 5.0× less CPU. That work is in
[docs/PERFORMANCE.md](docs/PERFORMANCE.md). **The sort code here is unchanged so
far**; everything below is measurement.

## Results

gensort ASCII records, `valsort`-verified. Every figure is three cold purged
runs, with power measured **during** the run rather than reconstructed from a
separate looped one. Spread is the run-to-run standard deviation.

| 47 GB, 5×10⁸ records | time | power | energy |
|---|---|---|---|
| split | 19.6s | 22.40 W | 439 J ± 5 (1.2%) |
| merge | 21.9s | 18.77 W | 412 J ± 15 (3.7%) |
| **total** | **41.5s** | — | **851 J** |

**587,752 records/joule.** Conditions: unplugged, all apps closed, **Low Power
Mode on**, display dimmed (level not recorded), idle 2.84 W, disk 50% full,
M3 Max (10P + 4E), 1 TB internal.

The unrecorded brightness matters less than it looks: a later run of the same
configuration at a known 50% reproduced this to **0.5%**. See *Conditions that
changed measurements here* below.

### These supersede the earlier figures, which were 42% pessimistic

| | as published before | measured directly |
|---|---|---|
| 47 GB total | 1,205 J | **851 J** |
| records/joule | 414,911 | **587,752** |

Two errors compounded.

**Power was measured in the wrong regime.** Energy used to be `looped power ×
cold time`. Looping was adopted to collect more power samples, but each
iteration inherits the previous one's writeback, so it measures a contended
machine. The looped runs reported 25.09 W on the split and 24.85 W on the
merge; measured properly on cold runs the same phases draw 22.40 W and
18.77 W — **12% and 32% high**. The assumption that both regimes draw the same
watts was never tested. It does not hold.

**The power tail was discarded.** `sys_power` reports a load change about 2s
late, so ending the window at the command's exit drops the end of its own
power curve. Recovering it raised split by 17% and merge by 4% — split is
worse because 50 GB of run-file writeback is still draining when the process
exits, and that energy belongs to the run.

Three cold runs cost half the disk writes of a 120s loop, produce error bars,
and need no assumption about regime equivalence. Run-to-run spread is 1.2% on
split, so changes of a few percent are now resolvable.

**A retraction, for the record.** This section briefly carried a caveat saying
Low Power Mode had been off for the old measurement and on for the new one,
making the 42% a two-variable change. That was recorded on recollection, and
the sweep below refutes it two ways.

By power draw: the pre-settle run averaged 20.66 W on the split. Measured
LPM-off draws 36.94 W and measured LPM-on draws 23.30 W. 20.66 belongs to one
of those families and not the other.

By conservation: if that run had been LPM-off, adding the tail would have to
carry it from 374.7 J to the 642 J that LPM-off actually measures. That is
267 J deposited in a 5-second settle window — a sustained **53 W**, when the
highest sys_power ever observed in any run here is 38.97 W. Impossible.

Low Power Mode was on throughout. **The 42% is methodology alone.**

### The 100 GB row is withdrawn

The previous 100 GB figure (2,842 J, 351,911 records/joule) was taken with the
superseded method and is not comparable to the number above. The scaling
question it was being used to answer has since been measured directly at
25 vs 47 GB, so re-running it is now a third point and a fan-in probe rather
than the thing everything waits on.

## Scaling: measured at 4.2% per doubling

2.5×10⁸ against 5×10⁸ records is exactly one doubling, measured as three
**interleaved** cycles — the sizes alternate rather than running in blocks, so
thermal state and disk condition cannot load onto whichever went second. Full
sort each time, not split only, because run count goes 50 → 100 and merge
fan-in is the likeliest place a penalty hides.

| cycle | 25 GB | 47 GB | 25 GB rec/J | 47 GB rec/J | slope |
|---|---|---|---|---|---|
| 1 | 418 J | 859 J | 598,086 | 582,072 | 2.7% |
| 2 | 414 J | 881 J | 603,865 | 567,537 | 6.0% |
| 3 | 415 J | 864 J | 602,410 | 578,704 | 3.9% |

**Mean 4.2% loss per doubling.** Carried across the 4.32 doublings from 5×10⁸
to 10¹⁰ records, that projects **20.9 kJ, or 23.0 kJ wall-side** once the ~10%
charger loss `sys_power` excludes is added back.

This supersedes the previous 15%-per-doubling figure, which came from comparing
two rows both taken with the discarded looped-power method.

The 25 GB totals reproduce to **0.5%** (418 / 414 / 415 J) even though the
individual merges are short enough to trip the under-10s warning: high
within-run variance, very tight run-to-run totals.

Cycle 2's 47 GB merge is an outlier — 30.3s and 468 J against 23.1s/423 J and
23.4s/431 J, and at the *lowest* power of the three, which reads as an I/O
stall rather than extra work. Excluding it gives 3.3%. The conclusion does not
turn on which you take.

**What this does not cover is fan-in.** The split writes 5M-record runs, so
this compared a 50-way merge against a 100-way one. At 10¹⁰ records the same
configuration is **2000-way**, and at `BUFRECS=8192` the read buffers alone
want roughly `10 threads × 2000 runs × 819 KB ≈ 16 GB`. Reaching that size
means raising records-per-run, which costs RAM for the in-memory sort, or
cutting `BUFRECS`, which costs I/O efficiency. A slope measured across a 2×
change in fan-in is being extrapolated across a 20× one.

Two observations from the superseded runs that still hold, because their wall
clock was always measured on cold single runs — only the power was wrong:

**The merge scaled linearly; the split did not.**

```
split   x2.60 wall time for x2.0 data     but x1.99 user time
merge   x2.12 wall time for x2.0 data
```

The merge going linear was the good news, since fan-in was the part of the
design most likely to break. The split's extra time is device I/O, not
algorithm — its CPU scaled exactly 2.0×. Throughput fell 4.14 → 3.57 GB/s
across those two runs.

**Disk utilisation is a controllable variable rather than inherent scaling** —
separately worth 2× on merge throughput between ~50% and 75% full. The knee is
higher than that range suggests, though: 50% vs 61% moved total energy by only
0.5%, so the 2× is doing its work somewhere above 61% rather than accumulating
steadily from 50.

## Low Power Mode is the largest lever found

A four-corner sweep of the split phase, three cold runs per corner, everything
else held constant and brightness verified unchanged at both ends of the run:

| config | energy | time | power | spread |
|---|---|---|---|---|
| LPM off, P-cores | 641.7 J | 17.37s | 36.94 W | ±5 (0.8%) |
| LPM off, `-c utility` | 606.0 J | 18.43s | 32.87 W | ±13 (2.2%) |
| **LPM on, P-cores** | **441.3 J** | 18.93s | 23.30 W | ±4 (0.9%) |
| LPM on, `-c utility` | 447.3 J | 19.70s | 22.70 W | ±25 (5.5%) |

**Low Power Mode is worth 31% of the energy for 9% more wall time.** That is
larger than every algorithmic change in the parent repo combined, and it is a
system setting rather than a line of code. It was already on for the headline
figure above, which is why that figure does not move.

It wins so decisively because of the shape of the metric. Energy at a fixed
record count has no time limit in it, so trading wall clock for watts is free
until platform idle draw eats the saving. Against 2.84 W idle and 37 W under
load there is a lot of room, and capping clocks buys far more power than it
costs in time.

The reproducibility is worth noting too: `LPM on, P-cores` is the same
configuration as the headline table, measured a day apart at different
brightness and 61% vs 50% disk fullness, and it landed within **0.5%**.

## Efficiency cores: still unanswered after three attempts

The M3 Max is 10 performance + 4 efficiency cores, and the sort's 8–10 threads
all land on P-cores. Since the workload is I/O-bound at 3.57 GB/s against a
device that does roughly 4, E-cores look like the obvious lever.

Three attempts, none of which measured what they were meant to:

| | split time | SoC power | what actually happened |
|---|---|---|---|
| baseline (LPM on) | 18.9s | 7.4 W | — |
| `taskpolicy -b` | 87.3s | 0.46 W | throttled I/O, QoS never changed |
| `-c background -d default` | 88.7s | 0.46 W | throttled I/O, wrong scope |
| `-c utility` | 19.7s | 7.5 W | no relocation at all |

The first two are ~4.5× slower, and their agreeing with each other is the
finding rather than the slowdown. A QoS probe shows `-b` calls
`setpriority(PRIO_DARWIN_BG)` and never sets a QoS clamp, so it moves nothing
onto an E-core; `-c background` does. Two unrelated mechanisms landing within
1.6% of each other means the cause is common to both, and so is not core
placement. It is disk throttling, which 0.46 W of SoC power confirms — four
saturated E-cores would draw several times that, so the CPU is asleep waiting
on I/O.

`-c utility` has the opposite problem. Its time and SoC power sit within a few
percent of the plain baseline, meaning the threads never left the P-cores:
utility QoS is a *preference*, the machine had idle P-cores, and the scheduler
used them.

So one flag forces E-cores but strangles I/O, and the other leaves I/O alone
but does not force E-cores. **One combination remains untried:
`taskpolicy -c background -g default`.** `-d` sets `IOPOL_SCOPE_PROCESS`, but
background QoS throttles at `IOPOL_SCOPE_DARWIN_BG`, and `-g` is the flag for
that scope. If that fails too, the next step is per-thread QoS in code — noting
that Apple Silicon exposes no CPU affinity API, so QoS is the only handle that
exists.

## Where the joules actually go

![energy breakdown](docs/energy-100gb.png)

Measured over the 100 GB looped runs (292s total, 3 split + 2 merge iterations),
so read the **shares**, not the absolute joules:

| band | share | what it is |
|---|---|---|
| idle | 13.9% | platform draw at 3.54 W, present whether or not you sort |
| SoC | 33.0% | CPU + GPU + ANE + RAM — the only band an algorithm touches |
| rest | 53.1% | SSD, regulators, board, display backlight |

The provisioning sweep corroborates the SoC share independently: 31–32% under
Low Power Mode and 37–38% without it, against the 33.0% above. So the shares
hold even though the absolute joules came from the superseded method.

**Two thirds of the energy is not computation.** A change that halved SoC work
would cut at most 16% of the total — and the parent repo already measured a 25%
reduction in comparison cost producing *zero* wall-clock gain, so it would not
even reach that.

The power distribution says the same thing from the other side. There are no
large idle gaps to reclaim:

```
split   6% of samples under 15 W,  40.9% in 30-50 W
merge   8% of samples under 15 W,  73.5% in 20-30 W
```

### So the levers are not algorithmic

1. **Low Power Mode — measured, and it is the big one.** 31% of the energy for
   9% more wall time. It caps clocks without touching disk policy, which is
   exactly where every E-core attempt died. Already applied to the headline
   figure.
2. **Core type, still untested after three attempts.** The other lever that
   changes power at constant time — see above for why none of the three
   actually measured it.
3. **Thread count, untested.** Distinct from core type: `merge_program 4` gives
   four threads that macOS remains free to place on P-cores.
4. **Display off, nominally ~14%** — but treat that with suspicion. It comes
   from the superseded looped runs, and going from "barely visible" to 50%
   brightness moved total energy by only 0.5%. Either marginal backlight draw
   is small at these levels and most of the 14% is the panel itself, or the 14%
   is wrong. Worth measuring as a real config rather than inheriting it from a
   band breakdown. It is also the like-for-like condition against any headless
   machine, which has no panel to power at all.
5. **Wall time**, which shrinks all three bands, but the sort is already at
   3.57 GB/s against a device that does roughly 4.
6. **I/O volume** is 4× the data for a two-pass external sort — less fixed than
   it looks. gensort ASCII records are 10 random key bytes followed by ~88 bytes
   of low-entropy filler, and compress **2.63× with lz4, 3.56× with zstd -1**.
   Compressing the run files, two of the four data movements, would cut total
   bytes moved to roughly 2.6× against an SSD that sits inside the 53% band.
   Measured on the data; untested in the sort. Note it would shift the workload
   toward CPU-bound, which changes what a cross-platform comparison compares.

## The merge is half kernel time

At 100 GB the merge spent **68.2s in sys against 70.2s in user** — about half
its CPU is the kernel copying pages to service 200-way concurrent reads rather
than merging anything. The split has the opposite profile: as the scaling
section shows, its extra time is device I/O, not kernel work.

Two attacks, neither resolved:

- **Larger `BUFRECS`** — fewer, bigger reads per stream. Unlike the output
  buffer this is on the read side, where there is no writer to starve.
- **Page-aligned direct I/O.** `F_NOCACHE` was tried *unaligned* in the parent
  repo and measured −13s sys for +4s wall, rejected on wall clock. On an energy
  metric that trade deserves recomputing rather than inheriting — but the
  break-even is steep. On today's 21.9s merge, +4s means power must fall from
  18.77 W to 15.9 W, a 15% cut in total draw or roughly half the entire SoC
  band, and `F_NOCACHE` only touches part of it.

Both figures are wall-clock verdicts from before the measurement fixes, so
neither should be believed until re-measured in joules.

## Measuring energy on Apple Silicon

`powermetrics` reports SoC package power only. Per the table above that is 33% of
the total under load and 3–6% at idle, so it cannot produce a total-system
figure.

`macmon` can, sudolessly at 1 Hz, by reading IOReport directly:

```bash
brew install macmon

./power_log.py --idle 60          # establish the baseline first
for i in 1 2 3; do
    rm -f run*.dat(N); sync && sleep 120 && sudo purge
    ./power_log.py --records 5e8 --idle-watts 2.84 --csv merge-$i.csv -- ./merge_program 10
done
```

Two harnesses run that protocol unattended, both holding a `caffeinate`
assertion so the display cannot sleep mid-protocol and shift the draw between
runs:

- [`sweep_provisioning.sh`](sweep_provisioning.sh) — sweeps several
  configurations, derives its win threshold from a baseline it measures itself
  rather than one carried in, and records brightness at both ends so a change
  is caught rather than absorbed.
- [`sweep_scaling.sh`](sweep_scaling.sh) — measures the slope by interleaving
  two data sizes, so drift lands on both equally, and prints disk fullness
  before every timed run.

Three cold runs, not one loop. `--interval` defaults to 1000 ms because the
IOReport channel behind `sys_power` only updates at 1 Hz — polling at 500 ms
returns every second value twice, which does not bias the integral but does
halve the real sample count and would deflate any standard deviation computed
from it. `--settle` defaults to 5 s and keeps sampling past the command's exit
so the late-reported power tail lands inside the window, netting out idle draw
across the extra seconds.

`sys_power` is the whole-system channel; `all_power` is the SoC subset. Cross-check:
in matched machine states `sys_power` agrees with the battery gauge within ~10%
(9.23 vs ~8.4 W, and 3.54 vs 3.74 W).

**It is still DC-side.** Charger conversion loss is excluded by construction, so
a wall meter should read ~10% higher. Anything published needs the wall reading.

### The battery gauge does not work for this. Four attempts:

| approach | result |
|---|---|
| integrate `InstantAmperage` | refreshes every **19.43s** (2,948 polls, 1 change in 40s). One 28s run gave 2 distinct values across 168 samples |
| `AppleRawCurrentCapacity` delta | went **up 17 mAh over 100s while discharging**. It is a state-of-charge estimate with model corrections, not a coulomb ledger |
| `AccumulatedBatteryPower` | did not change at all in 60s; update interval unknown |
| average the slow readings over 4 min | produced a 0.70 W sample during an active merge, sd 9.31 on a mean of 17.25 |

The gauge exists to say "3:58 remaining" — smooth and stable by design, which is
the opposite of what a 30-second measurement needs. `FilteredCurrent` sits beside
`InstantAmperage` in the same ioreg dump.

## Conditions that changed measurements here

Every one of these has silently corrupted at least one run. Record all five:

```
low power mode   split 642 J off  vs  441 J on                  (31%, the largest)
power source     idle 7.97 W plugged  vs  3.54 W unplugged
open apps        11.61 W  ->  8.39 W  ->  3.74 W as they were closed
disk fullness    merge 45.6s at ~75% full  vs  24.4s at ~50%    (2x)
display sleep    displaysleep is 2 min and the drain wait is 2 min, so the
                 panel can sleep for some runs and not others
```

The last one is why the sweep script holds a `caffeinate` assertion. It is not
convenience — without it the backlight state is a coin flip per run, and the
backlight is watts.

Brightness itself matters less than expected — "barely visible" and 50% differ
by 0.5% of total energy — but it still has to be held constant. See the levers
list for why that does not settle the display-off question.

The protocol that makes runs comparable, inherited from the parent repo:

```bash
rm -f run*.dat output.dat && sync && sleep 120 && sudo purge
```

`purge` drops clean pages but cannot drop dirty ones, so benchmarking shortly
after writing tens of GB means competing with your own writeback.

## Open

In priority order:

- **`taskpolicy -c background -g default`.** The last untried flag combination
  that could isolate core type from disk policy. If it fails, per-thread QoS in
  code is the only remaining route.
- **Confirm Low Power Mode on the merge phase.** The 31% is measured on split
  only; merge is I/O-heavier and may not behave the same.
- **Display fully off** as a measured config, to settle whether the inherited
  ~14% is real.
- **The Linux side.** The point of this fork is Apple Silicon versus x86 as a
  platform, and there is no x86 figure here measured the same way. Decide the
  basis before measuring: 53% of the energy is SSD, board and display, so a
  whole-system comparison is largely chassis. SoC-vs-RAPL and
  wall-meter-vs-wall-meter answer different questions.
- **Re-measure 100 GB** with the three-cold-run protocol. Now a third scaling
  point and a 200-way fan-in check rather than the blocker it used to be —
  though note its ~200 GB working set puts this machine near 70% full, which
  confounds it against the 54–59% the slope was measured at.
- **A wall meter** for an AC-side figure. Everything here is DC-side and ~10%
  optimistic by construction.
- **A thread-count sweep for joules**, not seconds. Still untested.
