#pragma once

#include "eui_neo.h"
#include <utility>

namespace neo {

// Keep both tiny page wrappers mounted. The hidden layout primes its next entry;
// only the visible page builds content, so returning never retains live settings
// controls over the editor. Both directions share the runtime's 60 Hz phase.
template <typename Render>
inline void pageTransitionView(eui::Ui& ui, const eui::Screen& screen, const char* id,
                               int zIndex, bool visible, bool animations, Render&& render) {
    ui.stack(id)
        .size(screen.width, screen.height).zIndex(zIndex)
        .disabled(!visible)
        .opacity(visible ? 1.0f : 0.0f)
        .translateY(visible ? 0.0f : 4.0f)
        .transition(visible && animations ? core::Transition::make(.20f, core::Ease::OutCubic)
                                                 .animate(core::AnimProperty::Opacity | core::AnimProperty::Transform)
                                             : core::Transition::none())
        .content([&] { if (visible) render(); })
        .build();
}

template <typename Render>
inline void settingsTransitionView(eui::Ui& ui, const eui::Screen& screen,
                                   bool visible, bool animations, Render&& render) {
    pageTransitionView(ui, screen, "settings.transition", 800, visible, animations,
                       std::forward<Render>(render));
}

template <typename Render>
inline void editorTransitionView(eui::Ui& ui, const eui::Screen& screen,
                                 bool visible, bool animations, Render&& render) {
    pageTransitionView(ui, screen, "editor.transition", 0, visible, animations,
                       std::forward<Render>(render));
}

} // namespace neo
