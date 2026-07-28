# External Merge Sort

A multithreaded external sort in C++17 for datasets larger than memory. Sorts 500,000,000 fixed-size records (~47 GB) in **82 seconds** on a laptop, verified byte-exact with `valsort`.

Records are the standard [gensort](http://www.ordinal.com/gensort.html) format:

```
[key (10 bytes)][value (90 bytes)]   = 100 bytes
```

Fixed-size records are what make most of this possible — record *i* always lives at byte *i × 100*, so any thread can compute where to read, and a sorted run can be binary-searched on disk.

---

## Design

Two phases, both parallel.

### Split — produce sorted runs

Each thread claims a chunk index from a shared `atomic<size_t>` counter, `pread`s that chunk at a computed byte offset, and writes one sorted run. No shared reader, no barrier, no locks — each thread owns its own buffers and writes its own file.

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

Every number below is 500,000,000 records / ~47 GB on the same machine: MacBook Pro, Apple Silicon, 10 performance cores (14 total), 36 GB RAM, 1 TB Apple SSD.

| version | split | merge | total | vs. v1 |
|---|---|---|---|---|
| **v1** — KV separation with a value log | 1323.6s | 1108.1s | 40.5 min | 1.0× |
| **v2** — sorted runs, key+index sort | 86.4s | 503s | 9.8 min | 4.1× |
| **v3** — buffered merge reads | 86.4s | 153.6s | 4.0 min | 10.1× |
| **v4** — parallel split + sampled splitters | **30.2s** | **51.9s** | **82.1s** | **29.6×** |

v1 used a 10M-record chunk (50 runs); v2 onward use 5M (100 runs).

The two changes that mattered most were not the parallelism:

- **v1 → v2** killed the value log. Sorting key+pointer pairs and then chasing pointers meant random reads across the value file during the merge. Storing full records in sorted runs made every read sequential.
- **v2 → v3** was a single idea: read ~800 KB at a time instead of 100 bytes. The old merge issued ~6.1M read syscalls at ~60 µs each ≈ 380s of pure latency, which matched the 384s of measured I/O wait almost exactly. **That one change was worth 3.27× — more than threading.**

---

## Detailed results

### Merge — thread scaling

Medians of 3 round-robin cycles at 47 GB, 100 runs.

| P | time | speedup | user | sys | CPU | effective R+W |
|---|---|---|---|---|---|---|
| 1 | 153.6s | 1.00× | 29.4s | 27.7s | 37% | 651 MB/s |
| 2 | 84.9s | 1.81× | 29.9s | 30.0s | 71% | 1.18 GB/s |
| 4 | 63.4s | 2.43× | 30.8s | 36.1s | 106% | 1.58 GB/s |
| 8 | 52.1s | 2.95× | 31.5s | 38.5s | 134% | 1.92 GB/s |
| **10** | **51.3s** | **3.00×** | 31.6s | 35.6s | 131% | **1.95 GB/s** |

`user` time is flat at ~30s across every thread count. The threads add no computation — they reclaim I/O wait. Throughput triples because an NVMe SSD needs multiple outstanding requests to reach its bandwidth; one thread issuing sequential reads leaves the device mostly idle.

### Split — thread scaling

Medians of 2 cycles, 5M-record chunks.

| config | time | speedup | user | sys | peak RSS |
|---|---|---|---|---|---|
| serial | 86.4s | 1.00× | 53.1s | 30.0s | 1.08 GB |
| T=1 | 78.9s | 1.10× | 47.1s | 28.3s | 1.08 GB |
| T=2 | 42.7s | 2.02× | 48.2s | 30.6s | 2.16 GB |
| T=4 | 35.8s | 2.41× | 50.2s | 59.5s | 4.32 GB |
| **T=8** | **32.0s** | **2.70×** | 53.1s | 118.2s | 8.64 GB |
| T=10 | 33.4s | 2.59× | 54.4s | 150.4s | 10.80 GB |

Memory is `T × (2 × CHUNK × 100 + CHUNK × 16)`, which matches the measured RSS exactly.

Note `T=1` is only **1.10×** over serial. That row isolates the bulk-read change — replacing 500M single-record reads with one `pread` per chunk. It is nearly worthless here, because split reads one file front to back and the kernel's readahead already covered it. The *same* change was worth 3.27× in the merge, where reads jump between 100 files and readahead cannot predict anything. **Access pattern, not read size, decides whether readahead can help you.**

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

Cold, 256 → 4096 is **3.6×**. Cached, everything from 1024 to 8192 is flat within noise. The right value depends entirely on whether the data fits in RAM.

### Creating files costs more than overwriting them

Split at T=8, identical in every respect except whether `run*.dat` already existed:

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

Fitting those three points gives **sys ≈ 115s + 0.59s × (number of files)**. Two predictions fall out, both confirmed against data the fit never saw: the per-file cost should be ~0.6s (measured 0.64s above), and the fixed component should equal the overwrite case's system time (measured 119.6s against a predicted 115s).

The mechanism is filesystem metadata contention between concurrent writers. The serial split barely notices create-vs-overwrite (87.3s vs 85.5s, 2%) because it writes one file at a time.

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

## Methodology notes

Several of these cost real time to learn and changed conclusions.

**Interleave configurations, don't block them.** Running all the P=1 trials first and all the P=10 trials afterwards reports **4.78×** instead of the honest **3.00×**, because the later trials read warmed page cache. The bias points in the direction that flatters the result, which is the direction these biases usually point.

**Record the load average with every measurement.** The single largest confound here was not page cache or trial ordering — it was an unrelated build running on the same machine. The same binary measured 59.3s at load 23 and 51.9s at load 2. Nothing in the program's output tells you which one you got.

**Test above RAM size.** 5 GB fits inside 36 GB and becomes a CPU benchmark after the first pass. It reported 1.92× where the real workload gives 3.00×, and pointed at an entirely different bottleneck.

**Report medians of repeated trials.** A periodic writeback flush produces one slow trial per sweep — a single timing caught it as 3.72s against a true 2.01s.

**Two hypotheses were tested and refuted.** Threading was predicted to help *less* under real disk I/O; it helps *more*, because I/O wait is exactly what parallelism hides. Split's rising system time was attributed to memory pressure; using *less* buffer memory made it worse, because smaller buffers meant smaller chunks meant more files — the causation was backwards.

---

## Current limits

Peak throughput is ~1.95 GB/s combined read+write, likely 30–40% of what the drive can sustain. Both phases are now bounded by kernel time in the I/O path rather than by the algorithm — `user` time is flat and small in both. Bypassing the page cache with `O_DIRECT` is the most promising next step.

## Possible next steps

- `O_DIRECT` / `F_NOCACHE` to skip page-cache copying
- Larger chunks (10M, 50 runs) — both phases prefer fewer, larger runs, and the fan-in cost measured 78 ns/record at 10 runs against 194 ns/record at 100
- Benchmark against GNU `sort` and the Sort Benchmark reference implementations for an external anchor
- Parallelize the cut table across runs — trivially parallel, and the only serial step that scales with segment count
