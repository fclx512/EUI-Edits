#include "model/i18n.h"
#include "platform/file_operations.h"

#include "model/text_file.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace neo::platform {
namespace {

bool resolveVaultTarget(const std::string& vaultRoot,
                        const std::string& relativePath,
                        std::filesystem::path& absoluteTarget,
                        std::string& errorMessage) {
    if (vaultRoot.empty() || relativePath.empty()) {
        errorMessage = i18n::tr("delete.empty_path");
        return false;
    }
    if (relativePath.find('\0') != std::string::npos ||
        relativePath.find_first_of("*?") != std::string::npos) {
        errorMessage = i18n::tr("delete.invalid_chars");
        return false;
    }

    const std::filesystem::path relative = textfile::pathFromUtf8(relativePath);
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        errorMessage = i18n::tr("delete.library_only");
        return false;
    }
    for (const auto& component : relative) {
        if (component == ".." || component == ".") {
            errorMessage = i18n::tr("delete.outside");
            return false;
        }
#if defined(_WIN32)
        const std::wstring native = component.native();
        if (native.find(L':') != std::wstring::npos) {
            errorMessage = i18n::tr("delete.stream");
            return false;
        }
        if (!native.empty() && (native.back() == L' ' || native.back() == L'.')) {
            errorMessage = i18n::tr("delete.trailing");
            return false;
        }
#endif
    }

    std::error_code error;
    std::filesystem::path root = std::filesystem::absolute(textfile::pathFromUtf8(vaultRoot), error);
    if (error) {
        errorMessage = i18n::format("delete.library_resolve", {{"error", error.message()}});
        return false;
    }
    root = root.lexically_normal();
    if (!std::filesystem::is_directory(root, error) || error) {
        errorMessage = error ? i18n::format("delete.library_unavailable", {{"error", error.message()}}) : i18n::tr("delete.not_folder");
        return false;
    }
    error.clear();
    absoluteTarget = (root / relative).lexically_normal();
    if (absoluteTarget == root || relative == "." || relative.empty()) {
        errorMessage = i18n::tr("delete.root");
        return false;
    }

    // The input is relative and has no parent traversal, but check the
    // normalized result as well before passing it to a platform API.
    const std::filesystem::path within = absoluteTarget.lexically_relative(root);
    if (within.empty() || within == "." || within.is_absolute()) {
        errorMessage = i18n::tr("delete.not_current");
        return false;
    }
    for (const auto& component : within) {
        if (component == "..") {
            errorMessage = i18n::tr("delete.not_current");
            return false;
        }
    }

    // Resolve every traversed parent so an in-vault symlink/junction cannot
    // redirect deletion outside the selected root. Deliberately leave the
    // final entry unresolved: deleting a final symlink/junction acts on that
    // entry itself instead of traversing its contents.
    const std::filesystem::path canonicalRoot = std::filesystem::canonical(root, error);
    if (error) {
        errorMessage = i18n::format("delete.library_access", {{"error", error.message()}});
        return false;
    }
    const std::filesystem::path canonicalParent =
        std::filesystem::canonical(absoluteTarget.parent_path(), error);
    if (error) {
        errorMessage = i18n::format("delete.parent_access", {{"error", error.message()}});
        return false;
    }

    auto pathsEqual = [](const std::filesystem::path& left,
                         const std::filesystem::path& right) {
#if defined(_WIN32)
        const std::wstring a = left.native();
        const std::wstring b = right.native();
        return ::CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                      static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
#else
        return left == right;
#endif
    };
    auto pathIsWithin = [&](const std::filesystem::path& candidate,
                            const std::filesystem::path& base) {
        auto candidateIt = candidate.begin();
        for (auto baseIt = base.begin(); baseIt != base.end(); ++baseIt, ++candidateIt) {
            if (candidateIt == candidate.end() || !pathsEqual(*candidateIt, *baseIt)) {
                return false;
            }
        }
        return true;
    };
    if (!pathIsWithin(canonicalParent, canonicalRoot)) {
        errorMessage = i18n::tr("delete.parent_outside");
        return false;
    }
    return true;
}

} // namespace

DeleteResult validateVaultDeleteTarget(const std::string& vaultRoot,
                                      const std::string& relativePath) {
    std::filesystem::path unusedTarget;
    std::string errorMessage;
    if (!resolveVaultTarget(vaultRoot, relativePath, unusedTarget, errorMessage)) {
        return {DeleteStatus::Error, std::move(errorMessage)};
    }
    return {DeleteStatus::Success, {}};
}

DeleteResult deleteVaultEntry(const std::string& vaultRoot,
                              const std::string& relativePath,
                              bool isDirectory) {
    std::filesystem::path target;
    std::string errorMessage;
    if (!resolveVaultTarget(vaultRoot, relativePath, target, errorMessage)) {
        return {DeleteStatus::Error, std::move(errorMessage)};
    }

#if defined(_WIN32)
    (void)isDirectory;
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(target, error);
    if (error) {
        return {DeleteStatus::Error, i18n::format("delete.path_resolve", {{"error", error.message()}})};
    }

    // SHFileOperation requires a double-NUL-terminated list, even for one path.
    std::wstring from = absolute.lexically_normal().wstring();
    from.push_back(L'\0');
    from.push_back(L'\0');

    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_WANTNUKEWARNING;
    const int result = SHFileOperationW(&operation);
    if (operation.fAnyOperationsAborted) {
        return {DeleteStatus::Cancelled, i18n::tr("delete.cancelled")};
    }
    if (result != 0) {
        return {DeleteStatus::Error,
                i18n::format("delete.shell_error", {{"code", std::to_string(result)}})};
    }
    return {DeleteStatus::Success, {}};
#else
    std::error_code error;
    if (isDirectory) {
        std::filesystem::remove_all(target, error);
    } else {
        std::filesystem::remove(target, error);
    }
    if (error) {
        return {DeleteStatus::Error, error.message()};
    }
    return {DeleteStatus::Success, i18n::tr("delete.permanent_done")};
#endif
}

} // namespace neo::platform
