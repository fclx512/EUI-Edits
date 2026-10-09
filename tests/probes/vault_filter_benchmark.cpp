#include "model/vault.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

long long elapsedMicroseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: vault_filter_benchmark <vault-root>\n";
        return 2;
    }

    const std::string root = argv[1];
    const auto scanBegin = Clock::now();
    const neo::vault::ScanResult scanned = neo::vault::scan(root);
    const auto scanEnd = Clock::now();
    if (!scanned.ok) {
        std::cerr << "scan failed: " << scanned.error << '\n';
        return 1;
    }
    std::cout << "{\"kind\":\"scan\",\"elapsed_us\":"
              << elapsedMicroseconds(scanBegin, scanEnd)
              << ",\"files\":" << scanned.fileCount
              << ",\"directories\":" << scanned.directoryCount
              << ",\"warning_bytes\":" << scanned.warning.size() << "}\n";

    // These are prefixes a user types when searching for the known deep fixture.
    // FlattenFiltered visits the entire scan tree on each input change.
    const std::vector<std::string> filters{
        "c", "cr", "cro", "cross", "cross-11", "cross-11.md", "no-such-page-xyz"
    };
    std::vector<neo::vault::Row> rows;
    for (const std::string& filter : filters) {
        neo::vault::flattenFiltered(scanned, filter, rows); // warm-up
        for (int round = 1; round <= 5; ++round) {
            const auto begin = Clock::now();
            neo::vault::flattenFiltered(scanned, filter, rows);
            const auto end = Clock::now();
            std::cout << "{\"kind\":\"filter\",\"filter\":\"" << filter
                      << "\",\"round\":" << round
                      << ",\"elapsed_us\":" << elapsedMicroseconds(begin, end)
                      << ",\"matches\":" << rows.size() << "}\n";
        }
        if (filter == "cross-11.md" && rows.size() != 1) {
            std::cerr << "expected exactly one deep match, observed " << rows.size() << '\n';
            return 1;
        }
    }
    return 0;
}
