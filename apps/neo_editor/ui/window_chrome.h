#pragma once
#include "platform/window_chrome.h"
#include "ui/markdown_toolbar.h"

namespace neo {
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
