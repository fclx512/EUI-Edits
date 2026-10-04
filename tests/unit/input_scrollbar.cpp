#include "components/input.h"
#include <cmath>
#include <iostream>

int main() {
    using Model = components::input_detail::InputModel;
    core::dsl::Ui ui;
    std::string value;
    for (int i = 0; i < 2000; ++i) value += "row " + std::to_string(i) + "\n";
    auto compose = [&](bool enabled, bool multiline = true, float height = 160.f, float width = 320.f) {
        ui.begin("scrollbar");
        components::input(ui, "field").size(width, height).inset(10.f).fontSize(16.f)
            .multiline(multiline).scrollbar(enabled).value(value).build();
        ui.end();
        ui.layout(width, height);
    };
    ui.begin("scrollbar");
    components::input(ui, "field").size(320.f, 160.f).multiline().value(value).build();
    ui.end();
    if (ui.find("field.scrollbar.thumb")) return 1;
    compose(true);
    auto& state = ui.state<Model::InputState>("field");
    auto* thumb = ui.find("field.scrollbar.thumb");
    auto* track = ui.find("field.scrollbar.track");
    auto* viewport = ui.find("field.textViewport");
    if (!thumb || !track || !viewport || !thumb->preserveFocusOnPress || !track->preserveFocusOnPress ||
        viewport->frame.x + viewport->frame.width >= track->frame.x) return 2;
    const float reservedWidth = viewport->frame.width;
    const auto cursor = state.cursor;
    const auto revision = state.textRevision;
    // 模拟 150% DPI 下点击轨道顶部，然后拖动滑块至中点与两端。
    core::Rect bounds{100.f, 200.f, track->frame.width * 1.5f, track->frame.height * 1.5f};
    core::PointerEvent press;
    press.y = bounds.y;
    track->onPress(press, bounds);
    compose(true);
    if (state.verticalScroll != 0.f || state.followCaret || !ui.find("field.text.0")) return 3;
    thumb = ui.find("field.scrollbar.thumb");
    const float travel = ui.find("field.scrollbar.track")->frame.height - thumb->frame.height;
    bounds.height = thumb->frame.height * 1.5f;
    thumb->onPress(press, bounds);
    core::dsl::DragEvent drag;
    drag.totalY = travel * 1.5f * 0.5f;
    thumb->onDrag(drag);
    // 滚动上限 = 内容高 − **可见文本区高**。可见高就是文本视口的裁剪框高度，必须从
    // 控件里读，不能写常数：2026-09-27 取消纵向内缩并给首行墨迹留 inkOverhang 之后，
    // 可见高从"控件高 − 2×内缩"变成了"控件高 − 2×(inkOverhang + 2)"，写死的 140
    // 早就不成立了（这条断言在改之前就已经失败，见批次记录）。
    const float viewportHeight = ui.find("field.textViewport")->frame.height;
    const float maximum = static_cast<float>(state.cachedLines.size()) * 19.2f - viewportHeight;
    if (std::fabs(state.verticalScroll - maximum * 0.5f) > 0.1f) return 4;
    compose(true);
    thumb = ui.find("field.scrollbar.thumb");
    // 重组 UI 后继续同一拖拽，不能以新位置作为起点导致跳动。
    drag.totalY = travel * 1.5f;
    thumb->onDrag(drag);
    compose(true);
    if (std::fabs(state.verticalScroll - maximum) > 0.1f || state.cursor != cursor || state.textRevision != revision) return 5;
    thumb = ui.find("field.scrollbar.thumb");
    drag.totalY = -100000.f;
    thumb->onDrag(drag);
    compose(true);
    if (state.verticalScroll != 0.f) return 6;
    core::ScrollEvent wheel;
    wheel.y = -1;
    ui.find("field.hit")->onScroll(wheel);
    compose(true);
    // "滑块离开了原位"要对着滚动条轨道自己的顶边量：纵向内缩取消后轨道顶不再是 10px，
    // 写死的 10 会把"刚滚动一点点"误判成没动（见上面 viewportHeight 处的说明）。
    const auto* movedTrack = ui.find("field.scrollbar.track");
    if (state.verticalScroll <= 0.f || !movedTrack ||
        ui.find("field.scrollbar.thumb")->frame.y <= movedTrack->frame.y) return 7;
    // 键盘导航恢复光标跟随，光标与滚动条使用同一 viewport。
    core::KeyEvent key;
    key.key = core::InputKey::End;
    key.action = core::KeyAction::Press;
    ui.find("field.hit")->onKeyEvent(key);
    compose(true);
    if (!state.followCaret || state.verticalScroll < maximum - 1.f) return 8;
    compose(false);
    if (ui.find("field.scrollbar.thumb") || !ui.find("field.hit")->onScroll) return 9;
    compose(true, false);
    if (ui.find("field.scrollbar.thumb")) return 10;
    value = "short";
    compose(true);
    if (ui.find("field.scrollbar.thumb") || state.verticalScroll != 0.f ||
        ui.find("field.textViewport")->frame.width != reservedWidth) return 11;
    value = "a\nb\nc";
    compose(true, true, 12.f);
    if (ui.find("field.scrollbar.thumb")) return 12;
    // 无换行符的长文本也按预留槽位后的宽度折行；扩大 viewport 后隐藏滚动条。
    value = std::string(500, 'W');
    compose(true, true, 100.f, 120.f);
    if (!ui.find("field.scrollbar.thumb") || state.cachedLines.size() <= 1) return 13;
    for (const auto& line : state.cachedLines) {
        if (line.metrics.width > ui.find("field.textViewport")->frame.width + 0.1f) return 14;
    }
    compose(true, true, 2000.f, 1000.f);
    if (ui.find("field.scrollbar.thumb") || state.verticalScroll != 0.f) return 15;
    std::cout << "Scrollbar defaults, overflow, DPI drag, recompose, wheel, caret and shrink passed\n";
}
