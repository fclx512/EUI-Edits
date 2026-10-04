#pragma once

#include <algorithm>
#include <cstdint>

namespace core::render::d2d {

struct CacheCapacity {
    int width = 0, height = 0;
    bool fits(int w, int h) const { return w > 0 && h > 0 && w <= width && h <= height; }
    bool oversized(int w, int h) const {
        return w > 0 && h > 0 &&
            static_cast<std::int64_t>(width) * height > 2 * static_cast<std::int64_t>(w) * h;
    }
    static int dimension(int required, int previous, int maximum) {
        const auto grown = std::max<std::int64_t>(required,
            required > previous ? static_cast<std::int64_t>(previous) + previous / 4 : previous);
        return static_cast<int>(std::min<std::int64_t>(maximum, ((grown + 63) / 64) * 64));
    }
    static CacheCapacity forSize(int w, int h, CacheCapacity previous, int maximum) {
        return {dimension(w, previous.width, maximum), dimension(h, previous.height, maximum)};
    }
};

} // namespace core::render::d2d
