// Real document-open and cold vault adoption; platform/persistence test doubles only.
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
#include "core/platform/async.h"
#include "components/input_model.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
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
// 选择目录对话框的替身返回值：默认取消，测试按需改写。
neo::dialogs::PickResult nextDirectoryPick{false, true, {}, {}};
int directoryPickCalls = 0;
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

PickResult pickDirectory(const std::string&) {
    ++directoryPickCalls;
    return nextDirectoryPick;
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



#include <atomic>
#include <condition_variable>
#include <mutex>

namespace {
using Clock = std::chrono::steady_clock;
bool pumpUntil(const std::function<bool()>& done) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!done()) {
        core::async::dispatchReady();
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
// A controllable scan double. Worker calls hold until released, so a test can
// assert the UI never blocked on the scan; caller-thread calls are counted and
// must stay zero -- that is the negative control against the old synchronous
// refreshVault path (which scanned on the caller thread and published at once).
struct Scanner {
    std::thread::id mainThread = std::this_thread::get_id();
    std::atomic<int> mainCalls{0}, workerCalls{0};
    std::mutex mutex;
    std::condition_variable cv;
    // Configured on the main thread before the scan is requested (that ordering
    // is what makes the plain reads below race-free on the worker).
    bool fail = false;
    bool addLater = false;
    // Encode the scanned root's own name in a leading row so a completion that
    // belongs to a different root can be told apart from the active root's rows.
    bool tagByRootName = false;
    bool releasesAll = false;                 // release(): every worker may run
    std::set<std::string> releasedRoots;      // releaseRoot(): one root may run
    std::vector<std::string> scannedRoots;    // worker-visible roots, in order

    neo::vault::ScanResult operator()(const std::string& root) {
        if (std::this_thread::get_id() == mainThread) {
            ++mainCalls;
            // A finite negative-control delay: the old path must fail assertions,
            // never deadlock waiting for a release on its own caller thread.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            return build(root, fail);
        }
        ++workerCalls;
        {
            std::unique_lock<std::mutex> lock(mutex);
            scannedRoots.push_back(root);
            cv.notify_all();
            cv.wait(lock, [&] { return releasesAll || releasedRoots.count(root) != 0; });
        }
        return build(root, fail);
    }
    neo::vault::ScanResult build(const std::string& root, bool failed) const {
        neo::vault::ScanResult result;
        result.ok = !failed;
        if (failed) { result.error = "test root unreadable"; return result; }
        result.fileCount = 4; result.directoryCount = 1;
        result.roots = {{u8"子目录", u8"子目录", true,
                         {{u8"当前.md", u8"子目录/当前.md", false, {}}}},
                        {".hidden", ".hidden", false, {}}, {"noext", "noext", false, {}},
                        {u8"当前.md", u8"当前.md", false, {}}};
        // This test double supplies a preloaded child subtree. Mark it loaded so
        // the lazy page scheduler does not also schedule a real child IO task.
        result.roots.front().childrenLoaded = true;
        if (addLater) { result.roots.push_back({"later.md", "later.md", false, {}}); ++result.fileCount; }
        if (tagByRootName) {
            const std::string marker = neo::textfile::fileName(root);
            result.roots.insert(result.roots.begin(), neo::vault::Entry{marker, marker, false, {}});
            ++result.fileCount;
        }
        return result;
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        releasesAll = true; cv.notify_all();
    }
    void releaseRoot(const std::string& root) {
        std::lock_guard<std::mutex> lock(mutex);
        releasedRoots.insert(root); cv.notify_all();
    }
    bool waitForRoot(const std::string& root) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return std::find(scannedRoots.begin(), scannedRoots.end(), root) != scannedRoots.end();
        });
    }
};
void drain(Scanner& scanner, const std::string& root) {
    scanner.release();
    check(pumpUntil([&] { return !neo::vaultcache::query(root).scanning; }), "scan drains before scanner destruction");
}

// ── chooseVaultDirectory / navigateToVaultPath 的异步换根场景 ──────────────────
// 这两个入口改用 prepareVault：换根本身不扫，返回时是 loading 态（rows 空），
// 后台只排一次扫描，完成回调在主线程 pump 时发布。共享同一套场景避免重复。
using VaultStart = void (*)(neo::AppState&, const std::string&);

void startChooseVault(neo::AppState& state, const std::string& root) {
    nextDirectoryPick = {true, false, root, {}};
    neo::chooseVaultDirectory(state);
}

void startNavigateVault(neo::AppState& state, const std::string& root) {
    neo::navigateToVaultPath(state, root);
}

void startParentVaultForEdit(neo::AppState& state, const std::string& root) {
    const auto child = neo::textfile::pathFromUtf8(root) / "navigation-child";
    fs::create_directories(child);
    state.vaultRoot = neo::textfile::pathToUtf8(child);
    neo::navigateToVaultParent(state);
}

std::shared_ptr<const neo::vault::ScanResult> warmRootWithFiles(const std::string& root,
                                                                const std::vector<std::string>& files);

void editAndSaveDuringScan(VaultStart start, const fs::path& rootDir, bool warm, bool fail) {
    neo::vaultcache::resetForTest();
    const std::string root = neo::textfile::pathToUtf8(rootDir);
    const auto path = rootDir / u8"编辑保存🙂.md";
    const std::string original = u8"正文中🙂\n";
    put(path, original);
    neo::AppState state;
    check(neo::loadDocument(state, neo::textfile::pathToUtf8(path)), "load editable fixture before gated scan");
    check(pumpUntil([&] { return !neo::vaultcache::query(root).scanning; }), "initial real scan drains");
    neo::vaultcache::resetForTest();
    if (warm) warmRootWithFiles(root, {u8"编辑保存🙂.md"});
    Scanner scanner; scanner.fail = fail;
    neo::vaultcache::setScannerForTest([&scanner](const std::string& value) { return scanner(value); });
    start(state, root);
    const bool reached = scanner.waitForRoot(root);
    check(reached && scanner.mainCalls == 0, "edit/save scan is gated on worker only");
    if (!reached) { drain(scanner, root); return; }

    // Exercise the actual UTF-8 input edit/undo model, then the editor callback's
    // document/revision settlement. No native input event or display is claimed.
    using Model = components::input_detail::InputModel;
    Model::InputState input;
    Model::loadDocument(input, state.doc.text);
    input.cursor = static_cast<int>(input.text.size());
    input.selectionStart = input.selectionEnd = input.cursor;
    Model::insertAtCursor(input, u8"扫描中编辑🙂\n");
    const std::string edited = input.text;
    check(Model::undoEdit(input) && input.text == original, "undo remains exact while scan is held");
    check(Model::redoEdit(input) && input.text == edited, "redo remains exact while scan is held");
    state.doc.text = input.text; ++state.revision;
    check(state.dirty() && neo::vaultcache::query(root).scanning, "document is dirty before scan completes");
    check(neo::saveDocument(state), "real saveDocument succeeds while scan is held");
    check(get(path) == edited && !state.dirty() && state.savedRevision == state.revision,
          "on-disk UTF-8 save receipt matches edited bytes and saved revision");
    check(scanner.mainCalls == 0 && neo::vaultcache::query(root).scanning,
          "same-root save does not force a caller-thread scan");
    scanner.release();
    check(pumpUntil([&] { return !neo::vaultcache::query(root).scanning; }), "edit/save scan drains");
    state.toastVisible = false; neo::tickVaultScan(state);
    check(state.vaultScan && state.vaultScan->ok == !fail && state.doc.text == edited && !state.dirty(),
          "success or scan failure preserves saved text and clean revision");
    if (fail) check(state.toastVisible && state.rows.empty(), "scan failure ends loading with an error notice");

    Model::insertAtCursor(input, "unsaved\n");
    state.doc.text = input.text; ++state.revision;
    put(path, "external rewrite\n");
    check(!neo::saveDocument(state) && state.dirty() && get(path) == "external rewrite\n",
          "external rewrite conflict rejects save without losing edits or overwriting disk");
    neo::vaultcache::invalidate(root);
    std::cout << "PASS scenario: edit-save-gated warm=" << warm << " fail=" << fail << '\n';
}

fs::path makeDirectory(const fs::path& parent, const char* name);
std::shared_ptr<const neo::vault::ScanResult> warmRootWithFiles(const std::string& root,
                                                                const std::vector<std::string>& files);

void parentVaultActionIsAsync(const fs::path& base) {
    neo::vaultcache::resetForTest();
    const fs::path childDir = makeDirectory(base, "parent-child");
    const std::string child = neo::textfile::pathToUtf8(childDir);
    const std::string parent = neo::textfile::pathToUtf8(childDir.parent_path());
    Scanner scanner;
    neo::vaultcache::setScannerForTest([&scanner](const std::string& value) { return scanner(value); });
    neo::AppState state;
    state.vaultRoot = child;
    state.vaultAddress = child;
    state.expanded.insert("stale");
    neo::navigateToVaultParent(state);
    check(state.vaultRoot == parent && state.vaultAddress == parent,
          "parent action switches to the lexical parent root");
    check(state.expanded.empty() && scanner.mainCalls == 0,
          "parent action resets projection without scanning on the caller thread");
    check(!state.vaultScan && state.rows.empty(),
          "cold parent action enters loading with no stale rows");
    check(scanner.waitForRoot(parent), "parent action schedules a background scan");
    check(neo::vaultcache::stats().scanRequests == 1 && scanner.workerCalls == 1,
          "parent action schedules exactly one background scan");
    scanner.release();
    check(pumpUntil([&] { return !neo::vaultcache::query(parent).scanning; }),
          "parent scan drains in bounded time");
    neo::tickVaultScan(state);
    check(state.vaultScan && state.vaultScan->ok && !state.rows.empty(),
          "parent action adopts the completed snapshot");
    neo::vaultcache::invalidate(parent);
}

void warmParentVaultActionReusesSnapshot(const fs::path& base) {
    neo::vaultcache::resetForTest();
    const fs::path childDir = makeDirectory(base, "warm-parent-child");
    const std::string child = neo::textfile::pathToUtf8(childDir);
    const std::string parent = neo::textfile::pathToUtf8(childDir.parent_path());
    const auto warmSnapshot = warmRootWithFiles(parent, {"warm.md"});
    Scanner scanner;
    neo::vaultcache::setScannerForTest([&scanner](const std::string& value) { return scanner(value); });
    neo::AppState state;
    state.vaultRoot = child;
    state.vaultAddress = child;
    neo::navigateToVaultParent(state);
    check(state.vaultRoot == parent && state.vaultScan == warmSnapshot && state.rows.size() == 1,
          "warm parent action reuses the cached snapshot immediately");
    check(scanner.mainCalls == 0 && scanner.waitForRoot(parent),
          "warm parent action revalidates in the background");
    scanner.release();
    check(pumpUntil([&] { return !neo::vaultcache::query(parent).scanning; }),
          "warm parent revalidation drains in bounded time");
    neo::tickVaultScan(state);
    check(state.vaultRowsGeneration == 2 && state.vaultScan == neo::vaultcache::query(parent).scan,
          "warm parent action adopts one newer snapshot");
    neo::vaultcache::invalidate(parent);
}

void parentVaultFailuresEndLoading(const fs::path& base) {
    for (bool warm : {false, true}) {
        neo::vaultcache::resetForTest();
        const fs::path childDir = makeDirectory(base, warm ? "parent-fail-warm" : "parent-fail-cold");
        const std::string parent = neo::textfile::pathToUtf8(base);
        if (warm) warmRootWithFiles(parent, {"warm.md"});
        Scanner scanner;
        scanner.fail = true;
        neo::vaultcache::setScannerForTest([&scanner](const std::string& value) { return scanner(value); });
        neo::AppState state;
        state.vaultRoot = neo::textfile::pathToUtf8(childDir);
        state.doc.text = u8"unsaved 正文🙂\n";
        ++state.revision;
        neo::navigateToVaultParent(state);
        check(scanner.mainCalls == 0 && scanner.waitForRoot(parent),
              "failed parent scan runs only in the background");
        check(warm ? bool(state.vaultScan && state.vaultScan->ok) : !state.vaultScan,
              "failed parent scan initially retains warm rows or cold loading");
        scanner.release();
        check(pumpUntil([&] { return !neo::vaultcache::query(parent).scanning; }),
              "failed parent scan drains in bounded time");
        neo::tickVaultScan(state);
        check(state.vaultScan && !state.vaultScan->ok && state.rows.empty() && state.toastVisible,
              "failed parent scan leaves loading and reports an error");
        check(state.doc.text == u8"unsaved 正文🙂\n" && state.dirty(),
              "parent navigation preserves unsaved document bytes");
    }
}

void consecutiveParentScansKeepActiveRoot(const fs::path& base) {
    neo::vaultcache::resetForTest();
    const fs::path firstDir = makeDirectory(base, "parent-chain");
    const fs::path childDir = makeDirectory(firstDir, "child");
    const std::string first = neo::textfile::pathToUtf8(firstDir);
    const std::string second = neo::textfile::pathToUtf8(base);
    Scanner scanner;
    scanner.tagByRootName = true;
    neo::vaultcache::setScannerForTest([&scanner](const std::string& value) { return scanner(value); });
    neo::AppState state;
    state.vaultRoot = neo::textfile::pathToUtf8(childDir);
    neo::navigateToVaultParent(state);
    check(scanner.waitForRoot(first), "first parent scan reaches the worker");
    neo::navigateToVaultParent(state);
    check(state.vaultRoot == second && scanner.waitForRoot(second) && scanner.mainCalls == 0,
          "second parent navigation returns before either scan completes");
    scanner.releaseRoot(first);
    check(pumpUntil([&] { return neo::vaultcache::stats().scansDropped >= 1; }),
          "previous parent scan is discarded after switching roots");
    neo::tickVaultScan(state);
    check(state.vaultRoot == second && !state.vaultScan && state.rows.empty(),
          "late previous parent result cannot populate the active root");
    scanner.releaseRoot(second);
    check(pumpUntil([&] { return !neo::vaultcache::query(second).scanning; }),
          "active parent scan drains");
    neo::tickVaultScan(state);
    check(state.vaultScan == neo::vaultcache::query(second).scan && !state.rows.empty() &&
              state.rows.front().name == neo::textfile::fileName(second),
          "active parent adopts only its own rows");
}

fs::path makeDirectory(const fs::path& parent, const char* name) {
    const fs::path dir = parent / name;
    fs::create_directories(dir);
    return dir;
}

// 同步预热某根的快照（走 requestScanSync），返回已发布的快照。
std::shared_ptr<const neo::vault::ScanResult> warmRootWithFiles(const std::string& root,
                                                                const std::vector<std::string>& files) {
    neo::vaultcache::setScannerForTest([files](const std::string&) {
        neo::vault::ScanResult result;
        result.ok = true;
        result.fileCount = static_cast<int>(files.size());
        for (const std::string& name : files) result.roots.push_back({name, name, false, {}});
        return result;
    });
    neo::vaultcache::requestScanSync(root);
    return neo::vaultcache::query(root).scan;
}

void coldVaultActionInBackground(VaultStart start, const fs::path& rootDir) {
    neo::vaultcache::resetForTest();
    const std::string root = neo::textfile::pathToUtf8(rootDir);
    Scanner scanner;
    neo::vaultcache::setScannerForTest([&scanner](const std::string& value) { return scanner(value); });
    neo::AppState state;
    start(state, root);
    check(state.vaultRoot == root, "cold vault action adopts the requested root");
    check(scanner.mainCalls == 0, "cold vault action never scans on the caller thread");
    check(!state.vaultScan && state.rows.empty(), "cold vault action shows loading without placeholder rows");
    eui::Ui loadingUi; loadingUi.begin("cold.loading"); neo::vaultPanelView(loadingUi, state, 264, 600); loadingUi.end();
    check(loadingUi.find("vault.loading"), "cold vault action renders the loading panel");
    check(scanner.waitForRoot(root), "cold vault action schedules a background scan");
    check(neo::vaultcache::query(root).scanning && scanner.workerCalls == 1 &&
              neo::vaultcache::stats().scanRequests == 1,
          "cold vault action schedules exactly one background scan");
    for (int i = 0; i < 20; ++i) start(state, root);
    check(!neo::vaultcache::query(root).rescanPending && neo::vaultcache::stats().scanRequests == 1,
          "repeated cold vault action does not enqueue a redundant scan");
    scanner.release();
    check(pumpUntil([&] { return !neo::vaultcache::query(root).scanning; }), "cold vault scan drains in bounded time");
    neo::tickVaultScan(state);
    check(state.vaultScan && state.vaultScan->ok && !state.rows.empty(),
          "cold vault action adopts the background snapshot");
    neo::vaultcache::invalidate(root);
}

void warmVaultActionAdoptsSnapshot(VaultStart start, const fs::path& rootDir) {
    neo::vaultcache::resetForTest();
    const std::string root = neo::textfile::pathToUtf8(rootDir);
    const auto warmSnapshot = warmRootWithFiles(root, {"warm.md", "notes.md"});
    check(warmSnapshot && warmSnapshot->ok, "warm fixture publishes a usable snapshot");
    Scanner scanner;
    neo::vaultcache::setScannerForTest([&scanner](const std::string& value) { return scanner(value); });
    neo::AppState state;
    start(state, root);
    check(scanner.mainCalls == 0, "warm vault action never scans on the caller thread");
    check(state.vaultScan == warmSnapshot && state.rows.size() == 2,
          "warm vault action adopts the cached rows immediately");
    check(scanner.waitForRoot(root), "warm vault action revalidates in the background");
    check(neo::vaultcache::query(root).scanning && neo::vaultcache::stats().scanRequests == 1,
          "warm vault action schedules exactly one background revalidation");
    for (int i = 0; i < 20; ++i) start(state, root);
    check(neo::vaultcache::stats().scanRequests == 1 && !neo::vaultcache::query(root).rescanPending,
          "repeated warm vault action shares the single revalidation");
    scanner.release();
    check(pumpUntil([&] { return !neo::vaultcache::query(root).scanning; }), "warm revalidation drains in bounded time");
    neo::tickVaultScan(state);
    check(state.vaultScan == neo::vaultcache::query(root).scan && state.vaultRowsGeneration == 2,
          "warm vault action publishes exactly one newer snapshot");
    neo::vaultcache::invalidate(root);
}

void lateRootCompletionCannotPollute(VaultStart start, const fs::path& firstDir, const fs::path& secondDir) {
    neo::vaultcache::resetForTest();
    const std::string first = neo::textfile::pathToUtf8(firstDir);
    const std::string second = neo::textfile::pathToUtf8(secondDir);
    Scanner scanner;
    scanner.tagByRootName = true;
    neo::vaultcache::setScannerForTest([&scanner](const std::string& value) { return scanner(value); });
    neo::AppState state;
    start(state, first);
    check(state.vaultRoot == first && scanner.waitForRoot(first), "first root scan reaches the worker");
    start(state, second);
    check(state.vaultRoot == second, "switching root adopts the second root immediately");
    check(scanner.mainCalls == 0, "root switch never scans on the caller thread");
    check(!state.vaultScan && state.rows.empty(), "switched root stays loading while its own scan is pending");
    neo::tickVaultScan(state);
    check(!state.vaultScan && state.rows.empty() && state.vaultRowsGeneration == 0,
          "generation reset cannot adopt the previous root's snapshot");
    scanner.releaseRoot(first);
    check(pumpUntil([&] { return neo::vaultcache::stats().scansDropped >= 1; }),
          "late first-root scan is discarded after cancellation");
    check(neo::vaultcache::query(first).scan == nullptr,
          "late first root cannot repopulate an inactive cache");
    neo::tickVaultScan(state);
    check(state.vaultRoot == second && !state.vaultScan && state.rows.empty(),
          "late completion of the previous root cannot populate the active view");
    check(neo::vaultcache::query(second).scan == nullptr, "second root still has no snapshot");
    scanner.releaseRoot(second);
    check(pumpUntil([&] { return !neo::vaultcache::query(second).scanning; }), "second-root scan completes");
    neo::tickVaultScan(state);
    check(state.vaultScan == neo::vaultcache::query(second).scan &&
              state.vaultScan != neo::vaultcache::query(first).scan,
          "second root adopts only its own snapshot");
    check(!state.rows.empty() && state.rows.front().name == neo::textfile::fileName(state.vaultRoot),
          "rows are projected from the second root, not the late first snapshot");
    neo::vaultcache::invalidate(first);
    neo::vaultcache::invalidate(second);
}

void coldFailureEndsLoading(VaultStart start, const fs::path& rootDir) {
    neo::vaultcache::resetForTest();
    const std::string root = neo::textfile::pathToUtf8(rootDir);
    Scanner scanner;
    scanner.fail = true;
    neo::vaultcache::setScannerForTest([&scanner](const std::string& value) { return scanner(value); });
    neo::AppState state;
    start(state, root);
    check(!state.vaultScan && state.rows.empty() && scanner.mainCalls == 0,
          "cold failure starts in loading without a caller-thread scan");
    check(scanner.waitForRoot(root), "cold failure scan reaches the worker");
    scanner.release();
    check(pumpUntil([&] { return !neo::vaultcache::query(root).scanning; }), "failed cold scan drains in bounded time");
    state.toastVisible = false;
    neo::tickVaultScan(state);
    check(state.vaultScan && !state.vaultScan->ok && state.rows.empty() && state.toastVisible,
          "cold failure ends loading and reports the root error");
    eui::Ui failedUi; failedUi.begin("cold.failed"); neo::vaultPanelView(failedUi, state, 264, 600); failedUi.end();
    check(!failedUi.find("vault.loading"), "cold failure leaves the loading state");
    neo::vaultcache::invalidate(root);
}

void warmFailureEndsLoading(VaultStart start, const fs::path& rootDir) {
    neo::vaultcache::resetForTest();
    const std::string root = neo::textfile::pathToUtf8(rootDir);
    const auto warmSnapshot = warmRootWithFiles(root, {"warm.md"});
    check(warmSnapshot && warmSnapshot->ok, "warm failure fixture publishes a usable snapshot");
    Scanner scanner;
    scanner.fail = true;
    neo::vaultcache::setScannerForTest([&scanner](const std::string& value) { return scanner(value); });
    neo::AppState state;
    start(state, root);
    check(state.vaultScan == warmSnapshot && state.vaultScan->ok,
          "warm snapshot stays usable until the revalidation fails");
    check(scanner.waitForRoot(root), "warm failure revalidation reaches the worker");
    scanner.release();
    check(pumpUntil([&] { return !neo::vaultcache::query(root).scanning; }), "failed warm revalidation drains in bounded time");
    state.toastVisible = false;
    neo::tickVaultScan(state);
    check(state.vaultScan && !state.vaultScan->ok && state.rows.empty() && state.toastVisible,
          "failed warm revalidation ends loading and reports the error");
    eui::Ui failedUi; failedUi.begin("warm.failed"); neo::vaultPanelView(failedUi, state, 264, 600); failedUi.end();
    check(!failedUi.find("vault.loading"), "failed warm revalidation leaves the loading state");
    neo::vaultcache::invalidate(root);
}

void navigateOpensOutsideDocument(const fs::path& base) {
    neo::vaultcache::resetForTest();
    const fs::path homeDir = makeDirectory(base, "nav-out-home");
    const fs::path targetDir = makeDirectory(base, "nav-out-target");
    const fs::path document = targetDir / "later.md";
    put(document, u8"navigate opened bytes 🙂\n");
    const std::string target = neo::textfile::pathToUtf8(targetDir);
    Scanner scanner;
    scanner.addLater = true;  // the deferred target appears once the scan completes
    neo::vaultcache::setScannerForTest([&scanner](const std::string& value) { return scanner(value); });
    neo::AppState state;
    state.vaultRoot = neo::textfile::pathToUtf8(homeDir);
    state.vaultAddress = state.vaultRoot;
    neo::navigateToVaultPath(state, neo::textfile::pathToUtf8(document));
    check(state.vaultRoot == target, "navigate to an outside file switches the root to its directory");
    check(state.path == neo::textfile::pathToUtf8(document) && state.doc.text == u8"navigate opened bytes 🙂\n",
          "navigate opens the outside document");
    check(state.vaultPendingReveal == "later.md", "navigate defers the reveal until rows arrive");
    check(scanner.mainCalls == 0 && !state.vaultScan && state.rows.empty(),
          "outside navigate scans the new root in the background");
    eui::Ui loadingUi; loadingUi.begin("nav.loading"); neo::vaultPanelView(loadingUi, state, 264, 600); loadingUi.end();
    check(loadingUi.find("vault.loading") && state.vaultPendingReveal == "later.md",
          "loading panel preserves the deferred reveal");
    scanner.release();
    check(pumpUntil([&] { return !neo::vaultcache::query(target).scanning; }), "outside navigate scan drains in bounded time");
    neo::tickVaultScan(state);
    eui::Ui readyUi; readyUi.begin("nav.ready"); neo::vaultPanelView(readyUi, state, 264, 600); readyUi.end();
    check(!readyUi.find("vault.loading") && state.vaultPendingReveal.empty(),
          "completed scan consumes the deferred reveal");
    neo::vaultcache::invalidate(target);
}

void navigateWarmMissingTargetClearsReveal(const fs::path& base) {
    neo::vaultcache::resetForTest();
    const fs::path homeDir = makeDirectory(base, "nav-warm-home");
    const fs::path targetDir = makeDirectory(base, "nav-warm-target");
    const fs::path document = targetDir / "appeared.md";
    put(document, "appeared\n");
    const std::string target = neo::textfile::pathToUtf8(targetDir);
    const auto warmSnapshot = warmRootWithFiles(target, {"present.md"});
    check(warmSnapshot && warmSnapshot->ok, "warm navigate fixture publishes a usable snapshot");
    Scanner scanner;  // revalidation returns the default set, still without appeared.md
    neo::vaultcache::setScannerForTest([&scanner](const std::string& value) { return scanner(value); });
    neo::AppState state;
    state.vaultRoot = neo::textfile::pathToUtf8(homeDir);
    state.vaultAddress = state.vaultRoot;
    neo::navigateToVaultPath(state, neo::textfile::pathToUtf8(document));
    check(state.vaultRoot == target && state.vaultScan == warmSnapshot && state.rows.size() == 1,
          "warm navigate adopts the cached rows immediately");
    check(state.vaultPendingReveal == "appeared.md", "a target missing from the warm snapshot keeps its reveal");
    check(scanner.mainCalls == 0, "warm navigate revalidates in the background");
    eui::Ui staleUi; staleUi.begin("nav.stale"); neo::vaultPanelView(staleUi, state, 264, 600); staleUi.end();
    check(state.vaultPendingReveal == "appeared.md", "reveal is preserved while revalidation is in flight");
    scanner.release();
    check(pumpUntil([&] { return !neo::vaultcache::query(target).scanning; }), "warm navigate revalidation drains in bounded time");
    neo::tickVaultScan(state);
    eui::Ui settledUi; settledUi.begin("nav.settled"); neo::vaultPanelView(settledUi, state, 264, 600); settledUi.end();
    check(state.vaultPendingReveal.empty(), "completed scan clears an unmatched warm target");
    neo::vaultcache::invalidate(target);
}

void navigateUnloadedChildDirectly(const fs::path& base) {
    neo::vaultcache::resetForTest();
    const fs::path homeDir = makeDirectory(base, "nav-unloaded-home");
    const fs::path childDir = makeDirectory(homeDir, "unloaded-child");
    const std::string child = neo::textfile::pathToUtf8(childDir);
    Scanner scanner;
    neo::vaultcache::setScannerForTest([&](const std::string& value) { return scanner(value); });
    neo::AppState state;
    state.vaultRoot = neo::textfile::pathToUtf8(homeDir);
    auto partial = std::make_shared<neo::vault::ScanResult>();
    partial->ok = true;
    partial->hasMore = true;
    partial->roots = {{"loaded.md", "loaded.md", false, {}}};
    state.vaultScan = partial;
    state.doc.text = "unsaved draft\n";
    ++state.revision;
    neo::navigateToVaultPath(state, child);
    check(state.vaultRoot == child && !state.vaultScan && state.rows.empty(),
          "address enters unseen child without loading preceding root pages");
    check(scanner.mainCalls == 0 && scanner.waitForRoot(child),
          "unseen child address starts its own background scan");
    check(state.doc.text == "unsaved draft\n" && state.dirty(),
          "direct child navigation preserves unsaved document");
    drain(scanner, child);
    neo::tickVaultScan(state);
    check(state.vaultRoot == child && state.vaultScan && state.vaultScan->ok && scanner.workerCalls == 1,
          "only requested child is scanned and adopted");
}

void collapsedPagesRereadOnlyTheirOwnLevel(const fs::path& base) {
    neo::vaultcache::resetForTest();
    neo::vaultcache::resetScannerForTest();
    const fs::path rootDir = makeDirectory(base, "collapse-pages");
    for (const char* folder : {"first", "sibling", "cancelled"})
        put(rootDir / folder / "initial.md", "initial\n");
    const std::string root = neo::textfile::pathToUtf8(rootDir);
    neo::vaultcache::activateRoot(root);
    neo::vaultcache::requestScanSync(root);
    neo::AppState state;
    state.vaultRoot = root;
    state.vaultScan = neo::vaultcache::query(root).scan;
    state.vaultRowsGeneration = neo::vaultcache::query(root).generation;
    state.doc.text = u8"未保存的正文🙂\n";
    ++state.revision;
    const auto revision = state.revision;
    const auto savedRevision = state.savedRevision;
    const auto text = state.doc.text;
    const auto hasChild = [&](const char* relative, const char* child) {
        const auto scan = neo::vaultcache::query(root).scan;
        if (!scan) return false;
        const auto folder = std::find_if(scan->roots.begin(), scan->roots.end(), [&](const auto& entry) {
            return entry.relative == relative;
        });
        return folder != scan->roots.end() && folder->childrenLoaded &&
            std::any_of(folder->children.begin(), folder->children.end(), [&](const auto& entry) {
                return entry.name == child;
            });
    };
    const auto unloaded = [&](const char* relative) {
        const auto scan = neo::vaultcache::query(root).scan;
        if (!scan) return false;
        const auto folder = std::find_if(scan->roots.begin(), scan->roots.end(), [&](const auto& entry) {
            return entry.relative == relative;
        });
        return folder != scan->roots.end() && !folder->childrenLoaded && folder->children.empty() &&
            !folder->cursor && !folder->hasMore;
    };
    const auto settle = [&] {
        check(pumpUntil([&] { return !neo::vaultcache::query(root).loadingPages; }),
              "real child-page request drains in bounded time");
        neo::tickVaultScan(state);
    };
    neo::toggleFolder(state, "first"); settle();
    neo::toggleFolder(state, "sibling"); settle();
    check(hasChild("first", "initial.md") && hasChild("sibling", "initial.md"),
          "explicit expansion loads both direct child levels");

    put(rootDir / "first" / "external.md", "external\n");
    put(rootDir / "sibling" / "unseen.md", "unseen\n");
    check(neo::vaultcache::requestPage(root, "first", true).scheduled,
          "a targeted first-level refresh is scheduled");
    settle();
    check(hasChild("first", "external.md") && !hasChild("sibling", "unseen.md") &&
          hasChild("sibling", "initial.md"),
          "refresh adopts external changes only at its direct parent level");

    neo::toggleFolder(state, "first");
    check(unloaded("first") && hasChild("sibling", "initial.md"),
          "collapse releases the selected child cache while retaining its expanded sibling");
    put(rootDir / "first" / "after-collapse.md", "later\n");
    neo::toggleFolder(state, "first"); settle();
    check(hasChild("first", "after-collapse.md") && !hasChild("sibling", "unseen.md"),
          "re-expansion rereads external child changes without refreshing siblings");

    // Completion publication runs on this thread. Collapse before pumping it, so
    // even a worker which already finished IO must not resurrect the hidden page.
    const auto droppedBefore = neo::vaultcache::stats().scansDropped;
    neo::toggleFolder(state, "cancelled");
    check(neo::vaultcache::query(root).loadingPages, "new child request is pending before collapse");
    neo::toggleFolder(state, "cancelled");
    check(pumpUntil([&] { return neo::vaultcache::stats().scansDropped > droppedBefore; }),
          "late child completion after collapse is explicitly discarded");
    neo::tickVaultScan(state);
    check(unloaded("cancelled") && !state.expanded.count("cancelled"),
          "discarded completion cannot restore a collapsed child cache");
    check(state.doc.text == text && state.revision == revision &&
          state.savedRevision == savedRevision && state.dirty(),
          "page refresh, collapse and cancellation preserve dirty document state");
    neo::vaultcache::invalidate(root);
}

void vaultActionCancellationPreservesDocument(const fs::path& base) {
    neo::vaultcache::resetForTest();
    const fs::path homeDir = makeDirectory(base, "nav-cancel-home");
    const std::string home = neo::textfile::pathToUtf8(homeDir);
    neo::AppState state;
    state.vaultRoot = home;
    state.vaultAddress = home;
    state.doc.text = "in progress\n";
    ++state.revision;  // unsaved
    neo::navigateToVaultPath(state, neo::textfile::pathToUtf8(homeDir / "ghost"));
    check(state.vaultRoot == home && state.doc.text == "in progress\n" && state.dirty(),
          "navigating to a missing path keeps the current root and document");
    check(state.toastVisible, "navigating to a missing path reports a toast");
    const int picksBefore = directoryPickCalls;
    nextDirectoryPick = {false, true, {}, {}};
    neo::chooseVaultDirectory(state);
    check(directoryPickCalls == picksBefore + 1 && state.vaultRoot == home &&
              state.doc.text == "in progress\n" && state.dirty(),
          "a cancelled chooser keeps the current root and document");
}
}

int main() {
    const auto base = fs::temp_directory_path() /
        ("neo-vault-loading-" + std::to_string(Clock::now().time_since_epoch().count()));
    fs::create_directories(base);
    sessionConfigDirectory = neo::textfile::pathToUtf8(base / "profile");
    neo::sessionwriter::resetForTest(); neo::sessionwriter::setInlineForTest(true);
    neo::i18n::initialize("en"); neo::vaultcache::resetForTest();
    const auto file = base / fs::u8path(u8"子目录/当前.md");
    put(file, u8"正文中🙂\n");
    const auto firstFile = base / fs::u8path(u8"当前.md");
    put(firstFile, u8"正文中🙂\n");
    const auto root = neo::textfile::pathToUtf8(base);
    {
        Scanner scanner;
        neo::vaultcache::setScannerForTest([&](const std::string& value) { return scanner(value); });
        neo::AppState state;
        // Startup ultimately enters this real document load path.
        const auto begin = Clock::now();
        check(neo::loadDocument(state, neo::textfile::pathToUtf8(firstFile)), "new-root document load returns while scan is gated");
        neo::prepareVault(state, true); // restoreSession prepares the final root again.
        check(neo::loadDocument(state, neo::textfile::pathToUtf8(file)), "document opens while scan is gated");
        const auto ms = std::chrono::duration<double, std::milli>(Clock::now()-begin).count();
        check(state.doc.text == u8"正文中🙂\n", "opened document bytes remain exact");
        check(scanner.mainCalls == 0, "startup preparation never scans on caller thread");
        check(!state.vaultScan && state.rows.empty(), "cold root is loading, without placeholder or stale rows");
        eui::Ui loadingUi; loadingUi.begin("loading");
        neo::vaultPanelView(loadingUi, state, 264, 600); loadingUi.end();
        check(loadingUi.find("vault.loading") && state.vaultPendingReveal == u8"子目录/当前.md",
              "loading panel preserves deferred document reveal until rows arrive");
        for (int i=0; i<20; ++i) { neo::prepareVault(state, true); neo::tickVaultScan(state); }
        check(!neo::vaultcache::query(root).rescanPending, "repeated preparation does not enqueue a redundant rescan");
        drain(scanner, root);
        check(neo::vaultcache::stats().scanRequests == 1 && scanner.workerCalls == 1,
              "cold preparation shares one in-flight task");
        neo::tickVaultScan(state);
        check(state.vaultScan && state.vaultRowsGeneration == 1 && state.rows.size() == 5,
              "tick adopts the first snapshot and opens current document ancestor chain");
        check(state.expanded.count(u8"子目录") == 1, "Unicode ancestor expands after first scan");
        eui::Ui readyUi; readyUi.begin("ready");
        neo::vaultPanelView(readyUi, state, 264, 600); readyUi.end();
        check(!readyUi.find("vault.loading") && state.vaultPendingReveal.empty(), "ready panel consumes deferred reveal");
        state.vaultPathInputMode = false; state.filter = "noext"; neo::rebuildRows(state);
        check(state.rows.size() == 1 && state.rows[0].relative == "noext", "no-extension filter is complete");
        auto snapshot = state.vaultScan;
        state.vaultScroll = 42;
        neo::tickVaultScan(state);
        check(state.vaultScan == snapshot && state.vaultScroll == 42 && state.rows.size() == 1,
              "unchanged generation preserves page filter and scroll");
        std::cout << "cold_prepare_ms=" << ms << " caller_scans=" << scanner.mainCalls
                  << " worker_scans=" << scanner.workerCalls << '\n';
    }
    {
        Scanner scanner;
        scanner.addLater = true;
        neo::vaultcache::setScannerForTest([&](const std::string& value) { return scanner(value); });
        neo::AppState state; state.vaultRoot = root; state.path = neo::textfile::pathToUtf8(file);
        auto oldSnapshot = neo::vaultcache::query(root).scan;
        neo::prepareVault(state, true);
        check(state.vaultScan == oldSnapshot && state.rows.size() == 5,
              "warm open reuses cached rows immediately while revalidating");
        neo::prepareVault(state, true);
        check(!neo::vaultcache::query(root).rescanPending, "warm repeated preparation shares revalidation task");
        state.vaultPendingReveal = "later.md";
        eui::Ui staleUi; staleUi.begin("stale"); neo::vaultPanelView(staleUi, state, 264, 600); staleUi.end();
        check(state.vaultPendingReveal == "later.md", "warm snapshot missing target preserves reveal during revalidation");
        drain(scanner, root); neo::tickVaultScan(state);
        check(scanner.mainCalls == 0 && scanner.workerCalls == 1 && state.vaultRowsGeneration == 2,
              "warm revalidation publishes exactly one newer snapshot");
        eui::Ui freshUi; freshUi.begin("fresh"); neo::vaultPanelView(freshUi, state, 264, 600); freshUi.end();
        check(state.vaultPendingReveal.empty() && state.vaultScroll > 0,
              "new warm snapshot reveals newly appeared target");
        state.vaultPendingReveal = "never-existed.md";
        eui::Ui missingUi; missingUi.begin("missing"); neo::vaultPanelView(missingUi, state, 264, 600); missingUi.end();
        check(state.vaultPendingReveal.empty(), "completed scan clears missing target instead of retaining forever");
    }
    {
        Scanner scanner; scanner.fail = true;
        neo::vaultcache::setScannerForTest([&](const std::string& value) { return scanner(value); });
        neo::AppState state; state.vaultRoot = root;
        neo::prepareVault(state, true);
        check(state.vaultScan && state.vaultScan->ok, "warm error revalidation initially retains usable snapshot");
        drain(scanner, root); neo::tickVaultScan(state);
        check(state.vaultScan && !state.vaultScan->ok && state.rows.empty() && state.toastVisible,
              "warm failed revalidation replaces stale rows with an explicit error");
        const auto otherRoot = neo::textfile::pathToUtf8(base / "other");
        fs::create_directory(base / "other");
        state.vaultRoot = otherRoot; state.vaultScan.reset(); state.vaultRowsGeneration = 0;
        neo::prepareVault(state, true);
        check(state.rows.empty() && !state.vaultScan, "cold replacement root clears previous root rows");
        drain(scanner, otherRoot); neo::tickVaultScan(state);
        check(state.vaultScan && !state.vaultScan->ok && state.rows.empty() && state.toastVisible,
              "failed first scan leaves loading state and exposes the root error");
        state.toastVisible = false; neo::tickVaultScan(state);
        check(!state.toastVisible, "unchanged error snapshot does not toast every frame");
        state.vaultRoot.clear(); neo::prepareVault(state, true);
        check(!state.vaultScan && state.rows.empty() && state.vaultRowsGeneration == 0, "empty root clears view");
    }
    {
        neo::vaultcache::clear();
        const auto oldFile = base / "old" / "CURRENT.md";
        const auto newFile = base / "new" / "CURRENT.md";
        put(oldFile, "old document\n"); put(newFile, "new document\n");
        const auto oldRoot = neo::textfile::pathToUtf8(oldFile.parent_path());
        const auto newRoot = neo::textfile::pathToUtf8(newFile.parent_path());
        Scanner oldScanner, newScanner;
        neo::vaultcache::setScannerForTest([&](const std::string& value) {
            return value == oldRoot ? oldScanner(value) : newScanner(value);
        });
        neo::AppState state;
        check(neo::loadDocument(state, neo::textfile::pathToUtf8(oldFile)), "load old root with scan in flight");
        state.expanded.insert("old-expanded"); state.filter = "old-filter";
        check(neo::loadDocument(state, neo::textfile::pathToUtf8(newFile)), "load new root before old scan finishes");
        check(state.vaultRoot == newRoot && state.filter.empty() && !state.expanded.count("old-expanded"),
              "actual follow-document switch resets old projection state");
        drain(oldScanner, oldRoot); neo::tickVaultScan(state);
        check(state.vaultRoot == newRoot && !state.vaultScan && state.rows.empty() && state.doc.text == "new document\n",
              "late old-root completion cannot populate active new-root view");
        drain(newScanner, newRoot); neo::tickVaultScan(state);
        check(state.vaultScan == neo::vaultcache::query(newRoot).scan && state.vaultScan != neo::vaultcache::query(oldRoot).scan,
              "new root adopts its own snapshot after actual document switch");
        check(oldScanner.mainCalls == 0 && newScanner.mainCalls == 0,
              "neither actual root switch scans on caller thread");
    }
    {
        // chooseVaultDirectory 与 navigateToVaultPath 是唯一改走 prepareVault 的两个
        // 入口：同一套有界场景对两者各跑一遍，避免重复。旧同步实现对每条断言都反向。
        const fs::path actionBase = base / "actions";
        fs::create_directories(actionBase);
        const VaultStart actions[] = {&startChooseVault, &startNavigateVault};
        const char* const kinds[] = {"choose", "navigate"};
        for (std::size_t a = 0; a < 2; ++a) {
            const std::string prefix = std::string("act-") + kinds[a] + "-";
            coldVaultActionInBackground(actions[a], makeDirectory(actionBase, (prefix + "cold").c_str()));
            warmVaultActionAdoptsSnapshot(actions[a], makeDirectory(actionBase, (prefix + "warm").c_str()));
            lateRootCompletionCannotPollute(actions[a],
                makeDirectory(actionBase, (prefix + "late-a").c_str()),
                makeDirectory(actionBase, (prefix + "late-b").c_str()));
            coldFailureEndsLoading(actions[a], makeDirectory(actionBase, (prefix + "fail-cold").c_str()));
            warmFailureEndsLoading(actions[a], makeDirectory(actionBase, (prefix + "fail-warm").c_str()));
        }
        parentVaultActionIsAsync(actionBase);
        const VaultStart editActions[] = {&startChooseVault, &startNavigateVault, &startParentVaultForEdit};
        for (std::size_t action = 0; action < 3; ++action)
            for (int warm = 0; warm < 2; ++warm)
                for (int fail = 0; fail < 2; ++fail) {
                    const auto name = "edit-save-" + std::to_string(action) + "-" + std::to_string(warm) + "-" + std::to_string(fail);
                    editAndSaveDuringScan(editActions[action], makeDirectory(actionBase, name.c_str()), warm != 0, fail != 0);
                }
        warmParentVaultActionReusesSnapshot(actionBase);
        parentVaultFailuresEndLoading(actionBase);
        consecutiveParentScansKeepActiveRoot(actionBase);
        navigateOpensOutsideDocument(actionBase);
        navigateWarmMissingTargetClearsReveal(actionBase);
        navigateUnloadedChildDirectly(actionBase);
        vaultActionCancellationPreservesDocument(actionBase);
        collapsedPagesRereadOnlyTheirOwnLevel(actionBase);
    }
    neo::vaultcache::resetScannerForTest();
    core::async::shutdown(); neo::vaultcache::resetForTest();
    const auto relative = fs::absolute(base).lexically_relative(fs::absolute(fs::temp_directory_path()));
    if (!relative.empty() && !relative.is_absolute() && *relative.begin() != ".." &&
        base.filename().string().rfind("neo-vault-loading-", 0) == 0) fs::remove_all(base);
    std::cout << (failures ? "FAIL" : "PASS") << ": document_vault_loading (" << failures << " failures)\n";
    return failures ? 1 : 0;
}
