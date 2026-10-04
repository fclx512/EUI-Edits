#pragma once

#include "model/text_file.h"
#include "model/i18n.h"
#include <array>
#include <cstdint>
#include <fstream>
#include <functional>

namespace neo::filesafety {
enum class DiskStatus { Unknown, Present, Missing, Error };
struct Fingerprint {
    DiskStatus status = DiskStatus::Unknown;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type modified{};
    std::uint64_t hash = 14695981039346656037ull;
    std::uint64_t hash2 = 0x9e3779b97f4a7c15ull;
    std::string error;
};
inline bool same(const Fingerprint& a, const Fingerprint& b) {
    if (a.status != b.status) return false;
    if (a.status == DiskStatus::Missing) return true;
    return a.status == DiskStatus::Present && a.size == b.size &&
        a.modified == b.modified && a.hash == b.hash && a.hash2 == b.hash2;
}
// Only called on open/reload/save. Streaming hashes also catch same-size edits
// with restored timestamps; there is no background polling of document bytes.
inline Fingerprint inspect(const std::string& path) {
    Fingerprint f;
    std::error_code ec;
    const auto native = textfile::pathFromUtf8(path);
    const auto status = std::filesystem::status(native, ec);
    if (status.type() == std::filesystem::file_type::not_found &&
        (!ec || ec == std::errc::no_such_file_or_directory)) {
        f.status = DiskStatus::Missing; return f;
    }
    const auto fail = [&] {
        f.status = DiskStatus::Error;
        f.error = ec ? ec.message() : i18n::tr("safety.inspect_failed");
        return f;
    };
    if (ec || !std::filesystem::is_regular_file(status)) return fail();
    f.size = std::filesystem::file_size(native, ec);
    if (ec) return fail();
    f.modified = std::filesystem::last_write_time(native, ec);
    if (ec || f.size > 64u * 1024u * 1024u) return fail();
    std::ifstream input(native, std::ios::binary);
    if (!input) return fail();
    std::array<char, 32768> buffer{};
    std::uintmax_t count = 0;
    while (input) {
        input.read(buffer.data(), buffer.size());
        const auto n = input.gcount(); count += static_cast<std::uintmax_t>(n);
        for (std::streamsize i = 0; i < n; ++i) {
            const auto byte = static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
            f.hash = (f.hash ^ byte) * 1099511628211ull;
            f.hash2 ^= byte + 0x9e3779b97f4a7c15ull + (f.hash2 << 6) + (f.hash2 >> 2);
        }
    }
    if (!input.eof() || input.bad() || count != f.size) return fail();
    const auto sizeAfter = std::filesystem::file_size(native, ec);
    if (ec || sizeAfter != f.size) return fail();
    const auto timeAfter = std::filesystem::last_write_time(native, ec);
    if (ec || timeAfter != f.modified) return fail();
    f.status = DiskStatus::Present; return f;
}
struct Loaded {
    textfile::LoadResult result;
    Fingerprint fingerprint;
};
inline Loaded load(const std::string& path, const textfile::ForcedEncoding* encoding = nullptr) {
    Loaded loaded;
    const auto before = inspect(path);
    if (before.status != DiskStatus::Present) {
        // Keep the loader's detailed missing/encoding/size errors.
        loaded.result = encoding ? textfile::loadWithEncoding(path, *encoding) : textfile::load(path);
        if (loaded.result.ok) { loaded.result.ok = false; loaded.result.error = before.error.empty() ? i18n::tr("safety.changed_during_open") : before.error; }
        return loaded;
    }
    loaded.result = encoding ? textfile::loadWithEncoding(path, *encoding) : textfile::load(path);
    if (!loaded.result.ok) return loaded;
    loaded.fingerprint = inspect(path);
    if (!same(before, loaded.fingerprint)) {
        loaded.result.ok = false;
        loaded.result.error = i18n::tr("safety.changed_during_open");
    }
    return loaded;
}
enum class UnsavedChoice { Save, Discard, Cancel };
// Shared by close/new/open/reload; failed or deferred saves cannot continue.
inline bool resolveUnsaved(UnsavedChoice choice, const std::function<bool()>& save,
                          const std::function<void()>& discard, const std::function<void()>& proceed) {
    if (choice == UnsavedChoice::Cancel) return false;
    if (choice == UnsavedChoice::Save && !save()) return false;
    if (choice == UnsavedChoice::Discard) discard();
    proceed(); return true;
}
} // namespace neo::filesafety
