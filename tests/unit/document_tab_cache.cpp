// 阶段 C 单元测试：页面派生缓存（LP 计划/装饰/源码）随页搬迁，暖切复用、跨页不误命中、
// 预算 LRU 逐出并真实释放排版缓存。
#include "core/platform/async.h"
#include "eui/platform.h"
#include "model/settings.h"
#include "model/theme_loader.h"
#include "platform/file_assoc.h"
#include "platform/file_operations.h"
#include "platform/font_safety.h"
#include "platform/native_dialogs.h"
#include "state/app_actions.h"
#include "state/app_state.h"
#include "state/session_writer.h"
#include "state/vault_cache.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ── 测试替身（与 document_tabs 单测同一套：app_actions 需要这些边界符号）──
namespace {
int failures = 0;
std::string sessionConfigDirectory;
neo::settings::RecoverySnapshot recoverySnapshot;
neo::dialogs::PickResult nextPick{false, true, {}, {}};
void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}
void put(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << bytes;
}
} // namespace

namespace neo::settings {
Data& current() { static Data data; return data; }
std::string configDirectory() { return sessionConfigDirectory; }
bool flush() { return true; }
// 跟随系统主题的替身：单测统一按亮色解析。
bool systemThemePrefersLight() { return true; }
bool writeRecovery(const std::string&, const std::string&, const textfile::Document*) { return true; }
bool readRecovery(RecoverySnapshot&) { return false; }
void clearRecovery() {}
} // namespace neo::settings

namespace neo::platform {
DeleteResult deleteVaultEntry(const std::string&, const std::string&, bool) { return {DeleteStatus::Cancelled, {}}; }
} // namespace neo::platform

namespace neo::dialogs {
PickResult pickDirectory(const std::string&) { return {false, true, {}, {}}; }
PickResult pickSavePath(const std::string&, const std::string&, const std::vector<std::string>&) { return nextPick; }
float systemScale() { return 1.0f; }
} // namespace neo::dialogs

namespace neo::fileassoc {
bool isRegistered() { return false; }
bool hasRegistrationEntries() { return false; }
DefaultStatus queryDefaultStatus() { return {}; }
std::vector<TypeStatus> queryTypes() { return {}; }
RegisterOutcome registerAsDefault() { return {}; }
bool openDefaultAppsSettings() { return false; }
bool unregister(std::string& error) { error = "stub"; return false; }
} // namespace neo::fileassoc

namespace neo::fontsafety {
ProbeResult validate(const std::string&, unsigned) { return ProbeResult{true, false, {}}; }
bool armSession(const settings::Data&, std::string&) { return true; }
} // namespace neo::fontsafety

namespace neo::themeloader { void reset() {} } // namespace neo::themeloader

namespace neo {
app::DslAppConfig& mutableAppConfig() { static app::DslAppConfig config; return config; }
} // namespace neo

namespace app {
void requestUpdate() {}
void requestClose() {}
namespace detail { void requestFullPaint() {} }
} // namespace app

namespace {

struct AsyncShutdownGuard {
    ~AsyncShutdownGuard() { core::async::shutdown(); }
};

unsigned long long fullPlanBuilds() { return neo::planDebugStats().full; }

std::size_t cachedHolders(const neo::AppState& state) {
    std::size_t count = 0;
    for (const auto& pair : state.inactiveTabs) {
        if (pair.second.derivedCacheUse != 0) ++count;
    }
    return count;
}

} // namespace

int main() {
    AsyncShutdownGuard shutdownGuard;
    neo::sessionwriter::resetForTest();
    neo::sessionwriter::setInlineForTest(true);
    neo::vaultcache::resetForTest();
    neo::vaultcache::clear();
    neo::i18n::initialize("en");

    std::error_code error;
    const fs::path dir = fs::temp_directory_path(error) /
        ("neo-tab-cache-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir, error);
    const auto path = [](const fs::path& p) { return neo::textfile::pathToUtf8(p); };
    const fs::path fileA = dir / "a.md";
    const fs::path fileB = dir / "b.md";
    const fs::path fileC = dir / "c.md";
    put(fileA, "# Alpha\n\nalpha body line\n");
    put(fileB, "# Beta\n\nbeta body text\n");
    put(fileC, "# Gamma\n\ngamma body text\n");

    neo::AppState state;
    check(neo::loadDocument(state, path(fileA)), "load A");
    const std::uint64_t aId = state.tabId;
    state.doc.text += "A dirty\n"; ++state.revision;
    check(neo::loadDocument(state, path(fileB)), "load B");
    const std::uint64_t bId = state.tabId;
    state.doc.text += "B dirty\n"; ++state.revision;
    check(neo::loadDocument(state, path(fileC)), "load C");
    const std::uint64_t cId = state.tabId;
    state.doc.text += "C dirty\n"; ++state.revision;
    check(state.tabOrder.size() == 3, "three tabs open");

    eui::Ui ui;
    // Mirror app.cpp's compose consumption of editorStateDirty after syncing inputs.
    // Without this, every synthetic tab activation looks like a fresh document load,
    // so syncDocumentTabInputs correctly discards editorMemory instead of restoring it.
    const auto syncInputsLikeCompose = [&]() {
        neo::syncDocumentTabInputs(ui, state);
        if (state.editorStateDirty) {
            neo::resetEditorInputState(ui, state.doc.text);
            state.editorStateDirty = false;
        }
    };

    // ── 阶段一：暖切复用（预算足够）──
    _putenv_s("NEO_TAB_CACHE_PAGES", "2");
    const auto warmActive = [&]() {
        neo::lp::cachedPlan(state.doc.text);
        return neo::lp::planCache().version;
    };

    check(neo::activateDocumentTab(state, aId), "activate A");
    const unsigned long long versionA = warmActive();
    const unsigned long long fullAfterA = fullPlanBuilds();
    check(neo::lp::planCache().text == state.doc.text, "A plan belongs to A text");

    check(neo::activateDocumentTab(state, bId), "activate B");
    warmActive();
    const unsigned long long fullAfterB = fullPlanBuilds();
    check(fullAfterB > fullAfterA, "cold page builds its own plan");
    check(neo::lp::planCache().text == state.doc.text, "B plan belongs to B text");

    check(neo::activateDocumentTab(state, aId), "return to A");
    const unsigned long long versionReturnA = warmActive();
    check(fullPlanBuilds() == fullAfterB, "warm return to A reuses its plan (no rebuild)");
    check(versionReturnA == versionA, "A plan version preserved across switch");
    check(neo::lp::planCache().text == state.doc.text, "returned plan is A's text, not B's");

    check(neo::activateDocumentTab(state, bId), "return to B");
    warmActive();
    check(fullPlanBuilds() == fullAfterB, "warm return to B reuses its plan (no rebuild)");
    check(neo::lp::planCache().text == state.doc.text, "returned plan is B's text, not A's");

    // ── 阶段二：预算 LRU 逐出并真实释放排版缓存 ──
    _putenv_s("NEO_TAB_CACHE_PAGES", "1");
    check(neo::activateDocumentTab(state, aId), "activate A (budget phase)");
    syncInputsLikeCompose();
    using Model = components::input_detail::InputModel;
    {
        auto& input = ui.state<Model::InputState>(neo::editorInputId(state));
        input.cachedLayoutText = "layout for A";
        input.cachedLines.resize(8);
    }
    check(neo::activateDocumentTab(state, bId), "activate B (evicts an older page)");
    syncInputsLikeCompose();
    check(neo::activateDocumentTab(state, cId), "activate C (keeps holder count bounded)");
    syncInputsLikeCompose();
    check(cachedHolders(state) <= 1, "inactive derived caches stay within the page budget");

    // 被逐出的页：派生槽位为空；若仍有输入内存，排版缓存必须已被 swap 释放。
    const auto* evicted = neo::documentTab(state, aId);
    check(evicted != nullptr && evicted->derivedCacheUse == 0 && !evicted->derivedPlan,
          "evicted page releases its derived cache slots");
    if (evicted != nullptr && evicted->editorMemory) {
        check(evicted->editorMemory->cachedLines.empty() && evicted->editorMemory->cachedLayoutText.empty(),
              "evicted page releases input layout capacity");
    }

    // The cache budget must include geometry arrays, not just visible line records.
    // Keep the document dirty and give its input state a real undo record so eviction
    // proves it only drops reconstructible layout data.
    _putenv_s("NEO_TAB_CACHE_BUDGET_MIB", "1");
    const auto addUndoableEdit = [&](std::uint64_t id) {
        auto* page = neo::documentTab(state, id);
        auto& input = ui.state<Model::InputState>(neo::editorInputId(id));
        input.text = page->doc.text;
        input.cursor = static_cast<int>(input.text.size());
        Model::beginEdit(input);
        input.text += " undo-check";
        input.cursor = static_cast<int>(input.text.size());
        Model::endEdit(input);
        page->doc.text = input.text;
        ++page->revision;
        return page->doc.text;
    };
    const std::string geometryDoc = addUndoableEdit(cId);
    {
        auto& input = ui.state<Model::InputState>(neo::editorInputId(cId));
        std::vector<float> heights(250000, 18.0f);  // two owned double arrays exceed 1 MiB
        input.cachedGeometry.build(heights);
        check(input.cachedGeometry.residentCapacityBytes() > 1024u * 1024u,
              "geometry fixture alone exceeds the configured derived-cache budget");
    }
    check(neo::activateDocumentTab(state, aId), "leave oversized geometry page");
    syncInputsLikeCompose();
    const auto* geometryEvicted = neo::documentTab(state, cId);
    check(geometryEvicted && geometryEvicted->editorMemory &&
              geometryEvicted->editorMemory->cachedGeometry.count() == 0 &&
              geometryEvicted->editorMemory->cachedGeometry.residentCapacityBytes() == 0,
          "geometry-only over-budget cache releases both prefix arrays");
    check(geometryEvicted && geometryEvicted->doc.text == geometryDoc && geometryEvicted->dirty() &&
              geometryEvicted->editorMemory && geometryEvicted->editorMemory->undoStack.size() == 1,
          "geometry eviction preserves dirty document text and undo history");

    check(neo::activateDocumentTab(state, cId), "return to page for table-capacity test");
    syncInputsLikeCompose();
    {
        auto& input = ui.state<Model::InputState>(neo::editorInputId(cId));
        Model::TableColumns measured;
        measured.tableId = 77;
        measured.x.resize(100000, 1.0f);
        measured.width.resize(100000, 2.0f);
        input.cachedTables.push_back(measured);
        Model::TableColumns intrinsic;
        intrinsic.tableId = 77;
        intrinsic.x.resize(100000, 3.0f);
        intrinsic.width.resize(100000, 4.0f);
        input.cachedTableIntrinsic.push_back(intrinsic);
        input.cachedTableIndex.emplace(77, 0);
        input.detailedRows.resize(3000);  // deliberately below 1 MiB; table columns alone exceed it
        input.detailTrackingValid = true;
        const std::size_t tableStorage =
            input.cachedTables.front().x.capacity() + input.cachedTables.front().width.capacity() +
            input.cachedTableIntrinsic.front().x.capacity() + input.cachedTableIntrinsic.front().width.capacity();
        check(tableStorage * sizeof(float) > 1024u * 1024u,
              "current and intrinsic table-column arrays alone exceed the configured budget");
    }
    check(neo::activateDocumentTab(state, aId), "leave oversized table cache page");
    syncInputsLikeCompose();
    const auto* tableEvicted = neo::documentTab(state, cId);
    check(tableEvicted && tableEvicted->editorMemory && tableEvicted->editorMemory->cachedTables.empty() &&
              tableEvicted->editorMemory->cachedTableIntrinsic.empty() &&
              tableEvicted->editorMemory->cachedTables.capacity() == 0 &&
              tableEvicted->editorMemory->cachedTableIntrinsic.capacity() == 0 &&
              tableEvicted->editorMemory->cachedTableIndex.empty() &&
              tableEvicted->editorMemory->cachedTableIndex.bucket_count() <=
                  Model::TableColumnIndex{}.bucket_count(),
          "table-column over-budget cache releases measured, intrinsic, and index storage");
    check(tableEvicted && tableEvicted->editorMemory && tableEvicted->editorMemory->detailedRows.empty() &&
              tableEvicted->editorMemory->detailedRows.capacity() == 0 &&
              !tableEvicted->editorMemory->detailTrackingValid,
          "table-cache eviction also releases detailed-row tracking state");
    check(tableEvicted && tableEvicted->doc.text == geometryDoc && tableEvicted->dirty() &&
              tableEvicted->editorMemory && tableEvicted->editorMemory->undoStack.size() == 1,
          "table eviction preserves dirty document text and undo history");
    _putenv_s("NEO_TAB_CACHE_BUDGET_MIB", "");

    neo::vaultcache::clear();
    fs::remove_all(dir, error);
    std::cout << (failures == 0 ? "PASS" : "FAIL") << ": document_tab_cache (" << failures << " failures)\n";
    return failures == 0 ? 0 : 1;
}
