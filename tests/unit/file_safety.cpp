// Real app action tests; system dialogs and persistence are isolated test doubles.
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
bool writeRecovery(const std::string& text, const std::string& originPath, const textfile::Document*) {
    ++recoveryCalls; lastRecoveryOrigin = originPath; return true;
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
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.path().filename().string().find(".neo-tmp-") != std::string::npos) ++temporaryFiles;
    check(temporaryFiles == 0, "rejected and failed saves remove their own temporary files");

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

    // Delete only this unique, verified temporary-directory child.
    fs::remove_all(dir);
    if (failures == 0) std::cout << "file_safety passed\n";
    return failures == 0 ? 0 : 1;
}
