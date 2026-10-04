#pragma once

// 标签展示身份（2026-10-03 视觉改进批次）——**纯数据/纯逻辑**，不依赖 AppState、主题与
// 渲染层，便于单测直接钉住"根身份 / 调色 / 槽位分配 / 碰撞溢出"这几条规则。
//
// 三条不可动摇的契约：
//   1. 身份只看**合并后的 vaultRoot**，与文件名、扩展名、图标无关；颜色只是展示信息，
//      绝不参与文件定位、库扫描或恢复身份判断。
//   2. 规范化复用项目既有的路径转换（textfile::pathFromUtf8/pathToUtf8）与 Windows
//      CompareStringOrdinal(...,TRUE) 的"忽略大小写"语义；canonical 失败退回词法身份。
//   3. 有限 8 色：当前独立根 ≤8 时槽位必须互不相同；>8 时允许复用颜色，但**组序号
//      不得复用**，据此仍可区分身份。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace neo {
namespace tabpresentation {

inline constexpr int kSlotCount = 8;

struct Rgb {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

// 浅色/暗色两套 8 槽库色。执行端须在实际浅/深/自定义主题上验收。
const Rgb* lightPalette();  // 长度 kSlotCount
const Rgb* darkPalette();   // 长度 kSlotCount
Rgb paletteColor(bool dark, int slot);

// ── 根身份 ────────────────────────────────────────────────────────────────
// canonicalRootKey：访问文件系统的规范化（weakly_canonical，失败退 absolute+lexically_normal），
//   再统一分隔符/尾分隔符并做 Unicode 忽略大小写折叠。**只能在动作阶段调用并缓存**，
//   绝不在每帧绘制/列表/提示路径里查询。
// lexicalRootKey：不访问文件系统的词法身份（absolute+lexically_normal 失败再退回纯词法），
//   用于 canonical 不可用（网络路径等）时的明确边界。
std::string canonicalRootKey(const std::string& rootUtf8);
std::string lexicalRootKey(const std::string& rootUtf8);

// 固定确定性首选槽位：FNV-1a(规范化 key) % kSlotCount。
int preferredSlot(const std::string& canonicalKey);

// ── 槽位分配 ──────────────────────────────────────────────────────────────
struct GroupState {
    int slot = 0;
    bool exclusive = false;      // true = 独占该槽（当前 ≤8 组的"互不相同"来源）
    std::uint64_t ordinal = 0;   // 本进程单调分配，从 1 开始；同组多页共用一个号
};

class Registry {
public:
    struct OpenTab {
        std::uint64_t tabId = 0;
        std::string key;  // 规范化后的展示根 key
    };

    // 用当前打开的分组刷新分配。openTabs 顺序无关（组内取最小 TabId 决定先后）；
    // prevKeyByTab 提供上一轮的 TabId→key，用于父子合并时"继承被合并祖先组"的槽位/序号。
    // 组集合与内容都没变时是 no-op（调用方还应先做签名短路）。
    void sync(const std::vector<OpenTab>& openTabs,
              const std::unordered_map<std::uint64_t, std::string>& prevKeyByTab);

    bool has(const std::string& key) const;
    GroupState state(const std::string& key) const;
    std::size_t groupCount() const { return groups_.size(); }
    std::uint64_t nextOrdinal() const { return nextOrdinal_; }
    void clear();

private:
    struct Group {
        int slot = 0;
        bool exclusive = false;
        std::uint64_t ordinal = 0;
        std::uint64_t minTabId = 0;
        bool assigned = false;  // 槽位是否已确定（继续/继承的组为 true，全新组为 false）
    };
    std::unordered_map<std::string, Group> groups_;
    std::uint64_t nextOrdinal_ = 1;
};

// ── 顶栏几何与关闭锚点（纯函数；绘制与测试共用同一套公式）────────────────
namespace layout {

inline constexpr float tabWidth = 176.0f;           // 常规卡面宽
inline constexpr float tabGap = 4.0f;               // 卡间距
inline constexpr float tabPitch = tabWidth + tabGap;  // 180 节距
inline constexpr float closeCenterInset = 162.0f;   // 关闭中心 = cardLeft + 162

inline float contentWidth(std::size_t count) {
    if (count == 0) return 0.0f;
    return static_cast<float>(count) * tabWidth + static_cast<float>(count - 1) * tabGap;
}

inline float maxScroll(std::size_t count, float regionWidth) {
    const float content = contentWidth(count);
    return content > regionWidth ? content - regionWidth : 0.0f;
}

inline float cardLeft(std::size_t index, float scroll, float drawOffset) {
    return static_cast<float>(index) * tabPitch - scroll + drawOffset;
}

inline float closeCenterX(std::size_t index, float scroll, float drawOffset) {
    return cardLeft(index, scroll, drawOffset) + closeCenterInset;
}

struct CloseAnchorInput {
    bool enabled = false;
    float anchorX = 0.0f;
    std::uint64_t closedTabId = 0;
    std::uint64_t expectedNextId = 0;
    std::vector<std::uint64_t> beforeTabOrder;
};

struct CloseAnchorPlan {
    bool valid = false;
    float drawOffset = 0.0f;
};

// 用"关闭前顺序 - closedTabId"严格证明本次删除，并算出固定指针所需的绘制偏移。
// 任一条件不满足（顺序被改、下一目标不在、最后一页被关、当前页不是预期下一目标）→ 无效。
inline CloseAnchorPlan planCloseAnchor(const CloseAnchorInput& anchor,
                                       const std::vector<std::uint64_t>& currentOrder,
                                       std::uint64_t currentTabId, float scroll) {
    CloseAnchorPlan plan;
    if (!anchor.enabled || !std::isfinite(anchor.anchorX) || !std::isfinite(scroll) ||
        anchor.closedTabId == 0 || anchor.expectedNextId == 0 || anchor.beforeTabOrder.empty()) return plan;
    if (std::count(anchor.beforeTabOrder.begin(), anchor.beforeTabOrder.end(), anchor.closedTabId) != 1) {
        return plan;
    }
    std::vector<std::uint64_t> expected = anchor.beforeTabOrder;
    expected.erase(std::remove(expected.begin(), expected.end(), anchor.closedTabId), expected.end());
    const bool closedGone =
        std::find(currentOrder.begin(), currentOrder.end(), anchor.closedTabId) == currentOrder.end();
    const bool nextAlive =
        std::find(currentOrder.begin(), currentOrder.end(), anchor.expectedNextId) != currentOrder.end();
    if (!closedGone || !nextAlive || currentOrder != expected || currentTabId != anchor.expectedNextId) {
        return plan;
    }
    const auto it = std::find(currentOrder.begin(), currentOrder.end(), anchor.expectedNextId);
    const std::size_t index = static_cast<std::size_t>(std::distance(currentOrder.begin(), it));
    const float drawOffset = anchor.anchorX - closeCenterX(index, scroll, 0.0f);
    if (!std::isfinite(drawOffset)) return plan;
    plan.valid = true;
    plan.drawOffset = drawOffset;
    return plan;
}

}  // namespace layout

}  // namespace tabpresentation
}  // namespace neo
