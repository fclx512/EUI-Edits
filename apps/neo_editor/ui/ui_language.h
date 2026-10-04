#pragma once

#include "model/i18n.h"
#include "state/app_actions.h"

namespace neo {

inline void applyUiLanguage(AppState& state, const std::string& preference) {
    i18n::setPreference(preference);
    state.openMenu = MenuKind::None;
    if (!state.settingsOpen) state.findEditorFocusPending = true;
    persistSettings(state);
    app::requestUpdate();
    app::detail::requestFullPaint();
}

} // namespace neo
