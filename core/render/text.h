#pragma once

#include "core/render/render_types.h"
#include "core/render/text_types.h"

#include <cstddef>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

namespace core {

// Read-only diagnostic snapshot used by atlas growth regression tests and paired
// measurements. This reports CPU atlas storage, not renderer-side bitmap/GPU memory.
struct TextAtlasDebugStats {
    int grayWidth = 0;
    int grayHeight = 0;
    std::size_t grayCapacityBytes = 0;
    std::size_t grayGlyphs = 0;
    std::uint64_t grayGeneration = 0;
    std::uint64_t grayGrowthCount = 0;
    std::uint64_t grayOverflowResetCount = 0;
    int colorWidth = 0;
    int colorHeight = 0;
    std::size_t colorCapacityBytes = 0;
    std::size_t colorGlyphs = 0;
    std::uint64_t colorGeneration = 0;
    std::uint64_t overflowResetCount = 0;
};

class TextPrimitive {
public:
    struct Glyph {
        float advance = 0.0f;
        float xOffset = 0.0f;
        float yOffset = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 0.0f;
        float v1 = 0.0f;
        bool colored = false;
        // 图集重置纪元。图集满时会整页作废重来，此时所有已缓存的 UV 全部失效；
        // 图元发现自己缓存里的 epoch 落后于全局值就重新栅格化（见 text.cpp）。
        std::uint64_t atlasEpoch = 0;
    };

    struct ShapedGlyph {
        std::uint64_t key = 0;
        unsigned int codepoint = 0;
        int byteStart = 0;
        int byteEnd = 0;
        float advance = 0.0f;
        float xOffset = 0.0f;
        float yOffset = 0.0f;
    };

    struct TextMetrics {
        float width = 0.0f;
        std::vector<int> byteIndices;
        std::vector<float> caretX;
    };

    TextPrimitive();
    TextPrimitive(float x, float y);
    ~TextPrimitive();

    TextPrimitive(const TextPrimitive&) = delete;
    TextPrimitive& operator=(const TextPrimitive&) = delete;
    TextPrimitive(TextPrimitive&&) noexcept;
    TextPrimitive& operator=(TextPrimitive&&) noexcept;

    bool initialize();
    void destroy();

    void setPosition(float x, float y);
    void setText(const std::string& text);
    void setFontFamily(const std::string& fontFamily);
    void setFontSize(float fontSize);
    void setFontWeight(int fontWeight);
    void setColor(const Color& color);
    void setMaxWidth(float maxWidth);
    void setWrap(bool wrap);
    void setHorizontalAlign(HorizontalAlign align);
    void setVerticalAlign(VerticalAlign align);
    void setLineHeight(float lineHeight);
    void setStyle(const TextStyle& style);
    void setVisualScale(float originX, float originY, float scale);
    void setTransform(const Transform& transform, const Rect& frame);
    void setTransformMatrix(const TransformMatrix& matrix);

    const TextStyle& style() const;
    Vec2 position() const;
    Vec2 measuredSize();
    static float measureTextWidth(const std::string& text,
                                  const std::string& fontFamily = {},
                                  float fontSize = 16.0f,
                                  int fontWeight = 400);
    static TextMetrics measureTextMetrics(const std::string& text,
                                          const std::string& fontFamily = {},
                                          float fontSize = 16.0f,
                                          int fontWeight = 400);
    static Vec2 measureTextSize(const TextStyle& style);
    static TextAtlasDebugStats debugAtlasStats();

    // ── 排版层（逻辑像素）的设备缩放 ────────────────────────────────────────
    // 两套坐标空间在同一个管线里并存：
    //   · **渲染**：runtime_render.h 交给 TextPrimitive 的字号是**设备像素**
    //     （toPixels(fontSize, dpiScale)），字形按 round(设备字号) 取 ppem；
    //   · **排版**：components/ 那层（input_model 的 measureMetrics、DSL 的
    //     固有尺寸）拿到的是**逻辑像素**字号，还得在逻辑空间里算宽度。
    // 麻烦在于 hint：带 hinting 时 FreeType 把左右边距 snap 到整数像素，
    // `advance.x` 是**量化**过的，因而 NOT 与字号成正比 ——
    // 14 逻辑 px（ppem 14）每字 8px，1.25 缩放下渲染的 17.5 设备 px（ppem 18）
    // 每字 11 设备 px = 8.8 逻辑 px，同一串文字两边差 10%。
    // 排版层因此必须知道这个缩放：用「逻辑字号 × scale」去取字形，再把结果除回
    // 逻辑单位。这样两边落在**同一个 ppem** 上，advance / caret / 选区矩形才与
    // 真正画出来的字格严丝合缝（不等的话每个 run 内部按自己的 ppem 排字，会溢出
    // 排版时留给它的宽度，压到下一个 run 的头上 —— 实测表现为代码块里
    // `function hello` 的空格被吞、行尾字符被后一个 run 覆盖）。
    // 默认 1.0：不设等于完全保持老行为（单测与不关心 DPI 的调用方都走这条）。
    static void setLayoutPixelScale(float scale);
    static float layoutPixelScale();

    static void setDefaultFontFiles(const std::string& textFontFile, const std::string& iconFontFile);
    // 斜体字面解析（S3d 降级版）：给常规字面找同族斜体文件（X.ttf → Xi.ttf / X-Italic.ttf /
    // Regular→Italic …），并用 FreeType 校验候选确实是斜体。找不到返回**空串**，调用方据此
    // 回落常规字面 —— 本框架不做合成斜体（与 resolveBoldFontPath 同理：合成要改渲染管线）。
    static std::string resolveItalicFontPath(const std::string& fontFamily);

    void prepare();
    void render(int windowWidth, int windowHeight);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace core
