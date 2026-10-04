// 图集溢出回归：先让灰度图集从512²增长到2048²，再撑爆最大页，验证旧的
// "整页作废重来"路径仍可恢复。
//
// 修过的 bug：图集满之后 appendToAtlas 失败，代码会把一枚"UV 全 0"的空字形
// 缓存进图元；而长驻图元让图集引用计数永不归零，图集再也出不来——
// 之后每个新字形都永久空白，表现为"字用着用着缺字、支持的字形也画不出来"。
// 现在的行为：溢出 → 整页作废（清 UV 索引与像素）+ 全局纪元 +1 →
// 各图元下次 prepare() 发现纪元落后，整体重排、重新栅格化。
//
// 只能做冒烟级断言（字形 UV 是内部状态，公开 API 看不见）：
// 溢出过程不炸、尺寸稳定、重置后再 prepare 走重排也不炸。

#include "core/render/text.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using core::TextPrimitive;

namespace {

// 生成 count 个互不相同的 CJK 汉字（基本区 U+4E00 起，默认字体链一定有字形）。
std::string uniqueChars(int count) {
    std::string text;
    text.reserve(static_cast<std::size_t>(count) * 3);
    for (int i = 0; i < count; ++i) {
        const unsigned int cp = 0x4E00u + static_cast<unsigned int>(i);
        text += static_cast<char>(0xE0 | (cp >> 12));
        text += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return text;
}

}  // namespace

int main() {
#ifdef _WIN32
    _putenv_s("NEO_GRAY_ATLAS_INITIAL_SIZE", "");
#else
    unsetenv("NEO_GRAY_ATLAS_INITIAL_SIZE");
#endif
    // 保持这些图元同时存活；不同字号/码点让共享字形池超过最大页容量。
    constexpr int kCharsPerSize = 1800;
    constexpr float kSizes[] = {16.0f, 24.0f, 32.0f, 40.0f};
    std::vector<TextPrimitive> primitives;
    std::vector<core::Vec2> sizes;
    primitives.reserve(std::size(kSizes));
    sizes.reserve(std::size(kSizes));

    for (std::size_t index = 0; index < std::size(kSizes); ++index) {
        const float size = kSizes[index];
        primitives.emplace_back();
        TextPrimitive& primitive = primitives.back();
        if (!primitive.initialize()) {
            std::cerr << "TextPrimitive::initialize failed at size " << size << "\n";
            return 1;
        }
        primitive.setFontSize(size);
        primitive.setMaxWidth(8192.0f);
        primitive.setText(uniqueChars(kCharsPerSize));
        primitive.prepare();  // rebuildLayout → ensureGlyph → 光栅化（中途可能溢出重置）

        const core::Vec2 first = primitive.measuredSize();
        if (!(first.x > 0.0f) || !(first.y > 0.0f)) {
            std::cerr << "measuredSize invalid at size " << size << "\n";
            return 1;
        }
        sizes.push_back(first);
    }

    const core::TextAtlasDebugStats overflow = TextPrimitive::debugAtlasStats();
    if (overflow.grayWidth != 2048 || overflow.grayHeight != 2048 ||
        overflow.grayOverflowResetCount == 0 || overflow.overflowResetCount == 0) {
        std::cerr << "maximum gray atlas overflow was not observed\n";
        return 1;
    }

    // A following prepare must re-layout against the reset generation without changing
    // font geometry, even when an earlier primitive's UVs were invalidated by another.
    for (std::size_t index = 0; index < primitives.size(); ++index) {
        primitives[index].prepare();
        const core::Vec2 second = primitives[index].measuredSize();
        if (std::fabs(sizes[index].x - second.x) > 0.5f ||
            std::fabs(sizes[index].y - second.y) > 0.5f) {
            std::cerr << "measuredSize drifted after atlas reset at index " << index << "\n";
            return 1;
        }
    }

    std::printf("text atlas overflow: ALL PASS\n");
    return 0;
}
