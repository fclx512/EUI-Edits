#include "platform/vault_rename.h"
#include "model/text_file.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <algorithm>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace fs = std::filesystem;
namespace {
int failures = 0;
void check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void put(const fs::path& p, const char* text) { std::ofstream stream(p, std::ios::binary); stream << text; }
std::string get(const fs::path& p) { std::ifstream stream(p, std::ios::binary); return {std::istreambuf_iterator<char>(stream), {}}; }
std::string utf8(const fs::path& p) { return neo::textfile::pathToUtf8(p); }
}
int main() {
    using neo::platform::RenameStatus;
    using neo::platform::renameVaultEntry;
    using neo::platform::validateRenameName;
    for (const std::string name : {"", ".", "..", "trailing.", "trailing ", "a/b", "a\\b", "a:b", "a?b", "a*b", "a<b", "a>b", "a|b", "a\"b", "CON", "con.md", "NUL.txt", "AUX", "PRN.log", "COM1", "com9.txt", "LPT1", "lpt9.md", "CONIN$", "CONOUT$", "COM\xC2\xB9.txt"})
        check(!validateRenameName(name).empty(), "invalid Windows name rejected");
    check(!validateRenameName(std::string("abc\0def", 7)).empty(), "embedded NUL rejected");
    check(!validateRenameName("a\x01" "b").empty(), "control character rejected");
    check(!validateRenameName(std::string(256, 'a')).empty(), "overlong name rejected");
    check(validateRenameName("COM10.md").empty() && validateRenameName(" LPT10.txt").empty(), "nonreserved names accepted");
    check(validateRenameName("中文 文件.md").empty(), "Unicode and internal spaces accepted");
    check(neo::platform::renameSelectionEnd("中文 文件.md", false) == 13, "file stem selection uses UTF-8 byte boundary");
    check(neo::platform::renameSelectionEnd(".gitignore", false) == 10, "dotfile selected fully");
    check(neo::platform::renameSelectionEnd("a.b", true) == 3, "folder selected fully");
    check(neo::platform::renameSelectionEnd("archive.tar.gz", false) == 11, "last extension stays unselected");

    const auto temp = fs::absolute(fs::temp_directory_path()).lexically_normal();
    const auto base = temp / ("neo-vault-rename-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto rel = base.lexically_relative(temp);
    if (rel.empty() || rel.is_absolute() || *rel.begin() == "..") return 2;
    const auto root = base / "root", outside = base / "outside";
    fs::create_directories(root); fs::create_directories(outside);
    put(root / "source.md", "source bytes");
    put(root / "taken.md", "other bytes");
    put(root / "taken (2).md", "second bytes");
    const auto conflict = renameVaultEntry(utf8(root), "source.md", "taken.md");
    check(conflict.status == RenameStatus::Conflict && conflict.suggestedName == "taken (3).md", "conflict offers the next available number");
    check(get(root / "source.md") == "source bytes" && get(root / "taken.md") == "other bytes", "conflict never overwrites either file");
    check(!fs::exists(root / "taken (3).md"), "a suggestion does not rename until explicitly accepted");
    check(renameVaultEntry(utf8(root), "source.md", conflict.suggestedName).status == RenameStatus::Success, "explicit numbered rename succeeds");
    check(get(root / "taken (3).md") == "source bytes" && get(root / "taken (2).md") == "second bytes", "numbered rename preserves previous siblings");
    const auto raceProposal = renameVaultEntry(utf8(root), "taken (3).md", "taken.md");
    put(root / neo::textfile::pathFromUtf8(raceProposal.suggestedName), "competing writer");
    const auto raced = renameVaultEntry(utf8(root), "taken (3).md", raceProposal.suggestedName);
    check(raced.status == RenameStatus::Conflict && get(root / neo::textfile::pathFromUtf8(raceProposal.suggestedName)) == "competing writer", "a claimed suggestion requires renewed consent without replacement");
    check(renameVaultEntry(utf8(root), "taken (3).md", "中文 文件.md").status == RenameStatus::Success, "Unicode rename succeeds");
    check(get(root / fs::u8path("中文 文件.md")) == "source bytes", "Unicode filename preserves bytes");
    put(root / "Case.md", "case bytes");
    check(renameVaultEntry(utf8(root), "Case.md", "case.md").status == RenameStatus::Success, "case-only rename succeeds");
    bool sawLowercase = false;
    for (const auto& entry : fs::directory_iterator(root)) if (entry.path().filename() == "case.md") sawLowercase = true;
    check(sawLowercase, "case-only rename updates the directory entry spelling");
    for (const std::string path : {"../outside/secret.md", ".", "..", "child/../Case.md", "child\\..\\Case.md"})
        check(renameVaultEntry(utf8(root), path, "renamed.md").status == RenameStatus::Error, "traversal or malformed parent rejected");
    check(renameVaultEntry(utf8(root), utf8(root / "case.md"), "new.md").status == RenameStatus::Error, "absolute source rejected");
    check(renameVaultEntry(utf8(root), "case.md", "con.txt").status == RenameStatus::Error && get(root / "case.md") == "case bytes", "invalid destination leaves source untouched");
    fs::create_directory(root / "folder"); put(root / "folder" / "child.md", "child bytes");
    check(renameVaultEntry(utf8(root), "folder", "新目录").status == RenameStatus::Success, "directory rename succeeds");
    check(get(root / fs::u8path("新目录") / "child.md") == "child bytes", "directory rename preserves descendants");
    put(outside / "secret.md", "outside bytes");
    std::error_code ec; fs::create_directory_symlink(outside, root / "escape", ec);
    if (!ec) {
        check(renameVaultEntry(utf8(root), "escape/secret.md", "stolen.md").status == RenameStatus::Error, "parent link escaping the vault is rejected");
        check(get(outside / "secret.md") == "outside bytes" && !fs::exists(outside / "stolen.md"), "outside bytes unchanged");
        check(renameVaultEntry(utf8(root), "escape", "alias").status == RenameStatus::Success, "final link itself may be renamed without traversing it");
    } else std::cout << "SKIP: symlink creation unavailable: " << ec.message() << '\n';
#if defined(_WIN32)
    {
        const auto loop = root / "escape-junction";
        check(CreateDirectoryW(loop.c_str(), nullptr) != 0, "create isolated junction entry");
    // Construct an NTFS mount-point reparse record directly, avoiding the
    // symlink privilege requirement. The junction points back to its parent.
    struct ReparseHeader { ULONG tag; USHORT dataLength; USHORT reserved; };
    struct MountPointFields {
        USHORT substituteOffset;
        USHORT substituteLength;
        USHORT printOffset;
        USHORT printLength;
    };
    static_assert(sizeof(ReparseHeader) == 8, "reparse header layout");
    static_assert(sizeof(MountPointFields) == 8, "mount-point header layout");
    const std::wstring printName = outside.native();
    const std::wstring substituteName = L"\\??\\" + printName;
    const std::size_t pathBytes = (substituteName.size() + 1 + printName.size() + 1) * sizeof(wchar_t);
    const USHORT reparseDataLength = static_cast<USHORT>(sizeof(MountPointFields) + pathBytes);
    std::vector<unsigned char> buffer(sizeof(ReparseHeader) + reparseDataLength, 0);
    auto* header = reinterpret_cast<ReparseHeader*>(buffer.data());
    header->tag = IO_REPARSE_TAG_MOUNT_POINT;
    header->dataLength = reparseDataLength;
    auto* fields = reinterpret_cast<MountPointFields*>(buffer.data() + sizeof(ReparseHeader));
    fields->substituteOffset = 0;
    fields->substituteLength = static_cast<USHORT>(substituteName.size() * sizeof(wchar_t));
    fields->printOffset = static_cast<USHORT>(fields->substituteLength + sizeof(wchar_t));
    fields->printLength = static_cast<USHORT>(printName.size() * sizeof(wchar_t));
    wchar_t* pathBuffer = reinterpret_cast<wchar_t*>(
        buffer.data() + sizeof(ReparseHeader) + sizeof(MountPointFields));
    std::copy(substituteName.begin(), substituteName.end(), pathBuffer);
    std::copy(printName.begin(), printName.end(),
              pathBuffer + substituteName.size() + 1);

    HANDLE handle = CreateFileW(loop.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    DWORD bytesReturned = 0;
    const BOOL created = handle != INVALID_HANDLE_VALUE &&
        DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
                        static_cast<DWORD>(buffer.size()), nullptr, 0, &bytesReturned, nullptr);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    if (!created) {
        RemoveDirectoryW(loop.c_str());
        check(false, "create isolated junction reparse point");
        return 1;
    }
        check(renameVaultEntry(utf8(root), "escape-junction/secret.md", "stolen.md").status == RenameStatus::Error,
              "NTFS junction parent escaping the vault rejects rename without special symlink privileges");
        check(get(outside / "secret.md") == "outside bytes" && !fs::exists(outside / "stolen.md"), "junction escape leaves outside contents unchanged");
        check(renameVaultEntry(utf8(root), "escape-junction", "renamed-junction").status == RenameStatus::Success,
              "a final NTFS junction itself can safely be renamed");
        check(RemoveDirectoryW((root / "renamed-junction").c_str()) != 0, "remove only the renamed junction entry");
    }
#endif
    fs::remove_all(base);
    std::cout << (failures ? "FAIL" : "PASS") << ": vault_rename (" << failures << " failures)\n";
    return failures ? 1 : 0;
}
