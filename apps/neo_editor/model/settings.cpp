#if defined(_WIN32)
// 只是读环境变量，getenv 的“不安全”提示在这个用法下没有意义。
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#endif

#include "model/settings.h"
#include "model/file_types.h"
#include "model/i18n.h"

#include "model/atomic_write.h"
#include "model/text_file.h"
#include "eui/json.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace neo::settings {
namespace {

namespace fs = std::filesystem;
constexpr std::size_t kRecentFileLimit = 8;

const char* const kRecoveryHeader = "#neo-recovery";
const char* const kRecoveryHeaderV2 = "#neo-recovery-v2";
// 单行元数据的安全上限：超长说明这行不是我们写出的，拒绝解析。
constexpr std::size_t kRecoveryMetaLimit = 4096;

// 首次运行时的动画默认值：跟随 Windows 的"辅助功能 → 视觉效果 → 动画效果"。
// settings.ini 里一旦写下 animations 键，之后一律以用户的选择为准。
bool systemAnimationsDefault() {
#if defined(_WIN32)
    BOOL enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) {
        return enabled != FALSE;
    }
#endif
    return true;
}

const char* recoveryEncodingName(textfile::Encoding encoding) {
    switch (encoding) {
        case textfile::Encoding::Utf8:
            return "utf8";
        case textfile::Encoding::Utf16Le:
            return "utf16le";
        case textfile::Encoding::Utf16Be:
            return "utf16be";
        case textfile::Encoding::Ansi:
            return "ansi";
    }
    return "utf8";
}

bool recoveryEncodingValue(const std::string& name, textfile::Encoding& out) {
    if (name == "utf8") {
        out = textfile::Encoding::Utf8;
    } else if (name == "utf16le") {
        out = textfile::Encoding::Utf16Le;
    } else if (name == "utf16be") {
        out = textfile::Encoding::Utf16Be;
    } else if (name == "ansi") {
        out = textfile::Encoding::Ansi;
    } else {
        return false;
    }
    return true;
}

// origin 是任意本地路径（可含引号、反斜杠、控制字符之外的任意 UTF-8 文本）；
// 走标准 JSON 字符串转义，元数据保持单行。
std::string jsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
            case '"':
                out.append("\\\"");
                break;
            case '\\':
                out.append("\\\\");
                break;
            case '\b':
                out.append("\\b");
                break;
            case '\f':
                out.append("\\f");
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            default:
                if (byte < 0x20u) {
                    char escape[7];
                    std::snprintf(escape, sizeof(escape), "\\u%04x", byte);
                    out.append(escape);
                } else {
                    out.push_back(character);
                }
                break;
        }
    }
    return out;
}

fs::path settingsFile() {
    return textfile::pathFromUtf8(configDirectory()) / "settings.ini";
}

fs::path recoveryFile() {
    return textfile::pathFromUtf8(configDirectory()) / "recovery.txt";
}

std::string trim(std::string value) {
    const std::size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

} // namespace

std::string configDirectory() {
    std::string base;
#if defined(_WIN32)
    // 必须走 environmentPathUtf8（内部是宽字符 API）：std::getenv 返回 ANSI/GBK 字节，
    // 当 UTF-8 用会让 pathFromUtf8 抛异常。这条链在启动时跑（current() 的静态初始化），
    // 抛出去就是"一开就崩"——中文用户名（C:\Users\张三\AppData\Roaming）正是这种情况。
    base = textfile::environmentPathUtf8("APPDATA");
#else
    base = textfile::environmentPathUtf8("XDG_CONFIG_HOME");
    if (base.empty()) {
        const std::string home = textfile::environmentPathUtf8("HOME");
        base = home.empty() ? std::string{} : home + "/.config";
    }
#endif
    // 兜底：即使环境变量被塞进非法字节，也不许这里抛出去（启动路径，异常=闪退）。
    // 退回工作目录下的 .eui-edits，功能可用，用户至少能进来看到状态栏提示。
    try {
        fs::path directory;
        if (base.empty()) {
            directory = fs::current_path() / ".eui-edits";
        } else {
            directory = textfile::pathFromUtf8(base) / "EUI-Edits";
        }
        std::error_code error;
        // 产品由 NeoEditor 更名而来：新目录不存在而旧目录还在时整体搬过来，
        // 设置/会话/恢复草稿/主题无缝继承。尽力而为，失败就留在旧目录。
        if (!base.empty()) {
            const fs::path legacy = textfile::pathFromUtf8(base) / "NeoEditor";
            if (fs::is_directory(legacy, error) && !fs::exists(directory, error)) {
                fs::copy(legacy, directory, fs::copy_options::recursive, error);
            }
        }
        fs::create_directories(directory, error);
        return textfile::pathToUtf8(directory);
    } catch (const std::exception&) {
        std::error_code fallbackError;
        const fs::path fallback = fs::current_path(fallbackError) / ".eui-edits";
        fs::create_directories(fallback, fallbackError);
        return textfile::pathToUtf8(fallback);
    }
}

Data& current() {
    static Data data = [] {
        Data loaded;
        // 先取系统默认值：文件里没有 animations 键时它就是首启默认。
        loaded.animations = systemAnimationsDefault();
        std::ifstream input(settingsFile(), std::ios::binary);
        if (input) {
            std::string line;
            while (std::getline(input, line)) {
                const std::size_t separator = line.find('=');
                if (separator == std::string::npos) {
                    continue;
                }
                const std::string key = trim(line.substr(0, separator));
                const std::string value = trim(line.substr(separator + 1));
                if (key == "vault") {
                    loaded.vault = value;
                } else if (key == "last_file") {
                    loaded.lastFile = value;
                } else if (key == "recent_file" && !value.empty() &&
                           loaded.recentFiles.size() < kRecentFileLimit) {
                    if (std::find(loaded.recentFiles.begin(), loaded.recentFiles.end(), value) ==
                        loaded.recentFiles.end()) {
                        loaded.recentFiles.push_back(value);
                    }
                } else if (key == "mode") {
                    loaded.mode = value == "1" ? 1 : 0;
                } else if (key == "ui_language") {
                    loaded.uiLanguage = i18n::normalizePreference(value);
                } else if (key == "show_status_bar") {
                    loaded.showStatusBar = value != "0";
                } else if (key == "ui_scale") {
                    try {
                        const float parsed = std::stof(value);
                        if (parsed >= 0.8f && parsed <= 2.0f) {
                            loaded.uiScale = parsed;
                        }
                    } catch (...) {
                    }
                } else if (key == "editor_font_size") {
                    try {
                        const float parsed = std::stof(value);
                        if (parsed >= 12.0f && parsed <= 32.0f) {
                            loaded.editorFontSize = parsed;
                        }
                    } catch (...) {
                    }
                } else if (key == "ui_font_size") {
                    try {
                        const float parsed = std::stof(value);
                        if (parsed >= 12.0f && parsed <= 18.0f) {
                            loaded.uiFontSize = parsed;
                        }
                    } catch (...) {
                    }
                } else if (key == "editor_font_file") {
                    loaded.editorFontFile = value;
                } else if (key == "ui_font_file") {
                    loaded.uiFontFile = value;
                } else if (key == "code_font_file") {
                    loaded.codeFontFile = value;
                } else if (key == "theme") {
                    loaded.theme = value == "1" ? 1 : 0;
                } else if (key == "last_theme_file") {
                    loaded.lastThemeFile = value;
                } else if (key == "line_numbers") {
                    loaded.lineNumbers = value != "0";
                } else if (key == "readable_width") {
                    loaded.readableWidth = value != "0";
                } else if (key == "animations") {
                    loaded.animations = value != "0";
                } else if (key == "attachment_mode") {
                    loaded.attachmentMode = value == "1" ? 1 : 0;
                } else if (key == "vault_width") {
                    try {
                        const float parsed = std::stof(value);
                        if (parsed >= 180.0f && parsed <= 520.0f) {
                            loaded.vaultWidth = parsed;
                        }
                    } catch (...) {
                    }
                }
            }
        }
        return loaded;
    }();
    return data;
}

bool flush() {
    const Data& data = current();
    // 先在内存里拼好整份内容，再走原子写（临时文件 + 替换）：
    // 直接 ofstream trunc 目标，写到一半崩溃/出错就会留下半份 settings.ini。
    std::ostringstream output;
    output << "vault=" << data.vault << '\n';
    output << "last_file=" << data.lastFile << '\n';
    for (const std::string& path : data.recentFiles) {
        if (!path.empty()) {
            output << "recent_file=" << path << '\n';
        }
    }
    output << "mode=" << data.mode << '\n';
    output << "ui_language=" << i18n::normalizePreference(data.uiLanguage) << '\n';
    output << "show_status_bar=" << (data.showStatusBar ? 1 : 0) << '\n';
    output << "line_numbers=" << (data.lineNumbers ? 1 : 0) << '\n';
    output << "readable_width=" << (data.readableWidth ? 1 : 0) << '\n';
    output << "animations=" << (data.animations ? 1 : 0) << '\n';
    output << "attachment_mode=" << data.attachmentMode << '\n';
    output << "theme=" << data.theme << '\n';
    std::ostringstream scale;
    scale << data.uiScale;
    output << "ui_scale=" << scale.str() << '\n';
    std::ostringstream fontSize;
    fontSize << data.editorFontSize;
    output << "editor_font_size=" << fontSize.str() << '\n';
    std::ostringstream width;
    width << data.vaultWidth;
    output << "vault_width=" << width.str() << '\n';
    std::ostringstream uiFont;
    uiFont << data.uiFontSize;
    output << "ui_font_size=" << uiFont.str() << '\n';
    output << "editor_font_file=" << data.editorFontFile << '\n';
    output << "ui_font_file=" << data.uiFontFile << '\n';
    output << "code_font_file=" << data.codeFontFile << '\n';
    // 主题文件只在非空时写出：空值不携带信息（读侧缺行 = 默认空，语义一致），
    // 而 tests/unit/settings_atomic.cpp 的黄金样例是逐字节钉死的"空值形态"——
    // 多一行空的 last_theme_file= 会把那份对照打成红的，本轮不动别的任务的测试。
    if (!data.lastThemeFile.empty()) {
        output << "last_theme_file=" << data.lastThemeFile << '\n';
    }
    return atomicwrite::writeFile(settingsFile(), output.str());
}

bool writeRecovery(const std::string& text, const std::string& originPath, const textfile::Document* doc) {
    return writeRecovery(text,originPath,doc,std::string{});
}

bool writeRecovery(const std::string& text, const std::string& originPath, const textfile::Document* doc, const std::string& language) {
    if (text.empty()) {
        clearRecovery();
        return true;
    }
    // 与 flush 同一条原子写路径：先拼好整份内容再替换，替换失败旧副本原样保留。
    std::ostringstream output;
    if (doc != nullptr) {
        output << kRecoveryHeaderV2 << " {\"origin\":\"" << jsonEscape(originPath) << "\",\"encoding\":\""
               << recoveryEncodingName(doc->encoding) << "\",\"codepage\":" << doc->ansiCodePage
               << ",\"bom\":" << (doc->hadBom ? "true" : "false")
               << ",\"crlf\":" << (doc->lineEnding == textfile::LineEnding::CrLf ? "true" : "false")
               << ",\"language\":\"" << jsonEscape(language) << "\"}\n";
    } else {
        output << kRecoveryHeader << ' ' << originPath << '\n';
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return atomicwrite::writeFile(recoveryFile(), output.str());
}

bool readRecovery(RecoverySnapshot& out) {
    out = RecoverySnapshot{};

    std::ifstream input(recoveryFile(), std::ios::binary);
    if (!input) {
        return false;
    }
    std::string header;
    if (!std::getline(input, header)) {
        return false;
    }

    if (header.rfind(kRecoveryHeaderV2, 0) == 0 &&
        (header.size() == std::string(kRecoveryHeaderV2).size() ||
         header[std::string(kRecoveryHeaderV2).size()] == ' ')) {
        // 新格式：单行 JSON 元数据。解析失败、字段缺失/类型不对、编码枚举未知、
        // ansi 缺页码、行超长，一律拒绝恢复——宁可丢应急副本，不冒认成旧格式。
        if (header.size() > std::string(kRecoveryHeaderV2).size() + kRecoveryMetaLimit) {
            return false;
        }
        const std::size_t metaBegin = header.find('{');
        if (metaBegin == std::string::npos) {
            return false;
        }
        eui::json::Document meta;
        if (!meta.parse(header.substr(metaBegin)) || !meta.valid()) {
            return false;
        }
        const eui::json::Value root = meta.root();
        std::string origin;
        std::string encodingName;
        if (!root.get("origin").string(origin) || !root.get("encoding").string(encodingName)) {
            return false;
        }
        textfile::Encoding encoding;
        if (!recoveryEncodingValue(encodingName, encoding)) {
            return false;
        }
        unsigned int codePage = 0;
        if (const eui::json::Value codeValue = root.get("codepage"); codeValue.valid()) {
            std::uint64_t number = 0;
            if (!codeValue.unsignedInteger(number) || number > 65535u) {
                return false;
            }
            codePage = static_cast<unsigned int>(number);
        }
        if (encoding == textfile::Encoding::Ansi && codePage == 0u) {
            return false;
        }
        bool hadBom = false;
        if (const eui::json::Value bomValue = root.get("bom"); bomValue.valid() && !bomValue.boolean(hadBom)) {
            return false;
        }
        bool crlf = false;
        if (const eui::json::Value eolValue = root.get("crlf"); eolValue.valid() && !eolValue.boolean(crlf)) {
            return false;
        }

        std::ostringstream body;
        body << input.rdbuf();
        out.text = body.str();
        if (out.text.empty()) {
            return false;
        }
        if(const auto languageValue=root.get("language");languageValue.valid()) {
            if(!languageValue.string(out.language) || (!out.language.empty() && !filetypes::validLanguage(out.language))) return false;
        }
        out.originPath = std::move(origin);
        out.hasMeta = true;
        out.doc.encoding = encoding;
        out.doc.ansiCodePage = codePage;
        out.doc.hadBom = hadBom;
        out.doc.lineEnding = crlf ? textfile::LineEnding::CrLf : textfile::LineEnding::Lf;
        return true;
    }

    if (header.rfind(kRecoveryHeader, 0) != 0 ||
        (header.size() > std::string(kRecoveryHeader).size() &&
         header[std::string(kRecoveryHeader).size()] != ' ')) {
        return false;
    }
    out.originPath = trim(header.substr(std::string(kRecoveryHeader).size()));
    std::ostringstream body;
    body << input.rdbuf();
    out.text = body.str();
    return !out.text.empty();
}

bool readRecovery(std::string& text, std::string& originPath) {
    RecoverySnapshot snapshot;
    if (!readRecovery(snapshot)) {
        text.clear();
        originPath.clear();
        return false;
    }
    text = std::move(snapshot.text);
    originPath = std::move(snapshot.originPath);
    return true;
}

void clearRecovery() {
    std::error_code error;
    fs::remove(recoveryFile(), error);
}

} // namespace neo::settings
