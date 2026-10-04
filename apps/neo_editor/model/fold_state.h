#pragma once

// R8：折叠状态的纯逻辑 —— 折叠键对账 + 命中行的章节祖先链。
//
// 为什么单独一层：foldedHeadings 是按**字节偏移** keyed 的集合，任何文本改动或换
// 文档都可能让它错位甚至指向别的标题。以前只有 CleanAi 成功路径清空，打字 / 粘贴 /
// 剪切 / 命令回写 / 换文档都漏了（指引 §4 R8 缺口 2）。判据只能有一份，应用层各处
// 调用它，不各写一份。
//
// 这里只放纯函数（不碰 AppState / UI），因此可以直接单测：折叠键的映射规则与
// "命中行要展开哪些标题"这两件事的边界都能逐条钉住。

#include "components/input_model.h"
#include "model/lp_plan.h"

#include <cstddef>
#include <set>
#include <vector>

namespace neo::foldstate {

using PendingTextEdit = components::input_detail::PendingTextEdit;

// 把折叠键按一次**可信**的文本改动平移（改动区间来自组件的 PendingTextEdit：
// 旧文本 [byteBeg, oldEnd) 被换成新文本 [byteBeg, newEnd)）。
//
// 返回 false = 证明不了（有键落在被替换区间内部）→ 调用方必须清空并展开，
// 不允许带着可能失效的键继续用。判据：
//   · key <= byteBeg  → 不动（编辑起点处及之前不变；行首偏移在此期间不受影响）
//   · key >= oldEnd   → 平移 delta = newEnd - oldEnd
//   · 其余（落在 [byteBeg, oldEnd) 内）→ false
inline bool mapFoldedKeys(std::set<int>& keys, const PendingTextEdit& edit) {
    const int delta = edit.newEnd - edit.oldEnd;
    // 空改动（没有字节进出）：键全部原样，不用重建集合。
    if (delta == 0 && edit.oldEnd == edit.byteBeg) {
        return true;
    }
    std::vector<int> mapped;
    mapped.reserve(keys.size());
    for (const int key : keys) {
        if (key <= edit.byteBeg) {
            mapped.push_back(key);
        } else if (key >= edit.oldEnd) {
            mapped.push_back(key + delta);
        } else {
            return false;
        }
    }
    keys.clear();
    keys.insert(mapped.begin(), mapped.end());
    return true;
}

// 命中偏移所在行的**章节祖先链**（由内到外）对应的标题起始字节偏移 ——
// 也就是"必须展开才能让这一行露出来"的那批折叠键。
//   · 正文行 → 从包住它的标题（sectionHeadingLine）起，沿 parentHeadingLine 上溯；
//   · 标题起始行（含 setext 文本行）→ 从**父标题**起：标题自己那一行是可见的，
//     折叠它只隐藏其后的内容，命中它不需要展开它自己；
//   · setext 下划线行 → 同样从配对文本行的父标题起（下划线行也可见）。
inline std::vector<int> sectionAncestorKeys(const LpPlan& plan, int byteOffset) {
    std::vector<int> keys;
    const int index = plan.lineIndexFor(byteOffset);
    const LpLine* line = index >= 0 ? plan.lineAt(index) : nullptr;
    if (line == nullptr) {
        return keys;
    }
    // sectionEndLine >= 0 是"标题起始行"的既有判据（见 LpPlan::foldHeadingBegFor），
    // setext 下划线行不记终点 → 落到"配对文本行"这一支。
    int start = line->sectionHeadingLine;
    if (line->kind == LpKind::Heading) {
        if (line->sectionEndLine >= 0) {
            start = line->parentHeadingLine;
        } else {
            const LpLine* paired =
                line->sectionHeadingLine >= 0 ? plan.lineAt(line->sectionHeadingLine) : nullptr;
            start = paired != nullptr ? paired->parentHeadingLine : -1;
        }
    }
    // 上限只是防御：文档再深也不会有几十层标题互相嵌套。
    for (int ancestor = start; ancestor >= 0 && keys.size() < 64;) {
        const LpLine* heading = plan.lineAt(ancestor);
        if (heading == nullptr) {
            break;
        }
        keys.push_back(heading->srcBeg);
        ancestor = heading->parentHeadingLine;
    }
    return keys;
}

} // namespace neo::foldstate
