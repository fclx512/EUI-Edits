#pragma once
#include <string_view>

namespace core::render::d2d {

struct RenderPolicy {
    bool software = false;
    bool gdiPresentation = false;

    static RenderPolicy fromOptions(bool nativeWin32, std::string_view softwareOption,
                                    std::string_view dcOption) {
        RenderPolicy result;
        result.software = nativeWin32 ? softwareOption != "0" : softwareOption == "1";
        result.gdiPresentation = nativeWin32 && result.software && dcOption != "0";
        return result;
    }

    bool useSoftwareFallback(bool nativeWin32) {
        if (software) return false;
        software = true;
        gdiPresentation = nativeWin32;
        return true;
    }
};

}
