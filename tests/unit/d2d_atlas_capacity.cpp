#include "core/render/d2d/atlas_capacity.h"
#include <iostream>

int main() {
    using core::render::d2d::atlasUploadHeight;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) { if (!ok) { ++failures; std::cerr << message << '\n'; } };
    check(atlasUploadHeight(1024, 117, 0, true) == 128, "occupied rows were not rounded");
    check(atlasUploadHeight(1024, 129, 128, true) == 192, "growth failed to include the next row");
    check(atlasUploadHeight(1024, 40, 192, true) == 192, "overflow reset shrank a live bitmap");
    check(atlasUploadHeight(1024, 1023, 0, true) == 1024, "last padded row was clipped");
    check(atlasUploadHeight(16, 10, 0, true) == 16, "small page exceeded source bounds");
    check(atlasUploadHeight(1024, 0, 128, true) == 1024, "unknown extent did not preserve legacy contract");
    check(atlasUploadHeight(1024, 1025, 128, true) == 1024, "invalid extent accepted");
    check(atlasUploadHeight(1024, 117, 128, false) == 1024, "diagnostic fallback did not use full page");
    return failures ? 1 : 0;
}
