// 标签展示身份 / 调色 / 槽位分配 / 顶栏几何与关闭锚点 的纯逻辑测试。
// 对应执行计划《NeoEditor-标签页与菜单视觉改进执行计划-2026-10-03》第 4、5、9 节。
#include "model/tab_presentation.h"
#include "model/text_file.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using namespace neo::tabpresentation;

namespace fs = std::filesystem;

fs::path fixtureRoot() {
    std::error_code error;
    fs::path base = fs::temp_directory_path() / "neo_tab_presentation_fixture";
    fs::create_directories(base, error);
    // CI 的 TEMP 可能带 8.3 短名（如 C:\Users\RUNNER~1\...）：canonical 把既有
    // 前缀解析成真实长名。否则 weakly_canonical（解短名）与 fs::absolute（不解）
    // 会对同一路径给出不同的 root key，rootIdentity 的回退断言必然假失败。
    const fs::path real = fs::canonical(base, error);
    if (!error) base = real;
    return base;
}

std::string utf8(const fs::path& value) { return neo::textfile::pathToUtf8(value); }

std::string makeDir(const std::string& relative) {
    std::error_code error;
    const fs::path path = fixtureRoot() / neo::textfile::pathFromUtf8(relative);
    fs::create_directories(path, error);
    return utf8(path);
}

bool closeEnough(float a, float b) { return std::fabs(a - b) <= 1.0f; }

// ── 根身份 ────────────────────────────────────────────────────────────────
bool rootIdentity() {
    const std::string a = makeDir("A");
    const std::string sub = makeDir("A/sub");
    const std::string b = makeDir("B");
    const std::string a2 = makeDir("A2");

    const std::string keyA = canonicalRootKey(a);
    if (keyA.empty() || keyA != canonicalRootKey(a + "/") ||
        keyA != canonicalRootKey(a + "\\") ||
        keyA != canonicalRootKey(a + "/./") ||
        keyA != canonicalRootKey(a + "/sub/..")) {
        std::cerr << "FAIL rootIdentity: separator/dot/trailing-slash forms differ for " << a << "\n";
        return false;
    }
    if (canonicalRootKey(a) == canonicalRootKey(a2) ||
        canonicalRootKey(a) == canonicalRootKey(b)) {
        std::cerr << "FAIL rootIdentity: distinct roots collapsed (A vs A2/B)\n";
        return false;
    }
    if (canonicalRootKey(sub).empty() || canonicalRootKey(sub) == canonicalRootKey(a)) {
        std::cerr << "FAIL rootIdentity: child root collapsed into parent\n";
        return false;
    }

    // ASCII 大小写等价。
    const std::string caseDir = makeDir("CaseRoot");
    std::string upper = caseDir;
    for (char& c : upper) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
    }
    if (canonicalRootKey(caseDir) != canonicalRootKey(upper)) {
        std::cerr << "FAIL rootIdentity: ASCII case variants differ\n";
        return false;
    }

    // Unicode 大小写等价（西里尔大写/小写指向同一目录）。
    const std::string unicodeDir = makeDir("\xD0\xA2\xD0\xB5\xD1\x81\xD1\x82");  // "Тест"
    std::string unicodeLower = unicodeDir;
    // "Тест" → "тест"：首字母 U+0422 → U+0442。
    const std::string upperTheta = "\xD0\xA2";
    const std::string lowerTheta = "\xD1\x82";
    const auto position = unicodeLower.find(upperTheta);
    if (position != std::string::npos) unicodeLower.replace(position, upperTheta.size(), lowerTheta);
    if (canonicalRootKey(unicodeDir) != canonicalRootKey(unicodeLower)) {
        std::cerr << "FAIL rootIdentity: Unicode case variants differ\n";
        return false;
    }

#if defined(_WIN32)
    if (canonicalRootKey("C:\\") != canonicalRootKey("c:/")) {
        std::cerr << "FAIL rootIdentity: drive root forms differ\n";
        return false;
    }
    // Identical leaf names on separate local volumes must remain distinct.
    // These are fixed local drive paths, never UNC/network paths.
    const std::string sameNameC = canonicalRootKey("C:/neo-tab-presentation-volume-check/same-name");
    const std::string sameNameD = canonicalRootKey("D:/neo-tab-presentation-volume-check/same-name");
    if (sameNameC.empty() || sameNameD.empty() || sameNameC == sameNameD) {
        std::cerr << "FAIL rootIdentity: same-basename roots on C: and D: collapsed\n";
        return false;
    }

    // Do not ask the filesystem to canonicalize an unavailable UNC share: that
    // may trigger slow network discovery. The lexical fallback must still make
    // equivalent UNC spellings identical and keep different shares separate.
    const auto unc = lexicalRootKey("\\\\Neo-Server\\Share");
    if (unc != lexicalRootKey("//neo-server/share/") ||
        unc != lexicalRootKey("\\\\NEO-SERVER\\SHARE\\.\\") ||
        unc == lexicalRootKey("\\\\Neo-Server\\Share2") ||
        unc == lexicalRootKey("\\\\Other-Server\\Share") ||
        unc == lexicalRootKey("\\\\Neo-Server\\Share\\child")) {
        std::cerr << "FAIL rootIdentity: UNC lexical fallback identity or root boundary differs\n";
        return false;
    }

    // 直接用 Windows 的 ordinal 比较作 oracle，核对 key 身份没有 over-fold 或漏折叠。
    // Σ/σ 与 A/a 是相等正例；K/K 是负例，旧 lowercase 会将两者误并。
    // 其余字符覆盖 Turkish I、ß 与补充平面；以运行系统的 API 为准。
    const std::vector<std::pair<std::wstring, std::wstring>> ordinalCases = {
        {L"A", L"a"}, {L"\u03A3", L"\u03C3"}, {L"I", L"\u0131"},
        {L"i", L"\u0130"}, {L"K", L"\u212A"}, {L"\u00DF", L"\u1E9E"},
        {L"\u00DF", L"SS"}, {L"\U00010400", L"\U00010428"}};
    for (const auto& pair : ordinalCases) {
        const bool ordinalEqual = CompareStringOrdinal(pair.first.data(), static_cast<int>(pair.first.size()),
                                                        pair.second.data(), static_cast<int>(pair.second.size()),
                                                        TRUE) == CSTR_EQUAL;
        const std::string left = utf8(fs::path(L"C:\\neo-" + pair.first));
        const std::string right = utf8(fs::path(L"C:\\neo-" + pair.second));
        const bool keyEqual = canonicalRootKey(left) == canonicalRootKey(right);
        if (keyEqual != ordinalEqual) {
            std::cerr << "FAIL rootIdentity: key normalization disagrees with CompareStringOrdinal\n";
            return false;
        }
    }
#endif

    // canonical 失败（不存在的 UNC / 非法字符）不得崩溃，退回词法身份且保持确定性。
    const std::string unavailable = "\\\\?\\Z:\\neo\\missing\\root";
    if (canonicalRootKey(unavailable) != canonicalRootKey(unavailable)) {
        std::cerr << "FAIL rootIdentity: fallback key is unstable\n";
        return false;
    }
    if (lexicalRootKey(a + "/./") != lexicalRootKey(a)) {
        std::cerr << "FAIL rootIdentity: lexical key normalization inconsistent\n";
        return false;
    }
    const fs::path missingLocal = fixtureRoot() / "missing-local-fallback" / "not-created";
    std::error_code missingError;
    (void)fs::canonical(missingLocal, missingError);
    if (!missingError || canonicalRootKey(utf8(missingLocal)) !=
                         lexicalRootKey(utf8(fs::absolute(missingLocal).lexically_normal()))) {
        std::cerr << "FAIL rootIdentity: failed local canonical did not use absolute lexical fallback\n";
        return false;
    }
    return true;
}

// ── 调色 ──────────────────────────────────────────────────────────────────
bool paletteDistinct() {
    const auto distinct = [](const Rgb* palette) {
        for (int i = 0; i < kSlotCount; ++i) {
            for (int j = i + 1; j < kSlotCount; ++j) {
                if (std::fabs(palette[i].r - palette[j].r) < 0.01f &&
                    std::fabs(palette[i].g - palette[j].g) < 0.01f &&
                    std::fabs(palette[i].b - palette[j].b) < 0.01f) {
                    return false;
                }
            }
        }
        return true;
    };
    if (!distinct(lightPalette()) || !distinct(darkPalette())) {
        std::cerr << "FAIL paletteDistinct: duplicate slot colors\n";
        return false;
    }
    if (lightPalette() == darkPalette()) {
        std::cerr << "FAIL paletteDistinct: light/dark palettes share storage\n";
        return false;
    }
    for (int slot = 0; slot < kSlotCount; ++slot) {
        const Rgb value = paletteColor(false, slot);
        if (value.r < 0.0f || value.r > 1.0f || value.g < 0.0f || value.g > 1.0f ||
            value.b < 0.0f || value.b > 1.0f) {
            std::cerr << "FAIL paletteDistinct: slot out of range\n";
            return false;
        }
    }
    return true;
}

bool preferredSlotDeterministic() {
    const std::string key = canonicalRootKey(makeDir("slot-probe"));
    if (preferredSlot(key) != preferredSlot(key) || preferredSlot(key) < 0 || preferredSlot(key) >= kSlotCount) {
        std::cerr << "FAIL preferredSlotDeterministic: not stable or out of range\n";
        return false;
    }
    bool collided = false;
    for (int i = 0; i < 200 && !collided; ++i) {
        const int left = preferredSlot(canonicalRootKey("D:/tab-visual/lib" + std::to_string(i)));
        for (int j = i + 1; j < 200; ++j) {
            if (left == preferredSlot(canonicalRootKey("D:/tab-visual/lib" + std::to_string(j)))) {
                collided = true;
                break;
            }
        }
    }
    if (!collided) {
        std::cerr << "FAIL preferredSlotDeterministic: no colliding pair found (fixture condition changed)\n";
        return false;
    }
    return true;
}

// ── 槽位分配 ──────────────────────────────────────────────────────────────
std::vector<Registry::OpenTab> buildTabs(const std::vector<std::string>& keys) {
    std::vector<Registry::OpenTab> tabs;
    std::uint64_t id = 1;
    for (const std::string& key : keys) tabs.push_back({id++, key});
    return tabs;
}

bool registryEightDistinctAndStable() {
    Registry registry;
    std::vector<std::string> keys;
    for (int i = 0; i < 8; ++i) keys.push_back(canonicalRootKey(makeDir("lib" + std::to_string(i))));
    std::unordered_map<std::uint64_t, std::string> previous;
    registry.sync(buildTabs(keys), previous);

    std::unordered_set<int> slots;
    for (const std::string& key : keys) {
        const GroupState state = registry.state(key);
        if (!state.exclusive || state.slot < 0 || state.slot >= kSlotCount || state.ordinal == 0) {
            std::cerr << "FAIL registryEightDistinctAndStable: group not exclusive/assigned\n";
            return false;
        }
        slots.insert(state.slot);
    }
    if (slots.size() != 8) {
        std::cerr << "FAIL registryEightDistinctAndStable: slots not unique for 8 independent roots\n";
        return false;
    }

    // 重排（输入顺序颠倒）不改色/不改序号。
    std::vector<Registry::OpenTab> reversed;
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        reversed.push_back({static_cast<std::uint64_t>(std::distance(keys.rbegin(), it) + 100), *it});
    }
    std::unordered_map<std::uint64_t, std::string> previous2;
    for (const Registry::OpenTab& tab : reversed) previous2[tab.tabId] = tab.key;
    registry.sync(reversed, previous2);
    for (const std::string& key : keys) {
        const GroupState state = registry.state(key);
        if (!state.exclusive || !slots.count(state.slot)) {
            std::cerr << "FAIL registryEightDistinctAndStable: reorder changed a slot\n";
            return false;
        }
    }
    return true;
}

bool registryOverflowNineToEight() {
    Registry registry;
    std::vector<std::string> keys;
    for (int i = 0; i < 9; ++i) keys.push_back(canonicalRootKey(makeDir("of" + std::to_string(i))));
    std::unordered_map<std::uint64_t, std::string> previous;
    registry.sync(buildTabs(keys), previous);

    std::unordered_set<std::uint64_t> ordinals;
    int exclusive = 0;
    for (const std::string& key : keys) {
        const GroupState state = registry.state(key);
        if (state.ordinal == 0 || !ordinals.insert(state.ordinal).second) {
            std::cerr << "FAIL registryOverflowNineToEight: ordinals not unique\n";
            return false;
        }
        if (state.exclusive) ++exclusive;
    }
    if (exclusive > kSlotCount) {
        std::cerr << "FAIL registryOverflowNineToEight: more exclusive slots than the palette\n";
        return false;
    }

    // 9 → 8：关闭一个独占组后，剩余 8 组必须占满互不相同的槽位。
    std::string removed;
    for (const std::string& key : keys) {
        if (registry.state(key).exclusive) { removed = key; break; }
    }
    std::vector<std::string> remaining;
    for (const std::string& key : keys) if (key != removed) remaining.push_back(key);
    std::unordered_map<std::uint64_t, std::string> previous2;
    registry.sync(buildTabs(remaining), previous2);

    std::unordered_set<int> slots;
    for (const std::string& key : remaining) {
        const GroupState state = registry.state(key);
        if (!state.exclusive || !slots.insert(state.slot).second) {
            std::cerr << "FAIL registryOverflowNineToEight: 9->8 fallback left duplicate slots\n";
            return false;
        }
    }
    if (registry.groupCount() != 8) {
        std::cerr << "FAIL registryOverflowNineToEight: group count did not drop to 8\n";
        return false;
    }
    return true;
}

bool registryMergeInheritance() {
    Registry registry;
    const std::string docsKey = canonicalRootKey(makeDir("mroot/docs"));
    const std::string rootKey = canonicalRootKey(makeDir("mroot"));

    // 先只打开子根 A/docs（TabId 7）。
    registry.sync({{7, docsKey}}, {});
    const GroupState before = registry.state(docsKey);
    if (before.ordinal == 0) {
        std::cerr << "FAIL registryMergeInheritance: child group missing\n";
        return false;
    }

    // A 根随后打开并把 docs 合并进 A：新 key(rootKey) 应继承 docs 的 slot 与 ordinal。
    std::unordered_map<std::uint64_t, std::string> previous;
    previous[7] = docsKey;
    registry.sync({{5, rootKey}, {7, rootKey}}, previous);
    const GroupState merged = registry.state(rootKey);
    if (merged.ordinal != before.ordinal || merged.slot != before.slot) {
        std::cerr << "FAIL registryMergeInheritance: merged ancestor did not inherit slot/ordinal ("
                  << merged.slot << "/" << merged.ordinal << " vs " << before.slot << "/" << before.ordinal << ")\n";
        return false;
    }
    if (registry.groupCount() != 1) {
        std::cerr << "FAIL registryMergeInheritance: merged-away child group not released\n";
        return false;
    }

    // 同一根的多页共用一个组序号。
    registry.sync({{5, rootKey}, {7, rootKey}, {9, rootKey}}, previous);
    if (registry.groupCount() != 1 || registry.state(rootKey).ordinal != merged.ordinal) {
        std::cerr << "FAIL registryMergeInheritance: same-root pages diverged\n";
        return false;
    }
    return true;
}

bool registryInheritanceDoesNotDuplicateLiveOrSplitSources() {
    const std::string oldRoot = canonicalRootKey(makeDir("inherit/old"));
    const std::string saveAsRoot = canonicalRootKey(makeDir("inherit/save-as"));
    const std::string splitA = canonicalRootKey(makeDir("inherit/split-a"));
    const std::string splitB = canonicalRootKey(makeDir("inherit/split-b"));

    // SaveAs 改变一页的根，但旧根仍由另一页持有：新根必须获得新 ordinal/slot。
    Registry saveAs;
    saveAs.sync({{10, oldRoot}, {11, oldRoot}}, {});
    const std::uint64_t oldOrdinal = saveAs.state(oldRoot).ordinal;
    const auto oldSlot = saveAs.state(oldRoot).slot;
    saveAs.sync({{10, saveAsRoot}, {11, oldRoot}}, {{10, oldRoot}, {11, oldRoot}});
    if (saveAs.state(oldRoot).ordinal != oldOrdinal || saveAs.state(saveAsRoot).ordinal == oldOrdinal ||
        saveAs.state(saveAsRoot).slot == oldSlot) {
        std::cerr << "FAIL registryInheritanceDoesNotDuplicateLiveOrSplitSources: unrelated SaveAs inherited identity\n";
        return false;
    }

    Registry singleMove;
    singleMove.sync({{30, oldRoot}}, {});
    const auto singleOldOrdinal = singleMove.state(oldRoot).ordinal;
    singleMove.sync({{30, saveAsRoot}}, {{30, oldRoot}});
    if (singleMove.has(oldRoot) || singleMove.state(saveAsRoot).ordinal == singleOldOrdinal) {
        std::cerr << "FAIL registryInheritanceDoesNotDuplicateLiveOrSplitSources: unrelated move inherited identity\n";
        return false;
    }

    // 一个消失的旧根产生多个无关新根时，均不得继承旧 ordinal。
    Registry split;
    split.sync({{20, oldRoot}, {21, oldRoot}, {22, oldRoot}}, {});
    const std::uint64_t splitOldOrdinal = split.state(oldRoot).ordinal;
    split.sync({{20, splitA}, {21, splitB}}, {{20, oldRoot}, {21, oldRoot}});
    if (split.state(splitA).ordinal == splitOldOrdinal || split.state(splitB).ordinal == splitOldOrdinal ||
        split.state(splitA).ordinal == split.state(splitB).ordinal) {
        std::cerr << "FAIL registryInheritanceDoesNotDuplicateLiveOrSplitSources: unrelated split reused old ordinal\n";
        return false;
    }
    return true;
}

bool registryOrdinalNotReusedAndPruned() {
    Registry registry;
    const std::string a = canonicalRootKey(makeDir("ord/a"));
    const std::string b = canonicalRootKey(makeDir("ord/b"));
    registry.sync({{1, a}, {2, b}}, {});
    const std::uint64_t nextAfterTwo = registry.nextOrdinal();
    if (nextAfterTwo != 3) {
        std::cerr << "FAIL registryOrdinalNotReused: expected nextOrdinal 3 after two groups, got "
                  << nextAfterTwo << "\n";
        return false;
    }
    // 关闭 b：条目释放，但序号不复用。
    registry.sync({{1, a}}, {});
    if (registry.groupCount() != 1 || registry.has(b)) {
        std::cerr << "FAIL registryOrdinalNotReused: closed group not released\n";
        return false;
    }
    const std::string c = canonicalRootKey(makeDir("ord/c"));
    registry.sync({{1, a}, {3, c}}, {});
    if (registry.state(c).ordinal < nextAfterTwo) {
        std::cerr << "FAIL registryOrdinalNotReused: reused a closed group ordinal\n";
        return false;
    }
    registry.sync({}, {});
    if (registry.groupCount() != 0) {
        std::cerr << "FAIL registryOrdinalNotReused: clearing all did not release groups\n";
        return false;
    }
    for (int cycle = 0; cycle < 100; ++cycle) {
        const auto ordinalBefore = registry.nextOrdinal();
        registry.sync({{1, a}, {2, b}}, {});
        if (registry.groupCount() != 2 || registry.state(a).ordinal < ordinalBefore ||
            registry.state(b).ordinal == registry.state(a).ordinal) {
            std::cerr << "FAIL registryOrdinalNotReused: repeated open reused identity\n";
            return false;
        }
        registry.sync({{1, a}}, {});
        if (registry.groupCount() != 1 || registry.has(b)) return false;
        registry.sync({}, {});
        if (registry.groupCount() != 0 || registry.has(a)) {
            std::cerr << "FAIL registryOrdinalNotReused: repeated close leaked entries\n";
            return false;
        }
    }
    return true;
}

// ── 顶栏几何 / 关闭锚点 ───────────────────────────────────────────────────
bool layoutFormula() {
    if (!closeEnough(layout::contentWidth(0), 0.0f) || !closeEnough(layout::contentWidth(1), 176.0f) ||
        !closeEnough(layout::contentWidth(4), 716.0f)) {
        std::cerr << "FAIL layoutFormula: contentWidth\n";
        return false;
    }
    if (!closeEnough(layout::maxScroll(4, 300.0f), 416.0f) || !closeEnough(layout::maxScroll(2, 500.0f), 0.0f)) {
        std::cerr << "FAIL layoutFormula: maxScroll\n";
        return false;
    }
    if (!closeEnough(layout::cardLeft(2, 10.0f, 0.0f), 350.0f) ||
        !closeEnough(layout::closeCenterX(2, 10.0f, 0.0f), 512.0f) ||
        !closeEnough(layout::cardLeft(2, 10.0f, 180.0f), 530.0f)) {
        std::cerr << "FAIL layoutFormula: cardLeft/closeCenterX\n";
        return false;
    }
    return true;
}

bool closeChainMiddleAndTail() {
    // 中间连续关闭 3 次：指针固定在被关页的关闭中心，右邻补位后中心误差 ≤1 DIP。
    {
        std::vector<std::uint64_t> order{1, 2, 3, 4, 5, 6};
        float scroll = 0.0f;
        float anchorX = layout::closeCenterX(2, scroll, 0.0f);
        for (int step = 0; step < 3; ++step) {
            const std::uint64_t closed = order[2];
            const std::uint64_t next = order[3];
            layout::CloseAnchorInput input;
            input.enabled = true;
            input.anchorX = anchorX;
            input.closedTabId = closed;
            input.expectedNextId = next;
            input.beforeTabOrder = order;
            order.erase(order.begin() + 2);
            const layout::CloseAnchorPlan plan = layout::planCloseAnchor(input, order, next, scroll);
            if (!plan.valid) {
                std::cerr << "FAIL closeChainMiddleAndTail: middle plan invalid at step " << step << "\n";
                return false;
            }
            const auto it = std::find(order.begin(), order.end(), next);
            const std::size_t index = static_cast<std::size_t>(std::distance(order.begin(), it));
            if (!closeEnough(layout::closeCenterX(index, scroll, plan.drawOffset), anchorX)) {
                std::cerr << "FAIL closeChainMiddleAndTail: middle center drifted at step " << step << "\n";
                return false;
            }
        }
    }
    // 尾部连续关闭 3 次：左邻整体右移到原关闭位置（允许左侧临时空白）。
    {
        std::vector<std::uint64_t> order{1, 2, 3, 4, 5};
        const float scroll = 0.0f;
        float anchorX = layout::closeCenterX(4, scroll, 0.0f);
        for (int step = 0; step < 3; ++step) {
            const std::size_t last = order.size() - 1;
            const std::uint64_t closed = order[last];
            const std::uint64_t next = order[last - 1];
            layout::CloseAnchorInput input;
            input.enabled = true;
            input.anchorX = anchorX;
            input.closedTabId = closed;
            input.expectedNextId = next;
            input.beforeTabOrder = order;
            order.pop_back();
            const layout::CloseAnchorPlan plan = layout::planCloseAnchor(input, order, next, scroll);
            if (!plan.valid) {
                std::cerr << "FAIL closeChainMiddleAndTail: tail plan invalid at step " << step << "\n";
                return false;
            }
            const auto it = std::find(order.begin(), order.end(), next);
            const std::size_t index = static_cast<std::size_t>(std::distance(order.begin(), it));
            if (!closeEnough(layout::closeCenterX(index, scroll, plan.drawOffset), anchorX)) {
                std::cerr << "FAIL closeChainMiddleAndTail: tail center drifted at step " << step << "\n";
                return false;
            }
            if (step == 0 && !closeEnough(plan.drawOffset, 180.0f)) {
                std::cerr << "FAIL closeChainMiddleAndTail: tail offset should move left neighbour by one pitch\n";
                return false;
            }
        }
    }
    return true;
}

bool closeAnchorInvalidation() {
    const std::vector<std::uint64_t> before{1, 2, 3};
    layout::CloseAnchorInput input;
    input.enabled = true;
    input.anchorX = layout::closeCenterX(1, 0.0f, 0.0f);
    input.closedTabId = 2;
    input.expectedNextId = 3;
    input.beforeTabOrder = before;

    // 最后一页被关闭：expectedNextId = 0 → 无效（关闭链结束）。
    layout::CloseAnchorInput lastPage = input;
    lastPage.closedTabId = 1;
    lastPage.expectedNextId = 0;
    if (layout::planCloseAnchor(lastPage, {2, 3}, 3, 0.0f).valid) {
        std::cerr << "FAIL closeAnchorInvalidation: last-page close kept the chain\n";
        return false;
    }
    // 顺序被改动（重排/新建/打开）→ 无效。
    if (layout::planCloseAnchor(input, {1, 3, 4}, 3, 0.0f).valid) {
        std::cerr << "FAIL closeAnchorInvalidation: reordered tab order kept the chain\n";
        return false;
    }
    // 下一目标不存在 → 无效。
    layout::CloseAnchorInput missing = input;
    missing.expectedNextId = 99;
    if (layout::planCloseAnchor(missing, {1, 3}, 3, 0.0f).valid) {
        std::cerr << "FAIL closeAnchorInvalidation: missing next target kept the chain\n";
        return false;
    }
    // 显式切页（当前页不是预期下一目标）→ 无效。
    if (layout::planCloseAnchor(input, {1, 3}, 1, 0.0f).valid) {
        std::cerr << "FAIL closeAnchorInvalidation: explicit switch kept the chain\n";
        return false;
    }
    // 正常中间关闭仍有效。
    if (!layout::planCloseAnchor(input, {1, 3}, 3, 0.0f).valid) {
        std::cerr << "FAIL closeAnchorInvalidation: valid middle close rejected\n";
        return false;
    }
    // NaN/Inf 几何不得进入绘制；长尾连续关闭的合法偏移不设置任意节距上限。
    layout::CloseAnchorInput nonFinite = input;
    nonFinite.anchorX = std::numeric_limits<float>::quiet_NaN();
    if (layout::planCloseAnchor(nonFinite, {1, 3}, 3, 0.0f).valid) {
        std::cerr << "FAIL closeAnchorInvalidation: NaN anchor accepted\n";
        return false;
    }
    nonFinite = input;
    if (layout::planCloseAnchor(nonFinite, {1, 3}, 3, std::numeric_limits<float>::infinity()).valid) {
        std::cerr << "FAIL closeAnchorInvalidation: infinite scroll accepted\n";
        return false;
    }
    layout::CloseAnchorInput longTail = input;
    longTail.anchorX = layout::closeCenterX(100, 0.0f, 0.0f);
    if (!layout::planCloseAnchor(longTail, {1, 3}, 3, 0.0f).valid) {
        std::cerr << "FAIL closeAnchorInvalidation: valid large finite offset rejected\n";
        return false;
    }
    longTail.anchorX = std::numeric_limits<float>::max();
    if (layout::planCloseAnchor(longTail, {1, 3}, 3, std::numeric_limits<float>::max()).valid) {
        std::cerr << "FAIL closeAnchorInvalidation: overflowing draw offset accepted\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!rootIdentity()) return 1;
    if (!paletteDistinct()) return 1;
    if (!preferredSlotDeterministic()) return 1;
    if (!registryEightDistinctAndStable()) return 1;
    if (!registryOverflowNineToEight()) return 1;
    if (!registryMergeInheritance()) return 1;
    if (!registryInheritanceDoesNotDuplicateLiveOrSplitSources()) return 1;
    if (!registryOrdinalNotReusedAndPruned()) return 1;
    if (!layoutFormula()) return 1;
    if (!closeChainMiddleAndTail()) return 1;
    if (!closeAnchorInvalidation()) return 1;
    std::cout << "tab_presentation: all checks passed\n";
    return 0;
}
