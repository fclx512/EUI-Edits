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
#include "ui/vault_view.h"
#include "ui/overlays.h"
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
bool systemPrefersLight = true;
int fullPaintRequests = 0;
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
bool systemThemePrefersLight() { return systemPrefersLight; }
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
void requestFullPaint() { ++fullPaintRequests; }
} // namespace detail
} // namespace app


struct AsyncShutdownGuard {
    ~AsyncShutdownGuard() { core::async::shutdown(); }
};


// Differential oracle frozen from the pre-r13 title rules.
std::string legacyTabLabel(const neo::TabInfo& tab, const std::vector<neo::TabInfo>& tabs) {
    using namespace neo::tab_bar_detail;
    const auto base=tab.name.empty()?std::string(neo::i18n::tr("tabs.untitled")):tab.name;
    if(tab.path.empty()) return base;
    const auto key=lowerAscii(basename(tab.path)); int names=0;
    for(const auto& other:tabs) if(!other.path.empty() && lowerAscii(basename(other.path))==key) ++names;
    if(names<2) return base;
    auto parent=parentName(tab.path); if(parent.empty()) parent=rootName(tab.vaultRoot);
    auto label=parent.empty()?base:parent+" / "+base; int labels=0;
    for(const auto& other:tabs) {
        const auto otherBase=other.name.empty()?std::string(neo::i18n::tr("tabs.untitled")):other.name;
        if(!other.path.empty() && lowerAscii(parentName(other.path)+" / "+otherBase)==lowerAscii(label)) ++labels;
    }
    if(labels>1) { const auto root=rootName(tab.vaultRoot); if(!root.empty()) label=root+" / "+label; }
    return label;
}
void checkPrepareProjection(const fs::path& directory) {
    const auto path=[](const fs::path& value){return neo::textfile::pathToUtf8(value);};
    neo::AppState state; state.tabOrder.clear();
    const std::vector<fs::path> documents={directory/"project/README.md",directory/"project/docs/README.md",
        directory/"project/docs/../docs/README.md",directory/"project-extra/x.txt",
        directory/"project/缺失🙂/note.md",directory/"project/README.md/invalid-child.md",
        directory/"other/README.md",directory/"project",fs::path{}};
    for(std::size_t i=0;i<documents.size();++i) {
        neo::DocumentSession doc;doc.tabId=i+1; doc.path=documents[i].empty()?std::string{}:path(documents[i]);
        if(i==0) static_cast<neo::DocumentSession&>(state)=std::move(doc);
        else state.inactiveTabs.emplace(doc.tabId,std::move(doc));
        state.tabOrder.push_back(i+1);
    }
    state.tabOrder.push_back(999); // stale order ID must be ignored.
    const auto compare=[&] {
        neo::VaultDocumentPaths legacy;
        legacy.activeRelative=state.path.empty()?std::string{}:neo::vaultRelativePath(state,state.path);
        for(const auto& tab:neo::documentTabs(state)) if(!tab.path.empty() && neo::documentWithinRoot(tab.path,state.vaultRoot))
            legacy.openedRelative.push_back(neo::vaultRelativePath(state,tab.path));
        const auto actual=neo::documentVaultPaths(state);
        check(actual.activeRelative==legacy.activeRelative && actual.openedRelative==legacy.openedRelative,
              "frame path projection preserves canonical containment, relative/error results and order");
    };
    for(const auto& root:{fs::path{},directory/"project",directory/"missing-root",directory/"project/docs/..",
                         directory/"project/README.md",directory/"project/README.md/not-a-directory"}) {
        state.vaultRoot=root.empty()?std::string{}:path(root);compare();
    }
#if defined(_WIN32)
    state.vaultRoot=path(directory/"PROJECT");compare();
#endif
    state.vaultRoot=path(directory/"project");
    state.tabOrder.erase(state.tabOrder.begin());compare();
    state.path=path(directory/"other/README.md");compare();
    // A new frame must observe root/path edits and an external link retarget.
    std::error_code error;
    fs::create_directory_symlink(directory/"project",directory/"projection-link",error);
    if(!error) {
        state.path=path(directory/"projection-link/README.md");compare();
        fs::remove(directory/"projection-link",error);
        fs::create_directory_symlink(directory/"other",directory/"projection-link",error);
        if(!error) compare();
    } else std::cout<<"SKIP: projection directory symlink unavailable: "<<error.message()<<'\n';
}
void checkPrepareTitlesAndLanguage() {
    const std::vector<std::string> paths={"C:/one/docs/README.md","D:/two/docs/readme.MD","C:/one/other/README.md",
        "C:\\one\\docs\\README.md","note.md","C:/README.md","C:/文档🙂/Note.md","C:/unique.txt",""};
    for(const char* language:{"en","zh"}) {
        neo::i18n::initialize(language);
        for(int seed=0;seed<32;++seed) {
            std::vector<neo::TabInfo> tabs;
            for(int i=0;i<1+seed*3;++i) tabs.push_back({static_cast<std::uint64_t>(i+1),
                i%4==0?"":i%3==0?"别名🙂":"README.md", paths[(i+seed)%paths.size()],
                i%2==0?"C:/one":"D:/two/", "markdown",i%2==0});
            const neo::tab_bar_detail::TabLabelIndex index(tabs);
            for(const auto& tab:tabs) check(neo::tab_bar_detail::tabLabel(tab,tabs,&index)==legacyTabLabel(tab,tabs),
                "per-frame title index preserves legacy disambiguation including unsaved and Unicode names");
        }
    }
    neo::i18n::initialize("en");
    neo::AppState state; eui::Ui ui; const eui::Screen screen{800,600};
    const auto compose=[&] {ui.begin("language-overlay-test");neo::languageOverlay(ui,state,screen);ui.end();ui.layout(screen);};
    for(bool animations:{false,true}) {
        state.animations=animations;state.languageMenuOpen=true;compose();
        auto* item=ui.find("editor.languageMenu.level.0.item.1");
        check(item && item->onClick,"language menu opens with plain-text action");
        if(item && item->onClick) item->onClick();
        check(state.languageOverride=="text" && !state.languageMenuOpen,"language dispatch still changes override and closes");
        state.languageMenuOpen=true;compose();
        item=ui.find("editor.languageMenu.level.0.item.4");
        if(item && item->onHoverChanged) item->onHoverChanged(true);
        compose();
        state.languageMenuOpen=false;
        ui.state<float>("editor.languageMenu.level.0.offset")=100;
        compose();
        check(!state.languageMenuOpen && !ui.find("editor.languageMenu.dismiss"),
              "closed language menu releases its logical open state and dismiss input layer");
        if(animations) {
            const auto* root=ui.find("editor.languageMenu.level.0");
            const auto* menu=ui.find("editor.languageMenu.menu");
            int retainedItems=0;
            const auto allItemsDisabled=[&](const auto& self,const core::dsl::Element& element)->bool {
                bool disabled=true;
                if(element.id.find(".item.")!=std::string::npos) {
                    ++retainedItems;
                    disabled=element.disabled;
                }
                for(const auto& child:element.children) disabled=self(self,*child) && disabled;
                return disabled;
            };
            const bool retainedInputDisabled=menu && allItemsDisabled(allItemsDisabled,*menu);
            check(root && root->disabled && root->opacity==0 && retainedItems>0 && retainedInputDisabled,
                  "animated language-menu exit retains zero-opacity target content with every item disabled");
            // This Ui-only harness does not advance Runtime time. The finite
            // exit, child pruning and idle wake contract is covered by menu_motion;
            // the closed root may remain as a disabled layout instance.
        } else {
            check(!ui.find("editor.languageMenu.level.0.item.1"),
                  "language menu without animations immediately omits closed items");
        }
        check(ui.state<float>("editor.languageMenu.level.0.offset")==0,
              "closed language menu resets root scroll within its non-scrolling viewport");
        state.languageMenuOpen=true;state.languageOverride="python";compose();
        const auto* reopenedRoot=ui.find("editor.languageMenu.level.0");
        check(reopenedRoot && !reopenedRoot->disabled && reopenedRoot->opacity==1 &&
              ui.state<float>("editor.languageMenu.level.0.offset")==0 &&
              !ui.find("editor.languageMenu.level.1.item.0"),
              "reopening language menu restores input and resets root scroll and previous cascade");
        item=ui.find("editor.languageMenu.level.0.item.4");
        check(item && !item->disabled && item->onHoverChanged,"language menu reopens with enabled scripts group");
        if(item && item->onHoverChanged) item->onHoverChanged(true);
        compose();
        item=ui.find("editor.languageMenu.level.1.item.0");
        check(item && !item->disabled && item->onClick,"reopened scripts cascade has an enabled action");
        if(item && item->onClick) item->onClick();
        check(state.languageOverride=="python" && !state.languageMenuOpen,"scripts dispatch retains selected language");
    }
}

int main() {
    for (const char* name : {"photo.PNG", "photo.jpg", "photo.svg", "unsupported.webp"}) {
        neo::vault::Row imageRow;imageRow.name=name;
        check(neo::vault_detail::rowIcon(imageRow)==neo::UiIcon::ImageFile,"image entries use the internal photograph-file icon");
        imageRow.isDir=true;
        check(neo::vault_detail::rowIcon(imageRow)==neo::UiIcon::Folder,"image suffix does not change a directory icon");
    }
    check(!neo::filetypes::associationEligible("png") && !neo::filetypes::associationEligible("jpg") &&
          !neo::filetypes::associationEligible("webp"),"internal image icons do not add image Open With candidates");
    AsyncShutdownGuard asyncShutdownGuard;
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setInlineForTest(true);
    neo::vaultcache::resetForTest();
    neo::i18n::initialize("en");
    {
        neo::AppState library;
        auto scan = std::make_shared<neo::vault::ScanResult>();
        scan->ok = true;
        scan->roots = {{"notes.md", "notes.md", false, {}},
                       {"other.txt", "other.txt", false, {}}};
        library.vaultScan = scan;
        library.filter = "notes";
        library.vaultPathInputMode = false;
        neo::rebuildRows(library);
        check(library.rows.size() == 1 && library.rows.front().relative == "notes.md",
              "search mode applies the retained filename filter");
        library.vaultPathInputMode = true;
        neo::rebuildRows(library);
        check(library.rows.size() == 2 && library.filter == "notes",
              "path mode restores the full tree while preserving the search query");
        library.vaultPathInputMode = false;
        neo::rebuildRows(library);
        check(library.rows.size() == 1, "returning to search mode restores matching rows");
        for (float font : {14.0f, 18.0f}) {
            library.uiFontSize = font;
            for (float width : {180.0f, 264.0f}) {
                eui::Ui vaultUi;
                vaultUi.begin("vault-ui");
                neo::vaultPanelView(vaultUi, library, width, 600);
                vaultUi.end(); vaultUi.layout(width, 600);
                check(vaultUi.find("vault.address") && !vaultUi.find("vault.filter"),
                      "both library modes share exactly one input");
                const auto* modeHit = vaultUi.find("vault.mode.filter.hit");
                const auto* parentHit = vaultUi.find("vault.parent.hit");
                check(!vaultUi.find("vault.head.title") && modeHit && parentHit &&
                      std::fabs(parentHit->frame.x - modeHit->frame.x - modeHit->frame.width - 2.0f) < 0.1f,
                      "toolbar omits directory name and groups mode and parent buttons closely");
                const auto* label = vaultUi.find("vault.choose.text");
                check(label && label->frame.x + label->frame.width <= width - 9 &&
                      core::TextPrimitive::measureTextWidth(label->text, label->fontFamily, label->fontSize)
                          <= label->frame.width + 0.1f,
                      "English folder button text fits the narrow and large-font toolbar");
                auto& sharedInput = vaultUi.state<components::input_detail::InputModel::InputState>("vault.address");
                sharedInput.undoStack.emplace_back(); sharedInput.redoStack.emplace_back();
                vaultUi.find("vault.mode.filter.hit")->onClick();
                check(sharedInput.undoStack.empty() && sharedInput.redoStack.empty(),
                      "mode changes clear undo history independently of value equality");
                check(library.vaultPathInputMode && library.rows.size() == 2 && library.filter == "notes",
                      "mode button restores full tree and keeps the query");
                vaultUi.find("vault.mode.filter.hit")->onClick();
                check(!library.vaultPathInputMode && library.rows.size() == 1,
                      "mode button restores the retained search");
            }
        }
        check(neo::createDocumentTab(library) && !library.vaultPathInputMode && library.filter == "notes",
              "a new tab inherits both the library query and input mode");
    }
    for (const char* language : {"en", "zh-CN"}) {
        neo::i18n::initialize(language);
        neo::AppState library;
        for (float font : {14.0f, 18.0f}) {
            library.uiFontSize = font;
            for (float width : {180.0f, 264.0f}) {
                eui::Ui ui;
                ui.begin("vault-alignment");
                neo::vaultPanelView(ui, library, width, 600.0f);
                ui.end(); ui.layout(width, 600.0f);
                const auto* title = ui.find("vault.files.title");
                check(!title && !ui.find("vault.tab.outline.hit") && ui.find("vault.head"),
                      "file sidebar starts with controls, without obsolete tabs or title space");
                for (const char* tooltip : {"vault.mode.tooltip", "vault.parent.tooltip", "vault.choose.tooltip"}) {
                    const std::string id(tooltip);
                    const auto* panel = ui.find(id + ".bg");
                    const auto* text = ui.find(id + ".text");
                    check(panel && text && text->horizontalAlign == eui::HorizontalAlign::Center &&
                          text->verticalAlign == eui::VerticalAlign::Center &&
                          std::fabs(text->frame.y + text->frame.height * 0.5f
                                    - panel->frame.y - panel->frame.height * 0.5f) < 0.1f,
                          "all new toolbar explanations center text inside their panel");
                }
            }
        }
    }
    neo::i18n::initialize("en");
    {
        neo::AppState appearance;
        neo::applyFollowSystemTheme(appearance);
        check(appearance.themeFollowSystem && appearance.theme==neo::ThemeMode::Light &&
              neo::settings::current().theme==2, "follow preference resolves light and persists automatic mode");
        systemPrefersLight=false;
        const int paints=fullPaintRequests;
        neo::refreshSystemTheme(appearance);
        check(appearance.themeFollowSystem && appearance.theme==neo::ThemeMode::Dark &&
              neo::settings::current().theme==2 && fullPaintRequests==paints+1,
              "system change resolves dark, preserves follow and invalidates retained colors");
        neo::refreshSystemTheme(appearance);
        check(fullPaintRequests==paints+1, "duplicate system notification does not repaint");
        systemPrefersLight=true;
        neo::refreshSystemTheme(appearance);
        check(appearance.theme==neo::ThemeMode::Light && fullPaintRequests==paints+2,
              "system change back to light repaints");
        neo::applyTheme(appearance,neo::ThemeMode::Dark);
        neo::refreshSystemTheme(appearance);
        check(!appearance.themeFollowSystem && appearance.theme==neo::ThemeMode::Dark &&
              neo::settings::current().theme==0, "fixed dark ignores system light");
        neo::applyTheme(appearance,neo::ThemeMode::Light);
        systemPrefersLight=false;
        neo::refreshSystemTheme(appearance);
        check(!appearance.themeFollowSystem && appearance.theme==neo::ThemeMode::Light &&
              neo::settings::current().theme==1, "fixed light ignores system dark");
        systemPrefersLight=true;
    }
    const auto dir = fs::temp_directory_path() / ("neo-tabs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir / "project/docs/nested"); fs::create_directories(dir / "project-extra"); fs::create_directories(dir / "other");
    checkPrepareTitlesAndLanguage();
    const auto child = dir / "project/docs/README.md", parent = dir / "project/README.md", unrelated = dir / "other/README.md";
    put(child, "# Child\nchild body\n"); put(parent, "# Parent\nparent body\n"); put(unrelated, "other body\n");
    checkPrepareProjection(dir);
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
    const auto activeTextBeforeReorder = barState.doc.text;
    check(neo::moveDocumentTabToIndex(barState, barAId, 2) &&
          barState.tabOrder == std::vector<std::uint64_t>({barBId, barCId, barAId}) &&
          barState.tabId == barCId && barState.doc.text == activeTextBeforeReorder,
          "cross-slot reorder changes only tab order, not active document");
    check(!neo::moveDocumentTabToIndex(barState, barAId, 2) &&
          !neo::moveDocumentTabToIndex(barState, 999999, 0) &&
          !neo::moveDocumentTabToIndex(barState, barAId, 3),
          "same-slot and invalid reorders are no-ops");
    check(neo::moveDocumentTabToIndex(barState, barAId, 0) &&
          barState.tabOrder == std::vector<std::uint64_t>({barAId, barBId, barCId}),
          "reorder restores the original order");
    check(neo::tab_bar_detail::dragTargetIndex(205.0f, 20.0f, 0.0f, 0.0f, 3) == 1 &&
          neo::tab_bar_detail::dragTargetIndex(-50.0f, 20.0f, 0.0f, 0.0f, 3) == 0 &&
          neo::tab_bar_detail::dragTargetIndex(1000.0f, 20.0f, 0.0f, 0.0f, 3) == 2,
          "drag slots clamp to the visible document order");

    neo::AppState dragState;
    neo::loadDocument(dragState, path(barA)); const auto dragA = dragState.tabId;
    neo::loadDocument(dragState, path(barB)); const auto dragB = dragState.tabId;
    neo::loadDocument(dragState, path(barC)); const auto dragC = dragState.tabId;
    neo::activateDocumentTab(dragState, dragA);
    eui::Ui dragUi;
    // Preserve the 256-DIP drag surface and the original pointer trajectory
    // while reserving the additional Pin action beside New/List.
    const float dragBarWidth = 320.0f + neo::tab_bar_detail::TabActionsLayout::width;
    const eui::Screen dragScreen{dragBarWidth, 400.0f};
    const auto composeDragBar = [&] {
        dragUi.begin("document-tabs-drag");
        neo::tabBarView(dragUi, dragState, dragScreen, 0.0f, dragBarWidth);
        dragUi.layout(dragBarWidth, 400.0f);
    };
    const auto hitRect = [](const auto* element) {
        return core::Rect{element->frame.x, element->frame.y,
                          element->frame.width, element->frame.height};
    };
    composeDragBar();
    const auto dragAHitId = "menubar.tabs." + std::to_string(dragA) + ".tooltip.hit";
    const auto* dragAHit = dragUi.find(dragAHitId);
    check(dragAHit && dragAHit->onPress && dragAHit->onDragUpdate && dragAHit->onRelease,
          "tab title exposes press, drag and release callbacks");
    if (dragAHit && dragAHit->onPress && dragAHit->onDragUpdate && dragAHit->onRelease) {
        core::PointerEvent press;
        press.x = 40.0; press.y = 16.0; press.action = core::PointerAction::Press;
        press.button = core::PointerButton::Left; press.buttons = core::PointerButtons(core::PointerButton::Left);
        dragAHit->onPress(press, hitRect(dragAHit));
        core::dsl::DragEvent motion;
        motion.x = 245.0; motion.y = 16.0; motion.totalX = 205.0;
        motion.button = core::PointerButton::Left;
        motion.buttons = core::PointerButtons(core::PointerButton::Left);
        check(dragAHit->onDragUpdate(motion) &&
              dragState.tabOrder == std::vector<std::uint64_t>({dragA, dragB, dragC}),
              "drag preview leaves the model order untouched");
        composeDragBar();
        const auto* movedHit = dragUi.find(dragAHitId);
        if (movedHit && movedHit->onHoverChanged) movedHit->onHoverChanged(true);
        check(dragUi.state<std::uint64_t>("menubar.tabs.hovered") == 0 &&
              !dragUi.find("menubar.tabs.tooltip"),
              "drag hover cannot restore a tooltip over the moving tab");
        const auto* edgeTimer = dragUi.find("menubar.tabs.drag.timer");
        check(movedHit && movedHit->onRelease &&
              dragUi.find("menubar.tabs.dragGhost") && edgeTimer && edgeTimer->onTimer,
              "recomposed preview keeps the source hit identity and an edge timer");
        if (edgeTimer && edgeTimer->onTimer) edgeTimer->onTimer();
        check(dragState.tabScroll > 0.0f &&
              dragState.tabOrder == std::vector<std::uint64_t>({dragA, dragB, dragC}),
              "stationary edge timer scrolls without committing order");
        composeDragBar();
        movedHit = dragUi.find(dragAHitId);
        if (movedHit && movedHit->onRelease) {
            core::PointerEvent release;
            release.x = 245.0; release.y = 16.0; release.action = core::PointerAction::Release;
            release.button = core::PointerButton::Left;
            movedHit->onRelease(release, hitRect(movedHit));
        }
        check(dragState.tabOrder == std::vector<std::uint64_t>({dragB, dragA, dragC}) &&
              dragState.tabId == dragA,
              "valid drag release commits one reorder without activating a different document");
    }

    composeDragBar();
    const auto dragBPrefix = "menubar.tabs." + std::to_string(dragB);
    const auto* dragBHit = dragUi.find(dragBPrefix + ".tooltip.hit");
    if (dragBHit && dragBHit->onPress && dragBHit->onDragUpdate) {
        core::PointerEvent press;
        press.x = 40.0; press.y = 16.0; press.action = core::PointerAction::Press;
        press.button = core::PointerButton::Left; press.buttons = core::PointerButtons(core::PointerButton::Left);
        dragBHit->onPress(press, hitRect(dragBHit));
        core::dsl::DragEvent motion;
        motion.x = 220.0; motion.y = 16.0; motion.totalX = 180.0;
        motion.button = core::PointerButton::Left;
        motion.buttons = core::PointerButtons(core::PointerButton::Left);
        dragBHit->onDragUpdate(motion);
        check(neo::tab_bar_detail::requestDragCancelOnEscape(), "Escape requests drag cancellation");
        composeDragBar();
        const auto* canceledHit = dragUi.find(dragBPrefix + ".tooltip.hit");
        const auto* canceledSurface = dragUi.find(dragBPrefix + ".surface");
        if (canceledSurface && canceledSurface->onClick) canceledSurface->onClick();
        if (canceledHit && canceledHit->onRelease) {
            core::PointerEvent release;
            release.x = 40.0; release.y = 16.0; release.action = core::PointerAction::Release;
            release.button = core::PointerButton::Left;
            canceledHit->onRelease(release, hitRect(canceledHit));
        }
        check(dragState.tabOrder == std::vector<std::uint64_t>({dragB, dragA, dragC}) &&
              dragState.tabId == dragA &&
              !neo::tab_bar_detail::requestDragCancelOnEscape(),
              "canceled drag suppresses late click and release without changing order");
    }
    composeDragBar();
    dragBHit = dragUi.find(dragBPrefix + ".tooltip.hit");
    if (dragBHit && dragBHit->onPress && dragBHit->onDragUpdate && dragBHit->onRelease) {
        core::PointerEvent press;
        press.x = 40.0; press.y = 16.0; press.action = core::PointerAction::Press;
        press.button = core::PointerButton::Left; press.buttons = core::PointerButtons(core::PointerButton::Left);
        dragBHit->onPress(press, hitRect(dragBHit));
        core::dsl::DragEvent motion;
        motion.x = 220.0; motion.y = 16.0; motion.totalX = 180.0;
        motion.button = core::PointerButton::Left;
        motion.buttons = core::PointerButtons(core::PointerButton::Left);
        dragBHit->onDragUpdate(motion);
        core::PointerEvent cancel;
        cancel.x = 40.0; cancel.y = 16.0; cancel.action = core::PointerAction::Cancel;
        cancel.button = core::PointerButton::Left;
        dragBHit->onRelease(cancel, hitRect(dragBHit));
        cancel.action = core::PointerAction::Release;
        dragBHit->onRelease(cancel, hitRect(dragBHit));
        check(dragState.tabOrder == std::vector<std::uint64_t>({dragB, dragA, dragC}) &&
              dragState.tabId == dragA && !neo::tab_bar_detail::dragInProgress,
              "capture cancellation and late release cannot activate or reorder a tab");
    }

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

    for (float width : {96.0f, 180.0f, 232.0f}) {
        eui::Ui dirtyTabUi;
        dirtyTabUi.begin("document-tabs-dirty-marker");
        neo::TabInfo dirtyTab = *narrowTab;
        dirtyTab.dirty = true;
        dirtyTab.name = "long-document-name-with-an-ellipsis.md";
        neo::UiMetrics metrics;
        metrics.menuBarHeight = 32.0f;
        neo::tab_bar_detail::tabCard(dirtyTabUi, barState, neo::editorColors(), metrics,
                                     dirtyTab, narrowTabs, 7.0f, width, false, true, false);
        dirtyTabUi.layout(300.0f, 40.0f);
        const std::string id = "menubar.tabs." + std::to_string(dirtyTab.id);
        const auto* name = dirtyTabUi.find(id + ".name");
        const auto* marker = dirtyTabUi.find(id + ".dirty");
        const auto* close = dirtyTabUi.find(id + ".close.hit");
        check(name && marker && close &&
              marker->frame.x >= name->frame.x + name->frame.width + 4.9f &&
              close->frame.x >= marker->frame.x + marker->frame.width + 4.9f,
              "dirty marker clears the title ellipsis and unchanged close hit area");
    }

    neo::tab_bar_detail::closeTabFromBar(barState, barBId, true, 162.0f, 16.0f);
    check(!neo::documentTab(barState, barBId) && barState.closeAnchor.enabled &&
          barState.closeAnchor.closedTabId == barBId && barState.tabId == barCId,
          "clean tab-card close starts the fixed-pointer chain on the surviving active page");
    neo::tab_bar_detail::clearCloseAnchor(barState);
    check(!barState.closeAnchor.enabled && barState.closeAnchorRevealPending,
          "clearing a close chain requests ordinary active-page reveal");

    // Visible action-group seams must stay in the client area, so the native
    // mouse-leave path cannot cancel nearby Reveal lights between buttons.
    for (float width : {380.0f, 420.0f, 500.0f, 1000.0f}) {
        const float right = width - 152.0f;
        const float height = neo::uiMetrics(barState).menuBarHeight;
        neo::platform::chrome::beginFrame(width, height);
        eui::Ui groupUi;
        groupUi.begin("action-seams");
        neo::tabBarView(groupUi, barState, {width, 400.0f}, 0.0f, right);
        groupUi.end();
        groupUi.layout(width, 400.0f);
        const auto* bg = groupUi.find("menubar.tabs.actions.bg");
        check(bg && !bg->onClick && bg->hitTestMode == core::dsl::HitTestMode::None,
              "group background stays passive");
        bool continuous = bg != nullptr;
        if (bg) for (float x = bg->frame.x + .25f; x < bg->frame.x + bg->frame.width; x += .5f) {
            for (float y : {bg->frame.y + .25f, 16.0f, bg->frame.y + bg->frame.height - .25f})
                continuous = continuous && !neo::platform::chrome::captionAt(x, y, width, height);
        }
        check(continuous, "entire visible action group including seams stays client");
        check(neo::platform::chrome::captionAt(right + 8, 16, width, height),
              "empty title strip beside action group remains draggable");
        for (const char* name : {"menubar.tabs.new.hit", "menubar.tabs.list.hit", "menubar.tabs.pin.hit"}) {
            const auto* hit = groupUi.find(name);
            check(hit && hit->onClick && hit->frame.width == 26.0f,
                  "individual action hit widths and handlers remain intact");
        }
    }

    neo::AppState revealState;
    neo::loadDocument(revealState, path(barA)); const auto revealAId = revealState.tabId;
    neo::loadDocument(revealState, path(barB)); const auto revealBId = revealState.tabId;
    neo::loadDocument(revealState, path(barC)); const auto revealCId = revealState.tabId;
    eui::Ui revealUi;
    const float revealBarWidth = 256.0f + neo::tab_bar_detail::TabActionsLayout::width;
    const eui::Screen revealScreen{revealBarWidth, 400.0f}; // 192 DIP region after New/List/Pin and the gap.
    revealUi.begin("document-tabs-reveal");
    neo::tabBarView(revealUi, revealState, revealScreen, 0.0f, revealBarWidth);
    check(std::fabs(revealState.tabScroll - 336.0f) < 0.01f,
          "initial tab-bar compose reveals the active tail page in the 192 DIP region");

    neo::tab_bar_detail::closeTabFromBar(revealState, revealCId, true, 186.0f, 16.0f);
    check(!neo::documentTab(revealState, revealCId) && revealState.tabId == revealBId &&
          revealState.closeAnchor.enabled,
          "closing the active tail page starts a valid chain on its preceding page");
    revealUi.begin("document-tabs-reveal");
    neo::tabBarView(revealUi, revealState, revealScreen, 0.0f, revealBarWidth);
    check(revealState.closeAnchor.enabled && !revealState.closeAnchorRevealPending,
          "tab-bar compose retains the close chain while its geometry remains valid");

    check(neo::activateDocumentTab(revealState, revealBId) && !revealState.closeAnchor.enabled &&
          revealState.closeAnchorRevealPending,
          "explicitly selecting the current page also interrupts the close chain");
    revealState.tabScroll = 0.0f;
    revealUi.begin("document-tabs-reveal");
    neo::tabBarView(revealUi, revealState, revealScreen, 0.0f, revealBarWidth);
    revealUi.layout(revealScreen);
    const auto* revealedActive = revealUi.find("menubar.tabs." + std::to_string(revealBId) + ".surface");
    check(std::fabs(revealState.tabScroll - 160.0f) < 0.01f && revealedActive &&
          std::fabs(revealedActive->frame.x - 16.0f) < 0.01f &&
          revealedActive->frame.x + revealedActive->frame.width <= 192.01f &&
          !revealState.closeAnchorRevealPending,
          "real tab-bar compose consumes a cleared chain and reveals the active card inside the region");
    const auto* bottomLeft = revealUi.find("menubar.tabs.region.bottom.left");
    const auto* bottomRight = revealUi.find("menubar.tabs.region.bottom.right");
    const auto* activeFace = revealUi.find("menubar.tabs." + std::to_string(revealBId) + ".face");
    check(revealedActive && !bottomLeft && !bottomRight && activeFace &&
          revealedActive->border.width == 0.0f &&
          !revealUi.find("menubar.tabs." + std::to_string(revealBId) + ".right.edge") &&
          activeFace->frame.width == revealedActive->frame.width + 16.0f,
          "active tab uses one shoulder-to-shoulder face without bottom or duplicate side strokes");

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

    // ── 只读思维导图预览缓存（阶段 D 回归）：页自有、计入派生预算、LRU 可淘汰、
    //    正文/撤销绝不因淘汰丢失。合成 20001 节点的缓存把每个页顶到约 5 MiB，
    //    用 NEO_TAB_CACHE_BUDGET_MIB=1 触发淘汰。
    {
        _putenv_s("NEO_TAB_CACHE_BUDGET_MIB", "1");
        const std::string docA = "# A\n\n```mermaid\nmindmap\n  root((A))\n    a1\n```\n";
        const std::string docB = "# B\n\n```mermaid\nmindmap\n  root((B))\n    b1\n```\n";
        neo::AppState mapState;
        mapState.tabOrder.clear();
        neo::DocumentSession pageA;
        pageA.tabId = 901; pageA.newDocumentLanguage = "markdown"; pageA.doc.text = docA;
        pageA.editorMemory = std::make_unique<components::input_detail::InputModel::InputState>();
        pageA.editorMemory->text = docA;
        pageA.editorMemory->cursor = static_cast<int>(docA.size());
        components::input_detail::InputModel::insertAtCursor(*pageA.editorMemory, "draft\n");
        pageA.doc.text = pageA.editorMemory->text;
        pageA.revision = 1;
        const std::string draftA = pageA.doc.text;
        const auto undoCountA = pageA.editorMemory->undoStack.size();
        neo::DocumentSession pageB;
        pageB.tabId = 902; pageB.newDocumentLanguage = "markdown"; pageB.doc.text = docB;
        static_cast<neo::DocumentSession&>(mapState) = std::move(pageA);
        mapState.inactiveTabs.emplace(902, std::move(pageB));
        mapState.tabOrder = {901, 902};
        mapState.nextTabId = 903;

        const auto inflate = [](neo::DocumentSession& page, std::uint64_t use) {
            page.derivedMindmap = std::make_unique<neo::mindmap::Cache>();
            neo::mindmap::Cache& cache = *page.derivedMindmap;
            cache.plan.nodes.assign(20001, neo::mindmap::Node{});
            cache.plan.nodes[0].children.resize(20000);
            for (std::size_t index = 1; index <= 20000; ++index) {
                cache.plan.nodes[0].children[index - 1] = static_cast<int>(index);
                cache.plan.nodes[index].parent = 0;
            }
            cache.layout.nodes.resize(20001);
            cache.source = "synthetic";
            cache.key.tabId = page.tabId;
            cache.valid = true;
            cache.bytes = neo::mindmap::estimateCacheBytes(cache);
            check(cache.bytes > 1024ull * 1024ull, "fixture: 合成缓存应超过 1 MiB");
            page.derivedCacheBytes = cache.bytes;
            page.derivedCacheUse = use;
        };
        inflate(static_cast<neo::DocumentSession&>(mapState), 2);
        inflate(mapState.inactiveTabs.at(902), 1);

        // 切到 B：A 离场入后台后按预算被淘汰；B 的缓存随页成为活动槽位而保留。
        check(neo::activateDocumentTab(mapState, 902), "导图缓存回归：切换到 B 应成功");
        const neo::DocumentSession* activeB = neo::documentTab(mapState, 902);
        const neo::DocumentSession* backgroundA = neo::documentTab(mapState, 901);
        check(activeB && activeB->derivedMindmap && activeB->derivedMindmap->valid &&
                  activeB->derivedMindmap->plan.nodes.size() == 20001,
              "活动页自有导图缓存随页保留");
        check(backgroundA && backgroundA->derivedMindmap == nullptr &&
                  backgroundA->derivedCacheBytes == 0,
              "超预算的最近最少使用页导图缓存被淘汰");
        check(backgroundA && backgroundA->doc.text == draftA && backgroundA->dirty() &&
                  backgroundA->editorMemory && backgroundA->editorMemory->undoStack.size() == undoCountA,
              "淘汰只清派生缓存，后台页正文、dirty 与撤销记录不丢");
        check(activeB->doc.text == docB, "活动页正文完整");

        // 切回 A：B 离场后被淘汰（其缓存计入预算），A 无缓存但正文完整。
        check(neo::activateDocumentTab(mapState, 901), "导图缓存回归：切回 A 应成功");
        check(neo::documentTab(mapState, 902)->derivedMindmap == nullptr &&
                  neo::documentTab(mapState, 902)->doc.text == docB,
              "离场页导图缓存被淘汰而正文保留");
        check(static_cast<const neo::DocumentSession&>(mapState).dirty() &&
                  static_cast<const neo::DocumentSession&>(mapState).doc.text == draftA &&
                  mapState.editorMemory && mapState.editorMemory->undoStack.size() == undoCountA,
              "活动页正文与 dirty 状态不受淘汰影响");

        // 预算内的小缓存页在后台保留（不被误杀）。
        {
            neo::DocumentSession& background = mapState.inactiveTabs.at(902);
            background.derivedMindmap = std::make_unique<neo::mindmap::Cache>();
            background.derivedMindmap->plan.nodes.resize(4);
            background.derivedMindmap->valid = true;
            background.derivedMindmap->bytes = neo::mindmap::estimateCacheBytes(*background.derivedMindmap);
            background.derivedCacheBytes = background.derivedMindmap->bytes;
            background.derivedCacheUse = 1;
        }
        check(neo::activateDocumentTab(mapState, 902), "导图缓存回归：小缓存页切换应成功");
        check(neo::documentTab(mapState, 901)->derivedMindmap == nullptr &&
                  neo::documentTab(mapState, 901)->doc.text == draftA,
              "预算内的小缓存页不触发误杀，后台页正文保留");
        check(neo::documentTab(mapState, 902)->derivedMindmap &&
                  neo::documentTab(mapState, 902)->derivedMindmap->valid,
              "预算内的小缓存随页成为活动槽位而保留");

        // 预览浮层是文档模态：open 时禁用切页。
        mapState.mindmapPreview.open = true;
        check(!neo::activateDocumentTab(mapState, 901), "思维导图预览打开时不得切换标签页");
        mapState.mindmapPreview.open = false;

        _putenv_s("NEO_TAB_CACHE_BUDGET_MIB", "");
    }

    core::async::shutdown();
    sessionConfigDirectory.clear();
    fs::remove_all(dir);
    if (failures) return 1;
    std::cout << "document tab state tests passed\n"; return 0;
}
