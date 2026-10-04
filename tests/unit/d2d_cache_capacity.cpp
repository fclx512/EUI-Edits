#include "core/render/d2d/cache_capacity.h"
#include <iostream>

int main() {
    using core::render::d2d::CacheCapacity;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) { if (!ok) { ++failures; std::cerr << message << '\n'; } };
    auto capacity = CacheCapacity::forSize(800, 600, {}, 8192);
    check(capacity.fits(800, 600), "initial capacity does not cover the logical rectangle");
    const auto before = capacity;
    for (int width = 700; width <= 800; ++width) {
        check(capacity.fits(width, 550), "shrinking/re-expanding within capacity requires allocation");
    }
    capacity = CacheCapacity::forSize(1000, 600, capacity, 8192);
    check(capacity.fits(1000, 600) && capacity.height == before.height, "growth changed an unaffected dimension");
    check(capacity.width <= 1280, "capacity growth exceeded its bounded rounding margin");
    check(capacity.oversized(320, 200) && !capacity.oversized(900, 600), "stable shrink threshold is incorrect");
    const auto shrunk = CacheCapacity::forSize(320, 200, {}, 8192);
    check(shrunk.fits(320, 200) && shrunk.width < capacity.width, "compaction does not release excess capacity");
    const auto capped = CacheCapacity::forSize(9000, 500, {}, 8192);
    check(!capped.fits(9000, 500) && capped.width == 8192, "device maximum was not respected");
    check(!capacity.fits(0, 1), "zero-size frame accepted");
    return failures ? 1 : 0;
}
