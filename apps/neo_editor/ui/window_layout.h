#pragma once

#include "state/app_state.h"

namespace neo {

// Logical units already account for system DPI and UI scale. A separate
// restore threshold prevents repeated reflow around the narrow-width boundary.
inline void updateCompactWindow(AppState& state, float width) {
    if (!state.autoCompactWindow) state.compactWindow = false;
    else if (width < 800.0f) state.compactWindow = true;
    else if (width >= 880.0f) state.compactWindow = false;
}
inline bool vaultPanelVisible(const AppState& state) {
    return state.mode == EditorMode::Vault && !state.compactWindow;
}
inline bool statusBarVisible(const AppState& state) {
    return state.showStatusBar && !state.compactWindow;
}
inline bool lineNumbersVisible(const AppState& state) {
    return state.showLineNumbers && !state.compactWindow;
}

} // namespace neo
