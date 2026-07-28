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
#include <fcntl.h>
#include <unistd.h>

#include <stdexcept>    // runtime_error

const int KS = 10;               // key size
const int RECORD_SIZE = 100;

namespace fs = std::filesystem;
using namespace std;


// A bounded, buffered reader over ONE slice of ONE run file.
// Bounded from the start: Stage A passes the whole file,
// Stage B passes a slice. Writing it this way now avoids a rewrite.
struct Seg {
    ifstream in;
    vector<char> buf;
    size_t pos = 0, len = 0, cap;
    size_t remaining;        // records left in MY slice, not the file

    Seg(const string& f, size_t startRec, size_t nrec, size_t bufRecs)
        : in(f, ios::binary), buf(bufRecs * RECORD_SIZE),
          cap(bufRecs * RECORD_SIZE), remaining(nrec) {
        if (!in) throw runtime_error("open failed: " + f);
        in.seekg((streamoff)startRec * RECORD_SIZE);
    }

    const char* front() const { return &buf[pos]; }

    bool refill() {
        size_t want = min(cap, remaining * RECORD_SIZE);
        if (!want) return false;
        in.read(buf.data(), (streamsize)want);
        len = (size_t)in.gcount();
        remaining -= len / RECORD_SIZE;
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
static size_t upperBoundInRun(ifstream& f, size_t n, const char* sp) {
    size_t lo = 0, hi = n;
    char key[KS];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        f.seekg((streamoff)mid * RECORD_SIZE);
        f.read(key, KS);
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

int main(int argc, char** argv) {
    auto start = chrono::high_resolution_clock::now(); // start timer
    const int    P       = argc > 1 ? stoi(argv[1])
                                    : (int)thread::hardware_concurrency();
    const size_t BUFRECS = argc > 2 ? stoull(argv[2]) : 8192;
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

    // Runs are ALREADY SORTED, so evenly-spaced records are that run's
    // quantiles. Oversample 32x per bucket so splitters land close to
    // the true quantiles.
    const int S = 32 * P;
    vector<Key> sample;
    for (int r = 0; r < K; r++) {
        ifstream f(runFiles[r], ios::binary);
        for (int t = 1; t <= S; t++) {
            size_t idx = (size_t)((double)t / (S + 1) * nrec[r]);
            if (idx >= nrec[r]) continue;
            Key k;
            f.seekg((streamoff)idx * RECORD_SIZE);
            f.read(k.data(), KS);
            sample.push_back(k);
        }
    }
    sort(sample.begin(), sample.end(), [](const Key& a, const Key& b){
        return memcmp(a.data(), b.data(), KS) < 0;
    });

    vector<Key> spl;                          // P-1 splitters
    for (int j = 1; j < P; j++)
        spl.push_back(sample[(size_t)((double)j / P * sample.size())]);


    // cut[r][j] = first record index in run r belonging to segment j.
    // Segment j of run r is records [ cut[r][j], cut[r][j+1] ).
    vector<vector<size_t>> cut(K, vector<size_t>(P + 1, 0));
    for (int r = 0; r < K; r++) {
        ifstream f(runFiles[r], ios::binary);
        cut[r][0] = 0;
        cut[r][P] = nrec[r];
        for (int j = 1; j < P; j++)
            cut[r][j] = upperBoundInRun(f, nrec[r], spl[j - 1].data());
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
            Seg* sg = new Seg(runFiles[r], a, b - a, BUFRECS);
            if (sg->refill()) {
                segs.push_back(sg);
                pq.push({ sg->front(), (int)segs.size() - 1 });
            } else delete sg;
        }

        vector<char> ob;
        ob.reserve(BUFRECS * RECORD_SIZE);
        off_t off = (off_t)outOff[j];

        while (!pq.empty()) {
            Item it = pq.top();
            pq.pop();

            // COPY OUT FIRST: advance() may overwrite the buffer
            // that it.rec points into.
            ob.insert(ob.end(), it.rec, it.rec + RECORD_SIZE);
            if (ob.size() >= BUFRECS * RECORD_SIZE) {
                if (!writeFully(fd, ob.data(), ob.size(), off)) return;
                off += ob.size();
                ob.clear();
            }

            Seg* sg = segs[it.seg];
            if (sg->advance()) pq.push({ sg->front(), it.seg });
        }
        if (!ob.empty()) writeFully(fd, ob.data(), ob.size(), off);

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

    auto end = chrono::high_resolution_clock::now();

    chrono::duration<double> elapsed = end - start;

    cout << "Merge time: " << elapsed.count() << " seconds"
        << "  [threads=" << P << " runs=" << K
        << " balance min=" << *min_element(segRec.begin(), segRec.end())
        << " max="        << *max_element(segRec.begin(), segRec.end())
        << " ideal="      << total / P << "]\n";

}
