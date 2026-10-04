#include "model/session_storage.h"

#include "eui/json.h"
#include "model/atomic_write.h"
#include "model/settings.h"
#include "model/text_file.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace neo::sessionstorage {
namespace {

namespace fs = std::filesystem;

// v2：正文按内容寻址（body-<contentId>.utf8），未变正文直接复用已提交 body，只写变化页。
// v1（generation + body-<generation>-<id>.utf8）仍可只读兼容，迁移后由 cleanup 清掉。
constexpr std::uint64_t kSchemaVersion = 2;
constexpr std::uint64_t kSchemaVersionV1 = 1;
constexpr std::size_t kMaximumTabs = 256;
constexpr std::size_t kMaximumManifestBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaximumStringBytes = 1024u * 1024u;
constexpr std::size_t kMaximumLanguageBytes = 128u;
constexpr std::uintmax_t kMaximumDocumentBytes = 256u * 1024u * 1024u;
constexpr std::uintmax_t kMaximumSessionDocumentBytes = 1024ull * 1024ull * 1024ull;
constexpr char kManifestName[] = "manifest.json";
constexpr char kBodyPrefix[] = "body-";
constexpr char kBodySuffix[] = ".utf8";

std::mutex& writeMutex() {
    static std::mutex mutex;
    return mutex;
}

fs::path sessionDirectory() {
    return textfile::pathFromUtf8(settings::configDirectory()) / "session";
}

// 正文内容标识：两路独立 64 位哈希拼成 32 位十六进制，作为不可变 body 的文件名。
// 内容相同 → 名字相同 → 已提交的 body 直接复用，不重复写盘（未变正文零字节写入）。
std::string contentId(const std::string& text) {
    std::uint64_t h1 = 14695981039346656037ull;
    std::uint64_t h2 = 0x9e3779b97f4a7c15ull;
    for (const unsigned char byte : text) {
        h1 = (h1 ^ byte) * 1099511628211ull;
        h2 ^= byte + 0x9e3779b97f4a7c15ull + (h2 << 6) + (h2 >> 2);
    }
    h1 ^= static_cast<std::uint64_t>(text.size()) * 0x9e3779b97f4a7c15ull;
    h2 += static_cast<std::uint64_t>(text.size()) * 0xc2b2ae3d27d4eb4full;
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%016llx%016llx",
                  static_cast<unsigned long long>(h1), static_cast<unsigned long long>(h2));
    return std::string(buffer, 32);
}

std::string bodyNameFromId(const std::string& id) {
    return std::string(kBodyPrefix) + id + kBodySuffix;
}

std::string legacyBodyName(const std::string& generation, TabId id) {
    return std::string(kBodyPrefix) + generation + "-" + std::to_string(id) + kBodySuffix;
}

bool isHex(char c);

// v2 body 名严格形状：body-<32hex>.utf8（不接受 v1 的 -<digits> 尾巴）。
bool isV2BodyName(const std::string& name) {
    const std::size_t prefix = sizeof(kBodyPrefix) - 1;
    const std::size_t suffix = sizeof(kBodySuffix) - 1;
    return name.size() == prefix + 32 + suffix && name.compare(0, prefix, kBodyPrefix) == 0 &&
           name.compare(name.size() - suffix, suffix, kBodySuffix) == 0 &&
           std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix),
                       name.begin() + static_cast<std::ptrdiff_t>(prefix + 32), isHex);
}

bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool isHex32(const std::string& value) {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), isHex);
}

bool validGeneration(const std::string& value) {
    return isHex32(value);
}

// v2：body-<32hex>.utf8；v1：body-<32hex>-<digits>.utf8。只在这两种受控命名空间内认领文件。
bool ownedBodyName(const std::string& name) {
    const std::size_t prefix = sizeof(kBodyPrefix) - 1;
    const std::size_t suffix = sizeof(kBodySuffix) - 1;
    if (name.size() < prefix + 32 + suffix || name.compare(0, prefix, kBodyPrefix) != 0 ||
        name.compare(name.size() - suffix, suffix, kBodySuffix) != 0) {
        return false;
    }
    if (!std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix),
                     name.begin() + static_cast<std::ptrdiff_t>(prefix + 32), isHex)) {
        return false;
    }
    const std::size_t after = prefix + 32;
    if (after == name.size() - suffix) {
        return true;  // v2 内容寻址名
    }
    if (name[after] != '-') {
        return false;
    }
    const auto idBegin = name.begin() + static_cast<std::ptrdiff_t>(after + 1);
    const auto idEnd = name.end() - static_cast<std::ptrdiff_t>(suffix);
    if (idBegin == idEnd) return false;
    if (*idBegin == '0' && idBegin + 1 != idEnd) return false;
    return std::all_of(idBegin, idEnd, [](char c) { return c >= '0' && c <= '9'; });
}

bool validUtf8(const std::string& value) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
    std::size_t i = 0;
    while (i < value.size()) {
        const unsigned char first = bytes[i++];
        if (first == 0) return false;
        if (first <= 0x7fu) continue;
        std::uint32_t codepoint = 0;
        std::size_t continuation = 0;
        if (first >= 0xc2u && first <= 0xdfu) {
            codepoint = first & 0x1fu; continuation = 1;
        } else if (first >= 0xe0u && first <= 0xefu) {
            codepoint = first & 0x0fu; continuation = 2;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            codepoint = first & 0x07u; continuation = 3;
        } else {
            return false;
        }
        if (continuation > value.size() - i) return false;
        for (std::size_t n = 0; n < continuation; ++n) {
            const unsigned char next = bytes[i++];
            if ((next & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (next & 0x3fu);
        }
        if ((continuation == 1 && codepoint < 0x80u) ||
            (continuation == 2 && codepoint < 0x800u) ||
            (continuation == 3 && codepoint < 0x10000u) ||
            codepoint > 0x10ffffu || (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
            return false;
        }
    }
    return true;
}

bool validRecordStrings(const std::string& path, const std::string& vaultRoot,
                        const std::string& language) {
    return path.size() <= kMaximumStringBytes && vaultRoot.size() <= kMaximumStringBytes &&
           language.size() <= kMaximumLanguageBytes && validUtf8(path) &&
           validUtf8(vaultRoot) && validUtf8(language);
}

void appendJsonString(std::string& output, const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char c : value) {
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (c < 0x20u) {
                    output += "\\u00";
                    output.push_back(hex[(c >> 4u) & 0xfu]);
                    output.push_back(hex[c & 0xfu]);
                } else {
                    output.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    output.push_back('"');
}

const char* encodingName(textfile::Encoding encoding) {
    switch (encoding) {
        case textfile::Encoding::Utf8: return "utf8";
        case textfile::Encoding::Utf16Le: return "utf16le";
        case textfile::Encoding::Utf16Be: return "utf16be";
        case textfile::Encoding::Ansi: return "ansi";
    }
    return nullptr;
}

bool parseEncoding(const std::string& name, textfile::Encoding& encoding) {
    if (name == "utf8") encoding = textfile::Encoding::Utf8;
    else if (name == "utf16le") encoding = textfile::Encoding::Utf16Le;
    else if (name == "utf16be") encoding = textfile::Encoding::Utf16Be;
    else if (name == "ansi") encoding = textfile::Encoding::Ansi;
    else return false;
    return true;
}

bool readFile(const fs::path& path, std::uintmax_t maximum, std::string& output) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error || size > maximum || size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (!bytes.empty()) input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) return false;
    output = std::move(bytes);
    return true;
}

bool jsonString(const eui::json::Value& value, std::size_t maximum, std::string& out) {
    return value.type() == eui::json::Type::String && value.string(out) &&
           out.size() <= maximum && validUtf8(out);
}

bool jsonUnsigned(const eui::json::Value& value, std::uint64_t& out) {
    return value.valid() && value.unsignedInteger(out);
}

bool jsonSmallSigned(const eui::json::Value& value, std::int64_t& out) {
    if (value.signedInteger(out)) return true;
    std::uint64_t unsignedValue = 0;
    if (!value.unsignedInteger(unsignedValue) ||
        unsignedValue > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    out = static_cast<std::int64_t>(unsignedValue);
    return true;
}

// v2 清单：dirty 记录引用内容寻址的 body（body 字段 + 字节数），不再有 generation。
bool writeManifest(const fs::path& path, const std::vector<WriteRecord>& records,
                   TabId activeId, const std::vector<std::string>& bodyNames) {
    std::string output;
    output.reserve(256 + records.size() * 320);
    output += "{\"version\":2,\"active\":" + std::to_string(activeId) + ",\"records\":[";
    bool first = true;
    for (std::size_t index = 0; index < records.size(); ++index) {
        const WriteRecord& record = records[index];
        if (!first) output.push_back(',');
        first = false;
        output += "{\"id\":" + std::to_string(record.id) + ",\"path\":";
        appendJsonString(output, record.path);
        output += ",\"vaultRoot\":";
        appendJsonString(output, record.vaultRoot);
        output += ",\"language\":";
        appendJsonString(output, record.language);
        output += ",\"wrap\":" + std::to_string(record.wrapOverride) +
                  ",\"dirty\":" + std::string(record.dirty ? "true" : "false");
        if (record.dirty) {
            const textfile::Document& document = *record.document;
            const char* encoding = encodingName(document.encoding);
            output += ",\"encoding\":";
            appendJsonString(output, encoding != nullptr ? encoding : "");
            output += ",\"codepage\":" + std::to_string(document.ansiCodePage) +
                      ",\"bom\":" + std::string(document.hadBom ? "true" : "false") +
                      ",\"crlf\":" + std::string(document.lineEnding == textfile::LineEnding::CrLf ? "true" : "false") +
                      ",\"bytes\":" + std::to_string(document.text.size()) +
                      ",\"body\":";
            appendJsonString(output, index < bodyNames.size() ? bodyNames[index] : std::string{});
        }
        output.push_back('}');
        if (output.size() > kMaximumManifestBytes) return false;
    }
    output += "]}\n";
    if (output.size() > kMaximumManifestBytes) return false;
    return atomicwrite::writeFile(path, output);
}

bool parseManifest(const std::string& bytes, const fs::path& directory,
                   std::vector<ReadRecord>& records, TabId& activeId,
                   std::set<std::string>& bodyFiles) {
    eui::json::Document json;
    if (!json.parse(bytes) || !json.valid()) return false;
    const eui::json::Value root = json.root();
    if (root.type() != eui::json::Type::Object) return false;
    std::uint64_t version = 0, active = 0;
    if (!jsonUnsigned(root.get("version"), version) ||
        (version != kSchemaVersion && version != kSchemaVersionV1) ||
        !jsonUnsigned(root.get("active"), active)) {
        return false;
    }
    std::string generation;
    if (version == kSchemaVersionV1) {
        // v1 只读兼容：仍按 generation + body-<generation>-<id>.utf8 解析。
        if (root.size() != 4 || !jsonString(root.get("generation"), 32, generation) ||
            !validGeneration(generation)) {
            return false;
        }
    } else if (root.size() != 3) {
        return false;
    }
    const eui::json::Value array = root.get("records");
    if (array.type() != eui::json::Type::Array || array.size() > kMaximumTabs) return false;
    if ((array.size() == 0 && active != 0) || (array.size() != 0 && active == 0)) return false;

    std::vector<ReadRecord> parsed;
    parsed.reserve(array.size());
    std::set<TabId> ids;
    std::uintmax_t totalDocumentBytes = 0;
    bool activeFound = active == 0;
    for (std::size_t index = 0; index < array.size(); ++index) {
        const eui::json::Value item = array.at(index);
        if (item.type() != eui::json::Type::Object) return false;
        const std::size_t expectedDirtyFields = version == kSchemaVersionV1 ? 10 : 12;
        if (item.size() != 6 && item.size() != expectedDirtyFields) return false;
        ReadRecord record;
        std::uint64_t id = 0;
        std::int64_t wrap = 0;
        if (!jsonUnsigned(item.get("id"), id) || id == 0 ||
            id == std::numeric_limits<TabId>::max() || !ids.insert(id).second ||
            !jsonString(item.get("path"), kMaximumStringBytes, record.path) ||
            !jsonString(item.get("vaultRoot"), kMaximumStringBytes, record.vaultRoot) ||
            !jsonString(item.get("language"), kMaximumLanguageBytes, record.language) ||
            !jsonSmallSigned(item.get("wrap"), wrap) || wrap < -1 || wrap > 1 ||
            !item.get("dirty").boolean(record.dirty)) {
            return false;
        }
        record.id = id;
        record.wrapOverride = static_cast<int>(wrap);
        activeFound = activeFound || id == active;
        if (record.dirty) {
            if (item.size() != expectedDirtyFields) return false;
            std::string encodingNameValue;
            std::uint64_t codepage = 0;
            bool bom = false, crlf = false;
            // v2 清单为每个脏页记录正文字节数；读取时用它核对磁盘上的 body。
            // 截断/损坏的 body（尤其是被截成 0 字节）若被静默接受，会把一份非空草稿
            // 恢复成空草稿——这正是"缺 body 返回失败"要防的丢稿。v1 没有该字段。
            std::uint64_t declaredBytes = 0;
            if (version == kSchemaVersion &&
                (!jsonUnsigned(item.get("bytes"), declaredBytes) ||
                 declaredBytes > kMaximumDocumentBytes)) {
                return false;
            }
            if (!jsonString(item.get("encoding"), 16, encodingNameValue) ||
                !parseEncoding(encodingNameValue, record.document.encoding) ||
                !jsonUnsigned(item.get("codepage"), codepage) || codepage > 65535u ||
                !item.get("bom").boolean(bom) || !item.get("crlf").boolean(crlf)) {
                return false;
            }
            if (record.document.encoding == textfile::Encoding::Ansi && codepage == 0) return false;
            record.document.ansiCodePage = static_cast<unsigned int>(codepage);
            record.document.hadBom = bom;
            record.document.lineEnding = crlf ? textfile::LineEnding::CrLf : textfile::LineEnding::Lf;
            std::string name;
            if (version == kSchemaVersionV1) {
                name = legacyBodyName(generation, id);
            } else {
                std::string bodyNameValue;
                if (!jsonString(item.get("body"), 64, bodyNameValue) || !isV2BodyName(bodyNameValue)) {
                    return false;  // 只接受本模块受控命名空间内的 body-<32hex>.utf8。
                }
                name = bodyNameValue;
            }
            if (!ownedBodyName(name)) return false;
            std::string body;
            if (!readFile(directory / textfile::pathFromUtf8(name), kMaximumDocumentBytes, body) ||
                !validUtf8(body)) {
                return false;
            }
            if (version == kSchemaVersion && body.size() != declaredBytes) {
                return false;  // 截断/膨胀的 body 与清单声明不符：fail closed，不恢复半个草稿。
            }
            if (body.size() > kMaximumSessionDocumentBytes - totalDocumentBytes) return false;
            totalDocumentBytes += body.size();
            record.document.text = std::move(body);
            bodyFiles.insert(name);
        } else if (item.size() != 6) {
            return false;
        }
        parsed.push_back(std::move(record));
    }
    if (!activeFound) return false;
    records = std::move(parsed);
    activeId = active;
    return true;
}

bool cleanupOwnedBodies(const fs::path& directory, const std::set<std::string>& keep) {
    std::error_code error;
    fs::directory_iterator iterator(directory, error);
    if (error) {
        return error == std::errc::no_such_file_or_directory;
    }
    const fs::directory_iterator end;
    while (iterator != end) {
        const fs::path candidate = iterator->path();
        const std::string name = textfile::pathToUtf8(candidate.filename());
        if (ownedBodyName(name) && keep.count(name) == 0) {
            std::error_code ignored;
            fs::remove(candidate, ignored);
            if (ignored) return false;
        }
        iterator.increment(error);
        if (error) return false;
    }
    return true;
}

bool clearSessionLocked(const fs::path& directory) {
    std::error_code error;
    fs::remove(directory / kManifestName, error);
    if (error) return false;
    return cleanupOwnedBodies(directory, {});
}

} // namespace

bool write(const std::vector<WriteRecord>& records, TabId activeId) {
    std::lock_guard<std::mutex> lock(writeMutex());
    if (records.size() > kMaximumTabs ||
        (records.empty() ? activeId != 0 : activeId == 0)) return false;

    std::set<TabId> ids;
    bool activeFound = activeId == 0;
    std::uintmax_t totalDocumentBytes = 0;
    for (const WriteRecord& record : records) {
        if (record.id == 0 || record.id == std::numeric_limits<TabId>::max() ||
            !ids.insert(record.id).second || record.wrapOverride < -1 ||
            record.wrapOverride > 1 || !validRecordStrings(record.path, record.vaultRoot, record.language)) {
            return false;
        }
        activeFound = activeFound || record.id == activeId;
        if (record.dirty) {
            if (record.document == nullptr || !validUtf8(record.document->text) ||
                record.document->text.size() > kMaximumDocumentBytes ||
                encodingName(record.document->encoding) == nullptr ||
                (record.document->encoding == textfile::Encoding::Ansi &&
                 (record.document->ansiCodePage == 0 || record.document->ansiCodePage > 65535u)) ||
                record.document->ansiCodePage > 65535u) {
                return false;
            }
            if (record.document->text.size() > kMaximumSessionDocumentBytes - totalDocumentBytes) return false;
            totalDocumentBytes += record.document->text.size();
        }
    }
    if (!activeFound) return false;

    const fs::path directory = sessionDirectory();
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) return false;
    const fs::path manifest = directory / kManifestName;

    // 正文按内容寻址：算出的名字已存在就复用（未变页零字节写入），否则原子写新 body。
    // body 是不可变文件，绝不原地覆盖旧清单引用的文件。
    std::vector<std::string> bodyNames(records.size());
    std::vector<fs::path> created;
    std::set<std::string> keep;
    for (std::size_t index = 0; index < records.size(); ++index) {
        const WriteRecord& record = records[index];
        if (!record.dirty) continue;
        const std::string name = bodyNameFromId(contentId(record.document->text));
        bodyNames[index] = name;
        keep.insert(name);
        const fs::path body = directory / name;
        std::error_code existsError;
        if (fs::exists(body, existsError) && !existsError) {
            continue;  // 内容未变：已有同内容 body，直接复用。
        }
        if (!atomicwrite::writeFile(body, record.document->text)) {
            for (const fs::path& candidate : created) { std::error_code ignored; fs::remove(candidate, ignored); }
            return false;
        }
        created.push_back(body);
    }

    if (!writeManifest(manifest, records, activeId, bodyNames)) {
        for (const fs::path& candidate : created) { std::error_code ignored; fs::remove(candidate, ignored); }
        return false;
    }

    // Only after the manifest commit may unreferenced bodies be removed (including
    // any legacy v1 generation files). Restrict cleanup to the private namespace.
    cleanupOwnedBodies(directory, keep);
    return true;
}

bool read(std::vector<ReadRecord>& records, TabId& activeId) {
    records.clear();
    activeId = 0;
    const fs::path directory = sessionDirectory();
    const fs::path manifest = directory / kManifestName;
    std::error_code error;
    const bool exists = fs::exists(manifest, error);
    if (error) return false;
    if (!exists) return true;

    std::string bytes;
    if (!readFile(manifest, kMaximumManifestBytes, bytes)) return false;
    std::vector<ReadRecord> parsed;
    TabId parsedActive = 0;
    std::set<std::string> bodyFiles;
    if (!parseManifest(bytes, directory, parsed, parsedActive, bodyFiles)) return false;
    records = std::move(parsed);
    activeId = parsedActive;
    return true;
}

bool clear() {
    std::lock_guard<std::mutex> lock(writeMutex());
    const std::string configPath = settings::configDirectory();
    if (configPath.empty()) return true;
    const fs::path directory = textfile::pathFromUtf8(configPath) / "session";
    return clearSessionLocked(directory);
}

bool clearForNormalExit() {
    std::lock_guard<std::mutex> lock(writeMutex());
    const std::string configPath = settings::configDirectory();
    if (configPath.empty()) return true;
    const fs::path configDirectory = textfile::pathFromUtf8(configPath);
    if (!clearSessionLocked(configDirectory / "session")) return false;
    std::error_code error;
    fs::remove(configDirectory / "recovery.txt", error);
    return !error;
}

} // namespace neo::sessionstorage
