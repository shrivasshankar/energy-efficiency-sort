#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <queue>
#include <filesystem>
#include <chrono>
#include <cstring>

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

int main() {
    auto start = chrono::high_resolution_clock::now(); // start timer

    const size_t BUFRECS = 8192;          // ~800KB per stream
    priority_queue<Item, vector<Item>, Cmp> pq;
    vector<string> runFiles;
    vector<Seg*> segs;
 

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
    for (const string& filename : runFiles) {
        const size_t nrec = fs::file_size(filename) / RECORD_SIZE;
        Seg* sg = new Seg(filename, 0, nrec, BUFRECS);
        if (sg->refill()) {
            segs.push_back(sg);
            pq.push({ sg->front(), (int)segs.size() - 1 });
        } else {
            delete sg;
        }
    }

    ofstream out("output.dat", ios::binary);
    vector<char> ob;                      // batch the writes too
    ob.reserve(BUFRECS * RECORD_SIZE);

    
    // pops the smallest value and writes to output
    // then it inserts the record from the same run
    while (!pq.empty()) {
        Item it = pq.top();
        pq.pop();

        // COPY OUT FIRST: advance() may overwrite the buffer
        // that it.rec points into.
        ob.insert(ob.end(), it.rec, it.rec + RECORD_SIZE);
        if (ob.size() >= BUFRECS * RECORD_SIZE) {
            out.write(ob.data(), (streamsize)ob.size());
            ob.clear();
        }

        Seg* sg = segs[it.seg];
        if (sg->advance()) pq.push({ sg->front(), it.seg });
    }
    if (!ob.empty()) out.write(ob.data(), (streamsize)ob.size());

    for (Seg* x : segs) delete x;

    auto end = chrono::high_resolution_clock::now();

    chrono::duration<double> elapsed = end - start;

    cout << "Merge time: "
        << elapsed.count()
        << " seconds\n";
    return 0;

}
