// Scan wall time only: no application action, message pump, rendering or display.
#include "model/text_file.h"
#include "model/vault.h"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

#ifdef VAULT_SCAN_PROFILE
#include "scan_profile.h"
#endif

namespace {
std::uint64_t fingerprint(const std::vector<neo::vault::Entry>& entries,
                          std::uint64_t hash = 14695981039346656037ull) {
    for (const auto& entry : entries) {
        for (unsigned char byte : entry.relative) hash = (hash ^ byte) * 1099511628211ull;
        hash = (hash ^ (entry.isDir ? 1u : 0u)) * 1099511628211ull;
        hash = fingerprint(entry.children, hash);
        hash = (hash ^ 255u) * 1099511628211ull;
    }
    return hash;
}

int run(const std::string& root, int rounds, int warmups) {
    if (rounds < 1 || rounds > 100 || warmups < 0 || warmups > 100) return 2;
    std::uint64_t expected = 0;
    bool first = true;
    std::cout << std::fixed << std::setprecision(3);
    for (int round = -warmups; round < rounds; ++round) {
#ifdef VAULT_SCAN_PROFILE
        scan_profile::reset();
#endif
        const auto begin = std::chrono::steady_clock::now();
        const auto scanned = neo::vault::scan(root);
        const auto end = std::chrono::steady_clock::now();
        if (!scanned.ok || !scanned.warning.empty()) {
            std::cerr << "scan failed or partial: " << scanned.error << scanned.warning << '\n';
            return 1;
        }
        // Validation deliberately follows the timed scan.
        const auto hash = fingerprint(scanned.roots);
        if (!first && hash != expected) {
            std::cerr << "tree changed during measurement\n";
            return 1;
        }
        expected = hash;
        first = false;
        std::cout << "{\"round\":" << round << ",\"scan_us\":"
                  << std::chrono::duration<double, std::micro>(end - begin).count()
                  << ",\"files\":" << scanned.fileCount
                  << ",\"directories\":" << scanned.directoryCount
                  << ",\"fingerprint\":\"" << hash << "\",\"warning_bytes\":0";
#ifdef VAULT_SCAN_PROFILE
        std::cout << ",\"attributes_calls\":" << scan_profile::calls
                  << ",\"attributes_us\":" << scan_profile::microseconds;
#endif
        std::cout << "}\n";
    }
    return 0;
}
} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 4) {
        std::cerr << "usage: vault_scan_benchmark <root> [rounds=5] [warmups=1]\n";
        return 2;
    }
    try {
        return run(neo::textfile::pathToUtf8(std::filesystem::path(argv[1])),
                   argc > 2 ? std::stoi(argv[2]) : 5, argc > 3 ? std::stoi(argv[3]) : 1);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
#else
int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) return 2;
    try {
        return run(argv[1], argc > 2 ? std::stoi(argv[2]) : 5, argc > 3 ? std::stoi(argv[3]) : 1);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
#endif
