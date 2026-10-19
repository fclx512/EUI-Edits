#include "ui/menu_bar.h"
#include "core/dsl_runtime.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

int main() {
    neo::AppState state;
    using namespace neo::menu_detail;
    using neo::i18n::setPreference;
    const auto id=[](Command command){return static_cast<int>(command);};
    for(const char* language : {"zh-CN","en"}) {
        setPreference(language);
        auto items=viewMenuItems(state);
        if(commandAtPath(items,{5,0})!=id(Command::FollowSystem) ||
           commandAtPath(items,{5,1})!=id(Command::Dark) ||
           commandAtPath(items,{5,2})!=id(Command::Light)) return 7;
        for (bool follow : {false,true}) {
            for (auto mode : {neo::ThemeMode::Dark,neo::ThemeMode::Light}) {
                state.themeFollowSystem=follow;
                state.theme=mode;
                const auto choices=viewMenuItems(state)[5].children;
                if(choices.size()!=3 || choices[0].checked!=follow ||
                   choices[1].checked!=(!follow && mode==neo::ThemeMode::Dark) ||
                   choices[2].checked!=(!follow && mode==neo::ThemeMode::Light)) return 8;
                if(std::count_if(choices.begin(),choices.end(),[](const auto& choice){return choice.checked;})!=1) return 9;
            }
        }
        if(commandAtPath(items,{3})!=id(Command::Wrap) || commandAtPath(items,{4})!=id(Command::Readable) ||
           commandAtPath(items,{7,0})!=id(Command::LanguageSystem) || commandAtPath(items,{7,1})!=id(Command::LanguageChinese) ||
           commandAtPath(items,{7,2})!=id(Command::LanguageEnglish) || commandAtPath(items,{8})!=id(Command::Settings)) return 1;
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
    if(sizes.front()!=17 || commandAtPath(custom,{6,0})!=id(Command::FontSizeBase) || !custom[6].children[0].checked) return 6;
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
    runtime.shutdown();
    std::cout<<"translated menu commands, grouping, invalid paths and custom size passed\n";
    return 0;
}
