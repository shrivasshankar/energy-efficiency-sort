# External Merge Sort

A multithreaded external sort in C++17 for datasets larger than memory. Sorts 500,000,000 fixed-size records (~47 GB) in **71 seconds** on a laptop from a cold page cache, verified byte-exact with `valsort`.

Records are the standard [gensort](http://www.ordinal.com/gensort.html) format:

```
[key (10 bytes)][value (90 bytes)]   = 100 bytes
```

Fixed-size records are what make most of this possible — record *i* always lives at byte *i × 100*, so any thread can compute where to read, and a sorted run can be binary-searched on disk.

Both phases do all file I/O through `pread`/`pwrite` on shared descriptors. That is not a stylistic preference — see [the `iostream` finding](#the-iostream-finding), which is the single largest optimization in the project.

---

## Design

Two phases, both parallel.

### Split — produce sorted runs

Each thread claims a chunk index from a shared `atomic<size_t>` counter, `pread`s that chunk at a computed byte offset, and `pwrite`s one sorted run. No shared reader, no barrier, no locks — each thread owns its own buffers and writes its own file.

Within a chunk, only a small key+index array is sorted, not the records:

```
read chunk  ->  build [key(10) | idx(4)] array  ->  sort THAT
            ->  gather full records in sorted order (one pass)
            ->  one sequential write
```

Sorting 16-byte entries instead of 100-byte records keeps far more of the working set in cache, and the records get moved exactly once.

### Merge — sampled splitters inside a k-way merge

This stays a merge sort. Samplesort contributes only its *splitter selection*, which cuts one k-way merge into `P` independent k-way merges over disjoint key ranges:

1. **Sample** — read evenly-spaced keys from each run. Runs are already sorted, so evenly-spaced records *are* that run's quantiles. Oversampled 32× per bucket.
2. **Cut table** — binary search each run for each splitter. `cut[r][j]` is where segment `j` begins in run `r`.
3. **Offsets** — sum segment sizes to get each thread's exact starting byte in the output.
4. **Merge** — `P` threads each run a buffered k-way merge over their key range and `pwrite` into their own region of one preallocated file.

Steps 1 and 2 are themselves parallel over runs. Each run is independent, and because every probe is a `pread` — offset passed per call, no seek cursor to serialize on — all threads share one descriptor per run. Done serially these two steps were thousands of *dependent* probes at queue depth 1: all latency, no throughput.

Because the runs are already sorted, the partition costs about **16,000 ten-byte probes** instead of the full read-and-write pass a real samplesort would need. That is roughly 2% of the merge, and it is the entire price of parallelism.

Segments are range-disjoint and written in order, so concatenation is implicit — **there is no final merge step**.

---

## Build and run

```bash
make
./split_program 5000000 8      # chunk size, threads
./merge_program 10             # threads
valsort output.dat
```

Both phases default to `hardware_concurrency()` if the thread count is omitted. `merge_program` takes an optional second argument for buffer records per stream (default 8192, ≈800 KB).

---

## Benchmark history

500,000,000 records / ~47 GB throughout, on the same machine: MacBook Pro, Apple Silicon, 10 performance cores (14 total), 36 GB RAM, 1 TB Apple SSD.

| version | split | merge | total | vs. v1 |
|---|---|---|---|---|
| **v1** — KV separation with a value log | 1323.6s | 1108.1s | 40.5 min | 1.0× |
| **v2** — sorted runs, key+index sort | 86.4s | 503s | 9.8 min | 4.1× |
| **v3** — buffered merge reads | 86.4s | 153.6s | 4.0 min | 10.1× |
| **v4** — parallel split + sampled splitters | 34.7s | 50.9s | 85.6s | 28.4× |
| **v5** — `pread`/`pwrite` instead of `iostream` | 30.0s | 40.8s | 70.8s | 34.3× |
| **v6** — parallel merge setup + decoupled merge writer | 31.2s | 32.7s | 63.9s | 38.0× |
| **v7** — decoupled split writer | **28.4s** | **26.8s** | **55.2s** | **44.0×** |

v1 used a 10M-record chunk (50 runs); v2 onward use 5M (100 runs).

**v1–v5 are medians; v6 and v7 are single cold trials.** They are not the same
kind of number, and this project has measured the same binary 13.5s apart on
different nights — so read the v6/v7 rows as one observation each, not as a
median. The per-change measurements below are the defensible ones.

**v5 is cold-start**: `sudo purge` before each trial, medians of 4. Individual samples were split 27.2 / 29.4 / 30.5 / 33.5 and merge 37.1 / 38.1 / 43.5 / 43.8 — a 6s spread on both, so treat single runs as indicative only.

**v6 adds two changes to the merge** — parallel sampling and cut table, and a
decoupled output writer. Measured cold end-to-end on 2026-07-30: split 31.17s,
merge 32.73s, **63.90s total**, `valsort` checksum `ee6b6a9da7427ce`. That is a
**single trial, not a median of 4**, so it is not directly comparable to the v5
row above — and it came out *slower in total* than an earlier v6 run at 61.23s,
entirely because split drew 31.17s that night against 23.86s the night before,
on byte-identical code.

The merge change itself is the part that is properly measured: a 3-cycle
interleaved cold A/B (purge before every run, one binary switched by a single
argument) gave **−12.5%**, winning all three pairs at −3.54 / −5.00 / −2.93s,
and end-to-end the merge went 37.35s → 32.73s, −12.4%. It is the first change
since the `iostream` fix to convert to wall clock.

Two results worth recording from that A/B. **A 52 MB output buffer is 8.9s
slower than 800 KB once a writer thread exists** — coarse handoffs starve the
writer between bursts, the reverse of the CPU-side reasoning that motivated the
big buffer. And **split, not merge, then decided the total**: it was the larger
phase and owned a 7.3s swing on unchanged code.

**v7 applies the same writer to split**, which had the identical flaw: each
thread ran `pread` → sort → `pwrite` serialized, so overlap happened only by
accident when eight threads drifted out of phase. Cold end-to-end on an idle
machine, 2026-07-30:

```
split  28.44s
merge  26.80s
─────
55.24s total, 47 GB, checksum ee6b6a9da7427ce, 0 duplicate keys
```

**The first run under 60.00 seconds** — the Sort Benchmark's MinuteSort bound —
by 4.76s, and 5.99s better than the previous best total of 61.23s.

Two limits on that number, both important. It is **one trial**: this project has
measured the same binary at 28.5s and 42s hours apart, so a single run is
indicative, not a result. And it is **unattributed** — that run had both the
split writer and a cooler machine than the 31.17s split it beat, so the depth A/B
has not yet separated the change from the conditions.

Both phases now run at 3.5–3.7 GB/s mixed against a measured 2.73–4.34 GB/s
device band. Whatever remains is not another serialization of the kind the two
writer changes removed.

Note that only *split* is genuinely cold. The merge reads run files that split wrote seconds earlier, so they are in page cache no matter what you purge beforehand. That is not a measurement artifact — it is what happens when you actually run the pipeline, and forcing those reads cold would measure a scenario that never occurs.

Earlier versions in this table were measured warm and are therefore slightly flattered; v4 measured cold would be a little above 85.6s.

Three of the five steps had nothing to do with the algorithm:

- **v1 → v2** killed the value log. Sorting key+pointer pairs and chasing pointers meant random reads across the value file during the merge. Storing full records in sorted runs made every read sequential.
- **v2 → v3** was one idea: read ~800 KB at a time instead of 100 bytes. The old merge issued ~6.1M read syscalls at ~60 µs each ≈ 380s of pure latency, matching the 384s of measured I/O wait almost exactly. **Worth 3.27× — more than threading.**
- **v4 → v5** removed `iostream` from both hot paths. See below.

---

## The `iostream` finding

The largest single optimization, and it existed because two of four I/O paths disagreed with the other two.

`iostream` does not hand a large request to the kernel. It routes it through `basic_filebuf`'s internal buffer — 4 KB on libc++. An 800 KB read becomes ~200 sequential 4 KB round-trips, each one blocking before the next is issued, so the SSD never sees a deep queue.

Measured directly, same byte count, same call size:

| 10 GB read, 800 KB chunks | wall | user | sys |
|---|---|---|---|
| `ifstream` | 1.74s | 0.48s | 1.25s |
| `pread` | **0.60s** | 0.00s | 0.59s |

| 10 GB written, 500 MB per call | wall | user | sys |
|---|---|---|---|
| `ofstream` | 8.72s | 0.59s | 8.02s |
| `pwrite` | **1.69s** | 0.01s | 1.26s |

**2.9× on reads, 5.2× on writes.** The bug survived because it was diagonal:

| | reads | writes |
|---|---|---|
| **split** | `pread` ✓ | `ofstream` ✗ |
| **merge** | `ifstream` ✗ | `pwrite` ✓ |

Each file looked correct in isolation. Nothing prompts you to compare *split's writes* against *merge's writes* specifically. The method that found it was not profiling — it was lining up every path that does the same job and checking they agree.

Fixing the merge read path was worth **11 seconds (21.6%)**. Fixing the split write path removed **83 seconds of system time** and no wall clock at all, for reasons covered next.

---

## Knowing when to stop

Raw device throughput, measured with `dd` to give the sort's numbers a reference point:

| | throughput |
|---|---|
| pure read (40 GB) | 6.19 GB/s |
| pure write (30 GB, sync'd) | 2.85 GB/s |
| mixed read+write, 3 samples | 2.73 / 3.69 / 4.34 GB/s |

**That last row is the honest one, and it is why this section does not quote a percentage.** Repeating the identical mixed test gave a 60% spread, and the `sync`'d run came out *faster* than the unsync'd one — impossible if the measurement were stable. A single `dd` sample is not a ceiling.

For scale: each phase moves 100 GB (50 read + 50 written). At v5 that put split at ~3.3 GB/s and merge at ~2.45 GB/s. As of v7 they run at **3.52 and 3.73 GB/s** — both now in the upper half of the band raw `dd` produces on this machine, where the merge previously sat below it. That is suggestive of being near the limit; it is not proof, and an earlier draft of this README overclaimed it as "95% of ceiling."

It does, however, change what is left. The two changes that converted in v6/v7 both removed a *serialization* — reads and writes never overlapping — rather than making any single operation faster. With both phases now inside the mixed-I/O band, there is no comparable structural win the measurements point at.

The stronger evidence is behavioral. Four optimizations were implemented and measured, and **exactly one produced a speedup:**

| change | CPU effect | wall effect |
|---|---|---|
| merge `ifstream` → `pread` | ~0 | **−11s** ✅ |
| split `ofstream` → `pwrite` | −83s sys | none |
| `F_NOCACHE` on output (page-cache bypass) | −13s sys | none (slightly worse) |
| inlined `uint64` key compare vs `memcmp` | −7.8s user | none |

The pattern is consistent: **below the disk ceiling, CPU work costs time; at the ceiling, it costs nothing.** The `pread` fix landed while the merge was at 71% of ceiling and had room. Everything attempted afterward produced real, verified, reproducible CPU savings and zero speedup.

Two of those are worth keeping anyway — `pwrite` frees 83 CPU-seconds, which matters when anything else is running. Two were reverted:

- `F_NOCACHE` eliminated a full memory-to-memory copy of 50 GB and still lost ~4s. The page cache's asynchronous writeback is worth more than its copy costs.
- The inlined comparator cut 25% of user time. Reverted for simplicity — a faster version that buys no speed is not worth the extra code.

---

## Detailed results

### Merge — thread scaling

Medians of 3 round-robin cycles at 47 GB, 100 runs (v4 code).

| P | time | speedup | user | sys | CPU | effective R+W |
|---|---|---|---|---|---|---|
| 1 | 153.6s | 1.00× | 29.4s | 27.7s | 37% | 651 MB/s |
| 2 | 84.9s | 1.81× | 29.9s | 30.0s | 71% | 1.18 GB/s |
| 4 | 63.4s | 2.43× | 30.8s | 36.1s | 106% | 1.58 GB/s |
| 8 | 52.1s | 2.95× | 31.5s | 38.5s | 134% | 1.92 GB/s |
| **10** | **51.3s** | **3.00×** | 31.6s | 35.6s | 131% | **1.95 GB/s** |

`user` time is flat at ~30s across every thread count. The threads add no computation — they reclaim I/O wait. Throughput triples because an NVMe SSD needs multiple outstanding requests to reach its bandwidth; one thread issuing sequential reads leaves the device mostly idle.

### Split — thread scaling

Medians of 2 cycles, 5M-record chunks (v4 code).

| config | time | speedup | user | sys | peak RSS |
|---|---|---|---|---|---|
| serial | 86.4s | 1.00× | 53.1s | 30.0s | 1.08 GB |
| T=1 | 78.9s | 1.10× | 47.1s | 28.3s | 1.08 GB |
| T=2 | 42.7s | 2.02× | 48.2s | 30.6s | 2.16 GB |
| T=4 | 35.8s | 2.41× | 50.2s | 59.5s | 4.32 GB |
| **T=8** | **32.0s** | **2.70×** | 53.1s | 118.2s | 8.64 GB |
| T=10 | 33.4s | 2.59× | 54.4s | 150.4s | 10.80 GB |

Memory is `T × (2 × CHUNK × 100 + CHUNK × 16)`, matching measured RSS exactly.

`T=1` is only **1.10×** over serial. That row isolates the bulk-read change — one `pread` per chunk instead of 500M single-record reads. Nearly worthless here, because split reads one file front to back and the kernel's readahead already covered it. The *same* change was worth 3.27× in the merge, where reads jump between 100 files and readahead cannot predict anything. **Access pattern, not read size, decides whether readahead can help you.**

### Splitter balance

Direct evidence that the sampling works. 32× oversampling.

| P | min segment | max segment | ideal | max over ideal |
|---|---|---|---|---|
| 2 | 246,370,442 | 253,629,558 | 250,000,000 | +1.45% |
| 4 | 124,056,354 | 127,615,793 | 125,000,000 | +2.09% |
| 8 | 62,169,205 | 63,988,374 | 62,500,000 | +2.38% |
| 10 | 49,747,800 | 51,232,565 | 50,000,000 | +2.47% |

Speed alone cannot tell you whether the splitters worked — a slow run and a lopsided run look identical from outside. These three numbers separate them, and they are what ruled out load imbalance as the cause of the 3× ceiling.

### Buffer size is an I/O parameter, not a CPU one

Merge at P=10, 5 GB dataset, varying records buffered per stream.

| BUFRECS | buffer memory | cold | cached |
|---|---|---|---|
| 256 | 25 MB | 8.30s | 2.61s |
| 512 | 51 MB | 4.46s | 2.05s |
| 1024 | 102 MB | 3.36s | 1.94s |
| 2048 | 204 MB | 2.56s | 1.97s |
| **4096** | 409 MB | **2.29s** | 1.98s |
| 8192 | 819 MB | 2.65s | 2.01s |
| 16384 | 1638 MB | 3.17s | 2.28s |

Cold, 256 → 4096 is **3.6×**. Cached, everything from 1024 to 8192 is flat within noise. The right value depends entirely on whether the data fits in RAM. At 47 GB with `pread`, 8192 measured best; 2048 was clearly worse (46.8s vs 38.7s).

### Creating files costs more than overwriting them

Split at T=8, identical except whether `run*.dat` already existed:

| | files existed | deleted first |
|---|---|---|
| time | **30.20s** | 37.30s |
| user | 52.56s | 52.71s |
| sys | **119.64s** | **183.69s** |

User time identical to 0.3% — same computation. All 7 seconds of wall clock is 64 extra seconds of kernel time, about **0.64s per file created** when 8 threads create them concurrently.

This also explains the chunk-size sweep, where every run deleted its output first:

| chunk | files | time | sys |
|---|---|---|---|
| 1.25M | 400 | 63.2s | 350s |
| 2.5M | 200 | 46.0s | 227s |
| 5M | 100 | 38.5s | 174s |

Fitting those three points gives **sys ≈ 115s + 0.59s × (number of files)**. Two predictions fall out, both confirmed against data the fit never saw: per-file cost ~0.6s (measured 0.64s above), and a fixed component equal to the overwrite case's system time (measured 119.6s against a predicted 115s).

The mechanism is filesystem metadata contention between concurrent writers. The serial split barely notices create-vs-overwrite (87.3s vs 85.5s, 2%) because it writes one file at a time.

### Run count does not matter

| merge, P=10 | K=50 | K=100 |
|---|---|---|
| time | 51.67s | 51.81s |
| CPU | 61.6s | 68.9s |
| peak RSS | 426 MB | 842 MB |

Halving the run count halves the concurrent read streams and cuts CPU 10.6% — and changes wall clock by 0.3%. Another confirmation that the merge is not CPU-bound. K=100 wins overall because split strongly prefers smaller chunks.

---

## Verification

Correctness is checked three ways, not by inspection.

**Byte-identical runs.** Keys are unique, so a sorted chunk has exactly one correct answer — the parallel split's runs must match the serial split's byte for byte, not merely be sorted:

```bash
for i in $(seq 0 99); do cmp -s run$i.dat reference/run$i.dat || echo "MISMATCH run$i"; done
```

**Byte-identical output** at 1, 8, 10 and 14 threads against the single-threaded merge.

**valsort:**

```
Records: 500000000
Checksum: ee6b6a9da7427ce
Duplicate keys: 0
SUCCESS - all records are in order
```

A correct sort of the same input always reproduces that checksum, so it works as an oracle without keeping a 47 GB reference file around.

There is also a cheap internal invariant that runs before any I/O: the cut table's segment sizes must sum to the total record count. It costs microseconds and catches nearly every partitioning bug before the merge writes a single byte.

---

## What was tried

Thirteen changes. **Three converted to wall clock.** The rest are here because
the failures were more informative than the wins.

| change | CPU | wall | outcome |
|---|---|---|---|
| merge `ifstream` → `pread` | ~0 | **−11s** | shipped |
| decoupled merge writer | — | **−4.6s** | shipped |
| decoupled split writer | — | unattributed | shipped |
| parallelize sampling + cut table | ~0 | −3.5s claimed | shipped, never isolated |
| split `ofstream` → `pwrite` | −83s sys | 0 | shipped anyway |
| 52 MB output buffer, no writer | −23s | 0 | knob, left off |
| 52 MB output buffer, with writer | — | **+8.9s** | rejected |
| `F_NOCACHE` on output | −13s sys | +4s | reverted |
| `F_RDADVISE` prefetch hint | 0 | 0 | reverted |
| `BUFRECS` 16384 / 32768 | 0 | 0 | rejected |
| `F_PREALLOCATE` | −1.2s sys | 0 | rejected |
| fused split+merge, 12 GB resident | — | **+14s** | rejected |
| threaded read prefetch, 1-deep | — | **+11s** | rejected |

**The rule that held every time: a change converts only when it removes
serialized work from the critical path.** All three winners did. Six changes
removed real CPU time from a CPU that was already idle and bought nothing —
including one that cut 25% of user time for exactly zero seconds.

Two entries are worth reading as warnings rather than results. The 52 MB output
buffer *reversed sign* depending on context: neutral alone, actively harmful
once a writer thread existed, because coarse handoffs starve the writer between
bursts. And the 1-deep read prefetch failed on a device that a `dd` test had
already shown gains only 21% from 8× the queue depth — the evidence was there
before the experiment was written.

---

## Methodology notes

Several of these cost real time to learn and changed conclusions.

**Interleave configurations, don't block them.** Running all the P=1 trials first and all the P=10 trials afterwards reports **4.78×** instead of the honest **3.00×**, because the later trials read warmed page cache. The bias points in the direction that flatters the result, which is the direction these biases usually point.

**Record the load average with every measurement.** The single largest confound here was not page cache or trial ordering — it was an unrelated process on the same machine. The same binary measured 59.3s at load 23 and 51.9s at load 2. Nothing in the program's output tells you which one you got.

**Test above RAM size.** 5 GB fits inside 36 GB and becomes a CPU benchmark after the first pass. It reported 1.92× where the real workload gives 3.00×, and pointed at an entirely different bottleneck.

**Medians hide small wins when variance is wide.** Split's `pwrite` change looked like a wash on medians (34.7s vs 35.3s) — but the three fastest split runs ever recorded were all `pwrite` runs, and the minimums differed by 6 seconds. With a 7–12s spread, three samples is not enough to call a null result.

**One sample is not a measurement, including when it's the one you're measuring against.** The `dd` ceiling used to justify "we're done" was a single run. Repeating it gave 2.73, 3.69 and 4.34 GB/s — the number this project's stopping criterion rested on had a 60% spread. The behavioral evidence (six CPU optimizations, zero speedups) turned out to be far more reliable than the direct measurement.

**Hypotheses that were tested and refuted.** Threading was predicted to help *less* under real disk I/O; it helps *more*, because I/O wait is exactly what parallelism hides. Split's rising system time was attributed to memory pressure; using *less* buffer memory made it worse, because smaller buffers meant smaller chunks meant more files — the causation was backwards. Fewer runs were predicted to speed the merge; no change. Bypassing the page cache was predicted to help; it lost.

---

## Current limits

Both phases now run at 3.5–3.7 GB/s mixed, inside the band raw `dd` produces on this machine. `user` time is flat and small in both, and six separate CPU optimizations produced no wall-clock gain — while all three changes that *did* convert removed a serialization rather than making any operation faster. That pattern is the strongest evidence available that what remains is bounded by the device. It is still an inference: the `dd` band spans 60%, so there is no trustworthy ceiling to measure against.

Two limits on the result itself, both worth stating plainly:

**The dataset is only ~1.3× RAM** (47 GB against 36 GiB). That makes this a mild external sort — the regime where buffer allocation and merge fan-in genuinely bite is many multiples of memory, and none of that pressure appears here. A run at 4–8× RAM would test the design far harder.

**There is no external baseline.** Every multiple in this README is measured against this project's own v1, which is self-relative. A GNU `sort` comparison on identical input, same thread count, reporting both wall clock and CPU-seconds, is the missing number — until it exists, "is 55s good for this hardware?" is unanswered.

## Possible next steps

- Reproduce the 55.24s total two more times on an idle machine. One run under 60s is not a sub-60 sort; three cold trials make it a claim
- A/B the split writer at `depth=1` vs `depth=2` to attribute the gain, since the 55.24s run changed the code and the machine temperature at the same time
- Measure the parallel sampling + cut table, which has never been isolated. The v5 merge median of 40.8s against v6's `nbuf=1` median of 29.82s hints it is worth far more than the 3.5s claimed — but that is a cross-session comparison, which this project's own test discipline rules inadmissible
- Benchmark against GNU `sort` and the Sort Benchmark reference implementations, for an external anchor rather than self-relative numbers
- Repeat the `dd` ceiling test enough times to get a trustworthy number — the current three samples span 60%
- A loser tree instead of `priority_queue` — halves comparisons, though the evidence says it would not convert to wall clock at this scale
- Confirm the parallel sampling and cut table actually convert to wall clock. Both are now parallel over runs, and the output is byte-identical (`valsort ee6b6a9da7427ce`, 0 duplicate keys at 47 GB) — but the ~3.5s that motivated the change was measured on a scratchpad build and has never been confirmed against the shipped binary. It needs `purge` before *every* merge: an end-to-end run reads run files the split just wrote, so it is partially warm, and the 5 GB set cannot see the change at all, because at that size the runs fit in page cache and the read latency it removes does not exist. Given that six of ten changes here removed real CPU and bought zero seconds, this one is unproven until measured that way.
