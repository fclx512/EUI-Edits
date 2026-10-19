#include "model/i18n.h"
#include "model/vault.h"

#include "model/text_file.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neo::vault {
struct PageCursor {
    std::filesystem::directory_iterator iterator;
    std::mutex mutex;
    std::atomic<bool> closed{false};
    void requestClose() {
        closed.store(true, std::memory_order_relaxed);
        if (mutex.try_lock()) {
            iterator = {};
            mutex.unlock();
        }
    }
};
namespace {

namespace fs = std::filesystem;

std::string toLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool containsAsciiCaseInsensitive(const std::string& value, const std::string& loweredNeedle) {
    if (loweredNeedle.empty()) return true;
    if (loweredNeedle.size() > value.size()) return false;

    const std::size_t lastStart = value.size() - loweredNeedle.size();
    for (std::size_t start = 0; start <= lastStart; ++start) {
        std::size_t offset = 0;
        while (offset < loweredNeedle.size() &&
               static_cast<char>(std::tolower(static_cast<unsigned char>(value[start + offset]))) ==
                   loweredNeedle[offset]) {
            ++offset;
        }
        if (offset == loweredNeedle.size()) return true;
    }
    return false;
}

std::string joinRelative(const std::string& parent, const std::string& name) {
    return parent.empty() ? name : parent + "/" + name;
}

void appendWarning(ScanResult& result, const std::string& relative,
                   const std::error_code& error) {
    if (!result.warning.empty()) result.warning += "\n";
    result.warning += i18n::format("vault.scan_error", {{"path", relative.empty() ? i18n::tr("vault.root") : relative}, {"error", error.message()}});
}

bool entryLess(const Entry& left, const Entry& right) {
    if (left.isDir != right.isDir) return left.isDir;
    const std::string leftLower = toLowerAscii(left.name);
    const std::string rightLower = toLowerAscii(right.name);
    return leftLower == rightLower ? left.name < right.name : leftLower < rightLower;
}

bool readDirectoryPage(const fs::path& directory, const std::string& relative,
                       std::shared_ptr<PageCursor> cursor,
                       const std::function<bool()>& cancelled, ScanResult& result) {
    if (!cursor) {
        cursor = std::make_shared<PageCursor>();
        std::error_code iteratorError;
        cursor->iterator = fs::directory_iterator(directory, fs::directory_options::none, iteratorError);
        if (iteratorError) {
            appendWarning(result, relative, iteratorError);
            return false;
        }
    }
    std::lock_guard<std::mutex> cursorGuard(cursor->mutex);
    if (cursor->closed.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        cursor->iterator = {};
        return false;
    }
    std::size_t examined = 0;
    while (cursor->iterator != fs::directory_iterator{} && examined < kPageSize) {
        if ((cancelled && cancelled()) || cursor->closed.load(std::memory_order_relaxed)) {
            result.cancelled = true;
            if (cursor->closed.load(std::memory_order_relaxed)) cursor->iterator = {};
            return false;
        }
        const fs::directory_entry item = *cursor->iterator;
        ++examined;
        std::error_code entryError;
        const std::string name = textfile::pathToUtf8(item.path().filename());
        bool directoryLink = false;
        bool directoryEntry = false;
        bool regularEntry = false;
#if defined(_WIN32)
        // The iterator's directory_entry already contains enumeration metadata.
        // Only ordinary files use that snapshot: directories and links retain
        // the tag/error checks below. This does not read file contents.
        std::error_code cachedError;
        const auto cachedStatus = item.symlink_status(cachedError);
        regularEntry = !cachedError && fs::is_regular_file(cachedStatus);
        // Junctions and symbolic links are reparse points, but cloud placeholders
        // and other reparse directories may contain listable children. Inspect
        // the tag so only link-like directory entries are leaves.
        const DWORD attributes = regularEntry ? INVALID_FILE_ATTRIBUTES
                                             : GetFileAttributesW(item.path().c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            directoryEntry = true;
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                WIN32_FIND_DATAW data{};
                HANDLE find = FindFirstFileW(item.path().c_str(), &data);
                if (find == INVALID_HANDLE_VALUE) {
                    appendWarning(result, joinRelative(relative, name),
                                  std::error_code(static_cast<int>(GetLastError()), std::system_category()));
                    // Unknown tag: do not risk following a cycle when metadata
                    // cannot be read. Keep the directory item and report partial scan.
                    directoryLink = true;
                } else {
                    const DWORD tag = data.dwReserved0;
                    FindClose(find);
                    directoryLink = tag == IO_REPARSE_TAG_SYMLINK ||
                                    tag == IO_REPARSE_TAG_MOUNT_POINT;
                }
            }
        }
#endif
        if (!regularEntry && !directoryEntry) {
            directoryLink = item.is_symlink(entryError);
            directoryEntry = !entryError && item.is_directory(entryError);
        }

        if (!name.empty() && directoryEntry && !entryError) {
            Entry entry;
            entry.name = name;
            entry.relative = joinRelative(relative, name);
            entry.isDir = true;
            // Link-like directories are leaves. Ordinary directories stay unopened.
            entry.childrenLoaded = directoryLink;
            entry.linkLeaf = directoryLink;
            result.roots.push_back(std::move(entry));
        } else if (!name.empty() && !entryError && (regularEntry || item.is_regular_file(entryError))) {
            Entry entry;
            entry.name = name;
            entry.relative = joinRelative(relative, name);
            entry.isDir = false;
            result.roots.push_back(std::move(entry));
        }

        // Use the error-code overload so one unreadable entry does not abort the
        // remaining siblings. Permission-denied directories remain visible above.
        std::error_code incrementError;
        cursor->iterator.increment(incrementError);
        if (incrementError) {
            // The iterator cannot reliably continue after this error. Keep the
            // entries already found and surface the partial result to the UI.
            appendWarning(result, relative, incrementError);
            break;
        }
    }

    if (cursor->closed.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        cursor->iterator = {};
        return false;
    }
    result.hasMore = cursor->iterator != fs::directory_iterator{};
    if (result.hasMore) result.cursor = std::move(cursor);
    std::sort(result.roots.begin(), result.roots.end(), entryLess);
    for (const auto& entry : result.roots) {
        if (entry.isDir) ++result.directoryCount;
        else ++result.fileCount;
    }
    return true;
}

void appendRows(const std::vector<Entry>& entries,
                const std::set<std::string>& expanded,
                int depth,
                std::vector<Row>& rows) {
    for (const Entry& entry : entries) {
        Row row;
        row.name = entry.name;
        row.relative = entry.relative;
        row.depth = depth;
        row.isDir = entry.isDir;
        row.expanded = entry.isDir && expanded.count(entry.relative) > 0;
        rows.push_back(std::move(row));
        if (entry.isDir && expanded.count(entry.relative) > 0) {
            appendRows(entry.children, expanded, depth + 1, rows);
            if (entry.hasMore || entry.cacheLimitReached || !entry.pageError.empty())
                rows.push_back({{}, entry.relative, depth + 1, false, false, true,
                                entry.cacheLimitReached, entry.pageError});
        }
    }
}

void appendFilteredFiles(const std::vector<Entry>& entries,
                         const std::string& needle,
                         std::vector<Row>& rows) {
    for (const Entry& entry : entries) {
        if (entry.isDir) {
            appendFilteredFiles(entry.children, needle, rows);
            continue;
        }
        if (!containsAsciiCaseInsensitive(entry.relative, needle)) continue;
        Row row;
        row.name = entry.relative;
        row.relative = entry.relative;
        row.depth = 0;
        row.isDir = false;
        row.expanded = false;
        rows.push_back(std::move(row));
    }
}

} // namespace

ScanResult scan(const std::string& root) {
    return scanPage(root, {});
}

void closeCursors(const ScanResult& result) {
    if (result.cursor) result.cursor->requestClose();
    const auto closeChildren = [&](const auto& self, const std::vector<Entry>& entries) -> void {
        for (const auto& entry : entries) {
            if (entry.cursor) entry.cursor->requestClose();
            self(self, entry.children);
        }
    };
    closeChildren(closeChildren, result.roots);
}

void closeCursors(const Entry& entry) {
    if (entry.cursor) entry.cursor->requestClose();
    for (const auto& child : entry.children) closeCursors(child);
}

void closeCursor(const std::shared_ptr<PageCursor>& cursor) {
    if (cursor) cursor->requestClose();
}

ScanResult scanPage(const std::string& root, const std::string& relative,
                    std::shared_ptr<PageCursor> cursor,
                    const std::function<bool()>& cancelled) {
    ScanResult result;
    if (root.empty()) {
        result.error = i18n::tr("vault.no_directory");
        return result;
    }

    std::error_code error;
    const fs::path rootPath = textfile::pathFromUtf8(root);
    if (!fs::is_directory(rootPath, error)) {
        result.error = i18n::tr("vault.unavailable");
        return result;
    }

    if (!readDirectoryPage(relative.empty() ? rootPath : rootPath / textfile::pathFromUtf8(relative),
                           relative, std::move(cursor), cancelled, result)) {
        if (result.cancelled) return result;
        result.error = result.warning.empty() ? i18n::tr("vault.read_failed") : result.warning;
        result.warning.clear();
        return result;
    }
    result.ok = true;
    return result;
}

void flatten(const ScanResult& result, const std::set<std::string>& expanded, std::vector<Row>& rows) {
    rows.clear();
    appendRows(result.roots, expanded, 0, rows);
    if (result.hasMore || result.cacheLimitReached || !result.pageError.empty())
        rows.push_back({{}, {}, 0, false, false, true, result.cacheLimitReached, result.pageError});
}

void flattenFiltered(const ScanResult& result, const std::string& filter, std::vector<Row>& rows) {
    rows.clear();
    const std::string needle = toLowerAscii(filter);
    appendFilteredFiles(result.roots, needle, rows);
}

} // namespace neo::vault
