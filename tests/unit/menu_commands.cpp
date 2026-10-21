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
    runtime.shutdown();
    std::cout<<"translated menu commands, compact layout and chrome exclusions passed\n";
    return 0;
}
