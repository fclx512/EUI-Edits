#include "platform/vault_rename.h"
#include "model/text_file.h"
#include "model/i18n.h"
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <system_error>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif

namespace neo::platform {
namespace {
namespace fs = std::filesystem;
bool equalPart(const fs::path& a, const fs::path& b) {
#if defined(_WIN32)
    const auto x = a.native(), y = b.native();
    return CompareStringOrdinal(x.data(), static_cast<int>(x.size()), y.data(),
                                static_cast<int>(y.size()), TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
}
bool within(const fs::path& path, const fs::path& base) {
    auto p = path.begin();
    for (auto b = base.begin(); b != base.end(); ++b, ++p)
        if (p == path.end() || !equalPart(*p, *b)) return false;
    return true;
}
bool entryExists(const fs::path& path, std::error_code& ec) {
    const auto status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found &&
        (!ec || ec == std::errc::no_such_file_or_directory)) { ec.clear(); return false; }
    return !ec;
}
RenameResult conflict(const fs::path& target, bool isDirectory) {
    const auto stem = isDirectory ? target.filename() : target.stem();
    const auto extension = isDirectory ? fs::path{} : target.extension();
    for (int n = 2; n <= 10000; ++n) {
        const std::string candidate = textfile::pathToUtf8(stem) + " (" + std::to_string(n) + ")" + textfile::pathToUtf8(extension);
        if (!validateRenameName(candidate).empty()) break;
        std::error_code ec;
        const bool exists = entryExists(target.parent_path() / textfile::pathFromUtf8(candidate), ec);
        if (ec) return {RenameStatus::Error, ec.message(), {}};
        if (!exists) return {RenameStatus::Conflict,
            i18n::format("rename.conflict", {{"name", candidate}}), candidate};
    }
    return {RenameStatus::Error, i18n::tr("rename.no_number"), {}};
}
} // namespace

std::string validateRenameName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return i18n::tr("rename.empty");
    if (name.find_first_of("<>:\"/\\|?*") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; }))
        return i18n::tr("rename.invalid");
    if (name.back() == '.' || name.back() == ' ') return i18n::tr("rename.trailing");
    auto base = name.substr(0, name.find('.'));
    while (!base.empty() && base.back() == ' ') base.pop_back();
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) {
        return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    });
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
        base == "CONIN$" || base == "CONOUT$" ||
        ((base.rfind("COM", 0) == 0 || base.rfind("LPT", 0) == 0) &&
         ((base.size() == 4 && base[3] >= '1' && base[3] <= '9') ||
          base.substr(3) == "\xC2\xB9" || base.substr(3) == "\xC2\xB2" || base.substr(3) == "\xC2\xB3")))
        return i18n::tr("rename.reserved");
    try {
        const auto native = textfile::pathFromUtf8(name).native();
#if defined(_WIN32)
        if (native.size() > 255) return i18n::tr("rename.long");
#else
        if (name.size() > 255) return i18n::tr("rename.long");
#endif
    } catch (const fs::filesystem_error&) { return i18n::tr("rename.invalid"); }
    return {};
}

int renameSelectionEnd(const std::string& name, bool isDirectory) {
    const auto dot = name.find_last_of('.');
    return static_cast<int>(!isDirectory && dot != std::string::npos && dot != 0 ? dot : name.size());
}

bool sameVaultRelativePath(const std::string& a, const std::string& b) {
    return equalPart(textfile::pathFromUtf8(a), textfile::pathFromUtf8(b));
}

RenameResult renameVaultEntry(const std::string& vaultRoot, const std::string& relativeName,
                             const std::string& newName) {
    const auto invalid = validateRenameName(newName);
    if (!invalid.empty()) return {RenameStatus::Error, invalid, {}};
    if (vaultRoot.empty() || relativeName.empty()) return {RenameStatus::Error, i18n::tr("rename.library_only"), {}};
    try {
        const auto relative = textfile::pathFromUtf8(relativeName);
        if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
            return {RenameStatus::Error, i18n::tr("rename.library_only"), {}};
        for (const auto& part : relative)
            if (!validateRenameName(textfile::pathToUtf8(part)).empty())
                return {RenameStatus::Error, i18n::tr("rename.library_only"), {}};
        std::error_code ec;
        const auto root = fs::canonical(textfile::pathFromUtf8(vaultRoot), ec);
        if (ec || !fs::is_directory(root, ec)) return {RenameStatus::Error, i18n::tr("rename.library_only"), {}};
        const auto source = (root / relative).lexically_normal();
        const auto parent = fs::canonical(source.parent_path(), ec);
        if (ec || !within(parent, root)) return {RenameStatus::Error, i18n::tr("rename.parent_outside"), {}};
        const auto target = parent / textfile::pathFromUtf8(newName);
        const auto actualSource = parent / source.filename();
        if (!entryExists(actualSource, ec) || ec)
            return {RenameStatus::Error, ec ? ec.message() : i18n::tr("rename.missing"), {}};
        if (source.filename() == target.filename()) return {RenameStatus::Success, {}, {}};
        const bool destinationExists = entryExists(target, ec);
        if (ec) return {RenameStatus::Error, ec.message(), {}};
        const bool caseOnly = equalPart(actualSource.filename(), target.filename());
        const bool isDirectory = fs::is_directory(fs::symlink_status(actualSource, ec));
        if (ec) return {RenameStatus::Error, ec.message(), {}};
        if (destinationExists && !caseOnly) return conflict(target, isDirectory);
        // Recheck canonical parent immediately before native IO; use its resolved
        // path so an existing in-vault junction cannot redirect this operation.
        if (fs::canonical(source.parent_path(), ec) != parent || ec)
            return {RenameStatus::Error, i18n::tr("rename.parent_outside"), {}};
#if defined(_WIN32)
        if (MoveFileExW(actualSource.c_str(), target.c_str(), 0)) return {RenameStatus::Success, {}, {}};
        const auto code = GetLastError();
        if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) return conflict(target, isDirectory);
        return {RenameStatus::Error, std::system_category().message(static_cast<int>(code)), {}};
#elif defined(__linux__) && defined(SYS_renameat2)
        if (syscall(SYS_renameat2, AT_FDCWD, actualSource.c_str(), AT_FDCWD, target.c_str(), RENAME_NOREPLACE) == 0)
            return {RenameStatus::Success, {}, {}};
        if (errno == EEXIST) return conflict(target, isDirectory);
        return {RenameStatus::Error, std::generic_category().message(errno), {}};
#elif defined(__APPLE__)
        if (renamex_np(actualSource.c_str(), target.c_str(), RENAME_EXCL) == 0) return {RenameStatus::Success, {}, {}};
        if (errno == EEXIST) return conflict(target, isDirectory);
        return {RenameStatus::Error, std::generic_category().message(errno), {}};
#else
        return {RenameStatus::Error, i18n::tr("rename.unsupported"), {}};
#endif
    } catch (const fs::filesystem_error& error) { return {RenameStatus::Error, error.what(), {}}; }
}
} // namespace neo::platform
