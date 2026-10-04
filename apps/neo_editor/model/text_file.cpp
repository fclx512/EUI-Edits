#include "model/i18n.h"
#include "model/text_file.h"

#include "model/atomic_write.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neo::textfile {
namespace {

void appendUtf8(std::string& out, unsigned int codePoint) {
    if (codePoint < 0x80u) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else if (codePoint < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    }
}

void appendUtf16(std::string& out, unsigned int codePoint, bool littleEndian) {
    const auto push = [littleEndian, &out](unsigned int unit) {
        if (littleEndian) {
            out.push_back(static_cast<char>(unit & 0xFFu));
            out.push_back(static_cast<char>((unit >> 8) & 0xFFu));
        } else {
            out.push_back(static_cast<char>((unit >> 8) & 0xFFu));
            out.push_back(static_cast<char>(unit & 0xFFu));
        }
    };
    if (codePoint >= 0x10000u) {
        const unsigned int adjusted = codePoint - 0x10000u;
        push(0xD800u + (adjusted >> 10));
        push(0xDC00u + (adjusted & 0x3FFu));
    } else {
        push(codePoint);
    }
}

// 严格 UTF-16 解码：奇数字节、孤立代理对一律失败，不静默替换。
bool decodeUtf16(const char* data, std::size_t size, bool littleEndian, std::string& out, std::string& error) {
    if (size % 2 != 0) {
        error = i18n::tr("text.utf16_odd");
        return false;
    }
    const auto unitAt = [littleEndian](const char* bytes, std::size_t index) {
        const auto low = static_cast<unsigned int>(static_cast<unsigned char>(bytes[index * 2]));
        const auto high = static_cast<unsigned int>(static_cast<unsigned char>(bytes[index * 2 + 1]));
        return static_cast<unsigned int>(littleEndian ? (low | (high << 8)) : ((low << 8) | high));
    };

    // 注意：调用方允许 data 与 out 是同一缓冲区（load 直接原地解码）。
    // 这里不能 out.clear()——MSVC 的 clear() 会在位置 0 写 '\0' 终结符，会把
    // data[0] 清掉；结尾的 move 赋值本来就会整体替换 out，无需预清理。
    std::string built;
    built.reserve(size);
    const std::size_t units = size / 2;
    for (std::size_t index = 0; index < units; ++index) {
        unsigned int unit = unitAt(data, index);
        if (unit >= 0xD800u && unit <= 0xDBFFu) {
            if (index + 1 >= units) {
                error = i18n::tr("text.utf16_high");
                return false;
            }
            const unsigned int low = unitAt(data, index + 1);
            if (low < 0xDC00u || low > 0xDFFFu) {
                error = i18n::tr("text.utf16_high");
                return false;
            }
            appendUtf8(built, 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u));
            ++index;
        } else if (unit >= 0xDC00u && unit <= 0xDFFFu) {
            error = i18n::tr("text.utf16_low");
            return false;
        } else {
            appendUtf8(built, unit);
        }
    }
    out = std::move(built);
    return true;
}

// text 按约定是合法 UTF-8；这里防御式解码，坏字节替换为 U+FFFD，保证函数全定义。
std::string utf8ToUtf16(const std::string& text, bool littleEndian) {
    std::string out;
    out.reserve(text.size() * 2);
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t trailing = 0;
        unsigned int codePoint = 0;
        if (lead < 0x80u) {
            appendUtf16(out, lead, littleEndian);
            ++index;
            continue;
        } else if ((lead & 0xE0u) == 0xC0u) {
            trailing = 1;
            codePoint = lead & 0x1Fu;
        } else if ((lead & 0xF0u) == 0xE0u) {
            trailing = 2;
            codePoint = lead & 0x0Fu;
        } else if ((lead & 0xF8u) == 0xF0u) {
            trailing = 3;
            codePoint = lead & 0x07u;
        } else {
            appendUtf16(out, 0xFFFDu, littleEndian);
            ++index;
            continue;
        }
        if (index + trailing >= text.size()) {
            appendUtf16(out, 0xFFFDu, littleEndian);
            ++index;
            continue;
        }
        bool valid = true;
        for (std::size_t offset = 1; offset <= trailing; ++offset) {
            const auto byte = static_cast<unsigned char>(text[index + offset]);
            if ((byte & 0xC0u) != 0x80u) {
                valid = false;
                break;
            }
            codePoint = (codePoint << 6) | (byte & 0x3Fu);
        }
        const std::size_t minimum = trailing == 1 ? 0x80u : (trailing == 2 ? 0x800u : 0x10000u);
        if (!valid || codePoint < minimum || codePoint > 0x10FFFFu || (codePoint >= 0xD800u && codePoint <= 0xDFFFu)) {
            appendUtf16(out, 0xFFFDu, littleEndian);
            ++index;
            continue;
        }
        appendUtf16(out, codePoint, littleEndian);
        index += trailing + 1;
    }
    return out;
}

bool isValidUtf8(const std::string& value) {
    std::size_t index = 0;
    while (index < value.size()) {
        const auto lead = static_cast<unsigned char>(value[index]);
        std::size_t trailing = 0;
        unsigned int codePoint = 0;
        if (lead < 0x80u) {
            ++index;
            continue;
        } else if ((lead & 0xE0u) == 0xC0u) {
            trailing = 1;
            codePoint = lead & 0x1Fu;
        } else if ((lead & 0xF0u) == 0xE0u) {
            trailing = 2;
            codePoint = lead & 0x0Fu;
        } else if ((lead & 0xF8u) == 0xF0u) {
            trailing = 3;
            codePoint = lead & 0x07u;
        } else {
            return false;
        }
        if (index + trailing >= value.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= trailing; ++offset) {
            const auto byte = static_cast<unsigned char>(value[index + offset]);
            if ((byte & 0xC0u) != 0x80u) {
                return false;
            }
            codePoint = (codePoint << 6) | (byte & 0x3Fu);
        }
        const std::size_t minimum = trailing == 1 ? 0x80u : (trailing == 2 ? 0x800u : 0x10000u);
        if (codePoint < minimum || codePoint > 0x10FFFFu) {
            return false;
        }
        if (codePoint >= 0xD800u && codePoint <= 0xDFFFu) {
            return false;
        }
        index += trailing + 1;
    }
    return true;
}

#if defined(_WIN32)

// 宽松 ANSI 解码（仅自动检测路径用）：非法字节走系统默认替换，保持既有打开行为。
std::string ansiToUtf8(const std::string& value, unsigned int codePage) {
    if (value.empty()) {
        return {};
    }
    const int wideLength = MultiByteToWideChar(static_cast<UINT>(codePage), 0, value.data(),
                                               static_cast<int>(value.size()), nullptr, 0);
    if (wideLength <= 0) {
        return value;
    }
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    MultiByteToWideChar(static_cast<UINT>(codePage), 0, value.data(), static_cast<int>(value.size()),
                        wide.data(), wideLength);

    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) {
        return value;
    }
    std::string out(static_cast<std::size_t>(utf8Length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, out.data(), utf8Length, nullptr, nullptr);
    return out;
}

// 严格 ANSI 解码（"以编码重新打开"用）：解释不了的字节直接报错。
bool decodeAnsiStrict(const std::string& value, unsigned int codePage, std::string& out, std::string& error) {
    if (value.empty()) {
        out.clear();
        return true;
    }
    const int wideLength = MultiByteToWideChar(static_cast<UINT>(codePage), MB_ERR_INVALID_CHARS, value.data(),
                                               static_cast<int>(value.size()), nullptr, 0);
    if (wideLength <= 0) {
        error = i18n::tr("text.decode_failed");
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    MultiByteToWideChar(static_cast<UINT>(codePage), MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), wide.data(), wideLength);
    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0) {
        error = i18n::tr("text.convert_failed");
        return false;
    }
    out.assign(static_cast<std::size_t>(utf8Length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, out.data(), utf8Length, nullptr, nullptr);
    return true;
}

// 严格 ANSI 编码（保存还原用）：遇到该编码无法表示的字符整体失败；
// 编码成功后还按同页码严格回解并逐字符比对，best-fit 映射同样算失败。
bool encodeAnsiStrict(const std::string& utf8, unsigned int codePage, std::string& out, std::string& error) {
    if (utf8.empty()) {
        out.clear();
        return true;
    }
    const int wideLength =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wideLength <= 0) {
        error = i18n::tr("text.invalid_utf8");
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), wideLength);

    BOOL usedDefaultChar = FALSE;
    const int targetLength = WideCharToMultiByte(static_cast<UINT>(codePage), WC_NO_BEST_FIT_CHARS, wide.data(),
                                                 wideLength, nullptr, 0, nullptr, &usedDefaultChar);
    if (targetLength <= 0) {
        error = i18n::tr("text.unrepresentable");
        return false;
    }
    usedDefaultChar = FALSE;
    std::string encoded(static_cast<std::size_t>(targetLength), '\0');
    WideCharToMultiByte(static_cast<UINT>(codePage), WC_NO_BEST_FIT_CHARS, wide.data(), wideLength, encoded.data(),
                        targetLength, nullptr, &usedDefaultChar);
    if (usedDefaultChar) {
        error = i18n::tr("text.unrepresentable");
        return false;
    }

    // 回解比对：GBK 的 best-fit 映射（如全角引号→半角）不置 usedDefaultChar，
    // 只能靠往返一致性拦住。
    const int backLength = MultiByteToWideChar(static_cast<UINT>(codePage), MB_ERR_INVALID_CHARS, encoded.data(),
                                               targetLength, nullptr, 0);
    if (backLength != wideLength) {
        error = i18n::tr("text.lossy");
        return false;
    }
    std::wstring back(static_cast<std::size_t>(backLength), L'\0');
    MultiByteToWideChar(static_cast<UINT>(codePage), MB_ERR_INVALID_CHARS, encoded.data(), targetLength,
                        back.data(), backLength);
    if (back != wide) {
        error = i18n::tr("text.lossy");
        return false;
    }
    out = std::move(encoded);
    return true;
}
#endif

// 把可能的 CRLF / CR 统一成 LF，并记录原文件的换行风格。
void normalizeNewlines(std::string& value, LineEnding& lineEnding) {
    const std::size_t crlf = value.find("\r\n");
    const std::size_t loneCr = value.find('\r');
    if (crlf != std::string::npos) {
        lineEnding = LineEnding::CrLf;
    } else {
        lineEnding = LineEnding::Lf;
    }
    if (loneCr == std::string::npos) {
        return;
    }

    std::string normalized;
    normalized.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char current = value[index];
        if (current == '\r') {
            normalized.push_back('\n');
            if (index + 1 < value.size() && value[index + 1] == '\n') {
                ++index;
            }
        } else {
            normalized.push_back(current);
        }
    }
    value = std::move(normalized);
}

// 保存前按记录的换行风格展开（与 normalizeNewlines 的统一规则互逆；单独 CR
// 载入时已归一成 LF，保存也写 LF——混合换行文件不做字节级保真，见需求文档）。
std::string expandNewlines(const std::string& text, LineEnding lineEnding) {
    if (lineEnding == LineEnding::Lf || text.find('\n') == std::string::npos) {
        return text;
    }
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (const char character : text) {
        if (character == '\n') {
            out.append("\r\n");
        } else {
            out.push_back(character);
        }
    }
    return out;
}

bool looksBinary(const std::string& value) {
    const std::size_t probe = std::min<std::size_t>(value.size(), 8192u);
    for (std::size_t index = 0; index < probe; ++index) {
        if (value[index] == '\0') {
            return true;
        }
    }
    return false;
}

std::string toLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(character >= 'A' && character <= 'Z' ? character + 32 : character);
    });
    return value;
}

// 读出整份字节内容（load 与 loadWithEncoding 共用）。
bool readFileBytes(const std::string& path, std::size_t maxBytes, std::string& bytes, std::string& error) {
    std::error_code fileError;
    const auto filePath = pathFromUtf8(path);
    const auto size = std::filesystem::file_size(filePath, fileError);
    if (fileError) {
        error = i18n::format("text.access_failed", {{"error", fileError.message()}});
        return false;
    }
    if (size > maxBytes) {
        error = i18n::tr("text.too_large");
        return false;
    }

    std::ifstream input(filePath, std::ios::binary);
    if (!input) {
        error = i18n::tr("text.open_failed");
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad()) {
        error = i18n::tr("text.read_failed");
        return false;
    }
    return true;
}

enum class Bom { None, Utf8, Utf16Le, Utf16Be };

Bom detectBom(const std::string& bytes) {
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEFu &&
        static_cast<unsigned char>(bytes[1]) == 0xBBu && static_cast<unsigned char>(bytes[2]) == 0xBFu) {
        return Bom::Utf8;
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFFu &&
        static_cast<unsigned char>(bytes[1]) == 0xFEu) {
        return Bom::Utf16Le;
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFEu &&
        static_cast<unsigned char>(bytes[1]) == 0xFFu) {
        return Bom::Utf16Be;
    }
    return Bom::None;
}

} // namespace

std::string pathToUtf8(const std::filesystem::path& value) {
    return value.u8string();
}

std::filesystem::path pathFromUtf8(const std::string& value) {
    return std::filesystem::u8path(value);
}

std::string environmentPathUtf8(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return {};
    }
#if defined(_WIN32)
    // 走宽字符 API：std::getenv 给的是 ANSI（GBK）字节，当 UTF-8 用会抛异常。
    // 名字都是 ASCII 字面量，逐字节加宽即可。
    std::wstring wideName;
    wideName.reserve(32);
    for (const char* p = name; *p != '\0'; ++p) {
        wideName.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    }
    std::wstring value(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetEnvironmentVariableW(wideName.c_str(), value.data(),
                                                     static_cast<DWORD>(value.size()));
        if (length == 0) {
            return {};  // 未设置 / 读不到
        }
        if (length < value.size()) {
            value.resize(length);
            break;
        }
        value.resize(static_cast<std::size_t>(length) + 1);  // 被截断：按所需长度重来
    }
    return pathToUtf8(std::filesystem::path(value));
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
#endif
}

std::string encodingLabel(const Document& document) {
    switch (document.encoding) {
        case Encoding::Utf8:
            return "UTF-8";
        case Encoding::Utf16Le:
            return "UTF-16 LE";
        case Encoding::Utf16Be:
            return "UTF-16 BE";
        case Encoding::Ansi:
            break;
    }
    if (document.ansiCodePage == 936u) {
        return "GBK";
    }
    if (document.ansiCodePage == 950u) {
        return "Big5";
    }
    if (document.ansiCodePage == 932u) {
        return "Shift-JIS";
    }
    return "ANSI (" + std::to_string(document.ansiCodePage) + ")";
}

LoadResult load(const std::string& path, std::size_t maxBytes) {
    LoadResult result;

    std::string bytes;
    if (!readFileBytes(path, maxBytes, bytes, result.error)) {
        return result;
    }

    Document& document = result.document;
    const Bom bom = detectBom(bytes);
    switch (bom) {
        case Bom::Utf8:
            document.hadBom = true;
            document.encoding = Encoding::Utf8;
            bytes.erase(0, 3);
            break;
        case Bom::Utf16Le:
            document.hadBom = true;
            document.encoding = Encoding::Utf16Le;
            bytes.erase(0, 2);
            break;
        case Bom::Utf16Be:
            document.hadBom = true;
            document.encoding = Encoding::Utf16Be;
            bytes.erase(0, 2);
            break;
        case Bom::None:
            break;
    }

    if (document.encoding == Encoding::Utf16Le || document.encoding == Encoding::Utf16Be) {
        std::string error;
        if (!decodeUtf16(bytes.data(), bytes.size(), document.encoding == Encoding::Utf16Le, bytes, error)) {
            result.error = error;
            return result;
        }
    }

    if (looksBinary(bytes)) {
        result.error = i18n::tr("text.binary");
        return result;
    }

    if (!isValidUtf8(bytes)) {
        // 没有 BOM 又不是合法 UTF-8：在 Windows 上按本地代码页（简体中文即 GBK）解码，
        // 这是 Windows 记事本遗留文本的常见形态。页码在加载时固化，保存时按它还原。
        // 其他平台只能原样显示。
#if defined(_WIN32)
        document.encoding = Encoding::Ansi;
        document.ansiCodePage = GetACP();
        bytes = ansiToUtf8(bytes, document.ansiCodePage);
        if (!isValidUtf8(bytes)) {
            result.error = i18n::tr("text.unknown_encoding");
            return result;
        }
#else
        result.error = i18n::tr("text.non_utf8");
        return result;
#endif
    }

    normalizeNewlines(bytes, document.lineEnding);
    document.text = std::move(bytes);
    result.ok = true;
    return result;
}

LoadResult loadWithEncoding(const std::string& path, const ForcedEncoding& forced, std::size_t maxBytes) {
    LoadResult result;

    std::string bytes;
    if (!readFileBytes(path, maxBytes, bytes, result.error)) {
        return result;
    }

    Document& document = result.document;
    document.encoding = forced.encoding;
    if (forced.encoding == Encoding::Ansi) {
        document.ansiCodePage = forced.codePage;
    }

    const Bom bom = detectBom(bytes);
    switch (bom) {
        case Bom::Utf8:
            if (forced.encoding != Encoding::Utf8) {
                result.error = i18n::tr("text.bom_utf8");
                return result;
            }
            document.hadBom = true;
            bytes.erase(0, 3);
            break;
        case Bom::Utf16Le:
            if (forced.encoding != Encoding::Utf16Le) {
                result.error = i18n::tr("text.bom_utf16le");
                return result;
            }
            document.hadBom = true;
            bytes.erase(0, 2);
            break;
        case Bom::Utf16Be:
            if (forced.encoding != Encoding::Utf16Be) {
                result.error = i18n::tr("text.bom_utf16be");
                return result;
            }
            document.hadBom = true;
            bytes.erase(0, 2);
            break;
        case Bom::None:
            break;
    }

    if (forced.encoding == Encoding::Utf16Le || forced.encoding == Encoding::Utf16Be) {
        std::string error;
        if (!decodeUtf16(bytes.data(), bytes.size(), forced.encoding == Encoding::Utf16Le, bytes, error)) {
            result.error = error;
            return result;
        }
    } else if (forced.encoding == Encoding::Utf8) {
        if (looksBinary(bytes)) {
            result.error = i18n::tr("text.binary");
            return result;
        }
        if (!isValidUtf8(bytes)) {
            result.error = i18n::tr("text.decode_utf8");
            return result;
        }
    }
#if defined(_WIN32)
    else if (forced.encoding == Encoding::Ansi) {
        std::string error;
        if (!decodeAnsiStrict(bytes, forced.codePage, bytes, error)) {
            result.error = error;
            return result;
        }
        if (looksBinary(bytes)) {
            result.error = i18n::tr("text.binary");
            return result;
        }
    }
#else
    else if (forced.encoding == Encoding::Ansi) {
        result.error = i18n::tr("text.ansi_reopen");
        return result;
    }
#endif

    normalizeNewlines(bytes, document.lineEnding);
    document.text = std::move(bytes);
    result.ok = true;
    return result;
}

bool save(const std::string& path, const Document& document, std::string& error, const std::function<bool()>& beforeReplace) {
    const std::string body = expandNewlines(document.text, document.lineEnding);

    std::string payload;
    switch (document.encoding) {
        case Encoding::Utf8:
            payload.reserve(body.size() + 3);
            if (document.hadBom) {
                payload.append("\xEF\xBB\xBF");
            }
            payload.append(body);
            break;
        case Encoding::Utf16Le:
        case Encoding::Utf16Be: {
            const bool littleEndian = document.encoding == Encoding::Utf16Le;
            const std::string units = utf8ToUtf16(body, littleEndian);
            payload.reserve(units.size() + 2);
            if (document.hadBom) {
                appendUtf16(payload, 0xFEFFu, littleEndian);
            }
            payload.append(units);
            break;
        }
        case Encoding::Ansi:
#if defined(_WIN32)
            if (!encodeAnsiStrict(body, document.ansiCodePage, payload, error)) {
                return false;
            }
#else
            error = i18n::tr("text.ansi_save");
            return false;
#endif
            break;
    }

    // 编码全部在内存完成才落盘：任何编码失败都发生在动目标文件之前。
    if (!atomicwrite::writeFile(pathFromUtf8(path), payload, beforeReplace)) {
        error = i18n::tr("text.save_failed");
        return false;
    }
    return true;
}

bool isMarkdown(const std::string& path) {
    const std::string extension = extensionLower(path);
    return extension == "md" || extension == "markdown" || extension == "mdx" || extension == "mkd";
}

std::string fileName(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    return pathToUtf8(pathFromUtf8(path).filename());
}

std::string parentPath(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    return pathToUtf8(pathFromUtf8(path).parent_path());
}

std::string extensionLower(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    std::string extension = pathToUtf8(pathFromUtf8(path).extension());
    if (!extension.empty() && extension.front() == '.') {
        extension.erase(extension.begin());
    }
    return toLowerAscii(extension);
}

Stats measure(const std::string& text) {
    Stats stats;
    stats.lines = 1;
    std::size_t index = 0;
    while (index < text.size()) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte == '\n') {
            ++stats.lines;
        }
        if ((byte & 0xC0u) != 0x80u) {
            ++stats.characters;
        }
        ++index;
    }
    return stats;
}

} // namespace neo::textfile
