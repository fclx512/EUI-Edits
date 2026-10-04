#pragma once

#include "core/dsl.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

// Small vector symbols shared by generic components. This header depends only
// on the core drawing DSL and deliberately has no EUI-Edits model dependency.
namespace components::vector_icon {

template <typename Draw>
inline void canvas(core::dsl::Ui& ui, const std::string& id,
                   float x, float y, float width, float height, Draw&& draw) {
    ui.stack(id).position(x, y).size(width, height)
        .content(std::forward<Draw>(draw)).build();
}

inline void segment(core::dsl::Ui& ui, const std::string& id,
                    float x, float y, float width, float height,
                    core::Vec2 from, core::Vec2 to, float thickness,
                    const core::Color& color) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.001f || thickness <= 0.0f) return;
    const float nx = -dy / length * thickness * 0.5f;
    const float ny = dx / length * thickness * 0.5f;
    std::vector<core::Vec2> points{
        {from.x + nx, from.y + ny}, {to.x + nx, to.y + ny},
        {to.x - nx, to.y - ny}, {from.x - nx, from.y - ny}
    };
    ui.polygon(id).position(x, y).size(width, height)
        .points(std::move(points)).color(color).build();
}

inline void drawCheckmark(core::dsl::Ui& ui, const std::string& id,
                          float x, float y, float size, const core::Color& color) {
    // 与界面其它 1.5–2px 线性符号同量级：笔画收细、两端与拐点收圆，
    // 勾不再比它标注的文字还重。圆头同样用多边形画，符号保持"纯矢量顶点"，
    // 不混入圆角矩形节点（tests/unit/context_menu.cpp 按此断言）。
    const float thickness = std::max(1.0f, size * 0.09f);
    const core::Vec2 start{size * 0.20f, size * 0.53f};
    const core::Vec2 corner{size * 0.42f, size * 0.75f};
    const core::Vec2 end{size * 0.82f, size * 0.23f};
    canvas(ui, id, x, y, size, size, [&] {
        const auto cap = [&](const char* part, const core::Vec2& point) {
            const float radius = thickness * 0.5f;
            std::vector<core::Vec2> disc;
            disc.reserve(10);
            for (int step = 0; step < 10; ++step) {
                const float angle = 6.28318531f * static_cast<float>(step) / 10.0f;
                disc.push_back({point.x + radius * std::cos(angle),
                                point.y + radius * std::sin(angle)});
            }
            ui.polygon(id + part).size(size, size).points(std::move(disc))
                .color(color).build();
        };
        segment(ui, id + ".short", 0.0f, 0.0f, size, size, start, corner, thickness, color);
        segment(ui, id + ".long", 0.0f, 0.0f, size, size, corner, end, thickness, color);
        cap(".cap.start", start);
        cap(".cap.corner", corner);
        cap(".cap.end", end);
    });
}

inline void drawChevronRight(core::dsl::Ui& ui, const std::string& id,
                             float x, float y, float size, const core::Color& color) {
    const float thickness = std::max(1.0f, size * 0.16f);
    canvas(ui, id, x, y, size, size, [&] {
        segment(ui, id + ".upper", 0.0f, 0.0f, size, size,
                {size * 0.34f, size * 0.17f}, {size * 0.68f, size * 0.50f}, thickness, color);
        segment(ui, id + ".lower", 0.0f, 0.0f, size, size,
                {size * 0.68f, size * 0.50f}, {size * 0.34f, size * 0.83f}, thickness, color);
    });
}

inline void drawChevronDown(core::dsl::Ui& ui, const std::string& id,
                           float x, float y, float size, const core::Color& color) {
    const float thickness = std::max(1.0f, size * 0.16f);
    canvas(ui, id, x, y, size, size, [&] {
        segment(ui, id + ".left", 0.0f, 0.0f, size, size,
                {size * 0.17f, size * 0.34f}, {size * 0.50f, size * 0.68f}, thickness, color);
        segment(ui, id + ".right", 0.0f, 0.0f, size, size,
                {size * 0.50f, size * 0.68f}, {size * 0.83f, size * 0.34f}, thickness, color);
    });
}

inline void drawImage(core::dsl::Ui& ui, const std::string& id,
                      float x, float y, float width, float height,
                      const core::Color& color) {
    const float frameWidth = std::min(width * 0.48f, height * 0.74f);
    const float frameHeight = std::min(height * 0.68f, frameWidth * 0.78f);
    if (frameWidth < 3.0f || frameHeight < 3.0f) return;
    const float line = std::max(1.0f, std::min(frameWidth, frameHeight) * 0.055f);
    canvas(ui, id, x, y, width, height, [&] {
        const float localFrameX = (width - frameWidth) * 0.5f;
        const float localFrameY = (height - frameHeight) * 0.5f;
        ui.rect(id + ".frame").position(localFrameX, localFrameY).size(frameWidth, frameHeight)
            .radius(line * 1.5f).color(core::Color{0, 0, 0, 0})
            .border(line, color).build();
        const float sun = frameHeight * 0.13f;
        ui.rect(id + ".sun").position(localFrameX + frameWidth * 0.67f,
                                       localFrameY + frameHeight * 0.18f)
            .size(sun, sun).radius(sun * 0.5f).color(color).build();

        const float baseY = frameHeight * 0.84f;
        const float mountainThickness = std::max(1.0f, line * 1.5f);
        const core::Vec2 leftPeak{frameWidth * 0.10f, baseY};
        const core::Vec2 leftTop{frameWidth * 0.38f, frameHeight * 0.44f};
        const core::Vec2 middleLow{frameWidth * 0.57f, frameHeight * 0.69f};
        const core::Vec2 rightTop{frameWidth * 0.75f, frameHeight * 0.50f};
        const core::Vec2 rightBase{frameWidth * 0.94f, baseY};
        segment(ui, id + ".mountain.left", localFrameX, localFrameY, frameWidth, frameHeight,
                leftPeak, leftTop, mountainThickness, color);
        segment(ui, id + ".mountain.middle", localFrameX, localFrameY, frameWidth, frameHeight,
                leftTop, middleLow, mountainThickness, color);
        segment(ui, id + ".mountain.right", localFrameX, localFrameY, frameWidth, frameHeight,
                middleLow, rightTop, mountainThickness, color);
        segment(ui, id + ".mountain.base", localFrameX, localFrameY, frameWidth, frameHeight,
                rightTop, rightBase, mountainThickness, color);
    });
}

} // namespace components::vector_icon
