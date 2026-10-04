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
        if(commandAtPath(items,{3})!=id(Command::Wrap) || commandAtPath(items,{4})!=id(Command::Readable) ||
           commandAtPath(items,{7,0})!=id(Command::LanguageSystem) || commandAtPath(items,{7,1})!=id(Command::LanguageChinese) ||
           commandAtPath(items,{7,2})!=id(Command::LanguageEnglish) || commandAtPath(items,{8})!=id(Command::Settings)) return 1;
        if(commandAtPath(items,{}) || commandAtPath(items,{-1}) || commandAtPath(items,{200}) ||
           commandAtPath(items,{0}) || commandAtPath(items,{0,200}) || commandAtPath(items,{0,0,0})) return 2;
        // A separator, translation or reordered display row must never change its action.
        std::reverse(items.begin(),items.end());
        if(commandAtPath(items,{0})!=id(Command::Settings) || commandAtPath(items,{1,2})!=id(Command::LanguageEnglish)) return 3;
        items[0].enabled=false;
        if(commandAtPath(items,{0})) return 4;
        const auto edit=editMenuItems();
        if(commandAtPath(edit,{6})!=id(Command::Syntax) || commandAtPath(edit,{5})!=id(Command::Find)) return 5;
    }
    state.editorFontSize=17;
    const auto sizes=fontSizeOptions(state);
    const auto custom=viewMenuItems(state);
    if(sizes.front()!=17 || commandAtPath(custom,{6,0})!=id(Command::FontSizeBase) || !custom[6].children[0].checked) return 6;
    std::cout<<"translated menu commands, grouping, invalid paths and custom size passed\n";
    return 0;
}
