// Real app action tests; system dialogs and persistence are isolated test doubles.
#include "eui/platform.h"
#include "model/settings.h"
#include "model/session_storage.h"
#include "model/text_file.h"
#include "model/theme_loader.h"
#include "model/vault.h"
#include "platform/file_operations.h"
#include "platform/font_safety.h"
#include "platform/native_dialogs.h"
#include "state/app_actions.h"
#include "state/session_writer.h"
#include "state/vault_cache.h"
#include "ui/tab_bar.h"
#include "core/platform/async.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
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
std::string sessionConfigDirectory;
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
} // namespace
namespace neo::settings {

Data& current() {
    static Data data;
    return data;
}

std::string configDirectory() { return sessionConfigDirectory; }
bool flush() { return true; }
// 跟随系统主题的替身：单测统一按亮色解析。
bool systemThemePrefersLight() { return true; }
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
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setInlineForTest(true);
    neo::vaultcache::resetForTest();
    neo::i18n::initialize("en");
    const auto dir = fs::temp_directory_path() / ("neo-tabs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir / "project/docs/nested"); fs::create_directories(dir / "project-extra"); fs::create_directories(dir / "other");
    const auto child = dir / "project/docs/README.md", parent = dir / "project/README.md", unrelated = dir / "other/README.md";
    put(child, "# Child\nchild body\n"); put(parent, "# Parent\nparent body\n"); put(unrelated, "other body\n");
    const auto path = [](const fs::path& p) { return neo::textfile::pathToUtf8(p); };
    neo::AppState state;
    check(neo::loadDocument(state, path(child)) && state.tabOrder.size() == 1, "first open reuses pristine initial tab");
    const auto childId = state.tabId;
    state.expanded.insert("nested"); state.filter = "README"; state.vaultScroll = 83;
    state.doc.text += "unsaved child\n"; ++state.revision;
    check(neo::loadDocument(state, path(parent)) && state.tabOrder.size() == 2, "open parent beside dirty child");
    const auto parentId = state.tabId;
    const auto* savedChild = neo::documentTab(state, childId);
    check(savedChild && savedChild->dirty() && savedChild->doc.text.find("unsaved child") != std::string::npos, "dirty child text survives open");
    check(savedChild && neo::sameDocumentFile(savedChild->vaultRoot, path(dir/"project")) && neo::sameDocumentFile(state.vaultRoot, path(dir/"project")), "shortest open ancestor merges child library root");
    check(savedChild && savedChild->expanded.count("docs/nested") && savedChild->filter == "README" && savedChild->vaultScroll == 83,
        "merged root rebases expansion and preserves filter/scroll");
    check(!neo::documentWithinRoot(path(dir/"project-extra/x.txt"), path(dir/"project")), "root containment respects component boundary");
    check(neo::activateDocumentTab(state, childId) && state.doc.text.find("unsaved child") != std::string::npos && state.filter == "README", "switch restores library and dirty buffer");
    neo::loadDocument(state, path(unrelated)); const auto otherId = state.tabId;
    check(state.tabOrder.size() == 3 && neo::sameDocumentFile(state.vaultRoot, path(dir/"other")), "unrelated library stays independent");
    neo::loadDocument(state, path(child));
    check(state.tabId == childId && state.tabOrder.size() == 3, "duplicate open activates existing dirty tab");
    std::error_code hardlinkError; fs::create_hard_link(child, dir/"alias.md", hardlinkError);
    if (!hardlinkError) { neo::loadDocument(state, path(dir/"alias.md")); check(state.tabId == childId && state.tabOrder.size() == 3, "physical-file alias deduplicates"); }
    const auto priorFingerprint = state.diskFingerprint;
    put(child, "external changed body\n");
    neo::activateDocumentTab(state, otherId); neo::activateDocumentTab(state, childId);
    check(state.dirty() && state.doc.text.find("unsaved child") != std::string::npos && neo::filesafety::same(priorFingerprint, state.diskFingerprint), "dirty activation preserves opened fingerprint and text");
    check(!neo::saveDocument(state) && state.saveConflictOpen, "background external change requires save conflict consent");
    check(!neo::activateDocumentTab(state, otherId), "conflict freezes tab activation"); neo::resolveSaveConflict(state, 2);

    using Model = components::input_detail::InputModel;
    eui::Ui ui;
    neo::syncDocumentTabInputs(ui, state); state.editorStateDirty = false;
    auto& input = ui.state<Model::InputState>(neo::editorInputId(state));
    Model::loadDocument(input, state.doc.text); input.cursor = 3; input.selectionStart = 1; input.selectionEnd = 3;
    Model::insertAtCursor(input, "Z"); state.doc.text = input.text; ++state.revision;
    const auto editedText = input.text; const auto selectedBegin = input.selectionStart; const auto cursor = input.cursor;
    input.verticalScroll = 91; input.horizontalScroll = 12;
    input.cachedLayoutText = "large layout cache"; input.cachedLines.resize(10); input.compositionText = "preedit";
    state.foldedHeadings.insert(0); state.foldsTextRevision = input.textRevision;
    neo::activateDocumentTab(state, parentId); neo::syncDocumentTabInputs(ui, state);
    const auto* memory = neo::documentTab(state, childId)->editorMemory.get();
    check(memory && memory->undoStack.size() == 1 && !memory->cachedLayoutText.empty() && !memory->cachedLines.empty() && memory->compositionText.empty(), "suspend keeps undo and layout cache, releases preedit");
    state.editorStateDirty = false; auto& parentInput = ui.state<Model::InputState>(neo::editorInputId(state)); Model::loadDocument(parentInput, state.doc.text);
    parentInput.cursor = 2; parentInput.verticalScroll = 21;
    neo::activateDocumentTab(state, childId); neo::syncDocumentTabInputs(ui, state);
    auto& restored = ui.state<Model::InputState>(neo::editorInputId(state));
    check(restored.text == editedText && restored.cursor == cursor && restored.selectionStart == selectedBegin && restored.verticalScroll == 91 && restored.horizontalScroll == 12 && state.foldedHeadings.count(0), "restore independent text/cursor/selection/scroll/folds");
    check(Model::undoEdit(restored) && restored.text != editedText, "undo survives switch and belongs to original tab");
    state.doc.text = restored.text; ++state.revision;
    neo::requestCloseTab(state, childId); check(state.pending == neo::PendingAction::CloseTab && state.confirmationTabId == childId, "dirty close captures target ID");
    check(!neo::activateDocumentTab(state, otherId), "unsaved confirmation freezes tabs");
    neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Cancel);
    check(state.tabOrder.size() == 3 && state.dirty(), "cancel retains target and order");
    neo::requestCloseTab(state, childId); neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Discard);
    neo::syncDocumentTabInputs(ui, state);
    check(state.tabOrder.size() == 2 && !neo::documentTab(state, childId), "discard closes only target tab");

    // Exercise the real tab-card compose and the actions that its callbacks route
    // to. A 24 DIP card is the narrowest width that may expose a close hit.
    const auto barA = dir / "tab-bar-a.md";
    const auto barB = dir / "tab-bar-b.md";
    const auto barC = dir / "tab-bar-c.md";
    put(barA, "A\n"); put(barB, "B\n"); put(barC, "C\n");
    neo::AppState barState;
    neo::loadDocument(barState, path(barA)); const auto barAId = barState.tabId;
    neo::loadDocument(barState, path(barB)); const auto barBId = barState.tabId;
    neo::loadDocument(barState, path(barC)); const auto barCId = barState.tabId;

    eui::Ui narrowTabUi;
    narrowTabUi.begin("document-tabs-narrow-card");
    const auto narrowTabs = neo::documentTabs(barState);
    const auto narrowTab = std::find_if(narrowTabs.begin(), narrowTabs.end(), [&](const neo::TabInfo& tab) {
        return tab.id == barAId;
    });
    check(narrowTab != narrowTabs.end(), "narrow tab fixture is present in the composed tab snapshot");
    if (narrowTab != narrowTabs.end()) {
        neo::UiMetrics narrowMetrics;
        narrowMetrics.menuBarHeight = 32.0f;
        neo::tab_bar_detail::tabCard(narrowTabUi, barState, neo::editorColors(), narrowMetrics,
                                     *narrowTab, narrowTabs, 7.0f, 24.0f, false, true, false);
        narrowTabUi.layout(240.0f, 40.0f);
        const auto* closeHit = narrowTabUi.find("menubar.tabs." + std::to_string(barAId) + ".close.hit");
        check(closeHit && std::fabs(closeHit->frame.x - 7.0f) < 0.01f &&
              std::fabs(closeHit->frame.width - 24.0f) < 0.01f &&
              closeHit->frame.x + closeHit->frame.width <= 31.01f,
              "24 DIP tab card keeps its close hit within the card bounds");
    }

    neo::tab_bar_detail::closeTabFromBar(barState, barBId, true, 162.0f, 16.0f);
    check(!neo::documentTab(barState, barBId) && barState.closeAnchor.enabled &&
          barState.closeAnchor.closedTabId == barBId && barState.tabId == barCId,
          "clean tab-card close starts the fixed-pointer chain on the surviving active page");
    neo::tab_bar_detail::clearCloseAnchor(barState);
    check(!barState.closeAnchor.enabled && barState.closeAnchorRevealPending,
          "clearing a close chain requests ordinary active-page reveal");

    neo::AppState revealState;
    neo::loadDocument(revealState, path(barA)); const auto revealAId = revealState.tabId;
    neo::loadDocument(revealState, path(barB)); const auto revealBId = revealState.tabId;
    neo::loadDocument(revealState, path(barC)); const auto revealCId = revealState.tabId;
    eui::Ui revealUi;
    const eui::Screen revealScreen{256.0f, 400.0f}; // 200 DIP tab region after the two trailing actions.
    revealUi.begin("document-tabs-reveal");
    neo::tabBarView(revealUi, revealState, revealScreen, 0.0f, 256.0f);
    check(std::fabs(revealState.tabScroll - 336.0f) < 0.01f,
          "initial tab-bar compose reveals the active tail page in the 200 DIP region");

    neo::tab_bar_detail::closeTabFromBar(revealState, revealCId, true, 186.0f, 16.0f);
    check(!neo::documentTab(revealState, revealCId) && revealState.tabId == revealBId &&
          revealState.closeAnchor.enabled,
          "closing the active tail page starts a valid chain on its preceding page");
    revealUi.begin("document-tabs-reveal");
    neo::tabBarView(revealUi, revealState, revealScreen, 0.0f, 256.0f);
    check(revealState.closeAnchor.enabled && !revealState.closeAnchorRevealPending,
          "tab-bar compose retains the close chain while its geometry remains valid");

    check(neo::activateDocumentTab(revealState, revealBId) && !revealState.closeAnchor.enabled &&
          revealState.closeAnchorRevealPending,
          "explicitly selecting the current page also interrupts the close chain");
    revealState.tabScroll = 0.0f;
    revealUi.begin("document-tabs-reveal");
    neo::tabBarView(revealUi, revealState, revealScreen, 0.0f, 256.0f);
    revealUi.layout(revealScreen);
    const auto* revealedActive = revealUi.find("menubar.tabs." + std::to_string(revealBId) + ".surface");
    check(std::fabs(revealState.tabScroll - 156.0f) < 0.01f && revealedActive &&
          std::fabs(revealedActive->frame.x - 24.0f) < 0.01f &&
          revealedActive->frame.x + revealedActive->frame.width <= 200.01f &&
          !revealState.closeAnchorRevealPending,
          "real tab-bar compose consumes a cleared chain and reveals the active card inside the region");

    barState.doc.text += " dirty C"; ++barState.revision;
    neo::tab_bar_detail::closeTabFromBar(barState, barCId, true, 162.0f, 16.0f);
    check(barState.pending == neo::PendingAction::CloseTab && barState.confirmationTabId == barCId &&
          !barState.closeAnchor.enabled,
          "dirty tab-card close enters the existing unsaved confirmation without extending the chain");
    neo::resolveDocumentConfirmation(barState, neo::filesafety::UnsavedChoice::Cancel);
    check(neo::documentTab(barState, barCId) && barState.dirty() &&
          barState.pending == neo::PendingAction::None,
          "dirty close Cancel ends the confirmation and preserves the edited page");
    neo::tab_bar_detail::closeTabFromBar(barState, barCId, true, 162.0f, 16.0f);
    neo::resolveDocumentConfirmation(barState, neo::filesafety::UnsavedChoice::Save);
    check(!neo::documentTab(barState, barCId) && barState.pending == neo::PendingAction::None &&
          get(barC).find("dirty C") != std::string::npos,
          "dirty close Save writes the target and ends the close transaction");

    barState.doc.text += " dirty A"; ++barState.revision;
    neo::tab_bar_detail::closeTabFromBar(barState, barAId, true, 162.0f, 16.0f);
    neo::resolveDocumentConfirmation(barState, neo::filesafety::UnsavedChoice::Discard);
    check(!neo::documentTab(barState, barAId) && barState.pending == neo::PendingAction::None &&
          barState.tabOrder.size() == 1,
          "dirty close Discard removes only its target and ends the close transaction");

    neo::AppState listActions;
    neo::loadDocument(listActions, path(barA)); const auto listA = listActions.tabId;
    neo::loadDocument(listActions, path(barB)); const auto listB = listActions.tabId;
    neo::loadDocument(listActions, path(barC)); const auto listC = listActions.tabId;
    // Full-path information must remain reachable without covering the row actions,
    // including a short window where neither side has enough popup space.
    listActions.tabListOpen = true;
    for (float screenHeight : {640.0f, 400.0f, 250.0f}) {
        eui::Ui listUi;
        listUi.begin("document-tabs-list-info");
        listUi.state<std::uint64_t>("menubar.tabs.list.infoTab") = listA;
        const auto listTabs = neo::documentTabs(listActions);
        const eui::Screen listScreen{440.0f, screenHeight};
        neo::tab_bar_detail::tabList(listUi, listActions, listScreen, neo::editorColors(),
                                    neo::uiMetrics(listActions), listTabs, 400.0f, true);
        listUi.layout(listScreen);
        const auto* panel = listUi.find("menubar.tabs.list");
        const auto* info = listUi.find("menubar.tabs.list.info");
        const auto* fullPath = listUi.find("menubar.tabs.list.info.path");
        check(panel && info && info->frame.y >= panel->frame.y + panel->frame.height + 7.99f &&
              info->frame.x >= 8.0f && info->frame.x + info->frame.width <= 432.01f &&
              info->frame.y + info->frame.height <= screenHeight - 7.99f,
              "narrow and short list information fits below the panel without covering actions");
        check(fullPath && fullPath->text.find(path(barA)) != std::string::npos,
              "full-path information retains the complete unelided path");
    }
    listActions.tabListOpen = false;
    neo::moveDocumentTab(listActions, listB, -1);
    check(listActions.tabOrder == std::vector<std::uint64_t>({listB, listA, listC}),
          "tab-list move action routes through the stable-ID reorder transition");
    neo::requestCloseTab(listActions, listA);
    check(!neo::documentTab(listActions, listA) &&
          listActions.tabOrder == std::vector<std::uint64_t>({listB, listC}),
          "tab-list close action routes through the stable-ID close transition");

    neo::activateDocumentTab(state, parentId); state.doc.text += "parent edit"; ++state.revision;
    neo::activateDocumentTab(state, otherId); state.doc.text += "other edit"; ++state.revision;
    check(!neo::requestCloseDocument(state) && state.tabId == parentId, "exit starts ordered dirty confirmation");
    neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Discard);
    check(state.pending == neo::PendingAction::CloseWindow && state.tabId == otherId && !state.closeApproved, "exit progresses to next dirty tab");
    neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Cancel);
    check(!state.exitClosing && !state.closeApproved && state.tabOrder.size() == 2 && !neo::documentTab(state, parentId)->suppressRecovery, "exit cancellation retains unasked tabs and restores recovery eligibility");
    nextSavePick = {true, false, path(parent), {}};
    check(!neo::saveDocumentAs(state), "Save As cannot overwrite another open document");
    if (state.saveConflictOpen) neo::resolveSaveConflict(state, 0);
    check(get(parent).find("parent edit") == std::string::npos && state.dirty(), "overlap Save As leaves other tab's disk bytes intact");
    neo::moveDocumentTab(state, otherId, -1); check(state.tabOrder.front() == otherId, "stable IDs support reorder");
    neo::requestCloseTab(state, otherId); neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Discard);
    neo::requestCloseTab(state, parentId); neo::resolveDocumentConfirmation(state, neo::filesafety::UnsavedChoice::Discard);
    check(state.tabOrder.size() == 1 && state.path.empty() && !state.dirty(), "closing final tab keeps a pristine blank window");
    const auto renameRoot = dir / "rename-project";
    put(renameRoot / "sub/a.md", "A source\n"); put(renameRoot / "sub/b.md", "B source\n"); put(renameRoot / "control.md", "control\n");
    neo::AppState renamed;
    neo::loadDocument(renamed, path(renameRoot / "control.md"));
    neo::loadDocument(renamed, path(renameRoot / "sub/a.md")); const auto aId = renamed.tabId;
    renamed.doc.text += "dirty A"; ++renamed.revision;
    const auto aFingerprint = renamed.diskFingerprint;
    neo::loadDocument(renamed, path(renameRoot / "sub/b.md")); const auto bId = renamed.tabId;
    neo::activateDocumentTab(renamed, 1);
    check(neo::vaultRenameEntry(renamed, "sub", "moved"), "real folder rename succeeds while two child tabs are background");
    check(neo::sameDocumentFile(neo::documentTab(renamed, aId)->path, path(renameRoot / "moved/a.md")) && neo::sameDocumentFile(neo::documentTab(renamed, bId)->path, path(renameRoot / "moved/b.md")) &&
        neo::documentTab(renamed, aId)->dirty() && neo::filesafety::same(aFingerprint, neo::documentTab(renamed, aId)->diskFingerprint), "folder rename updates all child paths and preserves dirty fingerprint");
    fs::remove(renameRoot / "moved/b.md");
    neo::vaultDeleteEntry(renamed, "moved", true); // Shell seam cancels, but an actually missing file still needs preserving.
    const auto* deleted = neo::documentTab(renamed, bId);
    check(deleted && deleted->path.empty() && deleted->dirty() && deleted->doc.text == "B source\n" && deleted->newDocumentLanguage == "markdown", "partial deletion preserves a clean background tab as an unsaved draft");
    check(neo::sameDocumentFile(neo::documentTab(renamed, aId)->path, path(renameRoot / "moved/a.md")) && neo::documentTab(renamed, aId)->dirty(), "partial deletion leaves surviving background manuscript intact");
    // Exercise startup against real committed manifests, including inaccessible
    // files and a discarded recovery record beside its clean original.
    sessionConfigDirectory = path(dir / "session-settings");
    const auto absent = dir / "missing-at-startup.txt";
    check(neo::sessionstorage::write({{61, path(absent), path(dir), "text", -1, false, nullptr}}, 61), "commit missing-file session fixture");
    neo::AppState unavailable;
    neo::restoreStartupDocument(unavailable, path(absent));
    check(unavailable.tabId == 61 && unavailable.unavailable && neo::sameDocumentFile(unavailable.path, path(absent)), "startup retains unavailable file as a retryable tab");
    put(absent, "file returned\n");
    neo::retryUnavailableDocument(unavailable);
    check(!unavailable.unavailable && !unavailable.dirty() && unavailable.doc.text == "file returned\n", "retry establishes verified text and fingerprint");
    check(neo::sessionstorage::write({{71, path(parent), path(dir/"project"), "markdown", -1, false, nullptr},
        {72, path(parent), path(dir/"project"), "markdown", -1, false, nullptr}}, 72), "commit duplicate clean recovery fixture");
    neo::AppState duplicates;
    neo::restoreStartupDocument(duplicates, path(parent));
    check(duplicates.tabOrder.size() == 1 && duplicates.tabId == 71 && !duplicates.dirty(), "startup coalesces clean physical duplicates and maps active ID");
    check(neo::sessionstorage::write({{81, "", "", "text", -1, false, nullptr}}, 81), "commit clean blank page fixture");
    neo::AppState blank;
    neo::restoreStartupDocument(blank, {});
    check(blank.doc.lineEnding == neo::DocumentSession{}.doc.lineEnding && !blank.dirty(), "restored clean blank keeps platform newline default");
    check(blank.path.empty() && blank.tabOrder.size() == 1 && blank.tabId == 1,
          "legacy clean blank record does not restore an old tab ID");
    check(neo::sessionstorage::write({{91, path(parent), path(dir/"project"), "markdown", -1, false, nullptr}}, 91),
          "commit old clean workspace fixture");
    neo::settings::current().lastFile = path(parent);
    neo::AppState noWorkspace;
    neo::restoreStartupDocument(noWorkspace, {});
    check(noWorkspace.path.empty() && noWorkspace.doc.text.empty() && noWorkspace.tabId == 1,
          "plain startup ignores both clean session and last-file legacy records");
    check(neo::requestCloseDocument(noWorkspace) && noWorkspace.closeApproved && !noWorkspace.sessionClosePending,
          "inline confirmed close clears records without waiting for checkpoint");
    std::vector<neo::sessionstorage::ReadRecord> closedRecords;
    std::uint64_t closedActive = 0;
    check(neo::sessionstorage::read(closedRecords, closedActive) && closedRecords.empty() &&
          neo::settings::current().lastFile.empty(), "normal close removes session and last-file auto-open state");

    // A malformed session manifest blocks automatic recovery writes and cleanup.
    // Closing the resulting blank window must not erase evidence that could be
    // useful for manual recovery, including the legacy single-document file.
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setInlineForTest(true);
    const std::string activeConfig = sessionConfigDirectory;
    const fs::path evidenceConfig = dir / "malformed-session-settings";
    sessionConfigDirectory = path(evidenceConfig);
    const fs::path evidenceDirectory = evidenceConfig / "session";
    const fs::path evidenceManifest = evidenceDirectory / "manifest.json";
    const fs::path evidenceBody = evidenceDirectory / ("body-" + std::string(32, 'a') + ".utf8");
    const fs::path evidenceLegacy = evidenceConfig / "recovery.txt";
    const std::string malformedManifestBytes = "{ malformed recovery manifest\r\nkeep bytes\n";
    const std::string evidenceBodyBytes = "orphaned draft body\r\n";
    const std::string evidenceLegacyBytes = "legacy recovery draft\r\n";
    put(evidenceManifest, malformedManifestBytes);
    put(evidenceBody, evidenceBodyBytes);
    put(evidenceLegacy, evidenceLegacyBytes);
    neo::settings::current().lastFile = path(parent);
    neo::AppState malformedRecovery;
    neo::restoreStartupDocument(malformedRecovery, {});
    check(malformedRecovery.sessionStorageBlocked && malformedRecovery.path.empty() &&
              malformedRecovery.doc.text.empty() && !malformedRecovery.dirty(),
          "malformed recovery manifest blocks storage while startup remains a blank document");
    check(neo::requestCloseDocument(malformedRecovery) && malformedRecovery.closeApproved,
          "blank startup with malformed recovery evidence can close normally");
    check(!malformedRecovery.dirty() && malformedRecovery.path.empty() &&
              neo::settings::current().lastFile.empty(),
          "closing malformed recovery evidence does not reopen a clean workspace");
    check(get(evidenceManifest) == malformedManifestBytes && get(evidenceBody) == evidenceBodyBytes &&
              get(evidenceLegacy) == evidenceLegacyBytes,
          "normal close preserves malformed manifest, owned body, and legacy recovery bytes");
    sessionConfigDirectory = activeConfig;

    // A clear failure keeps the window usable and allows a later retry. No save
    // or discard confirmation may be bypassed to get past a filesystem error.
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setInlineForTest(true);
    const fs::path blockedRecovery = neo::textfile::pathFromUtf8(sessionConfigDirectory) / "recovery.txt";
    put(blockedRecovery / "keep.txt", "clear obstruction");
    neo::AppState blockedClose;
    neo::loadDocument(blockedClose, path(parent));
    check(!neo::requestCloseDocument(blockedClose) && !blockedClose.closeApproved &&
          !blockedClose.sessionClosePending && blockedClose.toastVisible,
          "failed normal-exit cleanup retains an editable window and reports the error");
    check(neo::sameDocumentFile(blockedClose.path, path(parent)) && fs::exists(blockedRecovery / "keep.txt"),
          "cleanup failure preserves the document and the obstruction");
    fs::remove_all(blockedRecovery);
    check(neo::requestCloseDocument(blockedClose) && blockedClose.closeApproved,
          "normal-exit cleanup can be retried after a failure");

    neo::sessionwriter::resetForTest();
    neo::AppState asyncClose;
    check(!neo::requestCloseDocument(asyncClose) && asyncClose.sessionClosePending &&
          !asyncClose.closeApproved && neo::documentModalOpen(asyncClose),
          "production close returns to the event loop while records are being cleared");
    check(!neo::requestCloseDocument(asyncClose) && asyncClose.sessionClosePending,
          "repeated close cannot cancel the in-flight cleanup or permit editing");
    const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (asyncClose.sessionClosePending && std::chrono::steady_clock::now() < closeDeadline) {
        core::async::dispatchReady();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(asyncClose.closeApproved && !asyncClose.sessionClosePending,
          "successful async clear approves exit through its main-thread callback");

    // Exercise refreshTabPresentation against real AppState tab/root metadata
    // without opening 100 asynchronous file-verification sessions. Each cycle
    // introduces a new root plus a rooted blank draft, then removes both IDs.
    // Raw canonical roots and TabId maps must return to the live base tab.
    const fs::path presentationDir = dir / "presentation-cache-lifecycle";
    fs::create_directories(presentationDir);
    neo::AppState presentationState;
    presentationState.tabOrder = {presentationState.tabId};
    presentationState.nextTabId = presentationState.tabId + 1;
    const std::uint64_t baseTabId = presentationState.tabId;
    const std::string baseRoot = path(presentationDir / "base");
    presentationState.vaultRoot = baseRoot;
    presentationState.path = path(presentationDir / "base" / "base.md");
    neo::refreshTabPresentation(presentationState);
    check(presentationState.tabColorRegistry.groupCount() == 1 &&
          presentationState.tabCanonicalRootCache.size() == 1 &&
          presentationState.tabPresentationKeyByTab.size() == 1,
          "presentation cache starts at the one active base group");

    std::uint64_t previousTransientOrdinal = 0;
    for (int cycle = 0; cycle < 100; ++cycle) {
        const std::string transientRoot = path(presentationDir / ("root-" + std::to_string(cycle)));
        const std::uint64_t transientTabId = presentationState.nextTabId++;
        neo::DocumentSession transientPage;
        transientPage.tabId = transientTabId;
        transientPage.vaultRoot = transientRoot;
        transientPage.path = path(presentationDir / ("root-" + std::to_string(cycle)) /
                                  "transient.md");
        presentationState.inactiveTabs.emplace(transientTabId, std::move(transientPage));
        presentationState.tabOrder.push_back(transientTabId);
        neo::refreshTabPresentation(presentationState);
        const std::string transientKey = neo::tabpresentation::canonicalRootKey(transientRoot);
        const std::uint64_t transientOrdinal =
            presentationState.tabColorRegistry.state(transientKey).ordinal;
        check(presentationState.tabOrder.size() == 2 &&
              presentationState.tabColorRegistry.groupCount() == 2 &&
              presentationState.tabCanonicalRootCache.size() == 2 &&
              presentationState.tabPresentationKeyByTab.size() == 2 &&
              presentationState.tabPresentationSlot.size() == 2 &&
              presentationState.tabPresentationOrdinal.size() == 2 &&
              transientOrdinal > previousTransientOrdinal,
              "new AppState root gets a fresh ordinal and exactly two live cache entries");

        // A blank draft inherits the current transient root; retrieving tab
        // presentation data follows the same cached path used by tooltip/list UI.
        const std::uint64_t draftTabId = presentationState.nextTabId++;
        neo::DocumentSession draftPage;
        draftPage.tabId = draftTabId;
        draftPage.vaultRoot = transientRoot;
        presentationState.inactiveTabs.emplace(draftTabId, std::move(draftPage));
        presentationState.tabOrder.push_back(draftTabId);
        neo::refreshTabPresentation(presentationState);
        const auto draftTabs = neo::documentTabs(presentationState);
        bool tooltipRootsMatch = draftTabs.size() == 3;
        for (const neo::TabInfo& tab : draftTabs) {
            tooltipRootsMatch = tooltipRootsMatch &&
                neo::tab_bar_detail::displayedRoot(presentationState, tab) == tab.vaultRoot;
        }
        check(presentationState.tabOrder.size() == 3 &&
              presentationState.tabColorRegistry.groupCount() == 2 &&
              presentationState.tabCanonicalRootCache.size() == 2 &&
              presentationState.tabPresentationKeyByTab.size() == 3 &&
              presentationState.tabPresentationSlot.size() == 3 &&
              presentationState.tabPresentationOrdinal.size() == 3 &&
              neo::documentTab(presentationState, draftTabId) &&
              neo::documentTab(presentationState, draftTabId)->vaultRoot == transientRoot &&
              tooltipRootsMatch,
              "rooted blank draft shares the live group and tooltip root data");

        presentationState.inactiveTabs.erase(transientTabId);
        presentationState.tabOrder.erase(
            std::remove(presentationState.tabOrder.begin(), presentationState.tabOrder.end(), transientTabId),
            presentationState.tabOrder.end());
        neo::refreshTabPresentation(presentationState);
        check(!neo::documentTab(presentationState, transientTabId) &&
              presentationState.tabOrder.size() == 2 &&
              presentationState.tabColorRegistry.groupCount() == 2 &&
              presentationState.tabCanonicalRootCache.size() == 2 &&
              presentationState.tabPresentationKeyByTab.size() == 2 &&
              presentationState.tabPresentationSlot.size() == 2 &&
              presentationState.tabPresentationOrdinal.size() == 2 &&
              presentationState.tabColorRegistry.state(transientKey).ordinal == transientOrdinal,
              "closing the transient file retains its root while its blank draft is open");

        presentationState.inactiveTabs.erase(draftTabId);
        presentationState.tabOrder.erase(
            std::remove(presentationState.tabOrder.begin(), presentationState.tabOrder.end(), draftTabId),
            presentationState.tabOrder.end());
        neo::refreshTabPresentation(presentationState);
        check(!neo::documentTab(presentationState, draftTabId) &&
              presentationState.tabOrder == std::vector<std::uint64_t>{baseTabId} &&
              presentationState.tabId == baseTabId &&
              presentationState.tabColorRegistry.groupCount() == 1 &&
              presentationState.tabColorRegistry.has(
                  neo::tabpresentation::canonicalRootKey(baseRoot)) &&
              !presentationState.tabColorRegistry.has(transientKey) &&
              presentationState.tabCanonicalRootCache.size() == 1 &&
              presentationState.tabCanonicalRootCache.count(baseRoot) == 1 &&
              presentationState.tabPresentationKeyByTab.size() == 1 &&
              presentationState.tabPresentationKeyByTab.count(baseTabId) == 1 &&
              presentationState.tabPresentationSlot.size() == 1 &&
              presentationState.tabPresentationSlot.count(baseTabId) == 1 &&
              presentationState.tabPresentationOrdinal.size() == 1 &&
              presentationState.tabPresentationOrdinal.count(baseTabId) == 1 &&
              presentationState.tabPresentationSignature.size() == 1,
              "closing transient root and draft prunes every cache to the live base page");
        previousTransientOrdinal = transientOrdinal;
    }

    core::async::shutdown();
    sessionConfigDirectory.clear();
    fs::remove_all(dir);
    if (failures) return 1;
    std::cout << "document tab state tests passed\n"; return 0;
}
