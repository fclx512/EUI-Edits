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
struct Scanner {
    std::thread::id mainThread = std::this_thread::get_id();
    std::atomic<int> mainCalls{0}, workerCalls{0};
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;
    bool fail = false;
    bool addLater = false;
    neo::vault::ScanResult operator()(const std::string&) {
        if (std::this_thread::get_id() == mainThread) {
            ++mainCalls;
            // A finite negative-control delay: the old path must fail assertions,
            // never deadlock waiting for a release on its own caller thread.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        } else {
            ++workerCalls;
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return released; });
        }
        neo::vault::ScanResult result;
        result.ok = !fail;
        if (fail) { result.error = "test root unreadable"; return result; }
        result.fileCount = 4; result.directoryCount = 1;
        result.roots = {{u8"子目录", u8"子目录", true,
                         {{u8"当前.md", u8"子目录/当前.md", false, {}}}},
                        {".hidden", ".hidden", false, {}}, {"noext", "noext", false, {}},
                        {u8"当前.md", u8"当前.md", false, {}}};
        if (addLater) { result.roots.push_back({"later.md", "later.md", false, {}}); ++result.fileCount; }
        return result;
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true; cv.notify_all();
    }
};
void drain(Scanner& scanner, const std::string& root) {
    scanner.release();
    check(pumpUntil([&] { return !neo::vaultcache::query(root).scanning; }), "scan drains before scanner destruction");
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
    neo::vaultcache::resetScannerForTest();
    core::async::shutdown(); neo::vaultcache::resetForTest();
    const auto relative = fs::absolute(base).lexically_relative(fs::absolute(fs::temp_directory_path()));
    if (!relative.empty() && !relative.is_absolute() && *relative.begin() != ".." &&
        base.filename().string().rfind("neo-vault-loading-", 0) == 0) fs::remove_all(base);
    std::cout << (failures ? "FAIL" : "PASS") << ": document_vault_loading (" << failures << " failures)\n";
    return failures ? 1 : 0;
}
