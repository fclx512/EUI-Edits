#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>

#if defined(_WIN32)
// Keep Windows headers and their macros out of every UI/model translation unit.
// This is the Windows ABI declaration for the read-only display-language API.
extern "C" __declspec(dllimport) unsigned short __stdcall GetUserDefaultUILanguage(void);
#endif

namespace neo::i18n {

struct Message { const char* id; const char* zh; const char* en; };
inline constexpr Message messages[] = {
#define NEO_I18N(id, zh, en) {id, zh, en},
#include "i18n_core.inc"
#include "i18n_ui.inc"
#include "i18n_safety.inc"
#undef NEO_I18N
};

inline std::string normalizePreference(const std::string& value) {
    return value == "zh-CN" || value == "en" ? value : "system";
}
inline std::string resolveSystemLanguage(std::string locale) {
    std::transform(locale.begin(), locale.end(), locale.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 'a' - 'A') : static_cast<char>(c);
    });
    return locale == "zh" || locale.rfind("zh-", 0) == 0 || locale.rfind("zh_", 0) == 0 ||
        locale.rfind("zh.", 0) == 0 || locale.rfind("zh@", 0) == 0
        ? "zh-CN" : "en";
}
inline std::string systemLanguage() {
#if defined(_WIN32)
    // Display language, independent of input method, region and ANSI code page.
    constexpr unsigned short primaryLanguageMask = 0x03ff;
    constexpr unsigned short chineseLanguage = 0x0004;
    return (GetUserDefaultUILanguage() & primaryLanguageMask) == chineseLanguage ? "zh-CN" : "en";
#else
    for (const char* key : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* locale = std::getenv(key);
        if (locale && *locale) return resolveSystemLanguage(locale);
    }
    return "en";
#endif
}
inline std::string& preferenceStorage() { static std::string value = "system"; return value; }
inline std::string& languageStorage() { static std::string value = systemLanguage(); return value; }
inline const std::string& preference() { return preferenceStorage(); }
inline const std::string& language() { return languageStorage(); }
inline void setPreference(const std::string& value) {
    preferenceStorage() = normalizePreference(value);
    languageStorage() = preferenceStorage() == "system" ? systemLanguage() : preferenceStorage();
}
inline void initialize(const std::string& settingsPref) { setPreference(settingsPref); }
inline const char* tr(const char* stableId) {
    if (!stableId) return "";
    for (const auto& message : messages) {
        if (std::strcmp(message.id, stableId) == 0) {
            return language() == "zh-CN" && message.zh && *message.zh ? message.zh : message.en;
        }
    }
    // Unknown IDs stay diagnosable; every shipped call is checked by the catalog test.
    return stableId;
}
inline std::string format(const char* id,
                         std::initializer_list<std::pair<std::string, std::string>> parameters) {
    const std::string pattern = tr(id);
    std::string result;
    result.reserve(pattern.size());
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern[i] == '{') {
            const std::size_t end = pattern.find('}', i + 1);
            if (end != std::string::npos) {
                const std::string name = pattern.substr(i + 1, end - i - 1);
                const auto value = std::find_if(parameters.begin(), parameters.end(), [&](const auto& p) { return p.first == name; });
                if (value != parameters.end()) {
                    result += value->second;
                    i = end + 1;
                    continue;
                }
            }
        }
        result += pattern[i++];
    }
    return result;
}
} // namespace neo::i18n
