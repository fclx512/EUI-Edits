#include "ui/menu_bar.h"
#include "core/dsl_runtime.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace app { void requestUpdate() {} }

namespace neo {
// This target verifies menu command IDs and title geometry. Encoding discovery
// belongs to app_actions and is intentionally outside this fixture.
const std::vector<ReopenEncodingOption>& reopenEncodingOptions() {
    static const std::vector<ReopenEncodingOption> none;
    return none;
}
}

namespace {
struct PopupRecorder final : core::render::RenderBackend {
    float opacity = -1.0f;
    bool initialize() override { return true; }
    bool valid() const override { return true; }
    void makeCurrent() override {}
    void beginFrame(const core::render::RenderSurface&) override {}
    void present() override {}
    bool ensureRenderCache(int, int) override { return true; }
    bool renderCacheWasRecreated() const override { return false; }
    void releaseRenderCache() override {}
    void beginRenderCacheFrame(int, int, const std::vector<core::Rect>&) override {}
    void endRenderCacheFrame() override {}
    void blitRenderCache(int, int, core::render::RenderCacheBlitMode, const std::vector<core::Rect>&) override {}
    bool supportsRetainedLayers() const override { return false; }
    void clear(const core::Color&) override {}
    void setScissor(bool, const core::Rect&, int) override {}
    void prepareBackdropBlur(const core::Rect&, float, int, int) override {}
    void drawRoundedRect(const core::render::RoundedRectDrawCommand& command, int, int) override {
        if (!command.shadowPass && core::closeEnough(command.fillColor, core::Color{.73f,.79f,.87f,1}))
            opacity = command.opacity;
    }
    void drawPolygon(const core::render::PolygonDrawCommand&, int, int) override {}
    void drawText(const core::render::TextDrawCommand&, int, int) override {}
};

bool applicationAnimationToggle(neo::UiStyle style) {
    PopupRecorder backend;
    core::render::ScopedRenderBackend scoped(backend);
    core::dsl::Runtime runtime;
    if (!runtime.initialize()) return false;
    int tag = 0;
    auto window = reinterpret_cast<core::window::Handle>(&tag);
    neo::AppState state;
    state.uiStyle = style;
    state.animations = false;
    auto tokens = components::theme::light();
    tokens.surface = {.73f,.79f,.87f,1};
    core::dsl::Ui* ui = nullptr;
    const auto finish = [&](bool ok) {
        runtime.shutdown();
        core::releaseInputQueue(window);
        return ok;
    };
    const auto step = [&](float delta) {
        runtime.compose("application-menu-toggle", 640, 480,
            [&](core::dsl::Ui& view, const core::dsl::Screen&) {
                ui = &view;
                const bool open = state.openMenu != neo::MenuKind::None;
                using Item = components::ContextMenuItem;
                auto menu = components::contextMenu(view, "toggle")
                    .open(open).screen(640,480).position(30,40).size(210,32)
                    .theme(tokens).items(std::vector<Item>{Item("Parent", {Item("Child")}), Item("Action")});
                neo::popup_presentation::apply(menu, state, tokens, open);
                menu.skipClosedContent().build();
            });
        const bool paint = runtime.update(window, delta, 1, 1);
        runtime.requestFullPaint();
        backend.opacity = -1.0f;
        runtime.render(640,480,1,{1,1,1,1});
        return paint;
    };
    const auto noRevealWrappers = [&] {
        bool found = false;
        const auto inspect = [&](auto&& self, const core::dsl::Element& element) -> void {
            found = found || element.id.find(".reveal.") != std::string::npos;
            for (const auto& child : element.children) self(self, *child);
        };
        for (const auto& root : ui->roots()) inspect(inspect, *root);
        return !found;
    };
    const auto quiet = [&] {
        return !runtime.isAnimating() && std::isinf(runtime.nextTimerWakeSeconds());
    };
    const auto idleQuiet = [&] {
        // The state-inspection step requests a full paint. Consume that one
        // outstanding invalidation, then probe the real idle loop without
        // composing a new tree or explicitly requesting another paint.
        if (runtime.update(window, .05f, 1, 1)) runtime.render(640,480,1,{1,1,1,1});
        const bool paint = runtime.update(window, .05f, 1, 1);
        if (paint) runtime.render(640,480,1,{1,1,1,1});
        return !paint && quiet();
    };
    const auto fail = [&](const char* phase) {
        std::cerr << "menu toggle " << (style == neo::UiStyle::Square ? "win10" : "win11")
                  << " phase=" << phase << " opacity=" << backend.opacity
                  << " wrappers=" << !noRevealWrappers()
                  << " fast=" << runtime.isAnimating()
                  << " wake=" << runtime.nextTimerWakeSeconds()
                  << " paint=" << runtime.paintRequested() << '\n';
        return finish(false);
    };

    step(0);
    state.openMenu = neo::MenuKind::File;
    step(0);
    if (!core::closeEnough(backend.opacity, 1.0f) || !noRevealWrappers() || !quiet()) return fail("disabled-open");
    // Enabling while already open must not restart the root or flash it away.
    state.animations = true;
    step(.03f);
    if (!core::closeEnough(backend.opacity, 1.0f)) return fail("enable-open");
    state.openMenu = neo::MenuKind::None;
    step(.02f);
    if (!(backend.opacity > 0.0f && backend.opacity < 1.0f)) return fail("exit-intermediate");
    // Turning off during exit removes both the content and the priming seeds.
    state.animations = false;
    step(0);
    if (ui->find("toggle.level.0") || !noRevealWrappers() || !quiet()) return fail("disable-exit");
    if (!idleQuiet()) return fail("disabled-closed-idle");
    // Restore the hidden seed before opening: the next entry must interpolate.
    state.animations = true;
    step(0);
    state.openMenu = neo::MenuKind::File;
    step(.03f);
    if (!(backend.opacity > 0.0f && backend.opacity < 1.0f)) return fail("restored-entry-intermediate");
    state.animations = false;
    step(0);
    if (!core::closeEnough(backend.opacity, 1.0f) || !noRevealWrappers() || !quiet()) return fail("disable-entry");
    if (!idleQuiet()) return fail("disabled-open-idle");
    state.openMenu = neo::MenuKind::None;
    step(0);
    if (ui->find("toggle.level.0") || !noRevealWrappers() || !quiet()) return fail("disabled-close");
    return finish(true);
}
}

int main() {
    neo::AppState state;
    using namespace neo::menu_detail;
    using neo::i18n::setPreference;
    const auto id=[](Command command){return static_cast<int>(command);};
    for(const char* language : {"zh-CN","en"}) {
        setPreference(language);
        auto items=viewMenuItems(state);
        if(commandAtPath(items,{6,0})!=id(Command::FollowSystem) ||
           commandAtPath(items,{6,1})!=id(Command::Dark) ||
           commandAtPath(items,{6,2})!=id(Command::Light)) return 7;
        for (bool follow : {false,true}) {
            for (auto mode : {neo::ThemeMode::Dark,neo::ThemeMode::Light}) {
                state.themeFollowSystem=follow;
                state.theme=mode;
                const auto choices=viewMenuItems(state)[6].children;
                if(choices.size()!=3 || choices[0].checked!=follow ||
                   choices[1].checked!=(!follow && mode==neo::ThemeMode::Dark) ||
                   choices[2].checked!=(!follow && mode==neo::ThemeMode::Light)) return 8;
                if(std::count_if(choices.begin(),choices.end(),[](const auto& choice){return choice.checked;})!=1) return 9;
            }
        }
        if(commandAtPath(items,{3})!=id(Command::AutoCompact) ||
           commandAtPath(items,{4})!=id(Command::Wrap) || commandAtPath(items,{5})!=id(Command::Readable) ||
           commandAtPath(items,{8,0})!=id(Command::LanguageSystem) || commandAtPath(items,{8,1})!=id(Command::LanguageChinese) ||
           commandAtPath(items,{8,2})!=id(Command::LanguageEnglish) || commandAtPath(items,{9})!=id(Command::Settings)) return 1;
        if(commandAtPath(items,{}) || commandAtPath(items,{-1}) || commandAtPath(items,{200}) ||
           commandAtPath(items,{0,200}) || commandAtPath(items,{0,0,0})) return 2;
        if(commandAtPath(items,{0})!=id(Command::Vault) || !items[0].children.empty() ||
           items[0].checked!=(state.mode==neo::EditorMode::Vault)) return 11;
        // A separator, translation or reordered display row must never change its action.
        std::reverse(items.begin(),items.end());
        if(commandAtPath(items,{0})!=id(Command::Settings) || commandAtPath(items,{1,2})!=id(Command::LanguageEnglish)) return 3;
        const auto preview = std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.commandId==id(Command::Mindmap); });
        if (preview!=items.end()) return 10;
        items[0].enabled=false;
        if(commandAtPath(items,{0})) return 4;
        const auto edit=editMenuItems();
        if(commandAtPath(edit,{6})!=id(Command::Syntax) || commandAtPath(edit,{5})!=id(Command::Find)) return 5;
    }
    state.editorFontSize=17;
    const auto sizes=fontSizeOptions(state);
    const auto custom=viewMenuItems(state);
    if(sizes.front()!=17 || commandAtPath(custom,{7,0})!=id(Command::FontSizeBase) || !custom[7].children[0].checked) return 6;
    core::dsl::Runtime runtime;
    runtime.initialize();
    core::dsl::Ui* ui = nullptr;
    const auto metrics = neo::uiMetrics(state);
    const auto colors = neo::editorColors(state.theme);
    const float titleTop = menuTitleTextTop(neo::uiFontFamily(state), metrics.menuFontSize, metrics.menuBarHeight);
    runtime.compose("menu-baseline", 300, metrics.menuBarHeight, [&](core::dsl::Ui& view, const core::dsl::Screen&) {
        ui = &view;
        menuTitleView(view, colors, metrics, neo::uiFontFamily(state), "menu.zh", "视图", 0, 60,
                      metrics.menuFontSize, titleTop, false, true, {}, {});
        menuTitleView(view, colors, metrics, neo::uiFontFamily(state), "menu.en", "Settings", 70, 100,
                      metrics.menuFontSize, titleTop, false, true, {}, {});
    });
    const auto* zh = ui->find("menu.zh.label");
    const auto* en = ui->find("menu.en.label");
    if (!zh || !en || std::fabs(zh->frame.y - en->frame.y) > .01f ||
        zh->verticalAlign != core::VerticalAlign::Top || en->verticalAlign != core::VerticalAlign::Top) return 12;

    setPreference("en");
    state.uiFontSize = 18;
    const auto largeMetrics = neo::uiMetrics(state);
    const auto compactItems = compactMenuItems(state);
    if (commandAtPath(compactItems, {0, 0, 0}) != id(Command::NewText) ||
        commandAtPath(compactItems, {0, 3}) != id(Command::Save) ||
        commandAtPath(compactItems, {1, 0}) != id(Command::Undo) ||
        commandAtPath(compactItems, {2, 8, 2}) != id(Command::LanguageEnglish) ||
        commandAtPath(compactItems, {2, 9}) != id(Command::Settings) ||
        commandAtPath(compactItems, {3}) != id(Command::Settings) ||
        commandAtPath(compactItems, {0}) || commandAtPath(compactItems, {0, 0})) return 13;

    // This is the production geometry used by menuBarView, followed by its
    // actual title builder and caption exclusion (not a second width formula).
    state.mode = neo::EditorMode::Simple;
    const auto narrow = menuBarLayout(state, 380, largeMetrics);
    if (!narrow.compact || narrow.tabsRight - narrow.tabsLeft < 76.0f ||
        narrow.compactX + narrow.compactPopupWidth + narrow.compactSubmenuWidth + 16.0f > 380.0f ||
        narrow.compactX + narrow.compactWidth >= 380.0f - neo::platform::chrome::controlsWidth)
        return 14;
    if (compactMenuItems(state, narrow.compactSubmenuWidth)[0].childrenPresentation.preferredWidth !=
        narrow.compactSubmenuWidth) return 20;
    const float childX = components::context_menu_detail::childMenuX(
        narrow.compactX, narrow.compactPopupWidth, narrow.compactSubmenuWidth, 380,
        colors.tokens.metrics.spacing.tiny);
    if (childX < narrow.compactX + narrow.compactPopupWidth ||
        childX + narrow.compactSubmenuWidth + 8.0f > 380.0f) return 21;
    neo::platform::chrome::beginFrame(380, largeMetrics.menuBarHeight);
    runtime.compose("menu-narrow", 380, largeMetrics.menuBarHeight,
                    [&](core::dsl::Ui& view, const core::dsl::Screen&) {
        ui = &view;
        menuChromeTitleView(view, neo::editorColors(state.theme), largeMetrics,
                            neo::uiFontFamily(state), "menubar.compact", neo::i18n::tr("menu.all"),
                            narrow.compactX, narrow.compactWidth,
                            menuTitleTextTop(neo::uiFontFamily(state), largeMetrics.menuFontSize,
                                             largeMetrics.menuBarHeight),
                            false, true, [] {});
    });
    const auto* compactHit = ui->find("menubar.compact.hit");
    if (!compactHit || !compactHit->onClick ||
        std::fabs(compactHit->frame.x - narrow.compactX) > .01f ||
        std::fabs(compactHit->frame.width - narrow.compactWidth) > .01f ||
        neo::platform::chrome::captionAt(compactHit->frame.x + 1, 18, 380, largeMetrics.menuBarHeight) ||
        !neo::platform::chrome::captionAt(compactHit->frame.x + compactHit->frame.width + 5, 18,
                                          380, largeMetrics.menuBarHeight) ||
        neo::platform::chrome::captionAt(320, 18, 380, largeMetrics.menuBarHeight)) return 15;

    state.mode = neo::EditorMode::Vault;
    const auto vaultNarrow = menuBarLayout(state, 380, largeMetrics);
    if (!vaultNarrow.compact || vaultNarrow.tabsRight - vaultNarrow.tabsLeft < 76.0f) return 16;
    state.mode = neo::EditorMode::Simple;
    const auto wide = menuBarLayout(state, 1000, largeMetrics);
    if (wide.compact || wide.tabsRight - wide.tabsLeft < 76.0f ||
        wide.settingsX + wide.settingsWidth >= 1000.0f - neo::platform::chrome::controlsWidth)
        return 17;
    const float expandedMinimum = wide.settingsX + wide.settingsWidth + 12.0f +
        neo::tabpresentation::layout::tabWidth + 3.0f * 28.0f + 8.0f +
        neo::platform::chrome::controlsWidth + neo::platform::chrome::dragWidth;
    const auto belowThreshold = menuBarLayout(state, expandedMinimum - 1.0f, largeMetrics);
    const auto aboveThreshold = menuBarLayout(state, expandedMinimum + 1.0f, largeMetrics);
    if (!belowThreshold.compact || aboveThreshold.compact ||
        !menuBarLayout(state, 567, largeMetrics).compact ||
        aboveThreshold.tabsRight - aboveThreshold.tabsLeft - 92.0f < neo::tabpresentation::layout::tabWidth)
        return 22;
    neo::platform::chrome::beginFrame(1000, largeMetrics.menuBarHeight);
    runtime.compose("menu-wide", 1000, largeMetrics.menuBarHeight,
                    [&](core::dsl::Ui& view, const core::dsl::Screen&) {
        ui = &view;
        const float top = menuTitleTextTop(neo::uiFontFamily(state), largeMetrics.menuFontSize,
                                           largeMetrics.menuBarHeight);
        const char* labels[] = {"File", "Edit", "View"};
        for (int index = 0; index < 3; ++index)
            menuChromeTitleView(view, neo::editorColors(state.theme), largeMetrics,
                                neo::uiFontFamily(state), "menubar.title." + std::to_string(index),
                                labels[index], wide.titleX[index], wide.titleWidth[index], top,
                                false, true, [] {});
        menuChromeTitleView(view, neo::editorColors(state.theme), largeMetrics,
                            neo::uiFontFamily(state), "menubar.settings", "Settings",
                            wide.settingsX, wide.settingsWidth, top, false, true, [] {});
    });
    for (int index = 0; index < 3; ++index) {
        const auto* hit = ui->find("menubar.title." + std::to_string(index) + ".hit");
        if (!hit || neo::platform::chrome::captionAt(hit->frame.x + 1, 18, 1000,
                                                     largeMetrics.menuBarHeight)) return 18;
    }
    const auto* settingsHit = ui->find("menubar.settings.hit");
    if (!settingsHit || neo::platform::chrome::captionAt(settingsHit->frame.x + 1, 18, 1000,
                                                         largeMetrics.menuBarHeight) ||
        !neo::platform::chrome::captionAt(settingsHit->frame.x + settingsHit->frame.width + 5,
                                          18, 1000, largeMetrics.menuBarHeight)) return 19;
    state.mode = neo::EditorMode::Vault;
    neo::updateCompactWindow(state, 700);
    const auto adaptive = menuBarLayout(state, 700, largeMetrics);
    state.mode = neo::EditorMode::Simple;
    const auto manualHidden = menuBarLayout(state, 700, largeMetrics);
    if (!adaptive.compact || adaptive.tabsLeft != manualHidden.tabsLeft ||
        adaptive.tabsRight != manualHidden.tabsRight) return 23;
    for (const float width : {380.0f, 420.0f, 500.0f, 1000.0f}) {
        neo::updateCompactWindow(state, width);
        const auto header = menuBarLayout(state, width, largeMetrics);
        const auto actions = neo::tab_bar_detail::tabActionsLayout(header.tabsLeft, header.tabsRight);
        if (!actions.showNew || !actions.showList || !actions.showPin ||
            actions.newX < header.tabsLeft || actions.newX + 28 != actions.listX ||
            actions.listX + 28 != actions.pinX || actions.pinX + 28 != header.tabsRight ||
            header.tabsRight > width - neo::platform::chrome::controlsWidth - 32) return 24;
        neo::platform::chrome::beginFrame(width, largeMetrics.menuBarHeight);
        runtime.compose("pin-button", width, 80, [&](core::dsl::Ui& view, const core::dsl::Screen&) {
            ui = &view;
            neo::tab_bar_detail::pinButtonView(view, colors, actions.pinX, largeMetrics.menuBarHeight, true);
        });
        const auto* pin = ui->find("menubar.tabs.pin.hit");
        if (!pin || !pin->onClick || !ui->find("menubar.tabs.pin.icon.line.0") ||
            neo::platform::chrome::captionAt(pin->frame.x + 1, pin->frame.y + 1, width, largeMetrics.menuBarHeight) ||
            neo::platform::chrome::captionAt(pin->frame.x + pin->frame.width - 1,
                pin->frame.y + pin->frame.height - 1, width, largeMetrics.menuBarHeight) ||
            !neo::platform::chrome::captionAt(header.tabsRight + 8, 16, width, largeMetrics.menuBarHeight)) return 25;
    }
    using PopupOwner = neo::popup_presentation::Owner;
    neo::AppState popupState;
    if (neo::popup_presentation::activeOwner(popupState) != PopupOwner::None) return 26;
    const std::array<PopupOwner, 6> owners{{PopupOwner::Language, PopupOwner::VaultContext,
        PopupOwner::EditorContext, PopupOwner::FindOptions, PopupOwner::MainMenu, PopupOwner::MarkdownHeading}};
    for (const auto owner : owners) {
        popupState.openMenu = neo::MenuKind::Edit;
        popupState.contextMenuOpen = popupState.vaultContextMenuOpen = popupState.findOptionsOpen = true;
        popupState.markdownHeadingMenuOpen = popupState.languageMenuOpen = popupState.tabListOpen = true;
        neo::popup_presentation::activate(popupState, owner);
        if (neo::popup_presentation::activeOwner(popupState) != owner || popupState.tabListOpen) return 26;
    }
    neo::popup_presentation::activate(popupState, PopupOwner::None);
    if (neo::popup_presentation::activeOwner(popupState) != PopupOwner::None) return 26;

    for (const auto style : {neo::UiStyle::Square, neo::UiStyle::Rounded}) {
        popupState.uiStyle = style;
        const bool square = style == neo::UiStyle::Square;
        for (const auto mode : {neo::ThemeMode::Light, neo::ThemeMode::Dark}) {
            auto tokens = neo::editorColors(mode).tokens;
            // A style change may arrive with a token snapshot from the previous
            // compose. The explicit application style must still win.
            tokens.metrics = components::theme::ThemeMetricTokens{};
            components::theme::applyCornerStyle(tokens.metrics, square
                ? components::theme::CornerStyle::Rounded : components::theme::CornerStyle::Square);
            tokens.surface.a = 0.35f; // Imported themes cannot expose busy content behind a menu.
            tokens.border = {0.23f, 0.46f, 0.62f, 0.48f};
            const components::ContextMenuStyle base(tokens);
            const auto surface = neo::popup_presentation::style(style, tokens);
            const components::theme::RadiusTokens rounded;
            if (!core::closeEnough(surface.background.a, 1.0f) ||
                !core::closeEnough(surface.background.r, base.background.r) ||
                !core::closeEnough(surface.background.g, base.background.g) ||
                !core::closeEnough(surface.background.b, base.background.b) ||
                !core::closeEnough(surface.text, base.text) ||
                !core::closeEnough(surface.hover, base.hover) ||
                !core::closeEnough(surface.pressed, base.pressed) ||
                surface.radius != (square ? 0.0f : rounded.card) ||
                surface.highlightRadius != (square ? 0.0f : rounded.card - rounded.tiny) ||
                surface.highlightInset != (square ? 2.0f : 6.0f)) return 27;
            if (square) {
                if (!core::closeEnough(surface.border, neo::popup_presentation::opaque(tokens.border)) ||
                    surface.shadow.blur != 12.0f || surface.shadow.offset.y != 2.0f) return 30;
            } else if (!core::closeEnough(surface.border, base.border) ||
                       surface.shadow.blur != base.shadow.blur ||
                       !core::closeEnough(surface.shadow.offset, base.shadow.offset) ||
                       !core::closeEnough(surface.shadow.color, base.shadow.color)) return 30;
            for (bool animations : {true, false}) {
                popupState.animations = animations;
                const auto motion = neo::popup_presentation::motion(style, animations);
                const auto replacement = neo::popup_presentation::motion(style, animations, false);
                if (motion.enter.enabled != animations || motion.exit.enabled != animations ||
                    motion.highlight.enabled != (animations && !square) || replacement.exit.enabled ||
                    motion.clipContent != (square && animations) ||
                    motion.directionalSubmenus != (square && animations) ||
                    motion.offsetX != 0.0f || motion.offsetY != (square ? -8.0f : 4.0f)) return 31;
                core::dsl::Ui* popupUi = nullptr;
                const auto popupId = [](PopupOwner owner) {
                    return "popup." + std::to_string(static_cast<int>(owner)) + ".level.0";
                };
                const auto composePopups = [&](PopupOwner requestedOwner) {
                    runtime.compose("solid-popup-policy", 1000, 700, [&](core::dsl::Ui& view, const core::dsl::Screen&) {
                        popupUi = &view;
                        for (const auto owner : owners) {
                            const bool open = owner == requestedOwner;
                            auto menu = components::contextMenu(view, "popup." + std::to_string(static_cast<int>(owner)))
                                .open(open).screen(1000, 700).position(20, 40).items({"Action", "Other"}).theme(tokens);
                            neo::popup_presentation::apply(menu, popupState, tokens, open);
                            menu.build();
                        }
                    });
                };
                for (const auto active : owners) {
                    neo::popup_presentation::activate(popupState, active);
                    if (active == PopupOwner::MainMenu) popupState.openMenu = neo::MenuKind::File;
                    composePopups(active);
                    bool material = false;
                    const auto inspect = [&](auto&& self, const core::dsl::Element& element) -> void {
                        material = material || static_cast<bool>(element.frozenBackdrop);
                        for (const auto& child : element.children) self(self, *child);
                    };
                    for (const auto& root : popupUi->roots()) inspect(inspect, *root);
                    if (material || popupUi->frozenBackdropSession() || popupUi->cachedStripSession()) return 28;
                    for (const auto owner : owners) {
                        auto* level = popupUi->find(popupId(owner));
                        const bool open = owner == active;
                        if (!level || level->transition.enabled != (open && animations) ||
                            level->disabled == open ||
                            !core::closeEnough(level->transform.scale, core::Vec2{1,1})) return 29;
                    }
                    auto* item = popupUi->find(popupId(active) + ".item.0");
                    if (!item || !item->onHoverChanged) return 32;
                    item->onHoverChanged(true);
                    composePopups(active);
                    const auto* highlight = popupUi->find(popupId(active) + ".highlight");
                    const auto* background = popupUi->find(popupId(active) + ".bg");
                    if (!highlight || !background ||
                        !core::closeEnough(highlight->frame.x - background->frame.x, surface.highlightInset) ||
                        highlight->radius != surface.highlightRadius ||
                        highlight->transition.enabled != (animations && !square) ||
                        background->radius != surface.radius ||
                        !core::closeEnough(highlight->color, surface.hover)) return 33;
                    // A dismissal to the document retains the finite exit and
                    // disables all six popup trees immediately.
                    neo::popup_presentation::activate(popupState, PopupOwner::None);
                    composePopups(PopupOwner::None);
                    const auto* closing = popupUi->find(popupId(active));
                    if (!closing || !closing->disabled || closing->transition.enabled != animations) return 34;
                }
            }
        }
    }
    runtime.shutdown();
    for (const auto style : {neo::UiStyle::Square, neo::UiStyle::Rounded}) {
        if (!applicationAnimationToggle(style)) {
            std::cerr << "application menu animation toggle failed\n";
            return 35;
        }
    }
    const auto imageFileMenu=fileMenuItems(false);
    const auto imageEditMenu=editMenuItems(false);
    for (int index : {3,4,7,8}) if (imageFileMenu[index].enabled) return 36;
    for (const auto& item : imageEditMenu) if (item.enabled) return 37;
    std::cout<<"translated menu commands, compact layout and opaque popup policy passed\n";
    return 0;
}
