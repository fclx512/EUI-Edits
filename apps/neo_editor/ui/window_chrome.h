#pragma once
#include "platform/window_chrome.h"
#include "ui/markdown_toolbar.h"
#include "state/app_actions.h"

namespace neo {
inline void protectWindowOverlayChrome(const AppState& state, const eui::Screen& screen) {
    // Native hit testing happens before the DSL's full-window modal surfaces.
    // Let their backdrop/preview receive input over the former caption too.
    // Workbench dialogs start below their separate chrome and are not included.
    if (documentModalOpen(state))
        platform::chrome::exclude(0, 0, screen.width, screen.height);
}
inline void windowChromeView(eui::Ui& ui, const eui::Screen& screen, float height) {
    ui.stack("window.controls").position(screen.width-platform::chrome::controlsWidth,0)
        .size(platform::chrome::controlsWidth,height).zIndex(3000).content([&] {
            writingIconButton(ui,"window.minimize",UiIcon::Minus,i18n::tr("window.minimize"),0,0,
                false,true,[]{platform::chrome::minimize();},40,height);
            writingIconButton(ui,"window.maximize",platform::chrome::maximized()?UiIcon::Restore:UiIcon::Maximize,
                i18n::tr("window.maximize"),40,0,false,true,[]{platform::chrome::maximize();},40,height);
            writingIconButton(ui,"window.close",UiIcon::Close,i18n::tr("window.close"),80,0,
                false,true,[]{platform::chrome::close();},40,height,true);
        }).build();
}
}
