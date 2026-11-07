#pragma once

#include "components/button.h"
#include "core/platform/async.h"
#include "core/platform/platform.h"
#include "state/app_actions.h"
#include "model/settings.h"
#include "model/update_check.h"
#include "ui/metrics.h"
#include "ui/widgets.h"

#include <chrono>

namespace neo {

inline void updateHint(eui::Ui& ui, AppState& state, const eui::Screen& screen, bool workbenchOpen) {
    static bool started = false;
    static std::string available;
    const auto now = [] {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    };
    if (!settings::current().automaticUpdateCheck) {
        available.clear();
        return;
    }
    if (!started && updates::checkDue(settings::current().lastUpdateCheck, now())) {
        ui.rect("editor.update.timer").size(0, 0).ignoreLayout().hitTestMode(core::dsl::HitTestMode::None)
            .onTimer(5.0f, [now] {
                if (started || !settings::current().automaticUpdateCheck) return;
                started = true;
                settings::current().lastUpdateCheck = now();
                settings::flush();
                core::async::runOnce("neo.update.latest", updates::fetchLatest,
                    [](const core::async::Result<std::string>& result) {
                        if (settings::current().automaticUpdateCheck) available = result.value;
                        app::requestUpdate();
                    });
                app::requestUpdate();
            }).build();
    }
    if (available.empty() || documentModalOpen(state) || workbenchOpen) return;

    const auto& colors = editorColors();
    const auto metrics = uiMetrics(state);
    const float width = std::min(380.0f, screen.width - 24.0f);
    constexpr float height = 98.0f;
    const float x = screen.width - width - 12;
    const float y = std::max(metrics.menuBarHeight + 6.0f, screen.height - height - 42.0f);
    ui.stack("editor.update.hint").position(x, y).size(width, height).zIndex(1100).content([&] {
        ui.rect("editor.update.background").fill().radius(neo::corner(colors, 8.0f))
            .color({colors.panel.r, colors.panel.g, colors.panel.b, 1.0f}).border(1, colors.border).build();
        ui.text("editor.update.title").position(12, 8).size(width - 24, 42)
            .text(i18n::format("about.update_available", {{"version", available}}))
            .fontFamily(uiFontFamily(state)).fontSize(metrics.panelFontSize).wrap(true).maxWidth(width - 24)
            .color(colors.text).build();
        const float buttonWidth = (width - 32) * .5f;
        components::button(ui, "editor.update.release").theme(colors.tokens, true)
            .position(12, 54).size(buttonWidth, 32).text(i18n::tr("about.update_release"))
            .fontSize(metrics.panelFontSize).preserveFocusOnPress()
            .onClick([&state] {
                if (core::platform::openUrl(updates::kReleasePage)) available.clear();
                else showToast(state, i18n::tr("safety.link_failed"), updates::kReleasePage);
                app::requestUpdate();
            }).build();
        components::button(ui, "editor.update.dismiss").theme(colors.tokens, false)
            .position(20 + buttonWidth, 54).size(buttonWidth, 32).text(i18n::tr("about.update_dismiss"))
            .fontSize(metrics.panelFontSize).preserveFocusOnPress()
            .onClick([] { available.clear(); app::requestUpdate(); }).build();
    }).build();
}

} // namespace neo
