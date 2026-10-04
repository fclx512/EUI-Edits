#include "core/platform/platform.h"
#include "core/platform/tray_bridge.h"
#include "core/render/image_source.h"
#include "core/render/text.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace fs = std::filesystem;
namespace {
std::string trayPath;
bool trayReady = false;
fs::path executable() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return length && length < buffer.size() ? fs::path(std::wstring(buffer.data(), length)) : fs::path{};
}
int childCheck() {
    const auto expected = executable().parent_path() / "assets";
    const auto resource = core::platform::resolveResourcePath("assets/neo-portable-probe.svg");
    if (fs::u8path(resource) != expected / "neo-portable-probe.svg") return 10;
    bool pending = true;
    const auto image = core::render::image::resolveImagePath("neo-portable-probe.svg", &pending);
    if (pending || fs::u8path(image) != expected / "neo-portable-probe.svg") return 11;
    const auto decoded = core::render::image::loadStaticImageFromPath(image, false);
    // SVG 按渲染目标光栅化（见 rasterizeSvgString），不是固有的 2×3；
    // 这里只验证解码成功且尺寸有效。
    if (!decoded || decoded->width <= 0 || decoded->height <= 0 ||
        decoded->byteCount < static_cast<std::size_t>(decoded->width) * decoded->height * 4u) return 12;
    // Stub only the native tray boundary; exercise the real icon resolver and UTF-8 handoff.
    if (!core::platform::initializeTray({"probe", "neo-portable-probe.png"}) ||
        fs::u8path(trayPath) != expected / "neo-portable-probe.ico") return 13;
    core::platform::shutdownTray();
    const auto absolute = core::TextPrimitive::measureTextWidth("portable WiWi 123", (expected / "neo-portable-probe.ttf").u8string(), 17);
    const auto relative = core::TextPrimitive::measureTextWidth("portable WiWi 123", "neo-portable-probe.ttf", 17);
    if (!(absolute > 0) || std::fabs(absolute-relative) > .001f) return 14;
    const auto italic = core::TextPrimitive::resolveItalicFontPath("neo-portable-probe.ttf");
    if (fs::u8path(italic) != expected / "neo-portable-probe-Italic.ttf") return 15;
    const auto bold = core::TextPrimitive::measureTextWidth("portable WiWi 123", "neo-portable-probe.ttf", 17, 700);
    const auto explicitBold = core::TextPrimitive::measureTextWidth("portable WiWi 123", (expected / "neo-portable-probe-Bold.ttf").u8string(), 17);
    if (!(bold > 0) || std::fabs(bold-explicitBold) > .001f) return 16;
    // An existing but invalid font takes the fallback fingerprint path, unlike
    // the loaded-font case above. Exercise first load and cached fallback.
    const auto invalid = (expected / "invalid-font.ttf").u8string();
    const auto fallback = core::TextPrimitive::measureTextWidth("portable fallback", invalid, 17);
    const auto cachedFallback = core::TextPrimitive::measureTextWidth("portable fallback", invalid, 17);
    if (!(fallback > 0) || std::fabs(fallback-cachedFallback) > .001f) return 17;
    // Default UI/icon lookup rechecks resolved UTF-8 strings. Include that
    // boundary, not only the explicit file-family resolver exercised above.
    core::TextPrimitive::setDefaultFontFiles((expected / "neo-portable-probe.ttf").u8string(),
                                           (expected / "neo-portable-probe.ttf").u8string());
    const auto ui = core::TextPrimitive::measureTextWidth("portable WiWi 123", "", 17);
    const auto icon = core::TextPrimitive::measureTextWidth("portable WiWi 123", "FontAwesome", 17);
    if (std::fabs(ui-absolute) > .001f || std::fabs(icon-absolute) > .001f) return 18;
    return 0;
}
}
// No shell registration, tray window, or user's configuration is touched.
extern "C" int eui_tray_init(const char* path) { trayPath = path ? path : ""; trayReady = true; return 1; }
extern "C" int eui_tray_is_initialized() { return trayReady; }
extern "C" void eui_tray_poll(int) {}
extern "C" int eui_tray_consume_show_requested() { return 0; }
extern "C" int eui_tray_consume_exit_requested() { return 0; }
extern "C" void eui_tray_shutdown() { trayReady = false; }
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
    if (argc == 2 && std::string(argv[1]) == "--child") return childCheck();
    // Emoji forces a path that cannot round-trip through the legacy ANSI APIs,
    // even on a machine whose ANSI code page already handles the Chinese part.
    const auto root = fs::temp_directory_path() / fs::u8path(u8"neo-portable 中文 space 😀-") /
        (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    const auto assets = root / "assets", child = root / "portable_resource_paths.exe";
    fs::create_directories(assets);
    fs::copy_file(executable(), child);
    std::ofstream(assets / "neo-portable-probe.svg") <<
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"2\" height=\"3\"><rect width=\"2\" height=\"3\" fill=\"#123456\"/></svg>";
    std::ofstream(assets / "neo-portable-probe.ico") << "icon resolver fixture";
    wchar_t windowsPath[32768]{};
    const auto count = GetWindowsDirectoryW(windowsPath, 32768);
    if (!count || count >= 32768) return 20;
    const auto fonts = fs::path(std::wstring(windowsPath, count)) / "Fonts";
    fs::copy_file(fonts / "consola.ttf", assets / "neo-portable-probe.ttf");
    fs::copy_file(fonts / "consolai.ttf", assets / "neo-portable-probe-Italic.ttf");
    fs::copy_file(fonts / "consolab.ttf", assets / "neo-portable-probe-Bold.ttf");
    std::ofstream(assets / "invalid-font.ttf") << "invalid font fixture";
    // Launch with the original working directory, so package-relative resolution
    // cannot accidentally pass by finding assets in the current directory.
    std::wstring command = L"\"" + child.wstring() + L"\" --child";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(child.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) return 21;
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    DWORD result = 22;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &result);
    else TerminateProcess(process.hProcess, result);
    CloseHandle(process.hProcess);
    // Delete only this uniquely created, known absolute fixture subtree.
    std::error_code error;
    fs::remove_all(root, error);
    if (result) std::cerr << "Unicode portable resource child failed: " << result << '\n';
    else std::cout << "Unicode/space executable path, image decode, font resolution and icon handoff passed\n";
    return static_cast<int>(result);
#else
    (void)argc; (void)argv;
    return 0;
#endif
}
