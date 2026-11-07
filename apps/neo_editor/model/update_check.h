#pragma once

#include "eui/json.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace neo::updates {

inline constexpr char kReleasePage[] = "https://github.com/fclx512/EUI-Edits/releases/latest";

inline bool checkDue(std::int64_t lastAttempt, std::int64_t now) {
    constexpr std::int64_t day = 24 * 60 * 60;
    return lastAttempt <= 0 || lastAttempt > now || now - lastAttempt >= day;
}

inline bool parseVersion(std::string_view version, std::array<unsigned, 3>& parts) {
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto dot = version.find('.');
        const auto field = version.substr(0, dot);
        if (field.empty() || field.size() > 5 || (field.size() > 1 && field.front() == '0')) return false;
        const auto end = field.data() + field.size();
        const auto parsed = std::from_chars(field.data(), end, parts[i]);
        if (parsed.ec != std::errc{} || parsed.ptr != end || parts[i] > 65535) return false;
        if (dot == std::string_view::npos) return i == parts.size() - 1;
        version.remove_prefix(dot + 1);
    }
    return false;
}

inline std::string newerStableRelease(const std::string& json, std::string_view current) {
    eui::json::Document document;
    if (!document.parse(json)) return {};
    const auto root = document.root();
    bool draft = false, prerelease = false;
    std::string tag;
    constexpr std::string_view prefix = "neoeditor-v";
    if (!root.get("draft").boolean(draft) || draft ||
        !root.get("prerelease").boolean(prerelease) || prerelease ||
        !root.get("tag_name").string(tag) || tag.compare(0, prefix.size(), prefix) != 0) return {};
    const std::string latest = tag.substr(prefix.size());
    const auto suffix = current.find('-');
    std::array<unsigned, 3> published{}, installed{};
    if (!parseVersion(latest, published) || !parseVersion(current.substr(0, suffix), installed) ||
        published < installed || (published == installed && suffix == std::string_view::npos)) return {};

    const auto assets = root.get("assets");
    const std::string package = "EUI-Edits-" + latest + "-windows-x64.exe";
    for (std::size_t i = 0; i < assets.size(); ++i) {
        std::string name, state;
        const auto asset = assets.at(i);
        if (asset.get("name").string(name) && name == package &&
            asset.get("state").string(state) && state == "uploaded") return latest;
    }
    return {};
}

std::string fetchLatest();

} // namespace neo::updates
