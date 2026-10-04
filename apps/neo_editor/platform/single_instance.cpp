#include "platform/single_instance.h"

#include <string>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>

#include "model/text_file.h"

namespace neo::platform {
namespace {

// 名字带会话命名空间 Local\：同一登录会话内唯一；不同用户/会话各玩各的。
constexpr wchar_t kMutexName[] = L"Local\\EUI-Edits.SingleInstance";
constexpr wchar_t kEventName[] = L"Local\\EUI-Edits.OpenRequest";
constexpr wchar_t kForwardFileName[] = L"EUI-Edits.next-open";
// 路径文件的合理上限：命令行路径再长也到不了 4KiB（Windows 路径上限 32767，
// 但正常双击/关联启动远小于此），超长当坏请求丢弃，防止被塞大文件撑爆内存。
constexpr std::size_t kMaxForwardBytes = 4096;

HANDLE g_mutex = nullptr;   // 拥有者持有，不 Release，随进程退出回收
HANDLE g_event = nullptr;   // 主实例侧监听目标
std::function<void()> g_onWake;

std::filesystem::path forwardFilePath() {
    std::wstring dir(MAX_PATH + 1, L'\0');
    const DWORD len = GetTempPathW(static_cast<DWORD>(dir.size()), dir.data());
    if (len == 0 || len >= dir.size()) {
        return {};
    }
    dir.resize(len);
    return std::filesystem::path(dir) / kForwardFileName;
}

// 找已有实例的主窗口：按"可见的顶层窗口属于同映像名的进程"匹配，
// 不依赖窗口标题（标题里带文档名，会变）。
struct FoundWindow {
    HWND hwnd = nullptr;
};

std::wstring ownImageName() {
    std::wstring name(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, name.data(),
                                            static_cast<DWORD>(name.size()));
        if (len == 0) {
            return {};
        }
        if (len < name.size()) {
            name.resize(len);
            break;
        }
        name.resize(name.size() * 2);
    }
    const std::size_t slash = name.find_last_of(L"\\/");
    return slash == std::wstring::npos ? name : name.substr(slash + 1);
}

BOOL CALLBACK enumActivateProc(HWND hwnd, LPARAM lParam) {
    auto* found = reinterpret_cast<FoundWindow*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || !IsWindowVisible(hwnd)) {
        return TRUE;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return TRUE;
    }
    std::wstring image(MAX_PATH, L'\0');
    DWORD size = static_cast<DWORD>(image.size());
    BOOL ok = QueryFullProcessImageNameW(process, 0, image.data(), &size);
    CloseHandle(process);
    if (!ok) {
        return TRUE;
    }
    image.resize(size);
    const std::size_t slash = image.find_last_of(L"\\/");
    const std::wstring base = slash == std::wstring::npos ? image : image.substr(slash + 1);
    // 同一映像名 = neo_editor 的另一个窗口（忽略大小写，NTFS 惯例）
    if (_wcsicmp(base.c_str(), ownImageName().c_str()) != 0) {
        return TRUE;
    }
    found->hwnd = hwnd;
    return FALSE;
}

void activateExistingWindow() {
    FoundWindow found;
    EnumWindows(enumActivateProc, reinterpret_cast<LPARAM>(&found));
    if (found.hwnd == nullptr) {
        return;
    }
    if (IsIconic(found.hwnd)) {
        ShowWindow(found.hwnd, SW_RESTORE);
    }
    // Synthetic Alt can enter Windows' native menu loop and swallow the next
    // confirmation key. Use the normal foreground grant, then notify if locked.
    DWORD owner = 0;
    GetWindowThreadProcessId(found.hwnd, &owner);
    if (owner) AllowSetForegroundWindow(owner);
    if (!SetForegroundWindow(found.hwnd)) {
        FLASHWINFO flash{sizeof(flash), found.hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0};
        FlashWindowEx(&flash);
    }
}

bool writeForwardFile(const std::string& utf8Path) {
    const std::filesystem::path target = forwardFilePath();
    if (target.empty()) {
        return false;
    }
    // 先写唯一临时名再原子换名：主实例读到的一定是完整内容。
    std::filesystem::path tmp = target;
    tmp += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = utf8Path.empty() ||
        WriteFile(file, utf8Path.data(), static_cast<DWORD>(utf8Path.size()), &written, nullptr);
    CloseHandle(file);
    if (!ok || (!utf8Path.empty() && written != utf8Path.size())) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace

bool singleInstanceAcquire(std::function<void()> onWake, const std::string& forwardPath) {
    SetLastError(ERROR_SUCCESS);
    g_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (g_mutex == nullptr) {
        // 连 mutex 都建不出来（安全软件拦截？）不挡启动，按"单实例不可用"放行。
        return true;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已有实例：转发命令行文档并让调用方退出。写文件失败（目录不可写等）
        // 也照样 SetEvent + 置前——"唤醒已有实例"的语义不受影响。
        writeForwardFile(forwardPath);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, kEventName);
        if (event != nullptr) {
            SetEvent(event);
            CloseHandle(event);
        }
        activateExistingWindow();
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        return false;
    }

    // 主实例：记下回调、开监听线程（等 SetEvent → 唤醒 UI → compose tick 消费文件）。
    g_onWake = std::move(onWake);
    g_event = CreateEventW(nullptr, FALSE, FALSE, kEventName);
    if (g_event == nullptr) {
        return true;  // 唤醒通道建不出来：单实例仍生效，只是空闲时收不到转发
    }
    HANDLE event = g_event;
    std::thread([event] {
        for (;;) {
            const DWORD wait = WaitForSingleObject(event, INFINITE);
            if (wait != WAIT_OBJECT_0) {
                break;  // 句柄被 shutdown 关闭等，线程退出
            }
            if (g_onWake) {
                g_onWake();
            }
        }
    }).detach();
    return true;
}

std::string takeSingleInstanceOpenPath() {
    std::error_code error;
    const std::filesystem::path path = forwardFilePath();
    if (path.empty() || !std::filesystem::is_regular_file(path, error) || error) {
        return {};
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    char buffer[kMaxForwardBytes];
    DWORD read = 0;
    const BOOL ok = ReadFile(file, buffer, sizeof(buffer), &read, nullptr);
    CloseHandle(file);
    std::filesystem::remove(path, error);
    if (!ok || read == 0 || read >= sizeof(buffer)) {
        return {};
    }
    // 拒绝夹带换行/NUL 的坏请求；转发内容就一行路径。
    std::string result(buffer, read);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    if (result.find('\0') != std::string::npos) {
        return {};
    }
    return result;
}

void singleInstanceShutdown() {
    if (g_event != nullptr) {
        CloseHandle(g_event);  // INFINITE 等待失败，监听线程随即退出
        g_event = nullptr;
    }
    g_onWake = nullptr;
    if (g_mutex != nullptr) {
        // An explicit close ends this instance's deferred requests. A stale
        // request must not override a different file on a later fresh launch.
        std::error_code ignored;
        const auto request = forwardFilePath();
        if (!request.empty()) std::filesystem::remove(request, ignored);
        CloseHandle(g_mutex);
        g_mutex = nullptr;
    }
}

} // namespace neo::platform

#else  // !_WIN32

namespace neo::platform {

bool singleInstanceAcquire(std::function<void()> onWake) {
    (void)onWake;
    return true;
}

std::string takeSingleInstanceOpenPath() {
    return {};
}

void singleInstanceShutdown() {}

} // namespace neo::platform

#endif
