#include "model/i18n.h"
#include "font_safety.h"

#include "model/atomic_write.h"
#include "model/settings.h"
#include "model/text_file.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace neo::fontsafety {
namespace {

namespace fs = std::filesystem;
constexpr std::uintmax_t kMaximumFontBytes = 128u * 1024u * 1024u;
constexpr std::uintmax_t kMaximumMarkerPathBytes = 1024u * 1024u;
constexpr std::uintmax_t kMaximumMarkerBytes = 3u * kMaximumMarkerPathBytes + 256u;
constexpr std::uint64_t kMaximumRenderedGlyphBytes = 16u * 1024u * 1024u;
constexpr unsigned kMaximumGlyphDimension = 4096u;
constexpr char kMarkerHeader[] = "neo-font-session-v1\n";
// A failed recovery write must keep its journal even if startup subsequently
// falls back to empty defaults or exits normally.
std::atomic_bool recoveryWritePending{false};

struct FileIdentity {
    std::uintmax_t size = 0;
    fs::file_time_type modified{};
};

bool inspectFile(const std::string& utf8Path, FileIdentity& identity, std::string& error) {
    try {
        const fs::path path = textfile::pathFromUtf8(utf8Path);
        std::error_code ec;
        if (!fs::is_regular_file(path, ec) || ec) {
            error = i18n::tr("font.missing");
            return false;
        }
        identity.size = fs::file_size(path, ec);
        if (ec) {
            error = i18n::tr("font.size_read");
            return false;
        }
        if (identity.size == 0 || identity.size > kMaximumFontBytes) {
            error = i18n::tr("font.size_limit");
            return false;
        }
        identity.modified = fs::last_write_time(path, ec);
        if (ec) {
            error = i18n::tr("font.modified_read");
            return false;
        }
    } catch (const std::exception&) {
        error = i18n::tr("font.invalid_path");
        return false;
    }
    return true;
}

bool supportedExtension(const std::string& utf8Path) {
    try {
        std::string extension = textfile::pathToUtf8(textfile::pathFromUtf8(utf8Path).extension());
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return extension == ".ttf" || extension == ".otf" || extension == ".ttc";
    } catch (const std::exception&) {
        return false;
    }
}

bool readFontBytes(const std::string& utf8Path,
                   std::vector<unsigned char>& bytes,
                   std::string& error) {
    FileIdentity identity;
    if (!inspectFile(utf8Path, identity, error)) {
        return false;
    }
    try {
        // Opening a filesystem::path uses the native wide path on Windows.
        std::ifstream input(textfile::pathFromUtf8(utf8Path), std::ios::binary);
        if (!input) {
            error = i18n::tr("font.open");
            return false;
        }
        bytes.resize(static_cast<std::size_t>(identity.size));
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input || static_cast<std::size_t>(input.gcount()) != bytes.size()) {
            bytes.clear();
            error = i18n::tr("font.truncated");
            return false;
        }
    } catch (const std::bad_alloc&) {
        error = i18n::tr("font.memory_limit");
        return false;
    } catch (const std::exception&) {
        error = i18n::tr("font.read");
        return false;
    }
    return true;
}

bool bitmapWithinBounds(const FT_Bitmap& bitmap) {
    if (bitmap.width > kMaximumGlyphDimension || bitmap.rows > kMaximumGlyphDimension) {
        return false;
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(bitmap.width) * bitmap.rows;
    // Four bytes per pixel is a conservative bound for grayscale, LCD, and BGRA.
    constexpr std::uint64_t bytesPerPixel = 4u;
    return pixels <= kMaximumRenderedGlyphBytes / bytesPerPixel;
}

bool sameAdvanceSet(const std::array<FT_Pos, 4>& advances) {
    const auto bounds = std::minmax_element(advances.begin(), advances.end());
    return static_cast<std::int64_t>(*bounds.second) -
               static_cast<std::int64_t>(*bounds.first) <= 1;
}

struct FontProbe {
    bool monospace = false;
    std::string error;
};

FontProbe probeFont(const std::string& utf8Path) {
    FontProbe result;
    if (!supportedExtension(utf8Path)) {
        result.error = i18n::tr("font.types");
        return result;
    }

    std::vector<unsigned char> bytes;
    if (!readFontBytes(utf8Path, bytes, result.error)) {
        return result;
    }

    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0 || library == nullptr) {
        result.error = i18n::tr("font.freetype_init");
        return result;
    }

    FT_Face face = nullptr;
    FT_Open_Args openArgs{};
    openArgs.flags = FT_OPEN_MEMORY;
    openArgs.memory_base = bytes.data();
    openArgs.memory_size = static_cast<FT_Long>(bytes.size());
    const FT_Error openError = FT_Open_Face(library, &openArgs, 0, &face);
    if (openError != 0 || face == nullptr) {
        FT_Done_FreeType(library);
        result.error = i18n::tr("font.freetype_open");
        return result;
    }

    bool valid = true;
    bool monospacedAtEverySize = true;
    constexpr std::array<unsigned, 3> kSizes = {12u, 20u, 48u};
    constexpr std::array<FT_ULong, 6> kUnicodeCharacters = {
        0x4E2Du, 0x4F60u, 0x6587u, 0x3002u, 0xFF0Cu, 0xFF1Bu,
    };
    constexpr std::array<FT_ULong, 4> kWidthCharacters = {'i', 'M', 'W', '0'};
    constexpr FT_Int32 kLoadFlags = FT_LOAD_DEFAULT | FT_LOAD_COLOR | FT_LOAD_NO_SVG | FT_LOAD_TARGET_LIGHT;

    if (!FT_IS_SCALABLE(face)) {
        result.error = i18n::tr("font.bitmap_only");
        valid = false;
    } else if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) {
        result.error = i18n::tr("font.unicode");
        valid = false;
    }

    for (const unsigned size : kSizes) {
        if (!valid) {
            break;
        }
        if (FT_Set_Pixel_Sizes(face, 0, size) != 0) {
            result.error = i18n::tr("font.set_size");
            valid = false;
            break;
        }

        std::array<FT_Pos, kWidthCharacters.size()> widthAdvances{};
        bool widthCharactersPresent = true;
        for (FT_ULong character = 0x20u; character <= 0x7Eu; ++character) {
            const FT_UInt glyphIndex = FT_Get_Char_Index(face, character);
            if (glyphIndex == 0 && std::find(kWidthCharacters.begin(), kWidthCharacters.end(), character) !=
                                       kWidthCharacters.end()) {
                widthCharactersPresent = false;
            }
            if (FT_Load_Glyph(face, glyphIndex, kLoadFlags) != 0) {
                result.error = i18n::tr("font.load_glyph");
                valid = false;
                break;
            }
            for (std::size_t widthIndex = 0; widthIndex < kWidthCharacters.size(); ++widthIndex) {
                if (character == kWidthCharacters[widthIndex]) {
                    widthAdvances[widthIndex] = face->glyph->advance.x;
                    break;
                }
            }
            if (face->glyph->format != FT_GLYPH_FORMAT_BITMAP &&
                FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
                result.error = i18n::tr("font.render_glyph");
                valid = false;
                break;
            }
            if (!bitmapWithinBounds(face->glyph->bitmap)) {
                result.error = i18n::tr("font.glyph_limit");
                valid = false;
                break;
            }
        }
        for (const FT_ULong character : kUnicodeCharacters) {
            if (!valid) {
                break;
            }
            const FT_UInt glyphIndex = FT_Get_Char_Index(face, character);
            if (FT_Load_Glyph(face, glyphIndex, kLoadFlags) != 0) {
                result.error = i18n::tr("font.load_glyph");
                valid = false;
                break;
            }
            if (face->glyph->format != FT_GLYPH_FORMAT_BITMAP &&
                FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
                result.error = i18n::tr("font.render_glyph");
                valid = false;
                break;
            }
            if (!bitmapWithinBounds(face->glyph->bitmap)) {
                result.error = i18n::tr("font.glyph_limit");
                valid = false;
                break;
            }
        }
        if (valid && (!widthCharactersPresent || !sameAdvanceSet(widthAdvances))) {
            monospacedAtEverySize = false;
        }
    }

    if (valid) {
        result.monospace = monospacedAtEverySize && FT_IS_FIXED_WIDTH(face);
    }
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return result;
}

fs::path sessionMarkerPath() {
    return textfile::pathFromUtf8(settings::configDirectory()) / "font-session.txt";
}

bool removeMarker(std::string& error) {
    try {
        std::error_code ec;
        const fs::path path = sessionMarkerPath();
        const bool removed = fs::remove(path, ec);
        if (ec) {
            error = i18n::tr("font.guard_clear");
            return false;
        }
        (void)removed; // A missing marker is already clean.
        return true;
    } catch (const std::exception&) {
        error = i18n::tr("font.guard_access");
        return false;
    }
}

bool hasCustomFonts(const testing::SessionFonts& fonts) {
    return !fonts.editor.empty() || !fonts.ui.empty() || !fonts.code.empty();
}

testing::SessionFonts sessionFonts(const settings::Data& data) {
    return {data.editorFontFile, data.uiFontFile, data.codeFontFile};
}

struct CacheEntry {
    std::uintmax_t size = 0;
    fs::file_time_type modified{};
    ProbeResult result;
};

std::mutex& cacheMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, CacheEntry>& successfulCache() {
    static std::unordered_map<std::string, CacheEntry> entries;
    return entries;
}

std::string cachePathKey(const std::string& path) {
    return path;
}

#if defined(_WIN32)

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle = nullptr) : handle_(handle) {}
    ~UniqueHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    HANDLE get() const { return handle_; }
    HANDLE release() {
        HANDLE value = handle_;
        handle_ = nullptr;
        return value;
    }

private:
    HANDLE handle_ = nullptr;
};

void terminateAndReap(HANDLE process, UniqueHandle& job) {
    TerminateProcess(process, 1u);
    TerminateJobObject(job.get(), 1u);
    if (WaitForSingleObject(process, 5000u) == WAIT_OBJECT_0) {
        return;
    }
    // Closing a KILL_ON_JOB_CLOSE job is the final containment step. Wait once
    // more for that specific helper before releasing its process handle.
    HANDLE jobHandle = job.release();
    if (jobHandle != nullptr) {
        CloseHandle(jobHandle);
    }
    WaitForSingleObject(process, 5000u);
}

bool utf8ToWide(const std::string& utf8, std::wstring& wide) {
    if (utf8.empty()) {
        wide.clear();
        return true;
    }
    if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) {
        return false;
    }
    wide.resize(static_cast<std::size_t>(size));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                               static_cast<int>(utf8.size()), wide.data(), size) == size;
}

bool currentExecutablePath(std::wstring& path) {
    std::vector<wchar_t> buffer(512u);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return false;
        }
        if (length < buffer.size() - 1u) {
            path.assign(buffer.data(), length);
            return true;
        }
        if (buffer.size() >= 32768u) {
            return false;
        }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2u, 32768u));
    }
}

int runIsolatedProbe(const std::string& utf8Path, unsigned timeoutMs, std::string& error) {
    std::wstring executable;
    std::wstring path;
    if (!currentExecutablePath(executable) || !utf8ToWide(utf8Path, path)) {
        error = i18n::tr("font.child_path");
        return -1;
    }

    std::wstring command = testing::quoteWindowsArgument(executable);
    command.append(L" --neo-font-probe ");
    command.append(testing::quoteWindowsArgument(path));
    if (command.size() >= 32767u) {
        error = i18n::tr("font.command_limit");
        return -1;
    }

    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (job.get() == nullptr) {
        error = i18n::tr("font.job_create");
        return -1;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                               JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(256u * 1024u * 1024u);
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        error = i18n::tr("font.job_limit");
        return -1;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        flags, nullptr, nullptr, &startup, &process)) {
        error = i18n::tr("font.child_create");
        return -1;
    }
    UniqueHandle processHandle(process.hProcess);
    UniqueHandle threadHandle(process.hThread);

    if (!AssignProcessToJobObject(job.get(), processHandle.get())) {
        TerminateProcess(processHandle.get(), 1u);
        WaitForSingleObject(processHandle.get(), 5000u);
        error = i18n::tr("font.job_assign");
        return -1;
    }
    if (ResumeThread(threadHandle.get()) == static_cast<DWORD>(-1)) {
        terminateAndReap(processHandle.get(), job);
        error = i18n::tr("font.thread_start");
        return -1;
    }

    const DWORD waitMs = timeoutMs == 0u ? 1u : timeoutMs;
    const DWORD waitResult = WaitForSingleObject(processHandle.get(), waitMs);
    if (waitResult == WAIT_TIMEOUT) {
        terminateAndReap(processHandle.get(), job);
        error = i18n::tr("font.timeout");
        return -1;
    }
    if (waitResult != WAIT_OBJECT_0) {
        terminateAndReap(processHandle.get(), job);
        error = i18n::tr("font.child_wait");
        return -1;
    }
    DWORD exitCode = 1u;
    if (!GetExitCodeProcess(processHandle.get(), &exitCode)) {
        error = i18n::tr("font.child_result");
        return -1;
    }
    return static_cast<int>(exitCode);
}

#endif // _WIN32

} // namespace

namespace testing {

std::string encodeSessionMarker(const SessionFonts& fonts) {
    std::string output(kMarkerHeader);
    const std::array<const std::string*, 3> values = {&fonts.editor, &fonts.ui, &fonts.code};
    for (const std::string* value : values) {
        if (value->size() > kMaximumMarkerPathBytes) {
            return {};
        }
        output.append(std::to_string(value->size()));
        output.push_back('\n');
        output.append(*value);
        output.push_back('\n');
    }
    if (output.size() > kMaximumMarkerBytes) {
        return {};
    }
    return output;
}

bool decodeSessionMarker(std::string_view marker, SessionFonts& fonts) {
    if (marker.size() > kMaximumMarkerBytes || marker.substr(0, sizeof(kMarkerHeader) - 1u) != kMarkerHeader) {
        return false;
    }
    std::size_t cursor = sizeof(kMarkerHeader) - 1u;
    std::array<std::string*, 3> outputs = {&fonts.editor, &fonts.ui, &fonts.code};
    for (std::string* output : outputs) {
        const std::size_t newline = marker.find('\n', cursor);
        if (newline == std::string_view::npos || newline == cursor) {
            return false;
        }
        std::uint64_t length = 0;
        const char* begin = marker.data() + cursor;
        const char* end = marker.data() + newline;
        const auto parsed = std::from_chars(begin, end, length);
        if (parsed.ec != std::errc{} || parsed.ptr != end || length > kMaximumMarkerPathBytes) {
            return false;
        }
        cursor = newline + 1u;
        if (length > marker.size() - cursor) {
            return false;
        }
        output->assign(marker.data() + cursor, static_cast<std::size_t>(length));
        cursor += static_cast<std::size_t>(length);
        if (cursor >= marker.size() || marker[cursor] != '\n') {
            return false;
        }
        ++cursor;
    }
    return cursor == marker.size();
}

std::wstring quoteWindowsArgument(std::wstring_view argument) {
    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2u + 1u, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2u, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

} // namespace testing

ProbeResult validate(const std::string& utf8Path, unsigned timeoutMs) {
    if (utf8Path.empty()) {
        return {true, false, {}};
    }

    FileIdentity identity;
    std::string error;
    if (!inspectFile(utf8Path, identity, error)) {
#if !defined(_WIN32)
        return {false, false, i18n::format("font.in_process_error", {{"error", error}})};
#else
        return {false, false, error};
#endif
    }
    if (!supportedExtension(utf8Path)) {
#if !defined(_WIN32)
        return {false, false, i18n::tr("font.in_process_types")};
#else
        return {false, false, i18n::tr("font.types")};
#endif
    }

    const std::string key = cachePathKey(utf8Path);
    {
        std::scoped_lock lock(cacheMutex());
        const auto found = successfulCache().find(key);
        if (found != successfulCache().end() && found->second.size == identity.size &&
            found->second.modified == identity.modified) {
            return found->second.result;
        }
    }

    ProbeResult result;
#if defined(_WIN32)
    const int exitCode = runIsolatedProbe(utf8Path, timeoutMs, error);
    if (exitCode == 0 || exitCode == 2) {
        result.ok = true;
        result.monospace = exitCode == 0;
    } else {
        result.ok = false;
        result.error = error.empty() ? i18n::tr("font.validation_failed") : std::move(error);
    }
#else
    const FontProbe probed = probeFont(utf8Path);
    result.ok = probed.error.empty();
    result.monospace = probed.monospace;
    result.error = i18n::tr("font.in_process");
    if (!probed.error.empty()) {
        result.error.append(probed.error);
    }
    (void)timeoutMs;
#endif

    if (result.ok) {
        std::scoped_lock lock(cacheMutex());
        successfulCache()[key] = CacheEntry{identity.size, identity.modified, result};
    }
    return result;
}

int probeCommandLineIfRequested() {
#if defined(_WIN32)
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        return -1;
    }
    const bool requested = argumentCount >= 2 && std::wstring_view(arguments[1]) == L"--neo-font-probe";
    int exitCode = -1;
    if (requested) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        if (argumentCount != 3) {
            exitCode = 1;
        } else {
            try {
                const std::string path = textfile::pathToUtf8(fs::path(arguments[2]));
                const FontProbe result = probeFont(path);
                exitCode = result.error.empty() ? (result.monospace ? 0 : 2) : 1;
            } catch (const std::exception&) {
                exitCode = 1;
            }
        }
    }
    LocalFree(arguments);
    return exitCode;
#else
    return -1;
#endif
}

bool armSession(const settings::Data& data, std::string& error) {
    error.clear();
    if (recoveryWritePending.load()) {
        error = i18n::tr("font.recovery_unsaved");
        return false;
    }
    const testing::SessionFonts fonts = sessionFonts(data);
    if (!hasCustomFonts(fonts)) {
        return removeMarker(error);
    }
    const std::string marker = testing::encodeSessionMarker(fonts);
    if (marker.empty()) {
        error = i18n::tr("font.guard_path_limit");
        return false;
    }
    try {
        if (!atomicwrite::writeFile(sessionMarkerPath(), marker)) {
            error = i18n::tr("font.guard_save");
            return false;
        }
    } catch (const std::exception&) {
        error = i18n::tr("font.guard_access");
        return false;
    }
    return true;
}

bool recoverSession(settings::Data& data, std::string& message) {
    message.clear();
    std::string marker;
    try {
        const fs::path path = sessionMarkerPath();
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            if (ec) {
                message = i18n::tr("font.guard_check");
            }
            return false;
        }
        if (ec) {
            message = i18n::tr("font.guard_read");
            return false;
        }
        const std::uintmax_t size = fs::file_size(path, ec);
        if (ec || size > kMaximumMarkerBytes) {
            message = i18n::tr("font.guard_invalid");
            return removeMarker(message);
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            message = i18n::tr("font.guard_read");
            return false;
        }
        marker.resize(static_cast<std::size_t>(size));
        input.read(marker.data(), static_cast<std::streamsize>(marker.size()));
        if (!input || static_cast<std::size_t>(input.gcount()) != marker.size()) {
            message = i18n::tr("font.guard_partial");
            return false;
        }
    } catch (const std::exception&) {
        message = i18n::tr("font.guard_access");
        return false;
    }

    testing::SessionFonts previous;
    if (!testing::decodeSessionMarker(marker, previous)) {
        message = i18n::tr("font.guard_ignored");
        return removeMarker(message);
    }

    bool changed = false;
    if (!previous.editor.empty() && data.editorFontFile == previous.editor) {
        data.editorFontFile.clear();
        changed = true;
    }
    if (!previous.ui.empty() && data.uiFontFile == previous.ui) {
        data.uiFontFile.clear();
        changed = true;
    }
    if (!previous.code.empty() && data.codeFontFile == previous.code) {
        data.codeFontFile.clear();
        changed = true;
    }

    if (changed || recoveryWritePending.load()) {
        if (&data != &settings::current()) {
            message = i18n::tr("font.recovery_current");
            return false;
        }
        if (!settings::flush()) {
            recoveryWritePending.store(true);
            message = i18n::tr("font.recovery_save");
            return false;
        }
        message = i18n::tr("font.recovered");
    }

    std::string removeError;
    if (!removeMarker(removeError)) {
        if (message.empty()) {
            message = std::move(removeError);
        } else {
            message.append(" ").append(removeError);
        }
        return false;
    }
    recoveryWritePending.store(false);
    return true;
}

void cleanSession() {
    if (recoveryWritePending.load()) return;
    std::string ignored;
    removeMarker(ignored);
}

} // namespace neo::fontsafety
