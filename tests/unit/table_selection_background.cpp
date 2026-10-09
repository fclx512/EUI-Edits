#include "components/input_model.h"
#include <cmath>
#include <iostream>

using M = components::input_detail::InputModel;
using D = components::input_detail::LineDecoration;
int failures = 0;
void check(bool ok, const char* why) { if (!ok) { ++failures; std::cerr << why << '\n'; } }
bool near(float a, float b) { return std::fabs(a - b) < 0.02f; }

struct Fixture {
    M::InputState state;
    std::vector<D> rows{1};
    int leftBeg = 2, leftEnd, rightBeg, rightEnd;
    Fixture(const std::string& left, const std::string& right) {
        state.text = "| " + left + " | " + right + " |";
        state.textRevision = 1; state.followCaret = false;
        leftEnd = leftBeg + static_cast<int>(left.size());
        rightBeg = leftEnd + 3; rightEnd = rightBeg + static_cast<int>(right.size());
        auto& r = rows[0]; r.tableId = 0; r.fontSize = 16; r.lineHeight = 29.6f;
        r.textShiftY = 4.4f; r.cellPadding = 5;
        r.cells = {{leftBeg, leftEnd}, {rightBeg, rightEnd}};
        r.holes = {{0,leftBeg},{leftEnd,rightBeg},{rightEnd,static_cast<int>(state.text.size())}};
    }
    M::InputLayout build(int beg, int end, float width = 260, float height = 500) {
        state.selectionStart = beg; state.selectionEnd = end;
        return M::InputLayout::build(state,width,height,width+24,12,12,12,20.8f,
                                     "Microsoft YaHei",16,true,&rows);
    }
};

void fullSelection(const std::string& tail) {
    Fixture f("甲",tail);
    auto layout = f.build(f.leftBeg,f.rightEnd);
    check(layout.lineList().size() >= 3,"fixture must wrap into at least three lines");
    std::vector<bool> covered(layout.lineList().size(),false);
    for (const auto& rect : layout.selectionRects) {
        check(near(rect.height,20.8f) && near(rect.lineHeight,20.8f),"all table selection bands must be equal height");
        for (size_t i=0;i<covered.size();++i) {
            const float expectedY = 12+4.4f+static_cast<float>(i)*20.8f;
            if(near(rect.y,expectedY)) covered[i]=true;
        }
    }
    for(bool c:covered) check(c,"full selection must cover every visual line");
    check(layout.selectionRects.size()==covered.size()+1,"exhausted short column must not have continuation backgrounds");
    check(near(layout.geometryTable().total(),static_cast<float>(covered.size())*20.8f+8.8f),
          "padding redistribution must preserve total table source-row height");
    const auto rects=layout.selectionRects;
    auto reverse=f.build(f.rightEnd,f.leftBeg);
    check(reverse.selectionRects.size()==rects.size(),"reverse selection must have the same spans");
    for(size_t i=0;i<std::min(rects.size(),reverse.selectionRects.size());++i) {
        const auto& a=rects[i]; const auto& b=reverse.selectionRects[i];
        check(near(a.x,b.x)&&near(a.y,b.y)&&near(a.width,b.width)&&near(a.height,b.height),
              "forward/reverse selection must have identical geometry");
    }
    auto source=f.build(0,static_cast<int>(f.state.text.size()));
    check(source.selectionRects.size()==rects.size(),"hidden source-row edges must not drop visual segments");
    auto right=f.build(f.rightBeg,f.rightEnd);
    check(right.selectionRects.size()==covered.size(),"right-only selection must not cover left column");
    const auto* columns=right.tableColumnsFor(0);
    for(const auto& r:right.selectionRects) check(r.x>=12+columns->x[1]-0.02f,"right column spans must stay in that column");
    f.state.verticalScroll=25.2f;
    auto scrolled=f.build(f.leftBeg,f.rightEnd,260,21);
    for(const auto& r:scrolled.selectionRects)
        check(r.y+r.height>=12 && r.y<=33,"offscreen selection bands must be omitted");
}

int main() {
    const std::string longCn="批量操作的写回范式五步顺序事务外壳细则见模块设计验收判断完成后为假";
    fullSelection(longCn);
    fullSelection("Batch operations write back in five steps with transaction scope and validation😀");
    fullSelection("Batch operations write back in five steps with transaction scope and validation");
    Fixture single("甲",longCn);
    auto all=single.build(single.leftBeg,single.rightEnd);
    const auto& last=all.lineList().back();
    const auto tail=last.tableDocCaretStops.back();
    const auto before=last.tableDocCaretStops[last.tableDocCaretStops.size()-2];
    const float expectedY=12+4.4f+static_cast<float>(all.lineList().size()-1)*20.8f;
    auto one=single.build(before.byteIndex,tail.byteIndex);
    check(one.selectionRects.size()==1,"one final Chinese character must have exactly one span");
    if(one.selectionRects.size()==1) {
        const auto& r=one.selectionRects[0];
        check(near(r.x,12+before.x)&&near(r.width,tail.x-before.x)&&near(r.y,expectedY),
              "single-character background must match exact endpoints and final visual line");
    }
    auto hidden=single.build(single.leftEnd,single.rightBeg);
    check(hidden.selectionRects.empty(),"hidden pipes and spaces alone must not select visible glyphs");
    auto collapsed=single.build(single.rightBeg,single.rightBeg);
    check(collapsed.selectionRects.empty(),"collapsed selection must have no rectangles");
    Fixture both(longCn,longCn);
    auto bothAll=both.build(both.leftBeg,both.rightEnd);
    const auto& finalLine=bothAll.lineList().back();
    int leftTailStart=both.leftEnd;
    for(const auto& s:finalLine.tableDocCaretStops)
        if(s.column==0) {leftTailStart=s.byteIndex;break;}
    auto crossed=both.build(leftTailStart,both.rightBeg+3);
    bool firstRow=false,lastRow=false;
    for(const auto& r:crossed.selectionRects) {
        firstRow=firstRow||near(r.y,16.4f);
        lastRow=lastRow||near(r.y,16.4f+static_cast<float>(bothAll.lineList().size()-1)*20.8f);
    }
    check(firstRow&&lastRow,"source selection between columns may cover earlier AND later visual segments");
    Fixture empty("",longCn);
    auto emptyAll=empty.build(0,static_cast<int>(empty.state.text.size()));
    check(emptyAll.selectionRects.size()==emptyAll.lineList().size(),"empty column must not produce phantom spans");
    for(float scale:{1.25f,1.5f}) {
        core::TextPrimitive::setLayoutPixelScale(scale);
        auto dpi=empty.build(0,static_cast<int>(empty.state.text.size()));
        for(const auto& r:dpi.selectionRects) check(near(r.height,20.8f),"DPI scaling must preserve logical selection heights");
    }
    core::TextPrimitive::setLayoutPixelScale(1);
    return failures?1:0;
}
