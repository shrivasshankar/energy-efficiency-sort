// Chunks are written inline by the thread that sorted them.
//
// A writer thread was tried here (experiments/split_v2.cpp) and it's a null
// result -- three interleaved cold cycles gave +9.53 / -0.36 / -5.42s. The
// merge had a measured serialization; this doesn't. Each thread sorts for
// ~0.46s per 500MB chunk and writes for ~175ms, so eight threads already
// drift out of phase enough to overlap. Don't re-add one without measuring
// that reads and writes actually fail to overlap.

#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint> 
#include <thread>
#include <atomic> 
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
        
        // Advance pointers and offset by the number of bytes actually read
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

int main(int argc, char* argv[]) {
    auto start = chrono::high_resolution_clock::now(); 

    if (argc < 2 || argc > 3) {
        cout << "Usage: ./split_program <chunk_size> [threads]\n";
        return 1;
    }

    CHUNK_SIZE = stoi(argv[1]);
    if (CHUNK_SIZE <= 0) {
        cout << "Chunk size must be positive\n";
        return 1;
    }

    const int T = argc > 2 ? stoi(argv[2]) 
                           : (int)thread::hardware_concurrency();

    // One shared read-only fd.
    int fd = open("input.txt", O_RDONLY);
    if (fd < 0) { 
        perror("open input.txt"); 
        return 1; 
    }

    const size_t totalRecs = (size_t)lseek(fd, 0, SEEK_END) / RECORD_SIZE;
    const size_t nchunks   = (totalRecs + CHUNK_SIZE - 1) / CHUNK_SIZE;

    atomic<size_t> next{0};   // hands out chunk indices

    // The loop becomes a worker lambda
    auto worker = [&]() {
        // Per-thread buffers
        vector<char> buf(static_cast<size_t>(CHUNK_SIZE) * RECORD_SIZE);
        vector<char> out(static_cast<size_t>(CHUNK_SIZE) * RECORD_SIZE);
        vector<KeyIdx> keys;
        keys.reserve(CHUNK_SIZE);

        size_t c;
        // fetch_add coordinates which thread gets which chunk safely
        while ((c = next.fetch_add(1)) < nchunks) {
            
            const size_t startRec = c * static_cast<size_t>(CHUNK_SIZE);
            const size_t n = min(static_cast<size_t>(CHUNK_SIZE), totalRecs - startRec);
            
            if (!readFully(fd, buf.data(), n * RECORD_SIZE, static_cast<off_t>(startRec) * RECORD_SIZE)) {
                cerr << "read failed on chunk " << c << "\n";
                return;
            }

            // build small key+index array and sort ONLY that
            keys.resize(n);
            for (size_t i = 0; i < n; i++) {
                memcpy(keys[i].key, &buf[i * RECORD_SIZE], 10);
                keys[i].idx = static_cast<uint32_t>(i);
            }
            sort(keys.begin(), keys.end(),
                 [](const KeyIdx& a, const KeyIdx& b){
                     return memcmp(a.key, b.key, 10) < 0;
                 });

            // reorder the buffer ONE time
            for (size_t i = 0; i < n; i++) {
                memcpy(&out[i * RECORD_SIZE], &buf[keys[i].idx * RECORD_SIZE], RECORD_SIZE);
            }

            // one sequential write of full, sorted records
            string path = "run" + to_string(c) + ".dat";
            int rfd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (rfd < 0) { perror(path.c_str()); return; }
            writeFully(rfd, out.data(), n * RECORD_SIZE, 0);
            close(rfd);
        }
    };

    // spin up the threads
    vector<thread> pool;
    for (int t = 0; t < T; t++) {
        pool.emplace_back(worker);
    }
    
    // wait for all threads to finish
    for (auto& th : pool) {
        th.join();   
    }
    
    close(fd);

    auto end = chrono::high_resolution_clock::now(); 
    chrono::duration<double> elapsed = end - start;

    cout << "Split time: "
         << elapsed.count() << " seconds"
         << "  [threads=" << T 
         << " chunks=" << nchunks
         << " records=" << totalRecs << "]\n";

    return 0;
}