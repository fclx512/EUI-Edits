#pragma once

// 只读思维导图预览的**轻量**状态：身份锚 + 视口 + 拒绝原因。
// 解析树 / 块切片 / 标签尺寸 / 布局几何都在页自有的 mindmap::Cache 里
// （model/mindmap_cache.h）：open 冷路径把解析结果 move 进缓存，本结构不保留
// 第二份树。开块固定：打开后光标移动既不改变也不失效模型（只认语法，见
// state/mindmap_preview_state.h 的锚点重校验）。

#include "image_viewport.h"
#include "mindmap_plan.h"

#include <cstdint>
#include <string>

namespace neo::mindmap {

struct PreviewState {
    bool open = false;
    bool inlineMode = false;
    std::uint64_t tabId = 0;
    std::string path;
    unsigned long long revision = 0;
    unsigned long long textRevision = 0;
    ImageViewport viewport;
    RejectReason rejectReason = RejectReason::None;

    // 关闭 = 只清临时 UI 值与身份锚；页自有的 mindmap::Cache 原样保留（供暖开、
    // 可淘汰）。视口归零（实施计划 §8：视口/焦点临时值关闭时清零）。
    void close() {
        open = false;
        inlineMode = false;
        tabId = 0;
        std::string{}.swap(path);
        revision = 0;
        textRevision = 0;
        viewport = ImageViewport{};
        rejectReason = RejectReason::None;
    }
};

} // namespace neo::mindmap
