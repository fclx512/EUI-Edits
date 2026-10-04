#include "model/atomic_write.h"
#include "model/settings.h"
#include "model/text_file.h"
#include "platform/font_safety.h"
#include "model/i18n.h"
#include "core/render/text.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[font_safety] FAIL: " << message << "\n";
        ++g_failures;
    }
}

std::string readBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool writeBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    return output.good();
}

class IsolatedConfig {
public:
    IsolatedConfig() {
#if defined(_WIN32)
        DWORD needed = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
        if (needed == 0) {
            hadOriginal_ = GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        } else {
            std::vector<wchar_t> original(needed);
            const DWORD copied = GetEnvironmentVariableW(L"APPDATA", original.data(), needed);
            if (copied >= needed) {
                return;
            }
            oldWindowsValue_.assign(original.data(), copied);
            hadOriginal_ = true;
        }
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() /
                (L"EUI-Edits_字体安全_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                 std::to_wstring(suffix));
        std::error_code ec;
        fs::create_directories(root_, ec);
        if (ec) {
            return;
        }
        ready_ = SetEnvironmentVariableW(L"APPDATA", root_.c_str()) != 0;
        environmentChanged_ = ready_;
#else
        const char* original = std::getenv("XDG_CONFIG_HOME");
        if (original != nullptr) {
            hadOriginal_ = true;
            oldPosixValue_ = original;
        }
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() /
                ("EUI-Edits_font_safety_" + std::to_string(static_cast<long long>(::getpid())) +
                 "_" + std::to_string(suffix));
        std::error_code ec;
        fs::create_directories(root_, ec);
        if (ec) {
            return;
        }
        ready_ = setenv("XDG_CONFIG_HOME", root_.string().c_str(), 1) == 0;
        environmentChanged_ = ready_;
#endif
    }

    ~IsolatedConfig() {
        restore();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    bool ready() const { return ready_; }
    const fs::path& root() const { return root_; }

private:
    void restore() {
        if (restored_) {
            return;
        }
        if (!environmentChanged_) {
            restored_ = true;
            return;
        }
#if defined(_WIN32)
        SetEnvironmentVariableW(L"APPDATA", hadOriginal_ ? oldWindowsValue_.c_str() : nullptr);
#else
        if (hadOriginal_) {
            setenv("XDG_CONFIG_HOME", oldPosixValue_.c_str(), 1);
        } else {
            unsetenv("XDG_CONFIG_HOME");
        }
#endif
        restored_ = true;
    }

    fs::path root_;
    bool ready_ = false;
    bool hadOriginal_ = false;
    bool restored_ = false;
    bool environmentChanged_ = false;
#if defined(_WIN32)
    std::wstring oldWindowsValue_;
#else
    std::string oldPosixValue_;
#endif
};

bool exerciseMarkerAndRecovery() {
    using neo::fontsafety::testing::SessionFonts;
    using neo::fontsafety::testing::decodeSessionMarker;
    using neo::fontsafety::testing::encodeSessionMarker;

    const SessionFonts oddPaths{"C:/字体/编辑\n体.ttf", "D:/UI\n字体.otf", "E:/code\\Mono.ttf"};
    SessionFonts roundTrip;
    check(decodeSessionMarker(encodeSessionMarker(oddPaths), roundTrip),
          "长度前缀 marker 应接受含换行和反斜杠的路径");
    check(roundTrip.editor == oddPaths.editor && roundTrip.ui == oddPaths.ui &&
              roundTrip.code == oddPaths.code,
          "marker 编解码应逐字节保留三个路径");
    check(!decodeSessionMarker("neo-font-session-v1\n999999999999999999999999\n", roundTrip),
          "损坏的超长 marker 长度应被拒绝");

    auto& data = neo::settings::current();
    data.editorFontFile = "C:/fonts/editor.ttf";
    data.uiFontFile = "C:/fonts/ui.ttf";
    data.codeFontFile = "C:/fonts/code.ttf";
    std::string error;
    check(neo::fontsafety::armSession(data, error), "应能原子创建字体会话保护记录");

    const fs::path markerPath =
        neo::textfile::pathFromUtf8(neo::settings::configDirectory()) / "font-session.txt";
    check(fs::is_regular_file(markerPath), "armSession 后 marker 文件应存在");
    const std::string originalMarker = readBytes(markerPath);
    check(!originalMarker.empty(), "marker 文件应含三个自选字体路径");

    // 模拟用户只改过 code 字体；崩溃恢复只能清掉仍与旧会话完全一致的字段。
    data.codeFontFile = "C:/fonts/new-code.ttf";
    std::string message;
    check(neo::fontsafety::recoverSession(data, message), "旧字体会话 marker 应成功恢复并消费");
    check(data.editorFontFile.empty() && data.uiFontFile.empty(),
          "与旧 marker 匹配的正文和界面字体应回退为空预设");
    check(data.codeFontFile == "C:/fonts/new-code.ttf",
          "用户后来修改的代码字体必须保留");
    check(!fs::exists(markerPath), "恢复成功后 marker 应被消费");
    check(message == neo::i18n::tr("font.recovered"),
          "发生字体回退时应提供可展示的恢复说明");

    data.editorFontFile = "C:/fonts/armed-old.ttf";
    data.uiFontFile.clear();
    data.codeFontFile = "C:/fonts/armed-code.ttf";
    check(neo::fontsafety::armSession(data, error), "应能再次创建原子保护记录");
    const std::string armedBytes = readBytes(markerPath);
    neo::settings::Data attempted = data;
    attempted.editorFontFile = "C:/fonts/armed-new.ttf";
    neo::atomicwrite::testing::failBeforeReplace(true);
    const bool armedDuringFailure = neo::fontsafety::armSession(attempted, error);
    neo::atomicwrite::testing::failBeforeReplace(false);
    check(!armedDuringFailure, "原子写失败时 armSession 必须拒绝激活");
    check(data.editorFontFile == "C:/fonts/armed-old.ttf",
          "保护记录写失败时当前已激活的字体设置应保持不变");
    check(readBytes(markerPath) == armedBytes,
          "原子写失败必须保留先前 marker 的原始字节");

    neo::atomicwrite::testing::failBeforeReplace(true);
    check(!neo::fontsafety::recoverSession(data, message),
          "恢复设置写入失败必须报告失败");
    check(data.editorFontFile.empty() && data.codeFontFile.empty(),
          "恢复写入失败仍必须在本次会话使用默认字体");
    check(!neo::fontsafety::armSession(data, error),
          "启动后续 arm 不能覆盖待保存的恢复记录");
    neo::fontsafety::cleanSession();
    check(readBytes(markerPath) == armedBytes,
          "恢复写入失败后的正常关窗不能删除恢复记录");
    neo::atomicwrite::testing::failBeforeReplace(false);
    check(neo::fontsafety::recoverSession(data, message),
          "写入恢复后应允许重试保存并消费记录");

    check(neo::fontsafety::armSession(attempted, error),
          "恢复保存成功后应能重新创建字体保护记录");

    neo::fontsafety::cleanSession();
    check(!fs::exists(markerPath), "cleanSession 应删除当前进程的保护记录");
    data.editorFontFile.clear();
    data.uiFontFile.clear();
    data.codeFontFile.clear();
    check(neo::fontsafety::armSession(data, error), "空字体设置应能清除保护记录");
    check(!fs::exists(markerPath), "全空字体设置不应留下保护记录");
    return true;
}

#if defined(_WIN32)
std::wstring currentExecutablePath() {
    std::vector<wchar_t> buffer(512u);
    for (;;) {
        const DWORD size = GetModuleFileNameW(nullptr, buffer.data(),
                                              static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            return {};
        }
        if (size < buffer.size() - 1u) {
            return std::wstring(buffer.data(), size);
        }
        if (buffer.size() >= 32768u) {
            return {};
        }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2u, 32768u));
    }
}

std::size_t processCountForImage(const std::wstring& image) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::size_t count = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process == nullptr) {
                continue;
            }
            std::vector<wchar_t> path(32768u);
            DWORD length = static_cast<DWORD>(path.size());
            const BOOL queried = QueryFullProcessImageNameW(process, 0, path.data(), &length);
            CloseHandle(process);
            if (queried && CompareStringOrdinal(image.c_str(), static_cast<int>(image.size()),
                                                path.data(), static_cast<int>(length), TRUE) == CSTR_EQUAL) {
                ++count;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

void exerciseWindowsFontProbe(const fs::path& root) {
    const fs::path consola = L"C:/Windows/Fonts/consola.ttf";
    const fs::path arial = L"C:/Windows/Fonts/arial.ttf";
    check(fs::is_regular_file(consola), "Windows Consolas fixture must exist");
    check(fs::is_regular_file(arial), "Windows Arial fixture must exist");
    if (!fs::is_regular_file(consola) || !fs::is_regular_file(arial)) {
        return;
    }

    const fs::path fontDirectory = root / L"中文字体路径";
    std::error_code ec;
    fs::create_directories(fontDirectory, ec);
    check(!ec, "应能创建含中文字符的字体测试目录");
    if (ec) {
        return;
    }
    const fs::path monoCopy = fontDirectory / L"Consolas 副本.ttf";
    const fs::path proportionalCopy = fontDirectory / L"Arial 副本.ttf";
    check(fs::copy_file(consola, monoCopy, fs::copy_options::overwrite_existing, ec) && !ec,
          "应能复制 Consolas 到中文路径");
    ec.clear();
    check(fs::copy_file(arial, proportionalCopy, fs::copy_options::overwrite_existing, ec) && !ec,
          "应能复制 Arial 到中文路径");
    if (!fs::exists(monoCopy) || !fs::exists(proportionalCopy)) {
        return;
    }

    const fs::path timeoutCopy = fontDirectory / L"Consolas timeout copy.ttf";
    ec.clear();
    check(fs::copy_file(consola, timeoutCopy, fs::copy_options::overwrite_existing, ec) && !ec,
          "应能准备未缓存的超时字体副本");
    if (fs::exists(timeoutCopy)) {
        const std::wstring image = currentExecutablePath();
        const std::size_t baseline = image.empty() ? 0u : processCountForImage(image);
        const neo::fontsafety::ProbeResult timed =
            neo::fontsafety::validate(neo::textfile::pathToUtf8(timeoutCopy), 1u);
        check(!timed.ok && timed.error == neo::i18n::tr("font.timeout"),
              "冷路径 1ms 探测应超时并报告终止 helper");
        if (!image.empty() && baseline != 0u) {
            bool helperGone = false;
            for (int attempt = 0; attempt < 40; ++attempt) {
                if (processCountForImage(image) <= baseline) {
                    helperGone = true;
                    break;
                }
                Sleep(25u);
            }
            check(helperGone, "超时后 helper 进程应已回收");
        }
        const neo::fontsafety::ProbeResult afterTimeout =
            neo::fontsafety::validate(neo::textfile::pathToUtf8(timeoutCopy), 4000u);
        check(afterTimeout.ok && afterTimeout.monospace,
              "超时结果不能缓存；同一字体随后应能重新通过");
    }

    const neo::fontsafety::ProbeResult mono =
        neo::fontsafety::validate(neo::textfile::pathToUtf8(monoCopy));
    check(mono.ok && mono.monospace,
          "中文路径中的 Consolas 应通过并识别为等宽字体");
    const std::string metricSample = "iiiiWWWW0000{};";
    for (const float size : {12.0f, 20.0f, 32.0f}) {
        const float nativeWidth = core::TextPrimitive::measureTextWidth(metricSample,
            neo::textfile::pathToUtf8(consola), size);
        const float unicodeWidth = core::TextPrimitive::measureTextWidth(metricSample,
            neo::textfile::pathToUtf8(monoCopy), size);
        check(nativeWidth == unicodeWidth && unicodeWidth > 0,
              "真实渲染器必须加载中文路径字面而非静默回退比例字体");
    }
    const neo::fontsafety::ProbeResult proportional =
        neo::fontsafety::validate(neo::textfile::pathToUtf8(proportionalCopy));
    check(proportional.ok && !proportional.monospace,
          "Arial 应通过字体安全检查并识别为比例字体");

    const fs::path truncated = fontDirectory / L"截断字体.ttf";
    std::string firstBytes = readBytes(consola);
    firstBytes.resize(std::min<std::size_t>(100u, firstBytes.size()));
    check(firstBytes.size() == 100u && writeBytes(truncated, firstBytes),
          "应能生成 100 字节截断字体");
    const neo::fontsafety::ProbeResult truncatedResult =
        neo::fontsafety::validate(neo::textfile::pathToUtf8(truncated));
    check(!truncatedResult.ok, "100 字节截断字体必须被拒绝");

    const fs::path fake = fontDirectory / L"伪字体.ttf";
    check(writeBytes(fake, std::string(100u, 'x')), "应能写入 100 字节伪字体");
    const neo::fontsafety::ProbeResult fakeResult =
        neo::fontsafety::validate(neo::textfile::pathToUtf8(fake));
    check(!fakeResult.ok, "100 字节伪字体必须被拒绝");
}
#endif

} // namespace

int main() {
    const int helperCode = neo::fontsafety::probeCommandLineIfRequested();
    if (helperCode != -1) {
        return helperCode;
    }

    IsolatedConfig config;
    if (!config.ready()) {
        std::cerr << "[font_safety] 无法创建独立临时配置目录\n";
        return 1;
    }

    exerciseMarkerAndRecovery();
#if defined(_WIN32)
    exerciseWindowsFontProbe(config.root());
#else
    std::cout << "[font_safety] Windows helper integration skipped on this platform\n";
#endif

    neo::fontsafety::cleanSession();
    neo::atomicwrite::testing::failBeforeReplace(false);
    if (g_failures != 0) {
        std::cerr << "[font_safety] " << g_failures << " 项检查失败\n";
        return 1;
    }
    std::cout << "[font_safety] probe, timeout, recovery marker, and atomic failure checks passed\n";
    return 0;
}
