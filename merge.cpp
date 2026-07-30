#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <queue>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <thread>
#include <atomic>
#include <array>
#include <mutex>
#include <condition_variable>
#include <functional>   // ref
#include <fcntl.h>
#include <unistd.h>

#include <stdexcept>    // runtime_error

// Output writes are handed to a per-thread writer rather than issued inline.
//
// The measurement that motivated it: no-op'ing writeFully split the merge
// into 13.6s read+merge and 27.9s write, summing to EXACTLY the 41.5s
// total. An exact sum means zero overlap -- a thread blocked in pwrite
// issues no reads, and a thread blocked in pread issues no writes.
//
// Result, 47GB: a 3-cycle interleaved cold A/B gave -12.5% and won all
// three pairs (-3.54 / -5.00 / -2.93s). End to end the merge went
// 37.35s -> 32.71s, -12.4%, so the A/B held up outside the harness.
//
// NBUF (argv[4]) is the ring depth; NBUF=1 runs the old synchronous path
// verbatim, so this one binary still A/Bs itself. Leave OBRECS at 8192:
// a 52MB output buffer measured 8.9s SLOWER because coarse handoffs starve
// the writer between bursts -- the opposite of the CPU-side reasoning that
// originally motivated the big buffer.

const int KS = 10;               // key size
const int RECORD_SIZE = 100;

namespace fs = std::filesystem;
using namespace std;

// pread() may return fewer bytes than asked for; loop until whole.
static bool readFully(int fd, char* p, size_t n, off_t off) {
    while (n) {
        ssize_t r = pread(fd, p, n, off);
        if (r <= 0) return false;
        p += r; n -= (size_t)r; off += r;
    }
    return true;
}

// A bounded, buffered reader over ONE slice of ONE run file.
// Reads go straight to the kernel via pread on a shared fd. pread takes
// its offset as an argument, so many threads can share one descriptor --
// and one 800KB request stays one request instead of ~200 4KB round-trips.
struct Seg {
    int fd;                  // shared, NOT owned -- never closed here
    off_t off;               // my read cursor, in bytes
    vector<char> buf;
    size_t pos = 0, len = 0, cap;
    size_t remaining;        // records left in MY slice, not the file

    Seg(int fd_, size_t startRec, size_t nrec, size_t bufRecs)
        : fd(fd_), off((off_t)startRec * RECORD_SIZE),
          buf(bufRecs * RECORD_SIZE),
          cap(bufRecs * RECORD_SIZE), remaining(nrec) {}

    const char* front() const { return &buf[pos]; }

    bool refill() {
        size_t want = min(cap, remaining * RECORD_SIZE);
        if (!want) return false;
        if (!readFully(fd, buf.data(), want, off)) return false;
        off += (off_t)want;
        len = want;
        remaining -= want / RECORD_SIZE;
        pos = 0;
        return len >= RECORD_SIZE;
    }

    bool advance() {
        pos += RECORD_SIZE;
        if (pos + RECORD_SIZE <= len) return true;  // already in memory
        return refill();                            // drained -> one read
    }
};

// Heap entry points INTO a Seg's buffer -- no copy, no allocation.
struct Item { const char* rec; int seg; };
struct Cmp {
    bool operator()(const Item& a, const Item& b) const {
        return memcmp(a.rec, b.rec, KS) > 0;    // > 0 == min-heap
    }
};

typedef array<char, KS> Key;

// first record index in this run whose key is STRICTLY GREATER than sp
// Takes a shared fd rather than its own ifstream: an ifstream carries a
// seek cursor, so it cannot be shared, which forced this whole search to
// run one run at a time. pread carries its offset per call, so every run
// can be probed concurrently on one descriptor.
static size_t upperBoundInRun(int fd, size_t n, const char* sp) {
    size_t lo = 0, hi = n;
    char key[KS];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (pread(fd, key, KS, (off_t)mid * RECORD_SIZE) != (ssize_t)KS) break;
        if (memcmp(key, sp, KS) <= 0) lo = mid + 1;
        else                          hi = mid;
    }
    return lo;
}

// pwrite can write fewer bytes than asked. A short write on a full disk
// is exactly the failure that looks like a sort bug. Never ignore it.
static bool writeFully(int fd, const char* p, size_t n, off_t off) {
    while (n) {
        ssize_t w = pwrite(fd, p, n, off);
        if (w <= 0) { perror("pwrite"); return false; }
        p += w; n -= (size_t)w; off += w;
    }
    return true;
}

// A single-producer / single-consumer ring of output buffers between one
// merge thread and its own writer thread.
//
// Why this is safe without any ordering guarantee: each slot carries its
// own absolute file offset, and pwrite takes the offset per call. The
// writer can therefore issue slots in any order and they still land in the
// right place. There is no sequence to get wrong.
//
// The merger owns slot[tail] and fills it WITHOUT the lock; the writer only
// ever touches slots in [head, head+filled). Those ranges cannot overlap
// while filled < n, and only the merger advances tail, under the lock.
struct OutPipe {
    vector<vector<char>> slot;
    vector<size_t>       slen;
    vector<off_t>        soff;
    const int            n;
    int                  head = 0, tail = 0, filled = 0;
    bool                 done = false, failed = false;
    mutex                m;
    condition_variable   cvFilled, cvFree;

    OutPipe(int nbuf, size_t bytes)
        : slot(nbuf), slen(nbuf, 0), soff(nbuf, 0), n(nbuf) {
        for (auto& s : slot) s.reserve(bytes);
    }
};

// Claim the next free slot. Returns -1 if the writer has failed -- checking
// `failed` in the predicate is what keeps a dead writer from hanging the
// merger forever, which is the deadlock merge_v9 had.
static int acquireSlot(OutPipe& p) {
    unique_lock<mutex> lk(p.m);
    p.cvFree.wait(lk, [&]{ return p.filled < p.n || p.failed; });
    if (p.failed) return -1;
    return p.tail;
}

static void publishSlot(OutPipe& p, size_t nbytes, off_t off) {
    lock_guard<mutex> lk(p.m);
    p.slen[p.tail] = nbytes;
    p.soff[p.tail] = off;
    p.tail = (p.tail + 1) % p.n;
    p.filled++;
    p.cvFilled.notify_one();
}

static void writerLoop(int fd, OutPipe& p) {
    for (;;) {
        int idx; size_t nbytes; off_t off;
        {
            unique_lock<mutex> lk(p.m);
            p.cvFilled.wait(lk, [&]{ return p.filled > 0 || p.done; });
            if (p.filled == 0) return;      // done AND drained -- the only exit
            idx = p.head; nbytes = p.slen[idx]; off = p.soff[idx];
        }
        bool ok = writeFully(fd, p.slot[idx].data(), nbytes, off);
        {
            lock_guard<mutex> lk(p.m);
            p.slot[idx].clear();            // keeps capacity; no realloc later
            p.head = (p.head + 1) % p.n;
            p.filled--;
            if (!ok) p.failed = true;
            p.cvFree.notify_one();          // notify BEFORE bailing out
        }
        if (!ok) return;
    }
}

int main(int argc, char** argv) {
    auto start = chrono::high_resolution_clock::now(); // start timer
    const int    P       = argc > 1 ? stoi(argv[1])
                                    : (int)thread::hardware_concurrency();
    // Read and output buffers are sized separately. They serve opposite
    // patterns: BUFRECS is per input stream and there are P*K of them, so
    // it is a memory-footprint knob. OBRECS is per thread, only P of them,
    // and it sets how large one pwrite is.
    const size_t BUFRECS = argc > 2 ? stoull(argv[2]) : 8192;
    const size_t OBRECS  = argc > 3 ? stoull(argv[3]) : 8192;
    // Output ring depth. 1 = the old synchronous path (the A/B baseline).
    // Memory cost is P * NBUF * OBRECS * RECORD_SIZE, so raising OBRECS and
    // NBUF together multiplies -- 10 threads x 3 x 52MB is 1.57GB.
    const int    NBUF    = argc > 4 ? stoi(argv[4])   : 3;
    vector<string> runFiles;
 

    // find every run file generated during the split phase.

    for (const auto& entry : fs::directory_iterator(".")) {
        string filename = entry.path().filename().string();

        if (filename.rfind("run", 0) == 0 &&
            filename.size() > 4 &&
            filename.substr(filename.size() - 4) == ".dat") {
            runFiles.push_back(filename);
        }
    }

    // sort so files ran files are in order 

    sort(runFiles.begin(), runFiles.end());

    // open each run as a buffered reader over its whole length,
    // and prime the heap in the same pass
    const int K = (int)runFiles.size();
    vector<size_t> nrec(K);
    size_t total = 0;
    for (int r = 0; r < K; r++) {
        nrec[r] = fs::file_size(runFiles[r]) / RECORD_SIZE;
        total += nrec[r];
    }

    // One descriptor per run, opened once here and shared by the sampler,
    // the cut table, and the merge. All three only ever pread, so sharing
    // is safe, and each run gets opened once instead of three times.
    vector<int> runFd(K);
    for (int r = 0; r < K; r++) {
        runFd[r] = open(runFiles[r].c_str(), O_RDONLY);
        if (runFd[r] < 0) { perror(runFiles[r].c_str()); return 1; }
    }

    // Runs are alr sorted, so evenly-spaced records are that run's
    // quantiles. Oversample 32x per bucket so splitters land close to
    // the true quantiles.
    //
    // Runs sample independently, so this fans out over threads. Serial, it
    // was K*S dependent seek-then-read pairs at queue depth 1 -- all
    // latency, no throughput. Samples are gathered per run and concatenated
    // in run order below, so the sorted sample -- and therefore every
    // splitter -- is identical to what the serial version produced.
    const int S = 32 * P;
    vector<vector<Key>> perRun(K);
    {
        atomic<size_t> nextRun{0};
        auto sampler = [&]{
            size_t r;
            while ((r = nextRun.fetch_add(1)) < (size_t)K) {
                perRun[r].reserve(S);
                for (int t = 1; t <= S; t++) {
                    size_t idx = (size_t)((double)t / (S + 1) * nrec[r]);
                    if (idx >= nrec[r]) continue;
                    Key k;
                    if (pread(runFd[r], k.data(), KS,
                              (off_t)idx * RECORD_SIZE) == (ssize_t)KS)
                        perRun[r].push_back(k);
                }
            }
        };
        vector<thread> pool;
        for (int t = 0; t < P; t++) pool.emplace_back(sampler);
        for (auto& th : pool) th.join();
    }

    vector<Key> sample;
    for (int r = 0; r < K; r++)
        sample.insert(sample.end(), perRun[r].begin(), perRun[r].end());

    sort(sample.begin(), sample.end(), [](const Key& a, const Key& b){
        return memcmp(a.data(), b.data(), KS) < 0;
    });

    vector<Key> spl;                          // P-1 splitters
    for (int j = 1; j < P; j++)
        spl.push_back(sample[(size_t)((double)j / P * sample.size())]);


    // cut[r][j] = first record index in run r belonging to segment j.
    // Segment j of run r is records [ cut[r][j], cut[r][j+1] ).
    //
    // Parallel over runs, same reasoning as the sampler: K*(P-1) binary
    // searches, every probe a blocking read that nothing overlaps. Each
    // thread owns whole rows of `cut`, so no two threads touch the same
    // inner vector.
    vector<vector<size_t>> cut(K, vector<size_t>(P + 1, 0));
    {
        atomic<size_t> nextRun{0};
        auto cutter = [&]{
            size_t r;
            while ((r = nextRun.fetch_add(1)) < (size_t)K) {
                cut[r][0] = 0;
                cut[r][P] = nrec[r];
                for (int j = 1; j < P; j++)
                    cut[r][j] = upperBoundInRun(runFd[r], nrec[r],
                                                spl[j - 1].data());
            }
        };
        vector<thread> pool;
        for (int t = 0; t < P; t++) pool.emplace_back(cutter);
        for (auto& th : pool) th.join();
    }

    vector<size_t> segRec(P, 0), outOff(P, 0);
    for (int j = 0; j < P; j++)
        for (int r = 0; r < K; r++)
            segRec[j] += cut[r][j + 1] - cut[r][j];

    size_t acc = 0;
    for (int j = 0; j < P; j++) {
        outOff[j] = acc * RECORD_SIZE;
        acc += segRec[j];
    }

    // Cheap invariant that catches almost every cut-table bug. Keep it.
    if (acc != total) {
        cerr << "CUT TABLE BUG: " << acc << " vs " << total << "\n";
        return 1;
    }
    int fd = open("output.dat", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open output.dat"); return 1; }
    if (ftruncate(fd, (off_t)total * RECORD_SIZE) != 0) {
        perror("ftruncate"); return 1;
    }
        auto mergeSegment = [&](int j) {
        vector<Seg*> segs;
        priority_queue<Item, vector<Item>, Cmp> pq;

        for (int r = 0; r < K; r++) {
            size_t a = cut[r][j], b = cut[r][j + 1];
            if (b <= a) continue;          // this run has nothing here
            Seg* sg = new Seg(runFd[r], a, b - a, BUFRECS);
            if (sg->refill()) {
                segs.push_back(sg);
                pq.push({ sg->front(), (int)segs.size() - 1 });
            } else delete sg;
        }

        // --- baseline path, verbatim from merge.cpp ---
        if (NBUF <= 1) {
            vector<char> ob;
            ob.reserve(OBRECS * RECORD_SIZE);
            off_t off = (off_t)outOff[j];

            while (!pq.empty()) {
                Item it = pq.top();
                pq.pop();

                // copies out first: advance() may overwrite the buffer
                // that it.rec points into.
                ob.insert(ob.end(), it.rec, it.rec + RECORD_SIZE);
                if (ob.size() >= OBRECS * RECORD_SIZE) {
                    if (!writeFully(fd, ob.data(), ob.size(), off)) return;
                    off += ob.size();
                    ob.clear();
                }

                Seg* sg = segs[it.seg];
                if (sg->advance()) pq.push({ sg->front(), it.seg });
            }
            if (!ob.empty()) writeFully(fd, ob.data(), ob.size(), off);

            for (Seg* x : segs) delete x;
            return;
        }

        // --- pipelined path: fill a slot, hand it off, keep merging ---
        OutPipe pipe(NBUF, OBRECS * RECORD_SIZE);
        thread  writer(writerLoop, fd, ref(pipe));

        off_t off = (off_t)outOff[j];
        int   idx = acquireSlot(pipe);
        vector<char>* ob = idx >= 0 ? &pipe.slot[idx] : nullptr;

        while (ob && !pq.empty()) {
            Item it = pq.top();
            pq.pop();

            // copies out first: advance() may overwrite the buffer
            // that it.rec points into.
            ob->insert(ob->end(), it.rec, it.rec + RECORD_SIZE);
            if (ob->size() >= OBRECS * RECORD_SIZE) {
                size_t nbytes = ob->size();
                publishSlot(pipe, nbytes, off);   // buffer is the writer's now
                off += nbytes;
                idx = acquireSlot(pipe);          // -1 means the writer died
                ob  = idx >= 0 ? &pipe.slot[idx] : nullptr;
            }

            Seg* sg = segs[it.seg];
            if (sg->advance()) pq.push({ sg->front(), it.seg });
        }

        // The tail buffer must be published BEFORE done, or the output is
        // silently short by up to one buffer -- which valsort would catch,
        // but only after a 40-second run.
        if (ob && !ob->empty()) publishSlot(pipe, ob->size(), off);
        {
            lock_guard<mutex> lk(pipe.m);
            pipe.done = true;
            pipe.cvFilled.notify_one();
        }
        writer.join();

        for (Seg* x : segs) delete x;
    };

    atomic<size_t> next{0};
    auto worker = [&]{
        size_t j;
        while ((j = next.fetch_add(1)) < (size_t)P) mergeSegment((int)j);
    };

    vector<thread> pool;
    for (int t = 0; t < P; t++) pool.emplace_back(worker);
    for (auto& th : pool) th.join();
    close(fd);
    for (int r = 0; r < K; r++) close(runFd[r]);

    auto end = chrono::high_resolution_clock::now();

    chrono::duration<double> elapsed = end - start;

    cout << "Merge time: " << elapsed.count() << " seconds"
        << "  [threads=" << P << " runs=" << K
        << " obKB=" << (OBRECS * RECORD_SIZE / 1024) << " nbuf=" << NBUF
        << " balance min=" << *min_element(segRec.begin(), segRec.end())
        << " max="        << *max_element(segRec.begin(), segRec.end())
        << " ideal="      << total / P << "]\n";

}
