#include "model/i18n.h"
#include "model/vault.h"

#include "model/text_file.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neo::vault {
namespace {

namespace fs = std::filesystem;

std::string toLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string joinRelative(const std::string& parent, const std::string& name) {
    return parent.empty() ? name : parent + "/" + name;
}

void appendWarning(ScanResult& result, const std::string& relative,
                   const std::error_code& error) {
    if (!result.warning.empty()) result.warning += "\n";
    result.warning += i18n::format("vault.scan_error", {{"path", relative.empty() ? i18n::tr("vault.root") : relative}, {"error", error.message()}});
}

bool readDirectory(const fs::path& directory,
                   const std::string& relative,
                   ScanResult& result,
                   std::vector<Entry>& out) {
    std::vector<Entry> directories;
    std::vector<Entry> files;

    std::error_code iteratorError;
    fs::directory_iterator iterator(directory, fs::directory_options::none, iteratorError);
    if (iteratorError) {
        appendWarning(result, relative, iteratorError);
        return false;
    }

    while (iterator != fs::directory_iterator{}) {
        const fs::directory_entry item = *iterator;
        std::error_code entryError;
        const std::string name = textfile::pathToUtf8(item.path().filename());
        bool directoryLink = false;
        bool directoryEntry = false;
#if defined(_WIN32)
        // Junctions and symbolic links are reparse points, but cloud placeholders
        // and other reparse directories may contain listable children. Inspect
        // the tag so only link-like directory entries are leaves.
        const DWORD attributes = GetFileAttributesW(item.path().c_str());
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
        if (!directoryEntry) {
            directoryLink = item.is_symlink(entryError);
            directoryEntry = !entryError && item.is_directory(entryError);
        }

        if (!name.empty() && directoryEntry && !entryError) {
            Entry entry;
            entry.name = name;
            entry.relative = joinRelative(relative, name);
            entry.isDir = true;
            // A directory link is listed as a leaf. Following links can escape the
            // chosen tree or form cycles; ordinary directories are scanned completely.
            if (!directoryLink) readDirectory(item.path(), entry.relative, result, entry.children);
            ++result.directoryCount;
            directories.push_back(std::move(entry));
        } else if (!name.empty() && !entryError && item.is_regular_file(entryError)) {
            Entry entry;
            entry.name = name;
            entry.relative = joinRelative(relative, name);
            entry.isDir = false;
            ++result.fileCount;
            files.push_back(std::move(entry));
        }

        // Use the error-code overload so one unreadable entry does not abort the
        // remaining siblings. Permission-denied directories remain visible above.
        std::error_code incrementError;
        iterator.increment(incrementError);
        if (incrementError) {
            // The iterator cannot reliably continue after this error. Keep the
            // entries already found and surface the partial result to the UI.
            appendWarning(result, relative, incrementError);
            break;
        }
    }

    const auto byName = [](const Entry& left, const Entry& right) {
        const std::string leftLower = toLowerAscii(left.name);
        const std::string rightLower = toLowerAscii(right.name);
        return leftLower == rightLower ? left.name < right.name : leftLower < rightLower;
    };
    std::sort(directories.begin(), directories.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    out.reserve(directories.size() + files.size());
    out.insert(out.end(), std::make_move_iterator(directories.begin()), std::make_move_iterator(directories.end()));
    out.insert(out.end(), std::make_move_iterator(files.begin()), std::make_move_iterator(files.end()));
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
        if (toLowerAscii(entry.relative).find(needle) == std::string::npos) continue;
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

    if (!readDirectory(rootPath, {}, result, result.roots)) {
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
}

void flattenFiltered(const ScanResult& result, const std::string& filter, std::vector<Row>& rows) {
    rows.clear();
    const std::string needle = toLowerAscii(filter);
    appendFilteredFiles(result.roots, needle, rows);
}

} // namespace neo::vault
