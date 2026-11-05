// CLI version.  Usage:
//   ./vecsearch                 -> uses/creates mock vectors.bin
//   ./vecsearch data.csv        -> imports a CSV or JSON file, then searches
#include <chrono>
#include <iostream>
#include <random>
#include <sys/stat.h>
#include "VectorEngine.hpp"

int main(int argc, char** argv) {
    const std::string filename = "vectors.bin";
    const int top_k = 5;
    std::string err;

    std::cout << "===========================================\n";
    std::cout << "  MEMORY-MAPPED VECTOR SEARCH ENGINE v1.1  \n";
    std::cout << "===========================================\n\n";

    if (argc > 1) {
        std::cout << "[1/4] Importing " << argv[1] << "...\n";
        std::string text;
        VectorData vd;
        if (!readWholeFile(argv[1], text)) { std::cerr << "Error: cannot read " << argv[1] << "\n"; return 1; }
        if (!loadVectorText(argv[1], text, vd, err) || !saveDatabase(filename, vd, err)) {
            std::cerr << "Error: " << err << "\n";
            return 1;
        }
    } else {
        struct stat st;
        if (stat(filename.c_str(), &st) != 0) {   // only generate when missing
            std::cout << "[1/4] Generating mock database (100000 x 128)...\n";
            if (!generateMockDatabase(filename, 100000, 128)) { std::cerr << "Error: cannot write mock DB\n"; return 1; }
        } else {
            std::cout << "[1/4] Using existing " << filename << "\n";
        }
    }

    MappedDatabase db;
    if (!db.open(filename, err)) { std::cerr << "Error: " << err << "\n"; return 1; }
    std::cout << "[2/4] Memory-mapped " << db.count() << " vectors, " << db.dim() << "-dim ("
              << db.sizeBytes() / 1024 << " KB).\n";

    // Query: random for the mock DB, or the first vector of an imported DB
    std::vector<float> query(db.dim());
    if (argc > 1) {
        std::copy(db.vectors(), db.vectors() + db.dim(), query.begin());
    } else {
        std::mt19937 gen(1337);
        std::uniform_real_distribution<float> dist(-10.0f, 10.0f);
        for (auto& x : query) x = dist(gen);
    }

    std::cout << "[3/4] Executing Top-" << top_k << " cosine similarity scan...\n";
    auto t0 = std::chrono::high_resolution_clock::now();
    auto results = searchTopK(db.vectors(), db.count(), db.dim(), query, top_k);
    auto t1 = std::chrono::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    std::cout << "\n[4/4] Search complete\n-------------------------------------------\n";
    for (size_t r = 0; r < results.size(); ++r)
        std::cout << " Rank " << r + 1 << " | Vector ID: #" << results[r].id << " | Score: " << results[r].score << "\n";
    std::cout << "-------------------------------------------\n"
              << " > Vectors scanned : " << db.count() << "\n"
              << " > Latency         : " << us << " us (" << us / 1000.0 << " ms)\n"
              << "===========================================\n";
    return 0;
}