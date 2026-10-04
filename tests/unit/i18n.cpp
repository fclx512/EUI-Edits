#include "model/i18n.h"

#include <iostream>
#include <set>
#include <string>

namespace {
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) { std::cerr << message << '\n'; ++failures; }
}
std::set<std::string> parameters(const std::string& value) {
    std::set<std::string> result;
    for (std::size_t start = value.find('{'); start != std::string::npos; start = value.find('{', start + 1)) {
        const std::size_t end = value.find('}', start + 1);
        if (end == std::string::npos) continue;
        const std::string name = value.substr(start + 1, end - start - 1);
        if (!name.empty() && name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos)
            result.insert(name);
    }
    return result;
}
}

int main() {
    using namespace neo::i18n;
    for (const char* locale : {"zh", "zh-CN", "zh-TW", "zh-HK", "zh-MO", "zh-SG", "zh-Hans", "zh-Hant-TW", "ZH_cn.UTF-8", "zh.UTF-8", "zh@variant"})
        check(resolveSystemLanguage(locale) == "zh-CN", std::string("Chinese locale: ") + locale);
    for (const char* locale : {"", "en-US", "ja-JP", "fr-FR", "az", "zhish"})
        check(resolveSystemLanguage(locale) == "en", std::string("English fallback: ") + locale);
    std::set<std::string> ids;
    for (const auto& message : messages) {
        check(message.id && *message.id && ids.insert(message.id).second, std::string("Duplicate or empty message ID: ") + (message.id ? message.id : "<null>"));
        check(message.zh && *message.zh && message.en && *message.en, std::string("Incomplete translation: ") + message.id);
        check(parameters(message.zh) == parameters(message.en), std::string("Placeholder mismatch: ") + message.id);
    }
    initialize("zh-CN");
    check(preference() == "zh-CN" && language() == "zh-CN" && std::string(tr("common.cancel")) == "取消", "Chinese preference");
    check(format("settings.about_stack", {{"window_backend", "Win32"}, {"render_backend", "Direct2D"}}) ==
        "C++17 · EUI 界面框架（Win32 窗口 + Direct2D 渲染）", "Chinese About backend labels");
    setPreference("en");
    check(preference() == "en" && language() == "en" && std::string(tr("common.cancel")) == "Cancel", "English preference");
    check(format("settings.about_stack", {{"window_backend", "SDL2"}, {"render_backend", "Vulkan"}}) ==
        "C++17 · EUI UI framework (SDL2 windowing + Vulkan rendering)", "English About backend labels");
    check(format("status.lines", {{"count", "42"}}) == "Lines 42", "Named parameter formatting");
    check(format("theme.read_error", {{"error", "{path}"}, {"path", "中文 note.json"}}) == "Cannot read theme file 中文 note.json: {path}", "Reordered parameters and literal replacement braces");
    check(format("status.lines", {}) == "Lines {count}", "Missing parameter remains diagnosable");
    check(std::string(tr("unknown.message")) == "unknown.message", "Unknown message remains diagnosable");
    setPreference("invalid");
    check(preference() == "system" && language() == systemLanguage(), "Invalid preference uses system default");
    check(normalizePreference("en-US") == "system", "Only persisted supported preferences are accepted");
    return failures == 0 ? 0 : 1;
}
