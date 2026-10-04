#include "core/platform/bundled_resources.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <array>
#include <string>

namespace core::platform {
namespace {

void showLicenseError(HWND owner, const wchar_t* message) noexcept {
    MessageBoxW(owner, message, L"EUI-Edits", MB_OK | MB_ICONERROR | MB_TASKMODAL);
}

} // namespace

BundledResourceView bundledResource(BundledResourceId id) noexcept {
    const HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return {};
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(static_cast<WORD>(id)), MAKEINTRESOURCEW(10));
    if (!resource) return {};
    const DWORD size = SizeofResource(module, resource);
    if (size == 0) return {};
    const HGLOBAL loaded = LoadResource(module, resource);
    if (!loaded) return {};
    const auto* bytes = static_cast<const unsigned char*>(LockResource(loaded));
    return bytes ? BundledResourceView{bytes, static_cast<std::size_t>(size)} : BundledResourceView{};
}

bool exportEuiEditsLicenses(const std::filesystem::path& outputPath) noexcept {
    const BundledResourceView resource = bundledResource(BundledResourceId::EuiEditsLicenses);
    if (!resource || outputPath.empty()) return false;
    HANDLE file = CreateFileW(outputPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::size_t offset = 0;
    bool success = true;
    while (offset < resource.size) {
        const DWORD chunk = static_cast<DWORD>(
            (resource.size - offset) > MAXDWORD ? MAXDWORD : (resource.size - offset));
        DWORD written = 0;
        if (!WriteFile(file, resource.data + offset, chunk, &written, nullptr) || written == 0) {
            success = false;
            break;
        }
        offset += written;
    }
    if (!CloseHandle(file)) success = false;
    return success;
}

int handleEuiEditsLicenseCommandLine() noexcept {
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments) return -1;
    int result = -1;
    if (argumentCount >= 2 && std::wstring(arguments[1]) == L"--export-licenses") {
        std::filesystem::path outputPath = L"EUI-Edits-LICENSES.txt";
        if (argumentCount >= 3) outputPath = arguments[2];
        if (argumentCount > 3 || !exportEuiEditsLicenses(outputPath)) {
            result = 2;
        } else {
            result = 0;
        }
    }
    LocalFree(arguments);
    return result;
}

bool showEuiEditsLicenseExportDialog(void* ownerWindow) noexcept {
    std::array<wchar_t, 32768> fileName{};
    wcscpy_s(fileName.data(), fileName.size(), L"EUI-Edits-LICENSES.txt");
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = ownerWindow ? static_cast<HWND>(ownerWindow) : GetActiveWindow();
    dialog.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = fileName.data();
    dialog.nMaxFile = static_cast<DWORD>(fileName.size());
    dialog.lpstrDefExt = L"txt";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) return false;
    if (!exportEuiEditsLicenses(fileName.data())) {
        showLicenseError(dialog.hwndOwner, L"无法导出内置许可证文本。请重试或使用 --export-licenses 命令。\n\nCould not export the bundled license text.");
        return false;
    }
    const HINSTANCE opened = ShellExecuteW(dialog.hwndOwner, L"open", fileName.data(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32) {
        showLicenseError(dialog.hwndOwner, L"许可证已导出，但 Windows 无法打开文本文件。\n\nThe license text was exported, but Windows could not open it.");
        return true;
    }
    return true;
}

} // namespace core::platform

#else

namespace core::platform {

BundledResourceView bundledResource(BundledResourceId) noexcept { return {}; }
bool exportEuiEditsLicenses(const std::filesystem::path&) noexcept { return false; }
int handleEuiEditsLicenseCommandLine() noexcept { return -1; }
bool showEuiEditsLicenseExportDialog(void*) noexcept { return false; }

} // namespace core::platform

#endif
