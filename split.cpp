// Sorted chunks are handed to a per-thread writer instead of written inline.
//
// Same reasoning that bought -12.4% on the merge. Each split thread ran
// pread -> sort -> reorder -> pwrite serialized, so while it is writing
// 500MB it issues no reads, and while it is reading it issues no writes.
// Any overlap today happens only by accident, when eight threads happen to
// drift out of phase with each other -- which is the most likely reason
// split swings 7.3s (23.86s vs 31.17s) on byte-identical code.
//
// The win was expected to land on VARIANCE more than the mean, since split's
// good night (4.19 GB/s mixed) already sat at the top of the measured
// 2.73-4.34 GB/s band.
//
// First cold end-to-end run with it: split 28.44s + merge 26.80s = 55.24s
// total at 47GB, valsort ee6b6a9da7427ce -- the first run under MinuteSort's
// 60.00s, against a previous best total of 61.23s. NOT yet attributed: that
// run was also on a cooler machine than the 31.17s split it beat, so the
// depth=2 A/B -- 3 interleaved cold cycles of depth=1 against depth=2, with
// a cache purge before every run -- is still needed to separate the change
// from the conditions. And it is one trial; three is the house rule.
//
// DEPTH (argv[3]) is the ring depth. DEPTH=1 runs the original synchronous
// path verbatim, so one binary A/Bs itself -- exactly as NBUF=1 does in
// merge.cpp.
//
// MEMORY: T * (1 + DEPTH) * CHUNK_SIZE * 100 bytes, printed at startup.
// At T=8, DEPTH=2, CHUNK=5,000,000 that is 12GB against 38GB of RAM while
// 100GB streams through the page cache. If that pressure hurts writeback,
// try T=6 or DEPTH=1 before blaming the design -- the merge already taught
// us that a bigger buffer can lose.

#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>   // ref
#include <fcntl.h>
#include <unistd.h>

using namespace std;

int CHUNK_SIZE;
const int RECORD_SIZE = 100;

struct KeyIdx {
    char key[10];
    uint32_t idx;   // record's slot within the chunk buffer
};

// pread() may return fewer bytes than asked for; loop until the chunk is whole.
static bool readFully(int fd, char* p, size_t n, off_t off) {
    while (n) {
        ssize_t r = pread(fd, p, n, off);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
        off += r;
    }
    return true;
}

static bool writeFully(int fd, const char* p, size_t n, off_t off) {
    while (n) {
        ssize_t w = pwrite(fd, p, n, off);
        if (w <= 0) { perror("pwrite"); return false; }
        p += w; n -= (size_t)w; off += w;
    }
    return true;
}

// A single-producer / single-consumer ring of sorted-chunk buffers between
// one split thread and its own writer thread.
//
// Each slot carries its own chunk index, and the writer opens run<c>.dat
// itself. So slots are completely independent of one another -- the writer
// may take them in any order and every chunk still lands in the right file.
// There is no sequence to get wrong, which is what keeps this safe without
// a sequencer.
//
// Buffers are resize()d, not reserve()d, because the reorder step indexes
// into them directly. That also means the whole footprint is committed at
// startup rather than growing under load.
struct ChunkPipe {
    vector<vector<char>> slot;
    vector<size_t>       slen;
    vector<size_t>       schunk;
    const int            n;
    int                  head = 0, tail = 0, filled = 0;
    bool                 done = false, failed = false;
    mutex                m;
    condition_variable   cvFilled, cvFree;

    ChunkPipe(int nbuf, size_t bytes)
        : slot(nbuf), slen(nbuf, 0), schunk(nbuf, 0), n(nbuf) {
        for (auto& s : slot) s.resize(bytes);
    }
};

// Claim the next free slot. Returns -1 if the writer has failed -- testing
// `failed` in the predicate is what stops a dead writer from hanging the
// producer forever.
static int acquireSlot(ChunkPipe& p) {
    unique_lock<mutex> lk(p.m);
    p.cvFree.wait(lk, [&]{ return p.filled < p.n || p.failed; });
    if (p.failed) return -1;
    return p.tail;
}

static void publishSlot(ChunkPipe& p, size_t nbytes, size_t chunk) {
    lock_guard<mutex> lk(p.m);
    p.slen[p.tail]   = nbytes;
    p.schunk[p.tail] = chunk;
    p.tail = (p.tail + 1) % p.n;
    p.filled++;
    p.cvFilled.notify_one();
}

static void chunkWriter(ChunkPipe& p) {
    for (;;) {
        int idx; size_t nbytes, c;
        {
            unique_lock<mutex> lk(p.m);
            p.cvFilled.wait(lk, [&]{ return p.filled > 0 || p.done; });
            if (p.filled == 0) return;      // done AND drained -- the only exit
            idx = p.head; nbytes = p.slen[idx]; c = p.schunk[idx];
        }

        string path = "run" + to_string(c) + ".dat";
        bool ok = false;
        int rfd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (rfd < 0) perror(path.c_str());
        else {
            ok = writeFully(rfd, p.slot[idx].data(), nbytes, 0);
            close(rfd);
        }

        {
            lock_guard<mutex> lk(p.m);
            p.head = (p.head + 1) % p.n;
            p.filled--;
            if (!ok) p.failed = true;
            p.cvFree.notify_one();          // notify BEFORE bailing out
        }
        if (!ok) return;
    }
}

int main(int argc, char* argv[]) {
    auto start = chrono::high_resolution_clock::now();

    if (argc < 2 || argc > 4) {
        cout << "Usage: ./split_v2 <chunk_size> [threads] [depth]\n";
        return 1;
    }

    CHUNK_SIZE = stoi(argv[1]);
    if (CHUNK_SIZE <= 0) {
        cout << "Chunk size must be positive\n";
        return 1;
    }

    const int T = argc > 2 ? stoi(argv[2])
                           : (int)thread::hardware_concurrency();
    const int DEPTH = argc > 3 ? stoi(argv[3]) : 2;

    const size_t chunkBytes = (size_t)CHUNK_SIZE * RECORD_SIZE;
    const size_t footprint  = (size_t)T * (1 + (DEPTH > 1 ? DEPTH : 1)) * chunkBytes;
    cerr << "  buffers: " << T << " threads x " << (1 + (DEPTH > 1 ? DEPTH : 1))
         << " x " << (chunkBytes / 1000000) << "MB = "
         << (footprint / 1000000000.0) << "GB\n";

    // One shared read-only fd.
    int fd = open("input.txt", O_RDONLY);
    if (fd < 0) {
        perror("open input.txt");
        return 1;
    }

    const size_t totalRecs = (size_t)lseek(fd, 0, SEEK_END) / RECORD_SIZE;
    const size_t nchunks   = (totalRecs + CHUNK_SIZE - 1) / CHUNK_SIZE;

    atomic<size_t> next{0};   // hands out chunk indices

    auto worker = [&]() {
        vector<char> buf(chunkBytes);
        vector<KeyIdx> keys;
        keys.reserve(CHUNK_SIZE);

        // Build the key+index array for chunk c and sort it. Shared by both
        // paths so they cannot drift apart.
        auto loadAndSort = [&](size_t c, size_t& nOut) -> bool {
            const size_t startRec = c * (size_t)CHUNK_SIZE;
            nOut = min((size_t)CHUNK_SIZE, totalRecs - startRec);
            if (!readFully(fd, buf.data(), nOut * RECORD_SIZE,
                           (off_t)startRec * RECORD_SIZE)) {
                cerr << "read failed on chunk " << c << "\n";
                return false;
            }
            keys.resize(nOut);
            for (size_t i = 0; i < nOut; i++) {
                memcpy(keys[i].key, &buf[i * RECORD_SIZE], 10);
                keys[i].idx = (uint32_t)i;
            }
            sort(keys.begin(), keys.end(),
                 [](const KeyIdx& a, const KeyIdx& b){
                     return memcmp(a.key, b.key, 10) < 0;
                 });
            return true;
        };

        // --- baseline path, verbatim from split.cpp ---
        if (DEPTH <= 1) {
            vector<char> out(chunkBytes);
            size_t c;
            while ((c = next.fetch_add(1)) < nchunks) {
                size_t n;
                if (!loadAndSort(c, n)) return;
                for (size_t i = 0; i < n; i++) {
                    memcpy(&out[i * RECORD_SIZE],
                           &buf[keys[i].idx * RECORD_SIZE], RECORD_SIZE);
                }
                string path = "run" + to_string(c) + ".dat";
                int rfd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (rfd < 0) { perror(path.c_str()); return; }
                writeFully(rfd, out.data(), n * RECORD_SIZE, 0);
                close(rfd);
            }
            return;
        }

        // --- pipelined path ---
        // The slot is claimed AFTER the sort and just before the reorder, so
        // the read and the sort always proceed in parallel with the previous
        // chunk's write. The thread only stalls if every slot is still in
        // flight, which is the backpressure that bounds memory.
        ChunkPipe pipe(DEPTH, chunkBytes);
        thread    writer(chunkWriter, ref(pipe));

        size_t c;
        while ((c = next.fetch_add(1)) < nchunks) {
            size_t n;
            if (!loadAndSort(c, n)) break;

            int idx = acquireSlot(pipe);
            if (idx < 0) break;             // writer died
            vector<char>& out = pipe.slot[idx];
            for (size_t i = 0; i < n; i++) {
                memcpy(&out[i * RECORD_SIZE],
                       &buf[keys[i].idx * RECORD_SIZE], RECORD_SIZE);
            }
            publishSlot(pipe, n * RECORD_SIZE, c);
        }

        {
            lock_guard<mutex> lk(pipe.m);
            pipe.done = true;
            pipe.cvFilled.notify_one();
        }
        writer.join();
    };

    vector<thread> pool;
    for (int t = 0; t < T; t++) pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    close(fd);

    auto end = chrono::high_resolution_clock::now();
    chrono::duration<double> elapsed = end - start;

    cout << "Split time: "
         << elapsed.count() << " seconds"
         << "  [threads=" << T
         << " depth=" << DEPTH
         << " chunks=" << nchunks
         << " records=" << totalRecs << "]\n";

    return 0;
}
