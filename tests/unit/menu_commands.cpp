#include "ui/menu_bar.h"
#include <algorithm>
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
        if(commandAtPath(items,{3})!=id(Command::Wrap) || commandAtPath(items,{4})!=id(Command::Readable) ||
           commandAtPath(items,{5})!=id(Command::Mindmap) ||
           commandAtPath(items,{8,0})!=id(Command::LanguageSystem) || commandAtPath(items,{8,1})!=id(Command::LanguageChinese) ||
           commandAtPath(items,{8,2})!=id(Command::LanguageEnglish) || commandAtPath(items,{9})!=id(Command::Settings)) return 1;
        if(commandAtPath(items,{}) || commandAtPath(items,{-1}) || commandAtPath(items,{200}) ||
           commandAtPath(items,{0}) || commandAtPath(items,{0,200}) || commandAtPath(items,{0,0,0})) return 2;
        // A separator, translation or reordered display row must never change its action.
        std::reverse(items.begin(),items.end());
        if(commandAtPath(items,{0})!=id(Command::Settings) || commandAtPath(items,{1,2})!=id(Command::LanguageEnglish)) return 3;
        const auto preview = std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.commandId==id(Command::Mindmap); });
        if (preview==items.end()) return 10;
        preview->enabled=false;
        if (commandAtPath(items,{static_cast<int>(std::distance(items.begin(),preview))})) return 11;
        items[0].enabled=false;
        if(commandAtPath(items,{0})) return 4;
        const auto edit=editMenuItems();
        if(commandAtPath(edit,{6})!=id(Command::Syntax) || commandAtPath(edit,{5})!=id(Command::Find)) return 5;
    }
    state.editorFontSize=17;
    const auto sizes=fontSizeOptions(state);
    const auto custom=viewMenuItems(state);
    if(sizes.front()!=17 || commandAtPath(custom,{7,0})!=id(Command::FontSizeBase) || !custom[7].children[0].checked) return 6;
    std::cout<<"translated menu commands, grouping, invalid paths and custom size passed\n";
    return 0;
}
