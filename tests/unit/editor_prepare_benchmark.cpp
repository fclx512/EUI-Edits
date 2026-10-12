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

#include "ui/overlays.h"
#include <iomanip>
using BenchClock = std::chrono::steady_clock;
int runBench(int argc, char** argv) {
    if (argc != 1 && (argc < 4 || argc > 6)) return 2;
    const auto root = argc == 1 ? fs::current_path() : fs::absolute(neo::textfile::pathFromUtf8(argv[1]));
    const int count = argc == 1 ? 32 : std::stoi(argv[2]);
    if(count < 1 || count > 1024) return 2;
    const bool animations = argc > 3 && std::string(argv[3]) == "on";
    const int width = argc > 4 ? std::stoi(argv[4]) : 1120;
    const bool unique = argc > 5 && std::string(argv[5]) == "unique";
    neo::i18n::initialize("en");
    neo::AppState state;
    state.animations = animations;
    state.vaultRoot = neo::textfile::pathToUtf8(root);
    state.tabOrder.clear();
    for (int i=0; i<count; ++i) {
        const std::string filename=unique?"note-"+std::to_string(i)+".md":"note.md";
        neo::DocumentSession doc;
        doc.tabId = i+1;
        doc.path = neo::textfile::pathToUtf8(root/ ("group-"+std::to_string(i))/filename);
        doc.vaultRoot = state.vaultRoot;
        doc.doc.text = "## 小文档🙂\n\n[link](../group-0/note.md)\n";
        state.tabOrder.push_back(doc.tabId);
        if (i==0) static_cast<neo::DocumentSession&>(state) = std::move(doc);
        else state.inactiveTabs.emplace(doc.tabId,std::move(doc));
        state.rows.push_back({filename, "group-"+std::to_string(i)+"/"+filename,0,false,false});
    }
    eui::Ui ui;
    const eui::Screen screen{static_cast<float>(width),790};
    std::cout<<std::fixed<<std::setprecision(3);
    for (int round=-1;round<3;++round) {
        double language=0,vault=0,tabs=0,total=0;
        const int iterations=round<0?5:40;
        for (int i=0;i<iterations;++i) {
            auto t0=BenchClock::now();
            ui.begin("prepare-bench");
            auto t1=BenchClock::now();
            neo::languageOverlay(ui,state,screen);
            auto t2=BenchClock::now();
            neo::vaultPanelView(ui,state,264,730);
            auto t3=BenchClock::now();
            neo::tabBarView(ui,state,screen,150,screen.width-170);
            auto t4=BenchClock::now();
            neo::applyInteractionDefaults(ui,neo::uiFontFamily(state),animations);
            ui.end(); ui.layout(screen);
            auto t5=BenchClock::now();
            const auto us=[](auto a,auto b){return std::chrono::duration<double,std::micro>(b-a).count();};
            language+=us(t1,t2);vault+=us(t2,t3);tabs+=us(t3,t4);total+=us(t0,t5);
            if(state.inactiveTabs.size()!=static_cast<std::size_t>(count-1)) return 3;
        }
        if(round>=0) std::cout<<"{\"tabs\":"<<count<<",\"animations\":"<<(animations?"true":"false")
            <<",\"width\":"<<width<<",\"unique\":"<<(unique?"true":"false")<<",\"round\":"<<round<<",\"iterations\":"<<iterations
            <<",\"language_us\":"<<language/iterations<<",\"vault_us\":"<<vault/iterations
            <<",\"tabs_us\":"<<tabs/iterations<<",\"total_us\":"<<total/iterations<<"}\n";
    }
    return 0;
}

int main(int argc, char** argv) { try { return runBench(argc, argv); } catch(const std::exception& e) { std::cerr << e.what() << "\n"; return 4; } }
