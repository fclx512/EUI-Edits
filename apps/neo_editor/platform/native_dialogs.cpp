#include "model/i18n.h"
#include "platform/native_dialogs.h"
#include "platform/save_dialog_protocol.h"

#include "model/text_file.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <cstdlib>
#include <cerrno>
#include <limits>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
// 顺序要紧：windows.h 必须最先，shlobj.h 依赖它。
#include <windows.h>
#include <objbase.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>

#if defined(_MSC_VER)
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#endif
#endif

namespace neo::dialogs {
namespace {

#if defined(_WIN32)

namespace fs = std::filesystem;

constexpr std::size_t kMaxPathLength = 1024;

std::wstring toWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    return textfile::pathFromUtf8(value).wstring();
}

// BIF_NEWDIALOGSTYLE 要求调用线程先初始化 COM。S_OK 和 S_FALSE 都会增加该线程的
// COM 初始化计数，因此两种成功结果都必须配对 CoUninitialize；只有本次确实调用
// 成功时才设置 shouldUninitialize。RPC_E_CHANGED_MODE 等失败结果不能反初始化。
bool initializeCom(bool& shouldUninitialize) {
    shouldUninitialize = false;
    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(result)) {
        shouldUninitialize = true;
        return true;
    }
    return false;
}

std::wstring buildSaveFilter(const std::vector<std::string>& extensionList) {
    std::wstring patterns;
    for (const std::string& extension : extensionList) {
        if (extension.empty()) {
            continue;
        }
        if (!patterns.empty()) {
            patterns.push_back(L';');
        }
        patterns += L"*.";
        patterns += std::wstring(extension.begin(), extension.end());
    }
    if (patterns.empty()) {
        patterns = L"*.*";
    }

    std::wstring storage;
    storage += toWide(i18n::tr("dialog.documents")) + L" (" + patterns + L")";
    storage.push_back(L'\0');
    storage += patterns;
    storage.push_back(L'\0');
    storage += toWide(i18n::tr("dialog.all_files")) + L" (*.*)";
    storage.push_back(L'\0');
    storage += L"*.*";
    storage.push_back(L'\0');
    storage.push_back(L'\0');
    return storage;
}

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE h = nullptr) { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = h; }
};

bool readPipe(HANDLE pipe, std::string& packet) {
    packet.clear(); char block[4096]; DWORD read = 0;
    while (ReadFile(pipe, block, sizeof(block), &read, nullptr) && read) {
        if (packet.size() + read > wire::kMaximumPacket) return false;
        packet.append(block, read);
    }
    return !packet.empty() && GetLastError() == ERROR_BROKEN_PIPE;
}

bool writePipe(HANDLE pipe, const std::string& packet) {
    std::size_t at = 0;
    while (at < packet.size()) {
        DWORD written = 0;
        if (!WriteFile(pipe, packet.data() + at, static_cast<DWORD>(packet.size() - at), &written, nullptr) || !written) return false;
        at += written;
    }
    return true;
}

std::u16string wireText(const std::wstring& text) { return {text.begin(), text.end()}; }
std::wstring wideText(const std::u16string& text) { return {text.begin(), text.end()}; }

bool creatorIs(DWORD expected) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.value, &entry)) do {
        if (entry.th32ProcessID == GetCurrentProcessId()) return entry.th32ParentProcessID == expected;
    } while (Process32NextW(snapshot.value, &entry));
    return false;
}

PickResult isolatedSavePicker(const wire::Request& request) {
    PickResult result;
    const auto fail = [&] { result.error = i18n::tr("dialog.save_open"); return result; };
    std::string packet;
    if (!wire::encode(request, packet)) return fail();
    std::vector<wchar_t> executable(32768);
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return fail();

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    Handle requestRead, requestWrite, resultRead, resultWrite;
    if (!CreatePipe(&requestRead.value,&requestWrite.value,&inherit,65536) ||
        !CreatePipe(&resultRead.value,&resultWrite.value,&inherit,65536) ||
        !SetHandleInformation(requestWrite.value,HANDLE_FLAG_INHERIT,0) ||
        !SetHandleInformation(resultRead.value,HANDLE_FLAG_INHERIT,0)) return fail();

    // Only these two pipe handles reach the child; never inherit editor/file handles.
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr,1,0,&attributeSize);
    std::vector<unsigned char> storage(attributeSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes,1,0,&attributeSize)) return fail();
    struct AttributesGuard { LPPROC_THREAD_ATTRIBUTE_LIST list; ~AttributesGuard(){DeleteProcThreadAttributeList(list);} } guard{attributes};
    HANDLE allowed[]{requestRead.value,resultWrite.value};
    if (!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,allowed,sizeof(allowed),nullptr,nullptr)) return fail();
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
    std::wstring command = L"\"" + std::wstring(executable.data(),length) + L"\" --neo-save-picker " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(requestRead.value)) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(resultWrite.value)) + L" " + std::to_wstring(GetCurrentProcessId());
    HWND owner = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(request.owner));
    const bool disableOwner = owner && IsWindowEnabled(owner);
    struct OwnerGuard {
        HWND owner; bool enabled;
        ~OwnerGuard() { if(enabled && IsWindow(owner)) { EnableWindow(owner,TRUE); SetForegroundWindow(owner); } }
    } ownerGuard{owner,disableOwner};
    // Disable before launch: do not race the child picker disabling its owner.
    // This also restores the editor if the helper fails or exits unexpectedly.
    if (disableOwner) EnableWindow(owner,FALSE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.data(),command.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&startup.StartupInfo,&process)) return fail();
    Handle child(process.hProcess), childThread(process.hThread);
    requestRead.reset(); resultWrite.reset();
    const bool sent = writePipe(requestWrite.value,packet);
    requestWrite.reset(); // EOF allows a malformed/failed request to exit too.

    bool waited = true; bool quit = false; WPARAM quitCode = 0;
    // Pump native messages as a modal picker does; paints and cross-process owner
    // messages must continue while the helper displays the Windows dialog.
    for (;;) {
        const DWORD wait = MsgWaitForMultipleObjects(1,&child.value,FALSE,INFINITE,QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_OBJECT_0 + 1) { waited = false; break; }
        MSG message{};
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if (message.message == WM_QUIT) {quit=true;quitCode=message.wParam;continue;}
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    if (quit) PostQuitMessage(static_cast<int>(quitCode));
    DWORD exitCode = 1;
    if (!waited || !sent || !GetExitCodeProcess(child.value,&exitCode) || exitCode || !readPipe(resultRead.value,packet)) return fail();
    unsigned status = 2; std::uint32_t error = 0; std::u16string path;
    if (!wire::decodeResult(packet,status,error,path)) return fail();
    if (status == 0) result.cancelled = true;
    else if (status == 1) { result.path = textfile::pathToUtf8(fs::path(wideText(path))); result.ok = true; }
    else return fail();
    return result;
}

#endif

} // namespace

int runSaveDialogHelperIfRequested() {
#if defined(_WIN32)
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(),&count);
    if (!args) return -1;
    struct ArgsGuard { LPWSTR* args; ~ArgsGuard(){LocalFree(args);} } guard{args};
    if (count < 2 || std::wstring(args[1]) != L"--neo-save-picker") return -1;
    if (count != 5) return 2;
    std::uint64_t numbers[3]{};
    for (int i=0;i<3;++i) {
        const std::wstring value(args[i+2]);
        if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos) return 2;
        wchar_t* end=nullptr; errno=0;
        numbers[i]=_wcstoui64(value.c_str(),&end,10);
        if (errno || !end || *end || !numbers[i]) return 2;
    }
    if (numbers[2] > std::numeric_limits<DWORD>::max() || !creatorIs(static_cast<DWORD>(numbers[2]))) return 2;
    Handle input(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(numbers[0])));
    Handle output(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(numbers[1])));
    if (GetFileType(input.value) != FILE_TYPE_PIPE || GetFileType(output.value) != FILE_TYPE_PIPE) return 2;
    std::string packet; wire::Request request;
    if (!readPipe(input.value,packet) || !wire::decode(packet,request)) return 2;
    input.reset();
    HWND owner = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(request.owner));
    DWORD ownerPid=0;
    if (!owner || !IsWindow(owner) || !GetWindowThreadProcessId(owner,&ownerPid) || ownerPid != numbers[2]) return 2;
    // The ordinary Win32 host sets PMv2 at runtime; the early helper bypasses it.
    using SetDpiContext = BOOL (WINAPI*)(HANDLE);
    if (auto setDpi = reinterpret_cast<SetDpiContext>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext"))) {
        setDpi(reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-4))); // Per-monitor v2
    }
    const std::wstring initial=wideText(request.fields[0]), name=wideText(request.fields[1]),
        filter=wideText(request.fields[2]), extension=wideText(request.fields[3]);
    std::vector<wchar_t> buffer(kMaxPathLength,L'\0');
    std::copy_n(name.begin(),std::min(name.size(),buffer.size()-1),buffer.begin());
    OPENFILENAMEW options{}; options.lStructSize=sizeof(options); options.hwndOwner=owner;
    options.lpstrFilter=filter.c_str(); options.nFilterIndex=1; options.lpstrFile=buffer.data();
    options.nMaxFile=static_cast<DWORD>(buffer.size()); options.lpstrInitialDir=initial.empty()?nullptr:initial.c_str();
    options.lpstrDefExt=extension.empty()?nullptr:extension.c_str();
    options.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_EXPLORER;
    const bool selected=GetSaveFileNameW(&options)==TRUE;
    const DWORD error=selected?0:CommDlgExtendedError();
    if (!wire::encodeResult(selected?1:(error?2:0),error,selected?wireText(buffer.data()):std::u16string{},packet)) return 2;
    return writePipe(output.value,packet)?0:2;
#else
    return -1;
#endif
}

PickResult pickDirectory(const std::string& initialDirectory) {
    PickResult result;
#if defined(_WIN32)
    // IFileDialog + FOS_PICKFOLDERS = Vista 起的现代资源管理器风格目录选择窗口，
    // 替换 SHBrowseForFolderW 的老式树形对话框（不能拖拽、不能粘贴路径、像上个时代）。
    bool shouldUninitialize = false;
    if (!initializeCom(shouldUninitialize)) {
        result.error = i18n::tr("dialog.com_failed");
        return result;
    }

    IFileDialog* dialog = nullptr;
    const HRESULT created = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                             IID_PPV_ARGS(&dialog));
    if (FAILED(created)) {
        result.error = i18n::tr("dialog.folder_create");
        if (shouldUninitialize) {
            CoUninitialize();
        }
        return result;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    const std::wstring title = toWide(i18n::tr("dialog.choose_library"));
    dialog->SetTitle(title.c_str());

    // 初始目录：SetFolder 需要 IShellItem，从 PIDL 转一手（路径不存在时保持默认位置）。
    const std::wstring initial = toWide(initialDirectory);
    if (!initial.empty()) {
        PIDLIST_ABSOLUTE itemList = ILCreateFromPathW(initial.c_str());
        if (itemList != nullptr) {
            IShellItem* item = nullptr;
            if (SHCreateItemFromIDList(itemList, IID_PPV_ARGS(&item)) == S_OK && item != nullptr) {
                dialog->SetFolder(item);
                item->Release();
            }
            ILFree(itemList);
        }
    }

    if (dialog->Show(GetActiveWindow()) == S_OK) {
        IShellItem* chosen = nullptr;
        if (SUCCEEDED(dialog->GetResult(&chosen)) && chosen != nullptr) {
            // 不直接调 IShellItem::GetPath：部分 SDK 配置下该接口只有前置声明。
            PIDLIST_ABSOLUTE chosenList = nullptr;
            if (SUCCEEDED(SHGetIDListFromObject(chosen, &chosenList)) && chosenList != nullptr) {
                wchar_t pathBuffer[MAX_PATH] = {};
                if (SHGetPathFromIDListW(chosenList, pathBuffer) == TRUE) {
                    result.path = textfile::pathToUtf8(fs::path(pathBuffer));
                    result.ok = true;
                }
                ILFree(chosenList);
            }
            chosen->Release();
        }
        if (!result.ok) {
            result.error = i18n::tr("dialog.folder_path");
        }
    } else {
        result.cancelled = true;
    }

    dialog->Release();
    if (shouldUninitialize) {
        CoUninitialize();
    }
#else
    (void)initialDirectory;
    result.error = i18n::tr("dialog.folder_platform");
#endif
    return result;
}

PickResult pickSavePath(const std::string& initialDirectory,
                        const std::string& suggestedName,
                        const std::vector<std::string>& extensionList) {
    PickResult result;
#if defined(_WIN32)
    wire::Request request;
    request.owner=reinterpret_cast<std::uintptr_t>(GetActiveWindow());
    request.fields={wireText(toWide(initialDirectory)),wireText(toWide(suggestedName)),
        wireText(buildSaveFilter(extensionList)),wireText(extensionList.empty()?std::wstring{}:toWide(extensionList.front()))};
    return isolatedSavePicker(request);
#else
    (void)initialDirectory;
    (void)suggestedName;
    (void)extensionList;
    result.error = i18n::tr("dialog.save_platform");
#endif
    return result;
}

float systemScale() {
#if defined(_WIN32)
    // GetDpiForSystem 是 1607+ 的 API，动态取一次，免得为它在旧 SDK 上过不了链接。
    using GetDpiForSystemFn = UINT(WINAPI*)();
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        const auto getDpi = reinterpret_cast<GetDpiForSystemFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForSystem")));
        if (getDpi != nullptr) {
            const UINT dpi = getDpi();
            // 96 是 100%，往上 120/144/168/192 分别对应 125/150/175/200%。
            if (dpi >= 96 && dpi <= 480) {
                return static_cast<float>(dpi) / 96.0f;
            }
        }
    }
    // 兜底：GDI 的系统 DPI。没有 DPI 感知时两个 API 都会给 96，所以调用时机很关键。
    if (HDC screen = GetDC(nullptr)) {
        const int dpi = GetDeviceCaps(screen, LOGPIXELSY);
        ReleaseDC(nullptr, screen);
        if (dpi > 0) {
            return static_cast<float>(dpi) / 96.0f;
        }
    }
#endif
    return 1.0f;
}

} // namespace neo::dialogs
