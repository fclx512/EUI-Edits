#pragma once

#include <algorithm>

namespace core::render::d2d {

inline int atlasUploadHeight(int sourceHeight, int usedHeight, int previousHeight, bool compact) {
    if (!compact || usedHeight <= 0 || usedHeight > sourceHeight) return sourceHeight;
    const int rounded = ((usedHeight - 1) / 64 + 1) * 64;
    return std::min(sourceHeight, std::max(rounded, previousHeight));
}

} // namespace core::render::d2d
