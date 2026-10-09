// Headless behavioral checks for the app-level vault deletion and relocation
// actions. The platform deletion and persistence/dialog APIs are stubbed so no
// Shell UI, user files, registry, or user configuration can be touched.

#include "eui/platform.h"
#include "model/settings.h"
#include "model/text_file.h"
#include "model/theme_loader.h"
#include "model/vault.h"
#include "platform/file_operations.h"
#include "platform/font_safety.h"
#include "platform/native_dialogs.h"
#include "state/app_actions.h"
#include "state/session_writer.h"
#include "state/vault_cache.h"
#include "core/platform/async.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace fs = std::filesystem;

namespace {

int failures = 0;
int recoveryCalls = 0;
int deleteCalls = 0;
int directoryPickCalls = 0;
bool recoveryWriteOk = true;
bool deleteStubRemovesTarget = false;
fs::path allowedTestBase;
neo::platform::DeleteResult nextDeleteResult{neo::platform::DeleteStatus::Success, {}};
neo::dialogs::PickResult nextDirectoryPick{};
std::string lastRecoveryText;
std::string lastRecoveryOrigin;
std::string lastDirectoryInitialPath;

void check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool pathInside(const fs::path& candidate, const fs::path& base) {
    std::error_code error;
    const fs::path absoluteCandidate = fs::absolute(candidate, error).lexically_normal();
    if (error) return false;
    const fs::path absoluteBase = fs::absolute(base, error).lexically_normal();
    if (error) return false;
    const fs::path relative = absoluteCandidate.lexically_relative(absoluteBase);
    return !relative.empty() && relative != "." && !relative.is_absolute() &&
           *relative.begin() != "..";
}

struct TempTree {
    fs::path base;
    fs::path vault;
    fs::path otherVault;
    bool safe = false;

    TempTree() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path temp = fs::temp_directory_path();
        base = temp / ("neo-vault-delete-state-" + std::to_string(stamp));
        vault = base / "vault";
        otherVault = base / "other-vault";
        fs::create_directories(vault);
        fs::create_directories(otherVault);
        safe = pathInside(base, temp) && base.filename().string().rfind("neo-vault-delete-state-", 0) == 0;
    }

    ~TempTree() {
        // Only remove this test's uniquely named directory after confirming it
        // is an absolute descendant of the system temporary directory.
        if (safe && pathInside(base, fs::temp_directory_path())) {
            std::error_code ignored;
            fs::remove_all(base, ignored);
        }
    }
};

std::string utf8(const fs::path& path) {
    return neo::textfile::pathToUtf8(fs::absolute(path).lexically_normal());
}

bool writeSample(const fs::path& path, const std::string& bytes = "sample\n") {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    return static_cast<bool>(output);
}

fs::path flipAsciiCase(const fs::path& path) {
    std::wstring value = path.wstring();
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        if (ch >= L'a' && ch <= L'z') return static_cast<wchar_t>(ch - L'a' + L'A');
        if (ch >= L'A' && ch <= L'Z') return static_cast<wchar_t>(ch - L'A' + L'a');
        return ch;
    });
    return fs::path(value);
}

void resetStubs(const fs::path& testBase) {
    allowedTestBase = fs::absolute(testBase).lexically_normal();
    recoveryCalls = 0;
    deleteCalls = 0;
    directoryPickCalls = 0;
    recoveryWriteOk = true;
    deleteStubRemovesTarget = false;
    nextDeleteResult = {neo::platform::DeleteStatus::Success, {}};
    nextDirectoryPick = {};
    lastRecoveryText.clear();
    lastRecoveryOrigin.clear();
    lastDirectoryInitialPath.clear();
    neo::settings::current() = neo::settings::Data{};
}

neo::AppState makeState(const fs::path& root, const fs::path& document, bool dirty = false) {
    neo::AppState state;
    state.vaultRoot = utf8(root);
    state.vaultAddress = state.vaultRoot;
    state.path = utf8(document);
    state.doc.text = "kept in memory\n";
    state.revision = 42;
    state.savedRevision = dirty ? 41 : 42;
    state.doc.hadBom = true;
    state.doc.lineEnding = neo::textfile::LineEnding::CrLf;
    return state;
}

void successfulCleanCurrentFile(TempTree& tree) {
    const fs::path document = tree.vault / "current.md";
    check(writeSample(document), "create isolated current document");
    auto state = makeState(tree.vault, document);
    const std::string originalPath = state.path;
    const auto originalDocument = state.doc;
    const auto originalRevision = state.revision;
    deleteStubRemovesTarget = true;
    neo::vaultDeleteEntry(state, "current.md", false);

    check(state.path.empty(), "successful deletion clears the current file path");
    check(state.dirty(), "successful deletion makes a previously clean document dirty");
    check(state.recoveredFrom == originalPath, "successful deletion preserves the recovery origin");
    check(state.doc.text == originalDocument.text && state.doc.hadBom == originalDocument.hadBom &&
              state.doc.lineEnding == originalDocument.lineEnding,
          "successful deletion keeps the in-memory document unchanged");
    check(state.revision == originalRevision, "successful deletion leaves the document revision unchanged");
    check(recoveryCalls == 1 && lastRecoveryText == originalDocument.text &&
              lastRecoveryOrigin == originalPath,
          "successful deletion immediately writes recovery with the original path");
}

void successfulDirtyCurrentFile(TempTree& tree) {
    const fs::path document = tree.vault / "dirty.md";
    check(writeSample(document), "create isolated dirty document");
    auto state = makeState(tree.vault, document, true);
    const auto originalDocument = state.doc;
    const auto originalRevision = state.revision;
    const std::string originalPath = state.path;
    deleteStubRemovesTarget = true;
    neo::vaultDeleteEntry(state, "dirty.md", false);

    check(state.dirty() && state.path.empty(), "deleting a dirty current file keeps it unsaved");
    check(state.revision == originalRevision && state.doc.text == originalDocument.text,
          "deleting a dirty file does not alter text or revision");
    check(state.recoveredFrom == originalPath && lastRecoveryOrigin == originalPath,
          "dirty file deletion retains its origin in recovery");
}

void cancelOrErrorWithFilePresent(TempTree& tree, neo::platform::DeleteStatus status) {
    const std::string leaf = status == neo::platform::DeleteStatus::Cancelled ? "cancel.md" : "error.md";
    const fs::path document = tree.vault / leaf;
    check(writeSample(document), "create isolated document for non-success result");
    auto state = makeState(tree.vault, document);
    nextDeleteResult = {status, status == neo::platform::DeleteStatus::Cancelled
                                    ? "system cancelled"
                                    : "injected failure"};
    neo::vaultDeleteEntry(state, leaf, false);

    check(fs::exists(document), "cancel/error with an existing file does not delete it");
    check(state.path == utf8(document) && !state.dirty(),
          "cancel/error with an existing file preserves the clean document state");
    check(recoveryCalls == 0, "cancel/error with an existing file does not create recovery");
}

void partialResultWithDocumentMissing(TempTree& tree, neo::platform::DeleteStatus status) {
    const std::string leaf = status == neo::platform::DeleteStatus::Cancelled
                                 ? "partial-cancel.md"
                                 : "partial-error.md";
    const fs::path document = tree.vault / leaf;
    check(writeSample(document), "create isolated document for partial result");
    auto state = makeState(tree.vault, document);
    nextDeleteResult = {status, "injected partial result"};
    deleteStubRemovesTarget = true;
    neo::vaultDeleteEntry(state, leaf, false);

    check(!fs::exists(document), "partial-result stub removes only its isolated sample");
    check(state.path.empty() && state.dirty(), "missing document after partial result becomes unsaved");
    check(state.recoveredFrom == utf8(document) && recoveryCalls == 1 &&
              lastRecoveryOrigin == utf8(document),
          "partial result retains recovery origin and writes recovery");
}

void unrelatedDeleteDoesNotTouchDocument(TempTree& tree) {
    const fs::path document = tree.vault / "kept.md";
    const fs::path other = tree.vault / "unrelated.md";
    check(writeSample(document) && writeSample(other), "create isolated unrelated-delete samples");
    auto state = makeState(tree.vault, document);
    deleteStubRemovesTarget = true;
    neo::vaultDeleteEntry(state, "unrelated.md", false);

    check(state.path == utf8(document) && !state.dirty(), "deleting another entry preserves the clean document");
    check(recoveryCalls == 0 && fs::exists(document), "unrelated deletion does not write recovery or remove current file");
}

void finalSymlinkDeleteDoesNotFollowTarget(TempTree& tree) {
    const fs::path realDocument = tree.vault / "real" / "current.md";
    const fs::path alias = tree.vault / "alias";
    check(writeSample(realDocument), "create isolated final-symlink target document");
    std::error_code error;
    fs::create_directory_symlink(realDocument.parent_path(), alias, error);
    if (error) {
        std::cout << "SKIP: platform did not permit an isolated directory symlink: "
                  << error.message() << '\n';
        return;
    }

    auto state = makeState(tree.vault, realDocument);
    deleteStubRemovesTarget = true;
    neo::vaultDeleteEntry(state, "alias", true);
    check(!fs::exists(alias), "deleting a final symlink removes only its directory entry");
    check(fs::exists(realDocument) && state.path == utf8(realDocument) && !state.dirty(),
          "deleting a final symlink does not mark its target document deleted");
    check(recoveryCalls == 0, "final symlink deletion does not create recovery for its target");
}

void recoveryWriteFailureIsReported(TempTree& tree) {
    const fs::path document = tree.vault / "recovery-failure.md";
    check(writeSample(document), "create isolated recovery-failure sample");
    auto state = makeState(tree.vault, document);
    deleteStubRemovesTarget = true;
    recoveryWriteOk = false;
    neo::vaultDeleteEntry(state, "recovery-failure.md", false);

    check(state.path.empty() && state.dirty(), "recovery failure still leaves document dirty in memory");
    check(state.toastMessage.find("恢复副本写入失败") != std::string::npos,
          "recovery write failure is reported to the user");
}

void ancestorAndPrefixSibling(TempTree& tree) {
    const fs::path nested = tree.vault / "notes" / "child.md";
    check(writeSample(nested), "create isolated nested current document");
    auto state = makeState(tree.vault, nested);
#if defined(_WIN32)
    state.path = utf8(flipAsciiCase(nested));
#endif
    const std::string originalPath = state.path;
    deleteStubRemovesTarget = true;
    neo::vaultDeleteEntry(state, "notes", true);
    check(state.path.empty() && state.dirty() && state.recoveredFrom == originalPath,
          "deleting a case-varied ancestor directory marks the current document unsaved");
    check(lastRecoveryOrigin == originalPath, "ancestor deletion keeps the case-varied origin");

    const fs::path sibling = tree.vault / "notes-old.md";
    check(writeSample(sibling), "create isolated prefix-sibling document");
    fs::create_directories(tree.vault / "notes");
    auto siblingState = makeState(tree.vault, sibling);
    deleteStubRemovesTarget = true;
    neo::vaultDeleteEntry(siblingState, "notes", true);
    check(siblingState.path == utf8(sibling) && !siblingState.dirty(),
          "directory name prefix does not mark a sibling document as deleted");
    check(recoveryCalls == 1, "prefix sibling deletion does not write a second recovery");
}

void currentPathCaseVariantAndRelocation(TempTree& tree) {
#if defined(_WIN32)
    const fs::path caseDocument = tree.vault / "CaseDoc.md";
    check(writeSample(caseDocument), "create isolated case-variant current document");
    auto caseState = makeState(tree.vault, caseDocument);
    caseState.path = utf8(flipAsciiCase(caseDocument));
    const std::string originalCasePath = caseState.path;
    deleteStubRemovesTarget = true;
    neo::vaultDeleteEntry(caseState, "CaseDoc.md", false);
    check(caseState.path.empty() && caseState.dirty() && caseState.recoveredFrom == originalCasePath,
          "Windows case-varied current path is recognized as deleted");
#endif

    const fs::path document = tree.vault / "relocation-kept.md";
    check(writeSample(document), "create isolated relocation current document");
    auto state = makeState(tree.vault, document, true);
    const auto originalDoc = state.doc;
    const std::string originalPath = state.path;
    const auto originalRevision = state.revision;
    const auto originalSavedRevision = state.savedRevision;
    const auto originalRecent = std::vector<std::string>{"recent-marker"};
    neo::settings::current().recentFiles = originalRecent;
    neo::settings::current().lastFile = "last-marker";

    nextDirectoryPick = {false, true, {}, {}};
    neo::chooseVaultDirectory(state);
    check(directoryPickCalls == 1 && lastDirectoryInitialPath == utf8(tree.vault),
          "relocation chooser opens at the current browsing root");
    check(state.vaultRoot == utf8(tree.vault) && state.path == originalPath &&
              state.doc.text == originalDoc.text && state.dirty(),
          "relocation cancellation preserves the browsing root and current document");

    nextDirectoryPick = {true, false, utf8(tree.otherVault), {}};
    neo::chooseVaultDirectory(state);
    check(state.vaultRoot == utf8(tree.otherVault) && (state.vaultScan && state.vaultScan->ok),
          "successful relocation scans the selected isolated directory");
    check(state.path == originalPath && state.doc.text == originalDoc.text && state.dirty() &&
              state.revision == originalRevision && state.savedRevision == originalSavedRevision,
          "successful relocation preserves current document and dirty state");
    check(neo::settings::current().lastFile == "last-marker" &&
              neo::settings::current().recentFiles == originalRecent,
          "relocation leaves last and recent files unchanged");
}

bool containsEntry(const std::vector<neo::vault::Entry>& entries, const std::string& relative) {
    for (const auto& entry : entries) {
        if (entry.relative == relative) return true;
        if (entry.isDir && containsEntry(entry.children, relative)) return true;
    }
    return false;
}

void completeVaultListing(TempTree& tree) {
    const fs::path root = tree.vault / "listing-fixture";
    const auto missing = neo::vault::scan(neo::textfile::pathToUtf8(root / "missing"));
    check(!missing.ok && !missing.error.empty(), "missing vault root is an explicit scan failure");
    const fs::path hidden = root / ".hidden" / "build" / "node_modules" / "pkg";
    std::error_code error;
    fs::create_directories(hidden, error);
    check(!error, "create dot and commonly excluded nested directories");
    check(writeSample(hidden / "deep.md"), "create deep known document");
    check(writeSample(root / "unknown.bin"), "create unknown regular file");
    check(writeSample(root / "README"), "create extensionless known document");

    fs::path deep = root / "depth";
    for (int i = 0; i < 12; ++i) deep /= "d" + std::to_string(i);
    fs::create_directories(deep, error);
    check(!error && writeSample(deep / "last.txt"), "create document below the former depth cap");

    const auto all = neo::vault::scan(neo::textfile::pathToUtf8(root));
    check(all.ok, "complete listing scan completes");
    check(containsEntry(all.roots, ".hidden/build/node_modules/pkg/deep.md") &&
              containsEntry(all.roots, "depth/d0/d1/d2/d3/d4/d5/d6/d7/d8/d9/d10/d11/last.txt"),
          "dot directories, build directories, and paths beyond eight levels remain scannable");
    check(containsEntry(all.roots, "unknown.bin") && all.fileCount == 4,
          "the default complete listing includes unknown regular files without reading contents");
    std::vector<neo::vault::Row> filteredRows;
    neo::vault::flattenFiltered(all, ".hidden/build", filteredRows);
    check(filteredRows.size() == 1 &&
              filteredRows.front().relative == ".hidden/build/node_modules/pkg/deep.md",
          "path filtering emits only matching visible file rows");
    neo::vault::flattenFiltered(all, ".HIDDEN/BUILD", filteredRows);
    check(filteredRows.size() == 1 &&
              filteredRows.front().relative == ".hidden/build/node_modules/pkg/deep.md",
          "path filtering remains ASCII case-insensitive");

    const fs::path many = root / "many";
    fs::create_directories(many, error);
    check(!error, "create isolated large listing directory");
    bool created = true;
    for (int i = 0; i < 20005; ++i) {
        std::ofstream file(many / ("raw-" + std::to_string(i) + ".bin"), std::ios::binary);
        if (!file) { created = false; break; }
    }
    check(created, "create more than the former 20,000-entry listing cap");
    const auto large = neo::vault::scan(neo::textfile::pathToUtf8(many));
    check(large.ok && large.fileCount == 20005,
          "large vault listing retains every entry beyond the former cap");
}

void vaultListingOrderMatchesReference(TempTree& tree) {
    const fs::path root = tree.vault / "sorting-fixture";
    const std::vector<std::string> names{
        "z-last-with-a-long-common-prefix.md", "Alpha-long-common-prefix-2.md",
        "alpha-long-common-prefix-1.md", "BETA.md", "b-10.md", "b-2.md",
        "README", ".hidden.md", "\xE4\xB8\xAD\xE6\x96\x87.md", "\xF0\x9F\x98\x80.md"};
    const std::vector<std::string> directories{"z-folder", "Alpha-folder", "beta-folder"};
    for (const auto& name : names)
        check(writeSample(root / neo::textfile::pathFromUtf8(name)), "create sorting file fixture");
    for (const auto& name : directories)
        for (const auto& child : names)
            check(writeSample(root / neo::textfile::pathFromUtf8(name) /
                              neo::textfile::pathFromUtf8(child)), "create nested sorting fixture");

    const auto byName = [](const std::string& left, const std::string& right) {
        auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char byte) {
                return static_cast<char>(std::tolower(byte));
            });
            return text;
        };
        const auto leftLower = lower(left), rightLower = lower(right);
        return leftLower == rightLower ? left < right : leftLower < rightLower;
    };
    auto expectedFiles = names;
    auto expectedDirectories = directories;
    std::sort(expectedFiles.begin(), expectedFiles.end(), byName);
    std::sort(expectedDirectories.begin(), expectedDirectories.end(), byName);
    const auto result = neo::vault::scan(utf8(root));
    check(result.ok && result.warning.empty() && result.directoryCount == 3 && result.fileCount == 40,
          "sorting fixture retains every file and directory without warnings");
    check(result.roots.size() == expectedDirectories.size() + expectedFiles.size(),
          "sorting fixture root count is unchanged");
    if (result.roots.size() != expectedDirectories.size() + expectedFiles.size()) return;
    for (std::size_t i = 0; i < expectedDirectories.size(); ++i) {
        const auto& folder = result.roots[i];
        check(folder.isDir && folder.name == expectedDirectories[i] && folder.relative == folder.name,
              "directories precede files and retain reference order and relative paths");
        check(folder.children.size() == expectedFiles.size(), "nested sort retains all children");
        if (folder.children.size() != expectedFiles.size()) continue;
        for (std::size_t j = 0; j < expectedFiles.size(); ++j) {
            const auto& child = folder.children[j];
            check(!child.isDir && child.name == expectedFiles[j] && child.children.empty() &&
                      child.relative == folder.name + "/" + expectedFiles[j],
                  "nested files retain UTF-8 names, reference order and relative paths");
        }
    }
    for (std::size_t i = 0; i < expectedFiles.size(); ++i) {
        const auto& file = result.roots[expectedDirectories.size() + i];
        check(!file.isDir && file.name == expectedFiles[i] && file.relative == file.name && file.children.empty(),
              "root files retain reference order and original names");
    }
    const fs::path empty = root / "empty";
    fs::create_directories(empty);
    const auto emptyResult = neo::vault::scan(utf8(empty));
    check(emptyResult.ok && emptyResult.roots.empty(), "empty sorting group remains valid");
    check(writeSample(empty / "one.md"), "create singleton sorting fixture");
    const auto singleton = neo::vault::scan(utf8(empty));
    check(singleton.ok && singleton.roots.size() == 1 && singleton.roots[0].name == "one.md",
          "singleton sorting group remains valid");
}

void directoryReparsePointIsLeaf(TempTree& tree) {
#if defined(_WIN32)
    const fs::path root = tree.vault / "reparse-fixture";
    const fs::path loop = root / "loop";
    std::error_code error;
    fs::create_directories(root, error);
    check(!error, "create isolated reparse-point fixture");

    if (!CreateDirectoryW(loop.c_str(), nullptr)) {
        check(false, "create isolated junction directory");
        return;
    }

    // Construct an NTFS mount-point reparse record directly, avoiding the
    // symlink privilege requirement. The junction points back to its parent.
    struct ReparseHeader { ULONG tag; USHORT dataLength; USHORT reserved; };
    struct MountPointFields {
        USHORT substituteOffset;
        USHORT substituteLength;
        USHORT printOffset;
        USHORT printLength;
    };
    static_assert(sizeof(ReparseHeader) == 8, "reparse header layout");
    static_assert(sizeof(MountPointFields) == 8, "mount-point header layout");
    const std::wstring printName = root.native();
    const std::wstring substituteName = L"\\??\\" + printName;
    const std::size_t pathBytes = (substituteName.size() + 1 + printName.size() + 1) * sizeof(wchar_t);
    const USHORT reparseDataLength = static_cast<USHORT>(sizeof(MountPointFields) + pathBytes);
    std::vector<unsigned char> buffer(sizeof(ReparseHeader) + reparseDataLength, 0);
    auto* header = reinterpret_cast<ReparseHeader*>(buffer.data());
    header->tag = IO_REPARSE_TAG_MOUNT_POINT;
    header->dataLength = reparseDataLength;
    auto* fields = reinterpret_cast<MountPointFields*>(buffer.data() + sizeof(ReparseHeader));
    fields->substituteOffset = 0;
    fields->substituteLength = static_cast<USHORT>(substituteName.size() * sizeof(wchar_t));
    fields->printOffset = static_cast<USHORT>(fields->substituteLength + sizeof(wchar_t));
    fields->printLength = static_cast<USHORT>(printName.size() * sizeof(wchar_t));
    wchar_t* pathBuffer = reinterpret_cast<wchar_t*>(
        buffer.data() + sizeof(ReparseHeader) + sizeof(MountPointFields));
    std::copy(substituteName.begin(), substituteName.end(), pathBuffer);
    std::copy(printName.begin(), printName.end(),
              pathBuffer + substituteName.size() + 1);

    HANDLE handle = CreateFileW(loop.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    DWORD bytesReturned = 0;
    const BOOL created = handle != INVALID_HANDLE_VALUE &&
        DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
                        static_cast<DWORD>(buffer.size()), nullptr, 0, &bytesReturned, nullptr);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    if (!created) {
        RemoveDirectoryW(loop.c_str());
        check(false, "create isolated junction reparse point (Win32 error " +
                        std::to_string(createError) + ")");
        return;
    }
    const DWORD attributes = GetFileAttributesW(loop.c_str());
    check(attributes != INVALID_FILE_ATTRIBUTES &&
              (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
              (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0,
          "parent-referencing junction is a directory reparse point");

    neo::vault::ScanResult result = neo::vault::scan(neo::textfile::pathToUtf8(root));
    const auto found = std::find_if(result.roots.begin(), result.roots.end(), [](const auto& entry) {
        return entry.relative == "loop";
    });
    check(result.ok && found != result.roots.end() && found->isDir && found->children.empty(),
          "junction remains visible without following a cycle");
    check(RemoveDirectoryW(loop.c_str()) != 0, "remove only the isolated junction");
#else
    (void)tree;
#endif
}

} // namespace

namespace neo::settings {

Data& current() {
    static Data data;
    return data;
}

std::string configDirectory() { return {}; }
bool flush() { return true; }
// 跟随系统主题的替身：单测统一按亮色解析。
bool systemThemePrefersLight() { return true; }
bool writeRecovery(const std::string& text, const std::string& originPath, const textfile::Document*) {
    ++recoveryCalls;
    lastRecoveryText = text;
    lastRecoveryOrigin = originPath;
    return recoveryWriteOk;
}
bool readRecovery(RecoverySnapshot&) { return false; }
void clearRecovery() {}

} // namespace neo::settings

namespace neo::platform {

DeleteResult deleteVaultEntry(const std::string& vaultRoot,
                              const std::string& relativePath,
                              bool isDirectory) {
    ++deleteCalls;
    const fs::path root = fs::path(neo::textfile::pathFromUtf8(vaultRoot)).lexically_normal();
    const fs::path target = (root / neo::textfile::pathFromUtf8(relativePath)).lexically_normal();
    if (!pathInside(root, allowedTestBase) || !pathInside(target, allowedTestBase) ||
        !pathInside(target, root) || target == root) {
        return {DeleteStatus::Error, "test stub refused a path outside its sample directory"};
    }
    if (deleteStubRemovesTarget) {
        std::error_code error;
        if (isDirectory) fs::remove_all(target, error);
        else fs::remove(target, error);
        if (error) return {DeleteStatus::Error, "isolated sample removal failed: " + error.message()};
    }
    return nextDeleteResult;
}

} // namespace neo::platform

namespace neo::dialogs {

PickResult pickDirectory(const std::string& initialDirectory) {
    ++directoryPickCalls;
    lastDirectoryInitialPath = initialDirectory;
    return nextDirectoryPick;
}
PickResult pickSavePath(const std::string&, const std::string&, const std::vector<std::string>&) {
    return {false, true, {}, {}};
}
float systemScale() { return 1.0f; }

} // namespace neo::dialogs

namespace neo::fileassoc {

bool isRegistered() { return false; }
bool hasRegistrationEntries() { return false; }
DefaultStatus queryDefaultStatus() { return {}; }
std::vector<TypeStatus> queryTypes() { return {}; }
RegisterOutcome registerAsDefault() { return {}; }
bool openDefaultAppsSettings() { return false; }
bool unregister(std::string& error) {
    error = "stub";
    return false;
}

} // namespace neo::fileassoc

// 换字体前的安全校验：这里只走"空路径 ⇒ 应用预设 ⇒ 一定安全"这条分支，
// 不启动真实探测进程。见 platform/font_safety.h。
namespace neo::fontsafety {

ProbeResult validate(const std::string&, unsigned) { return ProbeResult{true, false, {}}; }
bool armSession(const settings::Data&, std::string&) { return true; }

} // namespace neo::fontsafety

namespace neo::themeloader {
void reset() {}
} // namespace neo::themeloader

namespace neo {
app::DslAppConfig& mutableAppConfig() {
    static app::DslAppConfig config;
    return config;
}
} // namespace neo

namespace app {
void requestUpdate() {}
void requestClose() {}
namespace detail {
void requestFullPaint() {}
} // namespace detail
} // namespace app

struct AsyncShutdownGuard {
    ~AsyncShutdownGuard() { core::async::shutdown(); }
};

int main() {
    AsyncShutdownGuard asyncShutdownGuard;
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setInlineForTest(true);
    neo::vaultcache::resetForTest();
    neo::i18n::initialize("zh-CN");
    TempTree tree;
    check(tree.safe, "test fixture is an isolated child of the system temporary directory");
    resetStubs(tree.base);

    successfulCleanCurrentFile(tree);
    resetStubs(tree.base);
    successfulDirtyCurrentFile(tree);
    resetStubs(tree.base);
    cancelOrErrorWithFilePresent(tree, neo::platform::DeleteStatus::Cancelled);
    resetStubs(tree.base);
    cancelOrErrorWithFilePresent(tree, neo::platform::DeleteStatus::Error);
    resetStubs(tree.base);
    partialResultWithDocumentMissing(tree, neo::platform::DeleteStatus::Cancelled);
    resetStubs(tree.base);
    partialResultWithDocumentMissing(tree, neo::platform::DeleteStatus::Error);
    resetStubs(tree.base);
    unrelatedDeleteDoesNotTouchDocument(tree);
    resetStubs(tree.base);
    finalSymlinkDeleteDoesNotFollowTarget(tree);
    resetStubs(tree.base);
    recoveryWriteFailureIsReported(tree);
    resetStubs(tree.base);
    ancestorAndPrefixSibling(tree);
    resetStubs(tree.base);
    currentPathCaseVariantAndRelocation(tree);
    completeVaultListing(tree);
    vaultListingOrderMatchesReference(tree);
    directoryReparsePointIsLeaf(tree);

    std::cout << (failures == 0 ? "PASS" : "FAIL") << ": vault_delete_state ("
              << failures << " failures)\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
