#include "core/render/text.h"
#include "core/render/render_backend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void useDefaultAtlasStart() {
#ifdef _WIN32
    _putenv_s("NEO_GRAY_ATLAS_INITIAL_SIZE", "");
#else
    unsetenv("NEO_GRAY_ATLAS_INITIAL_SIZE");
#endif
}

std::string utf8(unsigned int cp) {
    std::string out;
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
    return out;
}

std::string uniqueCjk(unsigned int first, int count) {
    std::string out;
    out.reserve(static_cast<std::size_t>(count) * 3);
    for (int i = 0; i < count; ++i) out += utf8(first + static_cast<unsigned int>(i));
    return out;
}

bool sameSize(core::Vec2 a, core::Vec2 b) {
    return std::fabs(a.x - b.x) < 0.01f && std::fabs(a.y - b.y) < 0.01f;
}

struct AtlasCopy {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<unsigned char> pixels;
};

struct TextCopy {
    std::vector<float> vertices;
    AtlasCopy gray;
    AtlasCopy color;
};

class RecordingBackend final : public core::render::RenderBackend {
public:
    bool initialize() override { return true; }
    bool valid() const override { return true; }
    void makeCurrent() override {}
    void beginFrame(const core::render::RenderSurface&) override {}
    void present() override {}
    bool ensureRenderCache(int, int) override { return false; }
    bool renderCacheWasRecreated() const override { return false; }
    void releaseRenderCache() override {}
    void beginRenderCacheFrame(int, int, const std::vector<core::Rect>&) override {}
    void endRenderCacheFrame() override {}
    void blitRenderCache(int, int, core::render::RenderCacheBlitMode,
                         const std::vector<core::Rect>&) override {}
    void clear(const core::Color&) override {}
    void setScissor(bool, const core::Rect&, int) override {}
    void prepareBackdropBlur(const core::Rect&, float, int, int) override {}
    void drawRoundedRect(const core::render::RoundedRectDrawCommand&, int, int) override {}
    void drawPolygon(const core::render::PolygonDrawCommand&, int, int) override {}
    void drawText(const core::render::TextDrawCommand& command, int, int) override {
        ++drawCalls;
        last = {};
        if (command.vertices != nullptr && command.vertexFloatCount > 0) {
            last.vertices.assign(command.vertices, command.vertices + command.vertexFloatCount);
        }
        copyAtlas(command.grayAtlas, last.gray);
        copyAtlas(command.colorAtlas, last.color);
    }

    static void copyAtlas(const core::render::TextAtlasPageData& page, AtlasCopy& out) {
        out.width = page.width;
        out.height = page.height;
        out.channels = page.channels;
        if (page.pixels == nullptr || page.width <= 0 || page.height <= 0 || page.channels <= 0) return;
        const std::size_t bytes = static_cast<std::size_t>(page.width) * page.height * page.channels;
        out.pixels.assign(page.pixels, page.pixels + bytes);
    }

    TextCopy last;
    int drawCalls = 0;
};

struct QuadInfo {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    bool colored = false;
};

std::vector<QuadInfo> quads(const TextCopy& copy) {
    std::vector<QuadInfo> result;
    if (copy.vertices.size() % 30 != 0) return result;
    result.reserve(copy.vertices.size() / 30);
    for (std::size_t base = 0; base < copy.vertices.size(); base += 30) {
        const bool colored = copy.vertices[base + 4] > 0.5f;
        const AtlasCopy& atlas = colored ? copy.color : copy.gray;
        if (atlas.width <= 0 || atlas.height <= 0 || atlas.channels <= 0 ||
            atlas.pixels.size() != static_cast<std::size_t>(atlas.width) * atlas.height * atlas.channels) return {};
        for (int vertex = 0; vertex < 6; ++vertex) {
            const std::size_t offset = base + static_cast<std::size_t>(vertex) * 5;
            if (!std::isfinite(copy.vertices[offset + 2]) || !std::isfinite(copy.vertices[offset + 3]) ||
                copy.vertices[offset + 2] < 0.0f || copy.vertices[offset + 2] > 1.0f ||
                copy.vertices[offset + 3] < 0.0f || copy.vertices[offset + 3] > 1.0f) return {};
        }
        const int x0 = static_cast<int>(std::lround(copy.vertices[base + 2] * atlas.width));
        const int y0 = static_cast<int>(std::lround(copy.vertices[base + 3] * atlas.height));
        const int x1 = static_cast<int>(std::lround(copy.vertices[base + 7] * atlas.width));
        const int y1 = static_cast<int>(std::lround(copy.vertices[base + 13] * atlas.height));
        if (x0 < 0 || y0 < 0 || x1 > atlas.width || y1 > atlas.height || x0 >= x1 || y0 >= y1) return {};
        for (int vertex = 0; vertex < 6; ++vertex) {
            const std::size_t offset = base + static_cast<std::size_t>(vertex) * 5;
            for (int field = 0; field < 5; ++field) {
                if (!std::isfinite(copy.vertices[offset + field])) return {};
            }
        }
        result.push_back({x0, y0, x1, y1, colored});
    }
    return result;
}

std::uint64_t quadPixelChecksum(const TextCopy& copy, const QuadInfo& quad, bool& hasInk) {
    const AtlasCopy& atlas = quad.colored ? copy.color : copy.gray;
    std::uint64_t hash = 1469598103934665603ull;
    hasInk = false;
    const int alphaChannel = atlas.channels == 4 ? 3 : 0;
    for (int y = quad.y0; y < quad.y1; ++y) {
        for (int x = quad.x0; x < quad.x1; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * atlas.width + x) * atlas.channels;
            for (int channel = 0; channel < atlas.channels; ++channel) {
                const unsigned char value = atlas.pixels[offset + channel];
                hash ^= value;
                hash *= 1099511628211ull;
            }
            hasInk = hasInk || atlas.pixels[offset + alphaChannel] != 0;
        }
    }
    return hash;
}

TextCopy draw(core::TextPrimitive& primitive, RecordingBackend& backend) {
    primitive.render(640, 480);
    return backend.last;
}

bool sameRenderedText(const TextCopy& before, const TextCopy& after, bool requireInk = true) {
    if (before.vertices.size() != after.vertices.size() || before.vertices.size() % 30 != 0) return false;
    const auto beforeQuads = quads(before);
    const auto afterQuads = quads(after);
    if (beforeQuads.size() != afterQuads.size() || beforeQuads.empty()) return false;
    for (std::size_t base = 0; base < before.vertices.size(); base += 30) {
        for (int vertex = 0; vertex < 6; ++vertex) {
            const std::size_t offset = base + static_cast<std::size_t>(vertex) * 5;
            if (before.vertices[offset] != after.vertices[offset] ||
                before.vertices[offset + 1] != after.vertices[offset + 1]) return false;
            const bool color = before.vertices[offset + 4] > 0.5f;
            const AtlasCopy& a = color ? before.color : before.gray;
            const AtlasCopy& b = color ? after.color : after.gray;
            if (a.width != b.width || a.height != b.height || a.channels != b.channels) {
                // Gray dimensions are expected to change; compare absolute texel coordinates below.
                if (color || a.channels != b.channels) return false;
            }
            const float au = before.vertices[offset + 2] * a.width;
            const float av = before.vertices[offset + 3] * a.height;
            const float bu = after.vertices[offset + 2] * b.width;
            const float bv = after.vertices[offset + 3] * b.height;
            if (std::fabs(au - bu) > 0.01f || std::fabs(av - bv) > 0.01f) return false;
        }
    }
    for (std::size_t index = 0; index < beforeQuads.size(); ++index) {
        if (beforeQuads[index].colored != afterQuads[index].colored ||
            beforeQuads[index].x0 != afterQuads[index].x0 || beforeQuads[index].y0 != afterQuads[index].y0 ||
            beforeQuads[index].x1 != afterQuads[index].x1 || beforeQuads[index].y1 != afterQuads[index].y1) return false;
        bool inkA = false, inkB = false;
        const std::uint64_t hashA = quadPixelChecksum(before, beforeQuads[index], inkA);
        const std::uint64_t hashB = quadPixelChecksum(after, afterQuads[index], inkB);
        if (hashA != hashB || (requireInk && (!inkA || !inkB))) return false;
    }
    return true;
}

bool sameGeometryWithInk(const TextCopy& before, const TextCopy& after) {
    if (before.vertices.size() != after.vertices.size() || before.vertices.size() % 30 != 0) return false;
    const auto oldQuads = quads(before);
    const auto newQuads = quads(after);
    if (oldQuads.empty() || oldQuads.size() != newQuads.size()) return false;
    for (std::size_t base = 0; base < before.vertices.size(); base += 30) {
        for (int vertex = 0; vertex < 6; ++vertex) {
            const std::size_t offset = base + static_cast<std::size_t>(vertex) * 5;
            if (before.vertices[offset] != after.vertices[offset] ||
                before.vertices[offset + 1] != after.vertices[offset + 1]) return false;
        }
    }
    for (const QuadInfo& quad : newQuads) {
        bool hasInk = false;
        (void)quadPixelChecksum(after, quad, hasInk);
        if (!hasInk) return false;
    }
    return true;
}

}  // namespace

int main() {
    useDefaultAtlasStart();
    RecordingBackend backend;
    core::render::ScopedRenderBackend activeBackend(backend);

    core::TextPrimitive stable;
    if (!stable.initialize()) {
        std::cerr << "TextPrimitive::initialize failed\n";
        return 1;
    }
    // Resolve to a concrete Windows system face instead of relying on a test runner's
    // current directory to locate the optional bundled display font.
    stable.setFontFamily("Microsoft YaHei");
    stable.setText("Geometry must survive atlas growth: 汉字");
    stable.prepare();
    const core::Vec2 before = stable.measuredSize();
    const TextCopy stableBefore = draw(stable, backend);
    if (quads(stableBefore).empty()) {
        std::cerr << "stable text produced no recorded quads: drawCalls=" << backend.drawCalls
                  << ", floats=" << stableBefore.vertices.size() << ", measured=" << before.x << "x" << before.y
                  << ", gray=" << stableBefore.gray.width << "x" << stableBefore.gray.height
                  << ", pixels=" << stableBefore.gray.pixels.size() << "\n";
        return 1;
    }
    const core::TextAtlasDebugStats initial = core::TextPrimitive::debugAtlasStats();
    if (initial.grayWidth != 512 || initial.grayHeight != 512 ||
        initial.grayCapacityBytes < 512u * 512u) {
        std::cerr << "gray atlas did not start at 512x512\n";
        return 1;
    }

    // Warm the color page first where the host font stack can supply a BGRA emoji.
    core::TextPrimitive emoji;
    if (!emoji.initialize()) return 1;
#ifdef _WIN32
    const std::filesystem::path emojiFont = L"C:/Windows/Fonts/seguiemj.ttf";
    if (std::filesystem::is_regular_file(emojiFont)) {
        emoji.setFontFamily("C:/Windows/Fonts/seguiemj.ttf");
    }
#endif
    emoji.setText(utf8(0x1f600));
    emoji.prepare();
    const TextCopy emojiBefore = draw(emoji, backend);
    const core::TextAtlasDebugStats afterEmoji = core::TextPrimitive::debugAtlasStats();
    if (afterEmoji.colorWidth != 0 &&
        (afterEmoji.colorWidth != 1024 || afterEmoji.colorHeight != 1024)) {
        std::cerr << "color atlas dimensions changed from 1024x1024\n";
        return 1;
    }

    core::TextPrimitive bulk;
    if (!bulk.initialize()) return 1;
    bulk.setFontSize(32.0f);
    bulk.setText(uniqueCjk(0x4e00u, 1500));
    bulk.prepare();
    const TextCopy bulkDraw = draw(bulk, backend);
    const auto bulkQuads = quads(bulkDraw);
    if (bulkQuads.size() != 1500) {
        std::cerr << "bulk text did not produce one quad per CJK codepoint; got " << bulkQuads.size() << "\n";
        return 1;
    }
    for (const QuadInfo& quad : bulkQuads) {
        bool hasInk = false;
        (void)quadPixelChecksum(bulkDraw, quad, hasInk);
        if (!hasInk) {
            std::cerr << "bulk glyph quad points to an empty atlas region\n";
            return 1;
        }
    }
    const core::TextAtlasDebugStats grown = core::TextPrimitive::debugAtlasStats();
    if (grown.grayWidth != 2048 || grown.grayHeight != 2048 ||
        grown.grayGrowthCount < 2 || grown.grayOverflowResetCount != initial.grayOverflowResetCount) {
        std::cerr << "gray atlas did not grow through 1024x1024 to 2048x2048 without reset; got "
                  << grown.grayWidth << "x" << grown.grayHeight << ", growths="
                  << grown.grayGrowthCount << ", resets=" << grown.grayOverflowResetCount << "\n";
        return 1;
    }
    if (afterEmoji.colorWidth != grown.colorWidth || afterEmoji.colorHeight != grown.colorHeight ||
        afterEmoji.colorGeneration != grown.colorGeneration) {
        std::cerr << "gray growth unexpectedly changed the color atlas\n";
        return 1;
    }

    // Its old normalized UVs must be rebuilt from the rebased shared index.
    const TextCopy stableAfter = draw(stable, backend);
    const core::Vec2 after = stable.measuredSize();
    if (!sameSize(before, after) || !sameRenderedText(stableBefore, stableAfter)) {
        std::cerr << "stable text geometry, texel coordinates, or pixels changed after gray atlas growth\n";
        return 1;
    }
    const TextCopy emojiAfter = draw(emoji, backend);
    if (afterEmoji.colorWidth != 0 && !sameRenderedText(emojiBefore, emojiAfter)) {
        std::cerr << "colored emoji changed after gray atlas growth\n";
        return 1;
    }
    const core::TextAtlasDebugStats preparedAgain = core::TextPrimitive::debugAtlasStats();
    if (preparedAgain.grayWidth != 2048 || preparedAgain.grayGeneration < grown.grayGeneration ||
        preparedAgain.grayGlyphs == 0) {
        std::cerr << "atlas state invalid after re-preparing prior text\n";
        return 1;
    }

    // Exercise a one-off max-page overflow, then verify an ordinary existing primitive
    // can re-rasterize and render on its next prepare after the reset.
    core::TextPrimitive transient;
    if (!transient.initialize()) return 1;
    transient.setFontSize(64.0f);
    transient.setText(uniqueCjk(0x6000u, 1400));
    transient.prepare();
    const core::TextAtlasDebugStats afterReset = core::TextPrimitive::debugAtlasStats();
    if (afterReset.grayOverflowResetCount <= grown.grayOverflowResetCount) {
        std::cerr << "max-page transient corpus did not trigger an overflow reset\n";
        return 1;
    }
    const TextCopy stableAfterReset = draw(stable, backend);
    if (!sameGeometryWithInk(stableBefore, stableAfterReset)) {
        std::cerr << "small text did not render after a one-off max-page reset\n";
        return 1;
    }

    std::printf("text atlas growth: ALL PASS\n");
    return 0;
}
