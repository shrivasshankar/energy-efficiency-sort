# energy-efficiency-sort

An external merge sort measured in **joules per record** rather than seconds.

Forked from [mergesort](https://github.com/shrivasshankar/mergesort), which chased
wall clock and got 500,000,000 records (47 GB) to 48.5s on a MacBook Pro — 5.9×
faster than GNU `sort` using 5.0× less CPU. That work is in
[docs/PERFORMANCE.md](docs/PERFORMANCE.md). **The sort code here is unchanged so
far**; everything below is measurement.

## Results

Two dataset sizes, gensort ASCII records, `valsort`-verified. Wall clock from
purged single runs; power from separate looped runs (looping inflates time
because each iteration inherits the previous one's writeback, but it gives many
more power samples). Energy is power × cold time.

| | records | split | merge | total | energy | records/joule |
|---|---|---|---|---|---|---|
| 47 GB | 5×10⁸ | 20.1s @ 25.09 W | 28.2s @ 24.85 W | 48.3s | 1,205 J | 414,911 |
| 100 GB | 1×10⁹ | 52.2s @ 26.35 W | 59.9s @ 24.49 W | 112.1s | 2,842 J | 351,911 |

100 GB checksum `1dcd615efb9dfe11`, 0 duplicate keys. Conditions: unplugged,
apps closed, idle 3.54 W, disk 55% full rising to 66%.

### Scaling is the finding, and it is not encouraging

Doubling the data cost **15% of records/joule** (414,911 → 351,911). Extrapolated
naively, 100 GB gives 28.4 kJ per 10¹⁰ records, or ~31.3 kJ once a ~10% charger
loss is added for a wall-side figure. But if the 15%-per-doubling trend continues
across the 3.32 doublings from 100 GB to 1 TB:

```
203,630 records/joule  ->  49.1 kJ  ->  54.0 kJ wall-equivalent
```

So the honest range for a 1 TB projection is **31 kJ if the degradation stops,
54 kJ if it continues** — and two points cannot distinguish those. A 200 GB run
would give the third point.

Two reasons to think part of that 15% is recoverable:

**Disk utilisation moved between the runs** — 50–60% for 47 GB, 55–66% for
100 GB. Fullness is separately measured as worth **2×** on merge throughput
between ~50% and 75%, so some of the loss is a controllable variable rather than
inherent scaling.

**The merge scaled linearly; the split did not.**

```
split   x2.60 wall time for x2.0 data     but x1.99 user time
merge   x2.12 wall time for x2.0 data
```

The merge going linear is the good news — 200-way fan-in instead of 100-way cost
nothing, and that was the part of the design most likely to break at scale. The
split's extra time is all I/O, since its CPU time scaled exactly 2.0×.

Throughput fell from 4.14 to 3.57 GB/s across the same two runs.

## Where the joules actually go

![energy breakdown](docs/energy-100gb.png)

Measured over the 100 GB looped runs (292s total, 3 split + 2 merge iterations),
so read the **shares**, not the absolute joules:

| band | share | what it is |
|---|---|---|
| idle | 13.9% | platform draw at 3.54 W, present whether or not you sort |
| SoC | 33.0% | CPU + GPU + ANE + RAM — the only band an algorithm touches |
| rest | 53.1% | SSD, regulators, board, display backlight |

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

1. **Thread count, untested.** The only lever that changes power at constant
   time. On an I/O-bound workload fewer threads may finish in the same wall clock
   at lower draw — invisible to every timing measurement taken so far.
2. **Display off, worth ~14%.** Server-class comparisons carry no display at all.
3. **Wall time**, which shrinks all three bands, but the sort is already at
   3.57 GB/s against a device that does roughly 4.
4. **I/O volume** is structurally fixed at 4× the data for a two-pass external
   sort. Nothing algorithmic changes that without making the sort less external.

## Measuring energy on Apple Silicon

`powermetrics` reports SoC package power only. Per the table above that is 33% of
the total under load and 3–6% at idle, so it cannot produce a total-system
figure.

`macmon` can, sudolessly at 1 Hz, by reading IOReport directly:

```bash
brew install macmon

./power_log.py --idle 60
./power_log.py --loop 120 --records 1e9 --idle-watts 3.54 --csv merge.csv -- ./merge_program 10
./plot_power.py --records 1e9 --idle 3.54 split.csv merge.csv -o energy.png
```

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

Every one of these silently corrupted at least one run today. Record all three:

```
power source     idle 7.97 W plugged  vs  3.54 W unplugged
open apps        11.61 W  ->  8.39 W  ->  3.74 W as they were closed
disk fullness    merge 45.6s at ~75% full  vs  24.4s at ~50%   (2x)
```

The protocol that makes runs comparable, inherited from the parent repo:

```bash
rm -f run*.dat output.dat && sync && sleep 120 && sudo purge
```

`purge` drops clean pages but cannot drop dirty ones, so benchmarking shortly
after writing tens of GB means competing with your own writeback.

## Open

- **A 200 GB run** for a third scaling point. It is the difference between a 31 kJ
  and a 54 kJ projection, and nothing else resolves that.
- **A thread-count sweep for joules**, not seconds. Untested by anyone.
- **A wall meter** for an AC-side figure.
- **Storage headroom.** A 1 TB sort needs ~2 TB of working set, and holding that
  on a 2 TB drive means running at ~75% full — the regime measured at half
  throughput. Sizing for ~50% means roughly 4 TB.
