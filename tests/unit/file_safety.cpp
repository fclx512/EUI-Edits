// Real app action tests; system dialogs and persistence are isolated test doubles.
#include "eui/platform.h"
#include "model/settings.h"
#include "model/text_file.h"
#include "model/png_encode.h"
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

#include "model/atomic_write.h"
#include <iterator>

namespace fs = std::filesystem;
namespace {
int failures = 0, recoveryCalls = 0, clearRecoveryCalls = 0, closeRequests = 0;
bool recoveryAvailable = false;
std::string lastRecoveryOrigin;
neo::textfile::Document lastRecoveryDocument;
neo::settings::RecoverySnapshot recoverySnapshot;
neo::dialogs::PickResult nextSavePick{false, true, {}, {}};
void check(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void put(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc); stream << bytes;
    check(bool(stream), "write test fixture");
}
std::string get(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
neo::AppState opened(const fs::path& path) {
    neo::AppState state;
    check(neo::loadDocument(state, neo::textfile::pathToUtf8(path)), "load real fixture and establish fingerprint");
    state.doc.text = "my unsaved changes\n"; ++state.revision;
    return state;
}

// A successful close permanently stops the process-global writer. These app
// action cases model separate application lifetimes in one test process.
void resetSessionWriterForCloseScenario() {
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setInlineForTest(true);
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
bool writeRecovery(const std::string& text, const std::string& originPath, const textfile::Document* document) {
    ++recoveryCalls; lastRecoveryOrigin = originPath;
    if (document) lastRecoveryDocument = *document;
    return true;
}
bool readRecovery(RecoverySnapshot& out) {
    if (!recoveryAvailable) return false;
    out = recoverySnapshot;
    return true;
}
void clearRecovery() { ++clearRecoveryCalls; }

} // namespace neo::settings

namespace neo::platform {

DeleteResult deleteVaultEntry(const std::string&, const std::string&, bool) {
    return {DeleteStatus::Cancelled, {}};
}

} // namespace neo::platform

namespace neo::dialogs {

PickResult pickDirectory(const std::string& initialDirectory) {
    return {false, true, {}, {}};
}
PickResult pickSavePath(const std::string&, const std::string&, const std::vector<std::string>&) {
    return nextSavePick;
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
void requestClose() { ++closeRequests; }
namespace detail {
void requestFullPaint() {}
} // namespace detail
} // namespace app


struct AsyncShutdownGuard {
    ~AsyncShutdownGuard() { core::async::shutdown(); }
};

int main() {
    AsyncShutdownGuard asyncShutdownGuard;
    resetSessionWriterForCloseScenario();
    neo::vaultcache::resetForTest();
    neo::i18n::initialize("en");
    const auto temp = fs::temp_directory_path();
    const auto dir = fs::absolute(temp / ("neo-file-safety-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).lexically_normal();
    const auto relative = dir.lexically_relative(fs::absolute(temp).lexically_normal());
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") return 2;
    fs::create_directory(dir);
    const auto file = dir / fs::u8path("中文 space.md");
    put(file, "original\n");
    auto state = opened(file);
    check(!neo::requestCloseDocument(state), "dirty close is vetoed");
    check(state.pending == neo::PendingAction::CloseWindow, "close requests route to confirmation");
    const auto originalPath = state.path;
    const auto originalText = state.doc.text;
    const auto originalRevision = state.revision;
    neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Cancel);
    check(!state.closeApproved && closeRequests == 0 && state.dirty(), "cancel close keeps document and window");
    check(state.path == originalPath && state.doc.text == originalText && state.revision == originalRevision,
          "cancel preserves path, text, and revision");

    // Same metadata but different bytes must be detected.
    const auto timestamp = fs::last_write_time(file);
    put(file, "external\n");
    fs::last_write_time(file, timestamp);
    check(!neo::saveDocument(state) && state.saveConflictOpen, "same-size external edit triggers conflict");
    neo::resolveSaveConflict(state, 2);
    check(state.dirty() && get(file) == "external\n", "conflict cancel leaves external bytes intact");
    check(!neo::saveDocument(state), "conflict remains detectable after cancellation");
    // User consent refers to the version shown, never a newer intervening write.
    put(file, "third writer\n");
    neo::resolveSaveConflict(state, 0);
    check(state.saveConflictOpen && get(file) == "third writer\n", "overwrite rechecks and requires renewed consent");
    neo::resolveSaveConflict(state, 0);
    check(!state.dirty() && get(file) == originalText, "explicit overwrite saves after renewed consent");

    state.doc.text = "new edits\n"; ++state.revision;
    fs::remove(file);
    check(!neo::saveDocument(state) && state.saveConflictOpen, "external deletion triggers conflict");
    neo::resolveSaveConflict(state, 2);
    check(!fs::exists(file) && state.path == originalPath && state.dirty(), "deleted-file cancel does not recreate file");
    check(!neo::saveDocument(state), "deleted file still requires consent");
    nextSavePick = {false, true, {}, {}};
    neo::resolveSaveConflict(state, 1);
    check(!fs::exists(file) && state.path == originalPath && state.dirty(), "Save As cancel preserves deleted-file state");

    // Closing a new document cannot continue after Save As cancellation.
    resetSessionWriterForCloseScenario();
    neo::AppState draft; draft.doc.text = "draft"; ++draft.revision;
    check(!neo::requestCloseDocument(draft), "new dirty document close is vetoed");
    neo::resolveDocumentConfirmation(draft, neo::filesafety::UnsavedChoice::Save);
    check(!draft.closeApproved && closeRequests == 0 && draft.dirty(), "Save As cancel vetoes close");
    const auto destination = dir / "saved draft.txt";
    nextSavePick = {true, false, neo::textfile::pathToUtf8(destination), {}};
    neo::resolveDocumentConfirmation(draft, neo::filesafety::UnsavedChoice::Save);
    check(draft.closeApproved && closeRequests == 1 && !draft.dirty() && get(destination) == "draft",
          "successful Save As dispatches close only after writing");

    resetSessionWriterForCloseScenario();
    put(file, "original\n"); state = opened(file);
    neo::atomicwrite::testing::failBeforeReplace(true);
    neo::requestCloseDocument(state);
    neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Save);
    check(!state.closeApproved && state.dirty() && closeRequests == 1 && get(file) == "original\n",
          "actual atomic save failure vetoes close and keeps original bytes");
    neo::atomicwrite::testing::failBeforeReplace(false);
    const int previousClears = clearRecoveryCalls;
    neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Discard);
    check(state.closeApproved && state.suppressRecovery && clearRecoveryCalls == previousClears + 1 && closeRequests == 2,
          "explicit discard authorizes close and clears recovery");
    const int previousRecovery = recoveryCalls;
    neo::maybeWriteRecovery(state);
    check(recoveryCalls == previousRecovery, "discarded-close document cannot rewrite recovery");

    // App continuation waits for conflict resolution, including Save As.
    resetSessionWriterForCloseScenario();
    put(file, "original\n"); state = opened(file);
    put(file, "other edit\n"); neo::requestCloseDocument(state);
    neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Save);
    check(state.saveConflictOpen && !state.closeApproved && closeRequests == 2, "save conflict vetoes close");
    const auto copy = dir / "copy.md";
    nextSavePick = {true, false, neo::textfile::pathToUtf8(copy), {}};
    neo::resolveSaveConflict(state, 1);
    check(state.closeApproved && closeRequests == 3 && get(file) == "other edit\n" && get(copy) == state.doc.text,
          "Save As resolves conflict and resumes close without overwriting original");

    put(file, "original\n"); state = opened(file);
    const auto expected = state.diskFingerprint;
    std::string error;
    check(!neo::textfile::save(state.path, state.doc, error, [&] {
        put(file, "changed while preparing save\n");
        return neo::filesafety::same(expected, neo::filesafety::inspect(state.path));
    }), "pre-replacement guard catches writer during save preparation");
    check(get(file) == "changed while preparing save\n", "rejected guard preserves newest writer bytes");
    std::size_t temporaryFiles = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        // .wstring() 而非 .string()：fixture 里有中文文件名，.string() 走 ANSI
        // 代码页转换，在非 GBK 代码页（如 CI 的 1252）表示不了会直接抛异常。
        if (entry.path().filename().wstring().find(L".neo-tmp-") != std::wstring::npos) ++temporaryFiles;
    }
    check(temporaryFiles == 0, "rejected and failed saves remove their own temporary files");

    // A competing creator can arrive after the temporary file is complete.
    // Publishing must reject that name without changing the competitor's bytes.
    const auto createdPath = dir / fs::u8path("新建竞争.md");
    neo::textfile::Document newDocument;
    newDocument.text = "our new document\n";
    const auto createdUtf8 = neo::textfile::pathToUtf8(createdPath);
    error.clear();
    check(!neo::textfile::create(createdUtf8, newDocument, error, [&] {
        put(createdPath, "competitor bytes\n");
        return true;
    }) && !error.empty(), "create-only reports a collision after writing its temporary file");
    check(get(createdPath) == "competitor bytes\n", "create-only preserves the competing writer's bytes");
    check(!neo::textfile::create(createdUtf8, newDocument, error),
          "create-only also rejects a target that existed before the call");
    check(get(createdPath) == "competitor bytes\n", "preexisting target remains untouched");
    const auto freshPath = dir / fs::u8path("新建成功.md");
    error.clear();
    check(neo::textfile::create(neo::textfile::pathToUtf8(freshPath), newDocument, error) &&
              get(freshPath) == newDocument.text, "create-only publishes a complete new document");
    temporaryFiles = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().filename().wstring().find(L".neo-tmp-") != std::wstring::npos) ++temporaryFiles;
    }
    check(temporaryFiles == 0, "create-only success and collision leave no temporary files");

    // New/open preserves the old buffer; closing its tab still uses the save gate.
    state = opened(file);
    const auto oldTab = state.tabId;
    const auto other = dir / "other.txt"; put(other, "opened document\n");
    neo::requestOpenPath(state, neo::textfile::pathToUtf8(other));
    check(state.tabOrder.size() == 2 && neo::documentTab(state, oldTab)->dirty(), "opening a file preserves the previous dirty tab");
    neo::requestCloseTab(state, oldTab);
    neo::atomicwrite::testing::failBeforeReplace(true);
    neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Save);
    check(state.path == neo::textfile::pathToUtf8(file) && state.dirty() && state.pending == neo::PendingAction::CloseTab,
        "failed save does not close the dirty tab");
    neo::atomicwrite::testing::failBeforeReplace(false);
    neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Save);
    check(state.path == neo::textfile::pathToUtf8(other) && !state.dirty() && state.doc.text == "opened document\n" && state.tabOrder.size() == 1,
          "successful save resumes the queued tab close");
    state = opened(file);
    const int beforeFailedOpenClear = clearRecoveryCalls;
    neo::requestOpenPath(state, neo::textfile::pathToUtf8(dir / "missing.txt"));
    neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Discard);
    check(state.dirty() && state.path == neo::textfile::pathToUtf8(file) && !state.suppressRecovery &&
              clearRecoveryCalls == beforeFailedOpenClear, "failed discard-open retains current draft and recovery eligibility");
    state.pending = neo::PendingAction::NewDocument;
    state.pendingNewMarkdown = true;
    neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Discard);
    check(state.path.empty() && !state.dirty() && state.newDocumentLanguage == "markdown" &&
              state.diskFingerprint.status == neo::filesafety::DiskStatus::Unknown, "new document clears the previous fingerprint");
    state = opened(file);
    neo::requestReloadWithEncoding(state, 0);
    neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Discard);
    check(!state.dirty() && neo::filesafety::same(state.diskFingerprint, neo::filesafety::inspect(state.path)),
          "encoding reload refreshes the opened fingerprint");

    // Inspection errors are protective, and successful renames retain baselines.
    state = opened(file);
    fs::remove(file); fs::create_directory(file);
    check(!neo::saveDocument(state) && !state.saveConflictOpen && state.dirty(), "non-file target rejects save without overwrite consent");
    fs::remove(file); put(file, "original\n"); state = opened(file);
    neo::vaultRenameEntry(state, "中文 space.md", "renamed.md");
    check(state.path == neo::textfile::pathToUtf8(dir / "renamed.md") && neo::saveDocument(state),
          "rename carries fingerprint to the renamed file");
    resetSessionWriterForCloseScenario();
    neo::AppState clean; check(neo::requestCloseDocument(clean), "clean close passes without confirmation");
    const int closedBeforeStale = closeRequests;
    neo::resolveDocumentConfirmation(clean, neo::filesafety::UnsavedChoice::Discard);
    neo::resolveDocumentConfirmation(clean, neo::filesafety::UnsavedChoice::Save);
    neo::resolveSaveConflict(clean, 0);
    check(closeRequests == closedBeforeStale && clean.path.empty() && !clean.dirty(),
          "stale confirmation callbacks do not save or resume a finished action");

    // A second process or a file drop cannot replace the transaction shown in
    // an existing modal; the original manuscript and pending choice stay put.
    auto modal = opened(dir / "renamed.md");
    const auto modalPath = modal.path;
    const auto modalText = modal.doc.text;
    neo::requestCloseDocument(modal);
    neo::requestOpenPath(modal, neo::textfile::pathToUtf8(other));
    check(modal.pending == neo::PendingAction::CloseWindow && modal.pendingPath.empty(),
          "external open does not replace an unsaved-close confirmation");
    neo::resolveDocumentConfirmation(modal, neo::filesafety::UnsavedChoice::Cancel);
    neo::requestOpenPath(modal, neo::textfile::pathToUtf8(other));
    neo::requestOpenPath(modal, modalPath);
    check(modal.pending == neo::PendingAction::None && modal.tabOrder.size() == 2 && modal.path == modalPath && modal.doc.text == modalText,
          "opening an already-open file activates its preserved dirty buffer");
    neo::resolveDocumentConfirmation(modal, neo::filesafety::UnsavedChoice::Cancel);
    modal.saveConflictOpen = true;
    neo::requestOpenPath(modal, neo::textfile::pathToUtf8(other));
    check(modal.pending == neo::PendingAction::None, "save conflict blocks a competing external open");
    modal.saveConflictOpen = false;
    neo::requestRisk(modal, "Reset?", "Description", "Reset", [] {});
    neo::requestOpenPath(modal, neo::textfile::pathToUtf8(other));
    check(modal.pending == neo::PendingAction::None, "risk confirmation blocks a competing external open");
    neo::cancelRisk(modal);
    modal.vaultPromptKind = neo::VaultPromptKind::Rename;
    neo::requestOpenPath(modal, neo::textfile::pathToUtf8(other));
    check(modal.pending == neo::PendingAction::None, "rename prompt blocks a competing external open");
    modal.vaultPromptKind = neo::VaultPromptKind::None;
    modal.vaultDeletePending = true;
    neo::requestOpenPath(modal, neo::textfile::pathToUtf8(other));
    check(modal.pending == neo::PendingAction::None, "delete prompt blocks a competing external open");
    check(modal.path == modalPath && modal.doc.text == modalText && modal.dirty(),
          "blocked external requests preserve the dirty manuscript");

    neo::AppState startup;
    neo::restoreStartupDocument(startup, neo::textfile::pathToUtf8(other));
    check(startup.path == neo::textfile::pathToUtf8(other) && !startup.dirty() &&
        neo::filesafety::same(startup.diskFingerprint, neo::filesafety::inspect(startup.path)),
        "ordinary explicit startup opens the requested file with a verified baseline");
    recoveryAvailable = true;
    recoverySnapshot.text = "recovered unsaved manuscript\n";
    recoverySnapshot.originPath = neo::textfile::pathToUtf8(dir / "crashed.md");
    recoverySnapshot.language = "markdown";
    recoverySnapshot.hasMeta = true;
    recoverySnapshot.doc.encoding = neo::textfile::Encoding::Utf16Be;
    recoverySnapshot.doc.hadBom = true;
    recoverySnapshot.doc.lineEnding = neo::textfile::LineEnding::CrLf;
    const int startupClears = clearRecoveryCalls;
    startup = neo::AppState{};
    neo::restoreStartupDocument(startup, neo::textfile::pathToUtf8(other));
    const auto* recoveredTab = neo::documentTab(startup, 1);
    check(startup.path == neo::textfile::pathToUtf8(other) && startup.tabOrder.size() == 2 && recoveredTab && recoveredTab->dirty() &&
        recoveredTab->recovered && recoveredTab->doc.text == recoverySnapshot.text && startup.pending == neo::PendingAction::None,
        "explicit startup file opens beside the crash recovery draft");
    check(recoveredTab && recoveredTab->doc.encoding == neo::textfile::Encoding::Utf16Be && recoveredTab->doc.hadBom &&
        recoveredTab->doc.lineEnding == neo::textfile::LineEnding::CrLf && recoveredTab->markdownCapable(),
        "startup recovery retains encoding, newline and syntax metadata");
    neo::requestCloseTab(startup, 1);
    neo::resolveDocumentConfirmation(startup, neo::filesafety::UnsavedChoice::Cancel);
    check(startup.recovered && startup.dirty() && startup.pending == neo::PendingAction::None &&
        startup.doc.text == recoverySnapshot.text && startup.tabOrder.size() == 2,
        "cancel closing recovery keeps both tabs");
    neo::requestCloseTab(startup, 1);
    nextSavePick = {false, true, {}, {}};
    neo::resolveDocumentConfirmation(startup, neo::filesafety::UnsavedChoice::Save);
    check(startup.recovered && startup.dirty() && startup.pending == neo::PendingAction::CloseTab && startup.tabOrder.size() == 2,
        "startup Save As cancellation preserves the recovered tab");
    neo::resolveDocumentConfirmation(startup, neo::filesafety::UnsavedChoice::Discard);
    check(startup.path == neo::textfile::pathToUtf8(other) && !startup.dirty() &&
        startup.doc.text == "opened document\n" && startup.tabOrder.size() == 1 && clearRecoveryCalls > startupClears,
        "explicit discard closes only the recovered tab");
    recoveryAvailable = false;
    startup = neo::AppState{};
    neo::settings::current().lastFile = neo::textfile::pathToUtf8(other);
    neo::restoreStartupDocument(startup, neo::textfile::pathToUtf8(dir / "missing-startup.txt"));
    check(startup.path.empty() && startup.doc.text.empty() && startup.toastVisible &&
        startup.toastTitle == neo::i18n::tr("safety.open_failed") && !startup.toastMessage.empty(),
        "failed explicit startup reports failure without opening an unrelated previous file");
    startup = neo::AppState{};
    neo::restoreStartupDocument(startup, {});
    check(startup.path.empty() && startup.doc.text.empty() && !startup.dirty(),
          "plain startup ignores stale last_file and starts blank");
    // Real rename action flow retains dirty text and keeps rejected prompts editable.
    auto renameState = opened(dir / "renamed.md");
    renameState.editorStateDirty = false;
    const auto renameText = renameState.doc.text;
    const auto renameRevision = renameState.revision;
    const auto renameSaved = renameState.savedRevision;
    neo::beginVaultRename(renameState, "renamed.md", false, true);
    renameState.vaultPromptText = "CON.md";
    check(!neo::confirmVaultRename(renameState) && renameState.vaultPromptKind == neo::VaultPromptKind::Rename &&
        renameState.vaultPromptText == "CON.md" && !renameState.vaultPromptError.empty(), "invalid rename retains error and editable input");
    renameState.vaultPromptText = "renamed.txt";
    check(!neo::confirmVaultRename(renameState) && fs::exists(dir / "renamed.md") && !fs::exists(dir / "renamed.txt"),
        "extension change waits for explicit consent");
    check(neo::confirmVaultRename(renameState) && renameState.path == neo::textfile::pathToUtf8(dir / "renamed.txt"),
        "second explicit confirmation changes the extension");
    check(renameState.dirty() && renameState.doc.text == renameText && renameState.revision == renameRevision &&
        renameState.savedRevision == renameSaved && !renameState.editorStateDirty, "rename preserves dirty manuscript, revisions, and input history");
    check(renameState.vaultFocusRestorePath == "renamed.txt" && lastRecoveryOrigin == renameState.path,
        "rename restores row focus and updates dirty recovery origin");
    put(dir / "taken.txt", "occupied\n");
    neo::beginVaultRename(renameState, "renamed.txt", false, true);
    renameState.vaultPromptText = "taken.txt";
    check(!neo::confirmVaultRename(renameState) && renameState.vaultRenameSuggestedName == "taken (2).txt" &&
        !fs::exists(dir / "taken (2).txt") && fs::exists(dir / "renamed.txt"), "conflict only proposes a name without silently renaming");
    put(dir / "taken (2).txt", "new occupant\n");
    check(!neo::confirmVaultRename(renameState, true) && renameState.vaultPromptKind == neo::VaultPromptKind::Rename &&
        renameState.vaultRenameSuggestedName == "taken (2) (2).txt" && get(dir / "taken (2).txt") == "new occupant\n",
        "a competing writer taking the proposal never gets overwritten");
    check(neo::confirmVaultRename(renameState, true) && fs::exists(dir / "taken (2) (2).txt"),
        "explicit renewed numbered consent completes the rename");
    neo::beginVaultRename(renameState, "taken (2) (2).txt", false, true);
    renameState.vaultPromptText = "cancel.txt";
    neo::cancelVaultPrompt(renameState);
    check(!fs::exists(dir / "cancel.txt") && renameState.dirty() && renameState.vaultFocusRestorePath == "taken (2) (2).txt",
        "cancel leaves filenames and dirty contents intact and restores library focus");
    fs::create_directories(dir / "notes" / "sub"); put(dir / "notes" / "sub" / "child.md", "child\n");
    auto folderState = opened(dir / "notes" / "sub" / "child.md");
    folderState.vaultRoot = neo::textfile::pathToUtf8(dir); folderState.vaultAddress = folderState.vaultRoot;
    folderState.editorStateDirty = false;
    folderState.expanded = {"notes", "notes/sub", "notes-other"};
    neo::beginVaultRename(folderState, "notes", true, true);
    folderState.vaultPromptText = "notes.changed";
    check(neo::confirmVaultRename(folderState) && folderState.path == neo::textfile::pathToUtf8(dir / "notes.changed" / "sub" / "child.md") &&
        folderState.dirty() && !folderState.editorStateDirty, "renaming an ancestor folder follows dirty opened document without resetting input");
    check(folderState.expanded.count("notes.changed") && folderState.expanded.count("notes.changed/sub") &&
        folderState.expanded.count("notes-other") && !folderState.expanded.count("notes"), "folder rename preserves expanded descendant paths");

    // Nested native message dispatch must not replace/close the current save transaction.
    neo::AppState nativeModal;
    nativeModal.savePickerOpen = true;
    nativeModal.doc.text = "draft";
    check(neo::documentModalOpen(nativeModal) && !neo::requestCloseDocument(nativeModal),
        "native save picker blocks close even when the current revision is clean");
    check(!neo::saveDocumentAs(nativeModal) && nativeModal.savePickerOpen && nativeModal.doc.text == "draft",
        "nested Save As preserves the outstanding picker transaction");
    const auto beforeOpenPath = nativeModal.path;
    neo::requestOpenPath(nativeModal, neo::textfile::pathToUtf8(other));
    check(nativeModal.path == beforeOpenPath && nativeModal.doc.text == "draft",
        "native save picker defers unrelated open requests");
    nativeModal.savePickerOpen = false;
    nextSavePick = {false, true, {}, {}};
    check(!neo::saveDocumentAs(nativeModal) && !nativeModal.savePickerOpen,
        "canceled picker clears its modal guard");

    // A successful write is not enough to clear the draft when read-back
    // verification observes a competing writer, deletion or unreadable payload.
    for (int fault = 0; fault != 3; ++fault) {
        resetSessionWriterForCloseScenario();
        put(file, "verification baseline\n");
        auto unverified = opened(file);
        const auto manuscript = unverified.doc.text;
        const int closesBefore = closeRequests;
        const int clearsBefore = clearRecoveryCalls;
        const int recoveryBefore = recoveryCalls;
        neo::testing::setBeforeSaveVerification([&](const std::string& path) {
            const auto target = neo::textfile::pathFromUtf8(path);
            if (fault == 0) put(target, "competing post-save writer\n");
            else if (fault == 1) fs::remove(target);
            else put(target, std::string("\xff\xfe\x00", 3)); // invalid forced UTF-8
        });
        check(!neo::requestCloseDocument(unverified), "verification case starts with a dirty close confirmation");
        neo::resolveDocumentConfirmation(unverified, neo::filesafety::UnsavedChoice::Save);
        neo::testing::setBeforeSaveVerification({});
        check(unverified.dirty() && unverified.doc.text == manuscript && !unverified.suppressRecovery,
              "unverified save retains the manuscript and dirty recovery eligibility");
        check(!unverified.closeApproved && !unverified.sessionClosePending && closeRequests == closesBefore,
              "post-save verification failure vetoes close continuation");
        check(recoveryCalls > recoveryBefore && clearRecoveryCalls == clearsBefore,
              "post-save verification failure persists rather than clears recovery");
        check(unverified.saveConflictOpen == (fault == 0),
              "verified competing content is a conflict; unreadable or missing content is a verification failure");
    }
    resetSessionWriterForCloseScenario();
    put(file, "original Save As source\n");
    auto unverifiedCopy = opened(file);
    const auto copyPathBefore = unverifiedCopy.path;
    const auto copyDestination = dir / "unverified-copy.txt";
    nextSavePick = {true, false, neo::textfile::pathToUtf8(copyDestination), {}};
    neo::testing::setBeforeSaveVerification([&](const std::string& path) {
        put(neo::textfile::pathFromUtf8(path), "competing copy\n");
    });
    check(!neo::saveDocumentAs(unverifiedCopy) && unverifiedCopy.path == copyPathBefore && unverifiedCopy.dirty(),
          "unverified Save As preserves the original document association and dirty state");
    neo::testing::setBeforeSaveVerification({});
    check(get(file) == "original Save As source\n" && get(copyDestination) == "competing copy\n",
          "failed Save As verification does not rewrite either source or competing destination");

    // Encoding conversion changes metadata/recovery revision, never reloads
    // disk text or resets the user's text undo state.
    resetSessionWriterForCloseScenario();
    const auto encodedFile = dir / "convert-utf8.txt";
    put(encodedFile, "plain ANSI source\r\n");
    auto converted = opened(encodedFile);
    converted.doc.encoding = neo::textfile::Encoding::Ansi;
    converted.doc.ansiCodePage = 936;
    converted.doc.hadBom = false;
    converted.doc.lineEnding = neo::textfile::LineEnding::CrLf;
    converted.doc.text = "中文 emoji 😀\n";
    const auto conversionRevision = converted.revision;
    const auto conversionSaved = converted.savedRevision;
    converted.editorStateDirty = false;
    check(!neo::saveDocument(converted) && get(encodedFile) == "plain ANSI source\r\n",
          "original ANSI save rejects an unrepresentable emoji without overwriting");
    check(neo::convertDocumentToUtf8(converted) && converted.dirty() &&
          converted.revision == conversionRevision + 1 && converted.savedRevision == conversionSaved &&
          converted.doc.encoding == neo::textfile::Encoding::Utf8 && converted.doc.ansiCodePage == 0 &&
          converted.doc.text == "中文 emoji 😀\n" && !converted.doc.hadBom &&
          converted.doc.lineEnding == neo::textfile::LineEnding::CrLf && !converted.editorStateDirty,
          "UTF-8 conversion preserves text and layout ownership, marks metadata dirty and advances revision");
    check(lastRecoveryDocument.encoding == neo::textfile::Encoding::Utf8 &&
          lastRecoveryDocument.text == converted.doc.text,
          "conversion publishes new encoding and current text to recovery");
    check(neo::convertDocumentToUtf8(converted) && converted.revision == conversionRevision + 1,
          "repeating UTF-8 conversion is a no-op");
    nextSavePick = {false, true, {}, {}};
    check(!neo::saveDocumentAs(converted) && converted.dirty() &&
          converted.doc.encoding == neo::textfile::Encoding::Utf8 && get(encodedFile) == "plain ANSI source\r\n",
          "canceling Save As preserves the explicit conversion intent and original disk file");
    neo::atomicwrite::testing::failBeforeReplace(true);
    check(!neo::saveDocument(converted) && converted.dirty() && get(encodedFile) == "plain ANSI source\r\n",
          "failed converted save retains draft and original disk bytes");
    neo::atomicwrite::testing::failBeforeReplace(false);
    check(neo::saveDocument(converted) && !converted.dirty() && get(encodedFile) == "中文 emoji 😀\r\n",
          "normal save commits UTF-8 text with original line-ending policy");
    const auto reopenedUtf8 = neo::textfile::load(neo::textfile::pathToUtf8(encodedFile));
    check(reopenedUtf8.ok && reopenedUtf8.document.encoding == neo::textfile::Encoding::Utf8 &&
          reopenedUtf8.document.text == converted.doc.text,
          "converted UTF-8 bytes reopen without losing the manuscript");

    // Exercise the complete metadata conversion with actual UTF-16 files, not
    // synthetic ANSI metadata. BOM presence and byte order are independent of
    // the in-memory UTF-8 manuscript and its retained editor history.
    for (const auto encoding : {neo::textfile::Encoding::Utf16Le, neo::textfile::Encoding::Utf16Be}) {
        for (const bool bom : {false, true}) {
            resetSessionWriterForCloseScenario();
            const auto conversionFile = dir / (std::string(encoding == neo::textfile::Encoding::Utf16Le ?
                "convert-le-" : "convert-be-") + (bom ? "bom.txt" : "no-bom.txt"));
            const auto conversionPath = neo::textfile::pathToUtf8(conversionFile);
            neo::textfile::Document source;
            source.text = "中文🙂\n第二行\n";
            source.encoding = encoding;
            source.hadBom = bom;
            source.lineEnding = bom ? neo::textfile::LineEnding::CrLf : neo::textfile::LineEnding::Lf;
            std::string encodingError;
            check(neo::textfile::save(conversionPath, source, encodingError),
                  "write a real UTF-16 source with explicit BOM and line-ending policy");
            const neo::textfile::ForcedEncoding forced{encoding, 0};
            const auto loaded = neo::filesafety::load(conversionPath, &forced);
            check(loaded.result.ok, "read the UTF-16 source and establish its actual disk fingerprint");
            if (!loaded.result.ok) continue;
            neo::AppState metadata;
            metadata.path = conversionPath;
            metadata.doc = loaded.result.document;
            metadata.diskFingerprint = loaded.fingerprint;
            metadata.revision = metadata.savedRevision = 1;
            using InputModel = components::input_detail::InputModel;
            metadata.editorMemory = std::make_unique<InputModel::InputState>();
            auto* retained = metadata.editorMemory.get();
            InputModel::loadDocument(*retained, metadata.doc.text);
            const auto appendEdit = [&](const std::string& text) {
                const int end = static_cast<int>(retained->text.size());
                retained->cursor = retained->selectionStart = retained->selectionEnd = end;
                InputModel::beginEdit(*retained, end, end);
                retained->text += text;
                retained->cursor = retained->selectionStart = retained->selectionEnd =
                    static_cast<int>(retained->text.size());
                InputModel::endEdit(*retained);
            };
            appendEdit("保留历史\n");
            const auto manuscript = retained->text;
            appendEdit("重做内容\n");
            const auto redoManuscript = retained->text;
            check(InputModel::undoEdit(*retained) && retained->text == manuscript,
                  "conversion fixture retains both a real undo and redo record");
            metadata.doc.text = retained->text;
            ++metadata.revision;
            const auto undoCount = retained->undoStack.size();
            const auto redoCount = retained->redoStack.size();
            const auto inputRevision = retained->textRevision;
            check(undoCount == 1 && redoCount == 1, "conversion starts with nonempty editor undo and redo history");
            check(neo::convertDocumentToUtf8(metadata) && metadata.doc.encoding == neo::textfile::Encoding::Utf8 &&
                  metadata.doc.hadBom == bom && metadata.doc.lineEnding == source.lineEnding &&
                  metadata.editorMemory.get() == retained && !metadata.editorStateDirty &&
                  retained->textRevision == inputRevision && retained->undoStack.size() == undoCount &&
                  retained->redoStack.size() == redoCount,
                  "UTF-16 conversion preserves BOM intent, line endings and the retained editor history");
            check(neo::saveDocument(metadata) && !metadata.dirty(), "converted UTF-16 document saves and verifies as UTF-8");
            const std::string physicalText = bom ? "中文🙂\r\n第二行\r\n保留历史\r\n" : manuscript;
            const std::string expectedBytes = (bom ? std::string("\xef\xbb\xbf", 3) : std::string{}) + physicalText;
            check(get(conversionFile) == expectedBytes,
                  "converted bytes contain only the intended UTF-8 BOM and original line-ending policy");
            const auto reopened = neo::textfile::load(conversionPath);
            check(reopened.ok && reopened.document.encoding == neo::textfile::Encoding::Utf8 &&
                  reopened.document.hadBom == bom && reopened.document.lineEnding == source.lineEnding &&
                  reopened.document.text == manuscript,
                  "converted UTF-16 source reopens with exact UTF-8 metadata and normalized manuscript");
            check(metadata.editorMemory.get() == retained && retained->undoStack.size() == undoCount &&
                  retained->redoStack.size() == redoCount &&
                  InputModel::redoEdit(*retained) && retained->text == redoManuscript &&
                  InputModel::undoEdit(*retained) && retained->text == manuscript &&
                  InputModel::undoEdit(*retained) && retained->text == source.text,
                  "metadata conversion and save preserve executable redo and undo history in order");
        }
    }

    const auto linkedSource = dir / "linked-source.md";
    const auto linkedTarget = dir / "linked#target.md";
    put(linkedSource, "# 起点\n\n## 中文 标题\ncontent\n");
    put(linkedTarget, "# Other\n\n## Destination\nbody\n");
    neo::AppState linked;
    check(neo::loadDocument(linked, neo::textfile::pathToUtf8(linkedSource)), "load heading link source");
    const auto linkedRevision = linked.revision;
    const auto linkedId = linked.tabId;
    const auto linkedText = linked.doc.text;
    neo::openLinkTarget(linked, "#%E4%B8%AD%E6%96%87%20%E6%A0%87%E9%A2%98", neo::textfile::pathToUtf8(dir));
    check(linked.linkAnchorPending && linked.tabId == linkedId && linked.revision == linkedRevision &&
          linked.doc.text == linkedText && !linked.dirty(), "same-page anchor queues without editing or switching document");
    const auto linkedPlan = neo::buildLpPlan(linked.doc.text);
    neo::resolvePendingLinkAnchor(linked, linkedPlan, "stale plan text");
    check(linked.linkAnchorPending && linked.pendingOutlineJumpByte == -1,
          "stale plan cannot resolve a pending heading link");
    neo::resolvePendingLinkAnchor(linked, linkedPlan, linked.doc.text);
    check(linked.linkAnchorPending && linked.pendingOutlineJumpByte == static_cast<int>(linkedText.find("## 中文")) &&
          linked.pendingLinkJumpRevision == linkedRevision && !linked.dirty(),
          "current plan resolves same-page heading and binds the jump to document revision");
    check(neo::takePendingOutlineJump(linked) == static_cast<int>(linkedText.find("## 中文")) &&
          !linked.linkAnchorPending && linked.pendingOutlineJumpByte == -1,
          "heading request is cleared only when its jump is consumed");
    neo::openLinkTarget(linked, "linked%23target.md#destination", neo::textfile::pathToUtf8(dir));
    check(linked.path == neo::textfile::pathToUtf8(linkedTarget) && linked.tabId != linkedId && linked.linkAnchorPending,
          "cross-page anchor opens the decoded filename and queues the target's heading");
    const auto targetTabId = linked.tabId;
    check(neo::activateDocumentTab(linked, linkedId) && !linked.linkAnchorPending &&
          linked.pendingOutlineJumpByte == -1,
          "pending cross-page anchor does not bleed into the source tab");
    check(neo::activateDocumentTab(linked, targetTabId) && linked.linkAnchorPending &&
          linked.pendingLinkTabId == targetTabId,
          "returning to the target tab retains its own unresolved heading request");
    neo::resolvePendingLinkAnchor(linked, neo::buildLpPlan(linked.doc.text), linked.doc.text);
    check(linked.pendingOutlineJumpByte == static_cast<int>(linked.doc.text.find("## Destination")) && !linked.dirty(),
          "cross-page heading resolves after its own document plan is ready");
    linked.doc.text = "# Inserted\n\n## Destination\nbody\n";
    ++linked.revision;
    linked.savedRevision = linked.revision; // Model an external file-check refresh of a clean tab.
    check(neo::takePendingOutlineJump(linked) == -1 && linked.linkAnchorPending &&
          linked.pendingOutlineJumpByte == -1,
          "revision change invalidates the byte offset but retains the heading request");
    neo::resolvePendingLinkAnchor(linked, neo::buildLpPlan(linked.doc.text), linked.doc.text);
    check(linked.pendingOutlineJumpByte == static_cast<int>(linked.doc.text.find("## Destination")) &&
          neo::takePendingOutlineJump(linked) == static_cast<int>(linked.doc.text.find("## Destination")) &&
          !linked.linkAnchorPending,
          "retained request resolves against refreshed text and is consumed once");
    neo::openLinkTarget(linked, "#destination", neo::textfile::pathToUtf8(dir));
    neo::resolvePendingLinkAnchor(linked, neo::buildLpPlan(linked.doc.text), linked.doc.text);
    linked.pendingOutlineJumpByte = -1;
    linked.pendingLinkJumpRevision = static_cast<unsigned long long>(-1); // editorStateDirty reset.
    neo::resolvePendingLinkAnchor(linked, neo::buildLpPlan(linked.doc.text), linked.doc.text);
    check(linked.linkAnchorPending &&
          neo::takePendingOutlineJump(linked) == static_cast<int>(linked.doc.text.find("## Destination")),
          "editor reset can discard a resolved byte and the retained request resolves again");
    neo::openLinkTarget(linked, "#absent", neo::textfile::pathToUtf8(dir));
    neo::resolvePendingLinkAnchor(linked, neo::buildLpPlan(linked.doc.text), linked.doc.text);
    check(!linked.linkAnchorPending && linked.pendingOutlineJumpByte == -1 && !linked.dirty(),
          "missing heading clears the request without moving or modifying text");
    const auto missingToast = linked.toastMessage;
    neo::resolvePendingLinkAnchor(linked, neo::buildLpPlan(linked.doc.text), linked.doc.text);
    check(!linked.linkAnchorPending && linked.toastMessage == missingToast,
          "missing heading terminates rather than repeatedly showing a toast");
    neo::openLinkTarget(linked, "#", neo::textfile::pathToUtf8(dir));
    neo::resolvePendingLinkAnchor(linked, neo::buildLpPlan(linked.doc.text), linked.doc.text);
    check(neo::takePendingOutlineJump(linked) == 0 && !linked.linkAnchorPending,
          "empty fragment navigates to document start and consumes the request");
    neo::openLinkTarget(linked, "#destination", neo::textfile::pathToUtf8(dir));
    neo::resolvePendingLinkAnchor(linked, neo::buildLpPlan(linked.doc.text), linked.doc.text);
    neo::cancelPendingLinkAnchor(linked); // User clicked an outline entry instead.
    linked.pendingOutlineJumpByte = 2;
    check(neo::takePendingOutlineJump(linked) == 2 && !linked.linkAnchorPending,
          "manual outline navigation cancels the older link request");
    neo::openLinkTarget(linked, "#destination", neo::textfile::pathToUtf8(dir));
    linked.path = neo::textfile::pathToUtf8(linkedSource); // Same tab now identifies a different document.
    neo::resolvePendingLinkAnchor(linked, neo::buildLpPlan(linked.doc.text), linked.doc.text);
    check(!linked.linkAnchorPending && linked.pendingOutlineJumpByte == -1,
          "document identity change cancels an unresolved link request");

    // A resolved byte offset must still carry document identity when consumed;
    // checking identity only during parsing would allow a late tab/path switch.
    for (const bool changeTab : {false, true}) {
        neo::AppState identity;
        identity.path = neo::textfile::pathToUtf8(linkedSource);
        identity.doc.text = "# Start\n\n## Destination\n";
        identity.revision = identity.savedRevision = 1;
        neo::openLinkTarget(identity, "#destination", neo::textfile::pathToUtf8(dir));
        neo::resolvePendingLinkAnchor(identity, neo::buildLpPlan(identity.doc.text), identity.doc.text);
        check(identity.linkAnchorPending && identity.pendingOutlineJumpByte > 0,
              "identity fixture has a fully resolved heading request before switching documents");
        if (changeTab) ++identity.tabId;
        else identity.path = neo::textfile::pathToUtf8(linkedTarget);
        check(neo::takePendingOutlineJump(identity) == -1 && !identity.linkAnchorPending &&
              identity.pendingOutlineJumpByte == -1 && identity.pendingLinkTabId == 0 &&
              identity.pendingLinkPath.empty(),
              "consuming a resolved heading after tab or path identity changes cancels the entire request");
    }

    // Images use ordinary tab identity while staying outside text/save paths.
    {
        const auto picture=dir/fs::u8path("独立图片.PNG");
        const auto picture2=dir/"second.png";
        unsigned char rgba[3*2*4];std::fill(std::begin(rgba),std::end(rgba),255);
        std::string pngBytes,pngError;
        check(neo::pngencode::encodeRgba(3,2,rgba,pngBytes,pngError),"encode image routing fixture");
        put(picture,pngBytes);put(picture2,pngBytes);
        neo::AppState viewer;
        viewer.path=neo::textfile::pathToUtf8(file);viewer.doc.text="unsaved manuscript";
        viewer.revision=7;viewer.savedRevision=6;
        using Model=components::input_detail::InputModel;
        eui::Ui ui;
        neo::syncDocumentTabInputs(ui,viewer);
        auto& input=ui.state<Model::InputState>(neo::editorInputId(viewer));
        Model::loadDocument(input,viewer.doc.text);input.cursor=3;
        Model::insertAtCursor(input,"中😀");viewer.doc.text=input.text;
        input.verticalScroll=45;input.compositionText="preedit";
        const auto original=viewer.doc.text;const auto tabId=viewer.tabId;
        check(neo::loadDocument(viewer,neo::textfile::pathToUtf8(picture)),"PNG opens through normal file dispatch");
        neo::syncDocumentTabInputs(ui,viewer);
        const auto firstImage=viewer.tabId;
        check(firstImage!=tabId && viewer.tabOrder.size()==2 && viewer.imageDocument() &&
              viewer.path==viewer.imagePreviewPath && viewer.doc.text.empty() && !viewer.dirty() &&
              viewer.language()=="image" && viewer.imagePreviewWidth==3 && viewer.imagePreviewHeight==2,
              "image receives its own clean, read-only tab with decoded dimensions");
        check(viewer.inactiveTabs.at(tabId).doc.text==original && viewer.inactiveTabs.at(tabId).dirty() &&
              viewer.inactiveTabs.at(tabId).editorMemory && viewer.inactiveTabs.at(tabId).editorMemory->undoStack.size()==1 &&
              viewer.inactiveTabs.at(tabId).editorMemory->compositionText.empty() && viewer.displayedInputTabId==0,
              "text tab keeps bytes/undo while image navigation clears only temporary preedit");
        viewer.imageViewport.zoom=2;viewer.imageViewport.panX=19;
        check(neo::loadDocument(viewer,neo::textfile::pathToUtf8(picture2)),"second image opens independently");
        const auto secondImage=viewer.tabId;viewer.imageViewport.zoom=3;
        check(viewer.tabOrder.size()==3 && secondImage!=firstImage &&
              neo::activateDocumentTab(viewer,firstImage) && viewer.imageViewport.zoom==2 && viewer.imageViewport.panX==19,
              "image tabs retain independent zoom/pan on switching");
        check(neo::loadDocument(viewer,neo::textfile::pathToUtf8(picture.parent_path()/"."/picture.filename())) &&
              viewer.tabId==firstImage && viewer.tabOrder.size()==3 && viewer.imageViewport.zoom==2,
              "reopening the same image activates its existing tab without resetting view");
        check(neo::moveDocumentTabToIndex(viewer,firstImage,0) && viewer.tabOrder.front()==firstImage,
              "image tab uses ordinary reorder action");
        nextSavePick={true,false,neo::textfile::pathToUtf8(dir/"must-not-save-image.txt"),{}};
        check(!neo::saveDocument(viewer) && !neo::saveDocumentAs(viewer) &&
              !neo::convertDocumentToUtf8(viewer) && !fs::exists(dir/"must-not-save-image.txt") && get(picture)==pngBytes,
              "image save/encoding actions cannot write empty text over binary data");
        nextSavePick={false,true,{},{}};
        viewer.pendingEditorCommand=neo::EditorCommand::Cut;
        neo::applyEditorCommand(ui,viewer);
        check(viewer.pendingEditorCommand==neo::EditorCommand::None && viewer.doc.text.empty(),
              "queued text commands cannot mutate an image tab");
        neo::requestCloseTab(viewer,secondImage);
        check(!neo::documentTab(viewer,secondImage) && neo::documentTab(viewer,firstImage) &&
              viewer.pending==neo::PendingAction::None,"closing background image removes only that clean tab");
        check(neo::activateDocumentTab(viewer,tabId),"return to dirty text tab");
        neo::syncDocumentTabInputs(ui,viewer);
        auto& restored=ui.state<Model::InputState>(neo::editorInputId(viewer));
        check(viewer.doc.text==original && restored.text==original && restored.verticalScroll==45 &&
              restored.undoStack.size()==1 && Model::undoEdit(restored),"text undo and scroll survive mixed image tabs");
        for(const char* name:{"broken.png","unsupported.webp","missing.avif"}) {
            const auto invalid=dir/name;
            if(std::string(name)!="missing.avif")put(invalid,"this is not a decodable image");
            const auto count=viewer.tabOrder.size();
            check(!neo::loadDocument(viewer,neo::textfile::pathToUtf8(invalid)) && !viewer.imageDocument() &&
                  viewer.tabId==tabId && viewer.tabOrder.size()==count && viewer.toastVisible,
                  "unreadable image leaves tabs and text intact without binary fallback");
        }
        const auto vectorImage=dir/"vector.svg";
        put(vectorImage,"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"20\"><rect width=\"40\" height=\"20\" fill=\"red\"/></svg>");
        check(neo::loadDocument(viewer,neo::textfile::pathToUtf8(vectorImage)) && viewer.imageDocument(),
              "SVG uses the same image-tab route and rasterizer");
        const bool hadRecovery=recoveryAvailable;recoveryAvailable=false;
        neo::AppState startup;neo::restoreStartupDocument(startup,neo::textfile::pathToUtf8(picture));
        recoveryAvailable=hadRecovery;
        check(startup.imageDocument() && startup.path==startup.imagePreviewPath && startup.tabOrder.size()==1 &&
              startup.doc.text.empty() && !startup.dirty(),"startup image reuses the pristine blank tab");
        viewer.vaultRoot=neo::textfile::pathToUtf8(dir);
        check(neo::vaultRenameEntry(viewer,neo::textfile::fileName(neo::textfile::pathToUtf8(picture)),"renamed-picture.PNG"),
              "library rename accepts a background image");
        const auto renamed=dir/"renamed-picture.PNG";
        check(neo::documentTab(viewer,firstImage)->path==neo::textfile::pathToUtf8(renamed) &&
              neo::documentTab(viewer,firstImage)->imagePreviewPath==neo::textfile::pathToUtf8(renamed),
              "image identity and decoded source path follow a library rename");
        fs::remove(renamed); // Simulate an externally missing target; Shell test double cancels.
        neo::vaultDeleteEntry(viewer,"renamed-picture.PNG",false);
        check(neo::documentTab(viewer,firstImage)->unavailable && !neo::documentTab(viewer,firstImage)->dirty() &&
              !neo::documentTab(viewer,firstImage)->path.empty(),"missing image remains a clean read-only error tab");
        put(renamed,pngBytes);
        neo::activateDocumentTab(viewer,firstImage);neo::retryUnavailableDocument(viewer);
        check(!viewer.unavailable && viewer.imagePreviewWidth==3 && get(renamed)==pngBytes,
              "retry reloads image bytes without a text save or new tab");
        check(neo::activateDocumentTab(viewer,firstImage),"activate image before closing");
        neo::closeImagePreview(viewer);
        check(!neo::documentTab(viewer,firstImage) && neo::documentTab(viewer,tabId),
              "image close control shares ordinary tab removal");
    }
    // Image tabs must not block window close or bypass another tab's dirty prompt.
    {
        resetSessionWriterForCloseScenario();
        neo::AppState cleanImage;cleanImage.path=cleanImage.imagePreviewPath="fixture.png";
        const auto priorCloseRequests=closeRequests;
        check(!neo::documentModalOpen(cleanImage) && neo::requestCloseDocument(cleanImage) &&
              cleanImage.closeApproved && closeRequests>priorCloseRequests,
              "clean image tab follows the normal window exit chain");
        resetSessionWriterForCloseScenario();
        neo::AppState dirtyImage;dirtyImage.doc.text="unsaved beside image";dirtyImage.revision=1;
        const auto textId=dirtyImage.tabId;
        neo::createDocumentTab(dirtyImage);dirtyImage.path=dirtyImage.imagePreviewPath="fixture.png";
        const auto imageId=dirtyImage.tabId;
        check(!neo::requestCloseDocument(dirtyImage) && dirtyImage.tabId==textId &&
              dirtyImage.pending==neo::PendingAction::CloseWindow && dirtyImage.doc.text=="unsaved beside image" &&
              neo::documentTab(dirtyImage,imageId)->imageDocument(),"window close still confirms dirty text beside images");
        neo::resolveDocumentConfirmation(dirtyImage,neo::filesafety::UnsavedChoice::Cancel);
        check(!dirtyImage.closeApproved && dirtyImage.dirty() && neo::documentTab(dirtyImage,imageId),
              "canceling window close retains both text and image tabs");
        neo::AppState nestedImage;nestedImage.imagePreviewPath="fixture.png";nestedImage.savePickerOpen=true;
        check(!neo::requestCloseDocument(nestedImage) && nestedImage.imageDocument() && nestedImage.savePickerOpen,
              "native picker retains its modal close guard");
    }
    // Delete only this unique, verified temporary-directory child.
    fs::remove_all(dir);
    if (failures == 0) std::cout << "file_safety passed\n";
    return failures == 0 ? 0 : 1;
}
