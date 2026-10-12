#include "core/render/d2d/render_policy.h"
#include <iostream>

int main() {
    using core::render::d2d::RenderPolicy;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failures; std::cerr << label << '\n'; }
    };
    const auto a = RenderPolicy::fromOptions(true, "", "");
    check(a.software && a.gdiPresentation, "native default must remain software DC");
    auto b = RenderPolicy::fromOptions(true, "1", "0");
    check(b.software && !b.gdiPresentation, "software Hwnd isolation option");
    auto c = RenderPolicy::fromOptions(true, "0", "1");
    check(!c.software && !c.gdiPresentation, "hardware-preferred always uses Hwnd");
    for (const auto invalid : {"00", "0 ", "false", "123456789"}) {
        const auto policy = RenderPolicy::fromOptions(true, invalid, "00");
        check(policy.software && policy.gdiPresentation, "non-exact native option keeps safe default");
    }
    check(c.useSoftwareFallback(true) && c.software && c.gdiPresentation,
          "hardware creation failure retries native software DC");
    check(!c.useSoftwareFallback(true), "fallback retries at most once");
    check(!b.useSoftwareFallback(true), "explicit software Hwnd must not silently change presentation");
    auto glfw = RenderPolicy::fromOptions(false, "", "");
    check(!glfw.software && !glfw.gdiPresentation, "non-native default unchanged");
    check(glfw.useSoftwareFallback(false) && glfw.software && !glfw.gdiPresentation,
          "non-native fallback remains Hwnd");
    const auto explicitSoftware = RenderPolicy::fromOptions(false, "1", "1");
    check(explicitSoftware.software && !explicitSoftware.gdiPresentation, "non-native explicit software");
    return failures ? 1 : 0;
}
