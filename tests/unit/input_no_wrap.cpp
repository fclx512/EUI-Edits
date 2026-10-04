#include "components/input.h"
#include <cmath>
#include <iostream>

namespace {

const core::dsl::Element* findTextElement(const core::dsl::Element& element) {
    if (element.kind == core::dsl::ElementKind::Text &&
        element.id.find(".text.") != std::string::npos && !element.dirtyKey.empty()) {
        return &element;
    }
    for (const auto& child : element.children) {
        if (const auto* found = findTextElement(*child)) return found;
    }
    return nullptr;
}

} // namespace

int main() {
    using M=components::input_detail::InputModel;
    using L=M::InputLayout;
    core::dsl::Ui ui;
    std::string value=std::string(240,'x')+" 中文 😀\nshort\n";
    bool wrap=false;
    auto compose=[&] {
        ui.begin("no-wrap");
        components::input(ui,"field").size(180,140).multiline().wordWrap(wrap)
            .fontFamily("monospace").fontSize(16).inset(12).scrollbar().value(value).build();
        ui.end();ui.layout(180,140);
    };
    compose();auto& state=ui.state<M::InputState>("field");
    if(state.cachedLines.size()!=3 || state.wordWrap) return 1;
    state.cursor=220;state.selectionStart=220;state.selectionEnd=220;state.followCaret=true;
    compose();
    if(state.horizontalScroll<=0 || !ui.find("field.hscroll.thumb")) return 2;
    auto layout=L::build(state,140,100,164,12,12,0,19.2f,"monospace",16,true);
    if(layout.cursorX<12 || layout.cursorX>152) return 3;
    core::Rect bounds{0,0,164,100};
    const auto hit=layout.pointerHit(layout.cursorX,5,bounds,164,12);
    if(hit.byteIndex!=220) return 4;
    state.selectionStart=215;state.selectionEnd=225;
    layout=L::build(state,140,100,164,12,12,0,19.2f,"monospace",16,true);
    if(layout.selectionRects.empty() || layout.selectionRects.front().x>layout.cursorX) return 5;
    state.compositionText="输入";
    auto& display=M::displayState(state,true);
    if(display.wordWrap || display.text==state.text) return 6;
    const auto preedit=L::build(display,140,100,164,12,12,0,19.2f,"monospace",16,true);
    if(preedit.lineList().size()!=3 || display.horizontalScroll<=0 || state.text!=value) return 7;
    state.compositionText.clear();
    state.cursor=0;state.selectionStart=0;state.selectionEnd=0;
    state.horizontalScroll=0;state.verticalScroll=0;state.followCaret=false;
    compose();
    const std::string nowrapRootKey = ui.find("field")->dirtyKey;
    const auto* nowrapText = findTextElement(*ui.find("field"));
    if (!nowrapText) return 11;
    const std::string nowrapTextId = nowrapText->id;
    const std::string nowrapTextKey = nowrapText->dirtyKey;
    const std::string nowrapTextValue = nowrapText->text;
    wrap=true;compose();
    if(state.cachedLines.size()<=3 || state.horizontalScroll!=0) return 8;
    const auto* wrapText = findTextElement(*ui.find("field"));
    if (ui.find("field")->dirtyKey == nowrapRootKey || !wrapText ||
        wrapText->id != nowrapTextId || wrapText->dirtyKey == nowrapTextKey ||
        wrapText->text == nowrapTextValue) return 12;
    wrap=false;compose();
    if(state.cachedLines.size()!=3 || state.text!=value) return 9;
    state.cursor=0;state.selectionStart=0;state.selectionEnd=0;state.followCaret=true;compose();
    if(state.horizontalScroll!=0) return 10;
    std::cout<<"no-wrap layout/caret/hit/selection/IME/wrap-toggle checks passed\n";
}
