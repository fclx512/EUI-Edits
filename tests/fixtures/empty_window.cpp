#include "eui_neo.h"

namespace app {

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("Empty EUI")
        .pageId("empty_window")
        .clearColor({0.16f, 0.18f, 0.20f, 1.0f})
        .windowSize(800, 600)
        .showDebugStatsInTitle(false)
        .fps(0.0)
        .iconPath("");
    return config;
}

void compose(eui::Ui&, const eui::Screen&) {}

} // namespace app
