#include "model/tab_presentation.h"

#include "model/text_file.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neo {
namespace tabpresentation {
namespace {

constexpr Rgb rgb(std::uint32_t hex) {
    return Rgb{static_cast<float>((hex >> 16) & 0xFFu) / 255.0f,
               static_cast<float>((hex >> 8) & 0xFFu) / 255.0f,
               static_cast<float>(hex & 0xFFu) / 255.0f};
}

// 计划给定候选（浅色：#2671B8 #B95411 #6F50B5 #17816C #AB396A #69780C #2C808E #995449；
// 暗色：#7AAEF5 #F3AA68 #B69CF2 #68C6A2 #EB8CB3 #BCC975 #7EC7D0 #D2A184）。
constexpr Rgb kLight[kSlotCount] = {
    rgb(0x2671B8u), rgb(0xB95411u), rgb(0x6F50B5u), rgb(0x17816Cu),
    rgb(0xAB396Au), rgb(0x69780Cu), rgb(0x2C808Eu), rgb(0x995449u)};
constexpr Rgb kDark[kSlotCount] = {
    rgb(0x7AAEF5u), rgb(0xF3AA68u), rgb(0xB69CF2u), rgb(0x68C6A2u),
    rgb(0xEB8CB3u), rgb(0xBCC975u), rgb(0x7EC7D0u), rgb(0xD2A184u)};

// Windows 的 CompareStringOrdinal 忽略大小写与 LCMapStringEx 大小写转换并非完全相同。
// 逐个 UTF-16 字符/代理对生成 invariant uppercase 候选，并用 CompareStringOrdinal
// 直接确认原码点与候选确实等价；有扩展、映射失败或 API 判为不等时保留原码点。
// 旧 lowercase 会把 K 与 Kelvin 符号 U+212A 误并，而本机 ordinal 判为不同。
// 其他平台退 ASCII 小写。
std::string caseFoldUtf8(const std::string& utf8) {
#if defined(_WIN32)
    std::filesystem::path path;
    try {
        path = textfile::pathFromUtf8(utf8);
    } catch (...) {
        return utf8;
    }
    std::wstring wide = path.native();
    std::wstring folded = wide;
    for (std::size_t i = 0; i < wide.size();) {
        std::size_t length = 1;
        const wchar_t ch = wide[i];
        if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < wide.size() &&
            wide[i + 1] >= 0xDC00 && wide[i + 1] <= 0xDFFF) {
            length = 2;
        }
        wchar_t mapped[2] = {0, 0};
        const int written = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, wide.data() + i,
                                          static_cast<int>(length), mapped, static_cast<int>(length),
                                          nullptr, nullptr, 0);
        // Ordinal ignore-case comparison has a one-to-one uppercase relation. Keep mappings
        // that expand or fail unchanged so unrelated multi-character strings cannot collapse.
        if (written == static_cast<int>(length) &&
            CompareStringOrdinal(wide.data() + i, static_cast<int>(length), mapped,
                                 static_cast<int>(length), TRUE) == CSTR_EQUAL) {
            for (std::size_t j = 0; j < length; ++j) folded[i + j] = mapped[j];
        }
        i += length;
    }
    return textfile::pathToUtf8(std::filesystem::path(folded));
#else
    std::string result = utf8;
    for (char& c : result) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 'A' && byte <= 'Z') c = static_cast<char>(byte + ('a' - 'A'));
    }
    return result;
#endif
}

std::string finishKey(std::string utf8) {
    for (char& c : utf8) {
        if (c == '\\') c = '/';
    }
    // 根以外的尾部分隔符归零；保留 "C:/" 这类盘符根的形式（长度 3 时不动）。
    while (utf8.size() > 3 && utf8.back() == '/') utf8.pop_back();
    if (utf8.size() == 3 && utf8[1] == ':' && utf8[2] == '/') {
        // 统一为 "c:/"，不做其他处理。
    }
    return caseFoldUtf8(utf8);
}

std::string normalizeRoot(const std::string& rootUtf8, bool touchDisk) {
    if (rootUtf8.empty()) return {};
    std::error_code error;
    std::filesystem::path input;
    try {
        input = textfile::pathFromUtf8(rootUtf8);
    } catch (...) {
        return finishKey(rootUtf8);  // 非法 UTF-8：退化为词法文本身份，不崩溃
    }
    std::filesystem::path normalized = input.lexically_normal();
    if (touchDisk) {
        normalized = std::filesystem::weakly_canonical(input, error);
        if (error) {
            error.clear();
            normalized = std::filesystem::absolute(input, error).lexically_normal();
            if (error) normalized = input.lexically_normal();
        }
    } else {
        normalized = std::filesystem::absolute(input, error).lexically_normal();
        if (error) normalized = input.lexically_normal();
    }
    return finishKey(textfile::pathToUtf8(normalized));
}

bool isStrictAncestorRoot(const std::string& ancestor, const std::string& descendant) {
    if (ancestor.empty() || descendant.size() <= ancestor.size() ||
        descendant.compare(0, ancestor.size(), ancestor) != 0) return false;
    // Keys already use slash separators and have redundant trailing separators removed,
    // except for drive/volume roots that end in '/'. Requiring the path boundary prevents
    // roots such as C:/work/A from matching C:/work/A2.
    return ancestor.back() == '/' || descendant[ancestor.size()] == '/';
}

}  // namespace

const Rgb* lightPalette() { return kLight; }
const Rgb* darkPalette() { return kDark; }
Rgb paletteColor(bool dark, int slot) {
    const int index = slot < 0 ? 0 : (slot >= kSlotCount ? kSlotCount - 1 : slot);
    return dark ? kDark[index] : kLight[index];
}

std::string canonicalRootKey(const std::string& rootUtf8) {
    return normalizeRoot(rootUtf8, true);
}
std::string lexicalRootKey(const std::string& rootUtf8) {
    return normalizeRoot(rootUtf8, false);
}

int preferredSlot(const std::string& canonicalKey) {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char byte : canonicalKey) {
        hash = (hash ^ byte) * 16777619u;
    }
    return static_cast<int>(hash % static_cast<std::uint32_t>(kSlotCount));
}

void Registry::sync(const std::vector<OpenTab>& openTabs,
                    const std::unordered_map<std::uint64_t, std::string>& prevKeyByTab) {
    // 1) 当前分组：key → 最小 TabId。
    std::unordered_map<std::string, std::uint64_t> keyMinTab;
    for (const OpenTab& tab : openTabs) {
        if (tab.key.empty()) continue;
        auto found = keyMinTab.find(tab.key);
        if (found == keyMinTab.end()) {
            keyMinTab.emplace(tab.key, tab.tabId);
        } else {
            found->second = std::min(found->second, tab.tabId);
        }
    }

    // 2) 合并继承 + 保留：读取旧表 groups_（此时尚未替换）。
    // 只有来源组在本轮已经消失时才允许继承；并且一个旧组最多被一个新组继承。
    // 若来源仍在当前集合中，它仍拥有自己的 ordinal/slot，不能把身份复制给 SaveAs 新根。
    std::unordered_map<std::string, Group> next;
    next.reserve(keyMinTab.size());
    std::unordered_map<std::string, std::string> inheritanceSourceByKey;
    std::unordered_map<std::string, std::pair<std::uint64_t, std::string>> firstTargetBySource;
    for (const OpenTab& tab : openTabs) {
        if (tab.key.empty() || keyMinTab.find(tab.key) == keyMinTab.end()) continue;
        auto prev = prevKeyByTab.find(tab.tabId);
        if (prev == prevKeyByTab.end() || prev->second.empty() || prev->second == tab.key ||
            keyMinTab.find(prev->second) != keyMinTab.end() || groups_.find(prev->second) == groups_.end()) {
            continue;
        }
        // Inheritance models a parent merge only. A disappearing source must never donate
        // its identity to an unrelated SaveAs/new root.
        if (!isStrictAncestorRoot(tab.key, prev->second)) continue;
        auto target = firstTargetBySource.find(prev->second);
        if (target == firstTargetBySource.end() || tab.tabId < target->second.first ||
            (tab.tabId == target->second.first && tab.key < target->second.second)) {
            firstTargetBySource[prev->second] = {tab.tabId, tab.key};
        }
    }
    std::unordered_map<std::string, std::pair<std::uint64_t, std::string>> chosenSourceByTarget;
    for (const auto& entry : firstTargetBySource) {
        const std::string& sourceKey = entry.first;
        const std::uint64_t tabId = entry.second.first;
        const std::string& targetKey = entry.second.second;
        auto chosen = chosenSourceByTarget.find(targetKey);
        if (chosen == chosenSourceByTarget.end() || tabId < chosen->second.first ||
            (tabId == chosen->second.first && sourceKey < chosen->second.second)) {
            chosenSourceByTarget[targetKey] = {tabId, sourceKey};
        }
    }
    for (const auto& entry : chosenSourceByTarget) {
        inheritanceSourceByKey.emplace(entry.first, entry.second.second);
    }
    for (const auto& entry : keyMinTab) {
        const std::string& key = entry.first;
        const std::uint64_t minTab = entry.second;
        auto existing = groups_.find(key);
        if (existing != groups_.end()) {
            Group group = existing->second;
            group.minTabId = minTab;
            next.emplace(key, group);
            continue;
        }
        // 新 key：若能从一个"已被合并掉/不再打开"的旧组承接，就继承其槽位与序号。
        Group group;
        group.minTabId = minTab;
        auto sourceKey = inheritanceSourceByKey.find(key);
        auto source = sourceKey == inheritanceSourceByKey.end() ? groups_.end() : groups_.find(sourceKey->second);
        if (source != groups_.end()) {
            group.slot = source->second.slot;
            group.exclusive = source->second.exclusive;
            group.ordinal = source->second.ordinal;
            group.assigned = true;
        }
        next.emplace(key, group);
    }
    groups_ = std::move(next);

    // 3) 给真正的新组按最小 TabId 升序分配组序号（保证确定性）。
    std::vector<std::string> fresh;
    for (const auto& entry : groups_) {
        if (entry.second.ordinal == 0) fresh.push_back(entry.first);
    }
    std::sort(fresh.begin(), fresh.end(), [this](const std::string& a, const std::string& b) {
        return groups_[a].minTabId < groups_[b].minTabId;
    });
    for (const std::string& key : fresh) {
        groups_[key].ordinal = nextOrdinal_++;
    }

    // 4) 槽位解析。
    bool owned[kSlotCount] = {false, false, false, false, false, false, false, false};
    std::vector<std::string> byOrdinal;
    byOrdinal.reserve(groups_.size());
    for (const auto& entry : groups_) byOrdinal.push_back(entry.first);
    std::sort(byOrdinal.begin(), byOrdinal.end(), [this](const std::string& a, const std::string& b) {
        return groups_[a].ordinal < groups_[b].ordinal;
    });

    // 4a) 存活且原本独占的组保留旧槽（按序号升序，冲突时后者降级）。
    for (const std::string& key : byOrdinal) {
        Group& group = groups_[key];
        if (!group.assigned || !group.exclusive) continue;
        if (group.slot < 0 || group.slot >= kSlotCount || owned[group.slot]) {
            group.exclusive = false;
            continue;
        }
        owned[group.slot] = true;
    }

    // 4b) 非独占（溢出/被降级）组按序号升序做至多 8 次线性探测，占空槽升级。
    for (const std::string& key : byOrdinal) {
        Group& group = groups_[key];
        if (!group.assigned || group.exclusive) continue;
        const int preferred = preferredSlot(key);
        for (int step = 0; step < kSlotCount; ++step) {
            const int slot = (preferred + step) % kSlotCount;
            if (!owned[slot]) {
                group.slot = slot;
                group.exclusive = true;
                owned[slot] = true;
                break;
            }
        }
    }

    // 4c) 全新组按最小 TabId 升序分配：首选槽位起线性探测至多 8 次，找不到就复用首选槽
    //     （非独占），由组序号补充区分 —— 绝不死循环。
    std::vector<std::string> freshByTab(fresh);
    for (const std::string& key : freshByTab) {
        Group& group = groups_[key];
        const int preferred = preferredSlot(key);
        int chosen = -1;
        for (int step = 0; step < kSlotCount; ++step) {
            const int slot = (preferred + step) % kSlotCount;
            if (!owned[slot]) {
                chosen = slot;
                owned[slot] = true;
                break;
            }
        }
        if (chosen >= 0) {
            group.slot = chosen;
            group.exclusive = true;
        } else {
            group.slot = preferred;
            group.exclusive = false;
        }
        group.assigned = true;
    }
}

bool Registry::has(const std::string& key) const {
    return groups_.find(key) != groups_.end();
}

GroupState Registry::state(const std::string& key) const {
    GroupState result;
    auto found = groups_.find(key);
    if (found == groups_.end()) return result;
    result.slot = found->second.slot;
    result.exclusive = found->second.exclusive;
    result.ordinal = found->second.ordinal;
    return result;
}

void Registry::clear() {
    groups_.clear();
    nextOrdinal_ = 1;
}

}  // namespace tabpresentation
}  // namespace neo
