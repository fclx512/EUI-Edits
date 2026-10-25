#include "ui/markdown_toolbar.h"
#include "ui/outline_view.h"
#include "ui/window_chrome.h"
#include "components/input.h"
#include "core/dsl_runtime.h"
#include "platform/window_chrome.h"
#include <cmath>
#include <iostream>

namespace app { void requestUpdate() {} }
namespace neo::settings {
bool writeRecovery(const std::string&, const std::string&, const textfile::Document*) { return true; }
}
namespace neo::cleanai {
Result clean(const std::string& document, std::size_t begin, std::size_t end) {
    return {document, begin, end, false};
}
}

int main() {
    using Model = components::input_detail::InputModel;
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << message << '\n'; }
    };
    core::dsl::Runtime runtime;
    runtime.initialize();
    neo::AppState state;
    state.newDocumentLanguage = "markdown";
    state.doc.text = "中文🙂";
    core::dsl::Ui* ui = nullptr;
    auto compose = [&](float width = 800) {
        runtime.compose("markdown-writing", width, 500, [&](core::dsl::Ui& view, const core::dsl::Screen&) {
            ui = &view;
            neo::applyEditorCommand(view, state);
            neo::markdownToolbarView(view, state, width);
            components::input(view, neo::editorInputId(state)).position(0, 80).size(width, 400)
                .multiline().typewriter(state.writingCapable() && state.markdownTypewriter)
                .valueRef(state.doc.text).build();
        });
    };
    compose();
    auto click = [&](const char* id) {
        auto* element = ui->find(id);
        check(element && element->onClick, id);
        if (element && element->onClick) element->onClick();
    };
    auto& input = ui->state<Model::InputState>(neo::editorInputId(state));
    Model::moveCursorTo(input, static_cast<int>(input.text.size()), false);
    Model::moveCursorTo(input, 0, true); // reverse Unicode selection
    click("markdown.bold.bg");
    compose();
    check(state.doc.text == "**中文🙂**" && input.undoStack.size() == 1, "bold uses selection and one undo");
    state.pendingEditorCommand = neo::EditorCommand::Undo;
    compose();
    check(state.doc.text == "中文🙂", "toolbar edit undo restores original");
    for (const auto& command : {std::pair<neo::EditorCommand, int>{neo::EditorCommand::SetHeading, 2},
                               {neo::EditorCommand::InsertBlock, 7}, {neo::EditorCommand::InsertBlock, 2},
                               {neo::EditorCommand::InsertBlock, 1}, {neo::EditorCommand::InsertBlock, 6}}) {
        Model::loadDocument(input, "中文🙂");
        state.doc.text = input.text;
        neo::queueMarkdownTool(state, command.first, command.second);
        compose();
        check(input.text != "中文🙂" && input.undoStack.size() == 1, "format command is a single edit");
        check(Model::undoEdit(input) && input.text == "中文🙂", "format command restores source in one undo");
        state.doc.text = input.text;
    }
    click("markdown.image.bg");
    check(state.pendingImageImport, "image toolbar uses existing picker import path");
    state.pendingImageImport = false;
    auto* bold = ui->find("markdown.bold.bg");
    check(bold && bold->onHoverChanged, "toolbar owns hover trigger");
    if (bold && bold->onHoverChanged) bold->onHoverChanged(true);
    compose();
    check(ui->find("markdown.bold.tip.text") && ui->find("markdown.toolbar")->zIndex > 0,
          "tooltip name exists above editor sibling");
    if (auto* b=ui->find("markdown.bold.bg"); b && b->onHoverChanged) b->onHoverChanged(false);
    click("markdown.typewriter.bg"); compose();
    check(state.markdownTypewriter && input.typewriter, "typewriter propagated to input");
    const bool oldStatus = state.showStatusBar;
    const auto oldMode = state.mode;
    click("markdown.immersive.bg"); compose();
    check(neo::markdownImmersive(state) && !ui->find("markdown.bold.bg") && ui->find("markdown.immersive.bg"),
          "immersive hides formatting and preserves an exit button");
    check(state.showStatusBar == oldStatus && state.mode == oldMode, "immersive does not overwrite layout preferences");
    state.newDocumentLanguage = "text"; compose();
    check(ui->find("writing.capsule") && !ui->find("markdown.bold.bg") && !ui->find("markdown.outline.bg") &&
          input.typewriter && neo::markdownImmersive(state) && neo::markdownToolbarHeight(state,800)==44,
          "plain text retains writing modes in a small capsule without MD formatting");
    state.newDocumentLanguage = "cpp"; compose();
    check(!ui->find("markdown.toolbar") && !input.typewriter && !neo::markdownImmersive(state),
          "source languages have no writing controls or effects");
    state.newDocumentLanguage = "text"; compose();
    click("markdown.immersive.bg"); compose();
    check(!neo::markdownImmersive(state) && ui->find("writing.capsule"), "plain text can exit focus");
    for (const auto style : {neo::UiStyle::Square, neo::UiStyle::Rounded}) {
        neo::setUiStyle(style);
        compose();
        neo::applyInteractionDefaults(*ui, neo::kUiFontFamily, true);
        const bool square = style == neo::UiStyle::Square;
        check(ui->find("writing.capsule")->radius == (square ? 0.0f : 9.0f),
              "plain writing capsule follows square and rounded styles");
        for (const char* id : {"markdown.typewriter.bg", "markdown.immersive.bg"}) {
            const auto* control = ui->find(id);
            check(control && control->radius == (square ? 0.0f : 6.0f) &&
                  bool(control->reveal) == square, "writing controls follow geometry and Reveal policy");
        }
    }
    const auto metrics = neo::uiMetrics(state);
    const float narrowRail = neo::writingCapsuleRailWidth(state, 800, metrics);
    const float wideRail = neo::writingCapsuleRailWidth(state, 1200, metrics);
    check(neo::markdownToolbarHeight(state, 800) == 0 && narrowRail > 0 && narrowRail <= 88 && wideRail == 0,
          "plain writing capsule uses existing readable margin before reserving a side rail");
    state.showLineNumbers = false;
    const float floatingRail = neo::writingCapsuleRailWidth(state, 800, metrics);
    runtime.compose("floating-writing", 800, 500, [&](core::dsl::Ui& view, const core::dsl::Screen&) {
        ui = &view;
        view.column("floating.column").size(800, 500).content([&] {
            neo::markdownToolbarView(view, state, 800);
            view.rect("floating.editor").size(800 - floatingRail, 500).build();
        }).build();
    });
    const auto* floating = ui->find("markdown.toolbar");
    const auto* capsule = ui->find("writing.capsule");
    const auto* editor = ui->find("floating.editor");
    const float available = 800 - floatingRail - 2.0f * metrics.editorInset;
    const float readable = metrics.editorFontSize * 43.75f;
    const float textRight = 800 - floatingRail - metrics.editorInset
        - std::max(0.0f, (available - readable) * 0.5f);
    check(floating && floating->ignoreLayout && capsule && editor && !ui->find("markdown.toolbar.bg") &&
          std::fabs(editor->frame.y) < .01f && capsule->frame.x >= textRight,
          "floating capsule leaves the first plain-text line unobstructed");
    neo::queueMarkdownTool(state, neo::EditorCommand::FormatInline, 1);
    check(state.pendingEditorCommand == neo::EditorCommand::None, "plain document rejects toolbar command");
    state.newDocumentLanguage = "markdown"; state.markdownImmersive = false; compose(360);
    check(neo::markdownToolbarHeight(state, 360) == 80 && ui->find("markdown.bold.bg"), "narrow toolbar uses two rows");
    const auto* sun = ui->find("markdown.image.icon.sun");
    check(sun && std::fabs(sun->frame.width - sun->frame.height) < .01f &&
          std::fabs(sun->radius * 2.0f - sun->frame.width) < .01f,
          "image icon sun is circular");
    for (neo::ThemeMode theme : {neo::ThemeMode::Light, neo::ThemeMode::Dark}) {
        state.theme = theme;
        compose(360);
        const auto* bar = ui->find("markdown.toolbar.bg");
        const auto& body = neo::editorColors(theme).editor;
        check(bar && std::fabs(bar->color.r - body.r) < .0001f &&
              std::fabs(bar->color.g - body.g) < .0001f &&
              std::fabs(bar->color.b - body.b) < .0001f &&
              std::fabs(bar->color.a - body.a) < .0001f,
              "Markdown toolbar joins its theme's editor surface without a color seam");
    }
    check(!state.markdownOutlineOpen, "outline defaults to collapsed");
    click("markdown.outline.bg"); compose();
    check(neo::markdownOutlineWidth(state,1000)>0 && neo::markdownOutlineWidth(state,600)==0,
          "outline docks only when text has enough width");
    click("markdown.outline.bg"); compose();
    check(!state.markdownOutlineOpen && neo::markdownOutlineWidth(state,1000)==0,"outline closes from toolbar");
    click("markdown.outline.bg"); compose();
    state.mode=neo::EditorMode::Simple;
    state.doc.text="# 中文标题\n\n## Second\n";
    const auto outlinePlan=neo::buildLpPlan(state.doc.text);
    check(neo::refreshVisibleOutline(state,state.doc.text,outlinePlan,1234) && state.outline.size()==2,
          "simple mode updates right outline from editor plan");
    check(!neo::refreshVisibleOutline(state,"stale text",outlinePlan,1235),"outline rejects stale document text");
    state.markdownOutlineOpen=false;
    check(!neo::refreshVisibleOutline(state,state.doc.text,outlinePlan,1235),"closed outline skips projection");
    state.markdownOutlineOpen=true;
    state.outline={{"中文标题",1,0,0},{"Second",2,16,1}};
    runtime.compose("outline",280,400,[&](core::dsl::Ui& view,const core::dsl::Screen&){
        ui=&view;neo::outlinePanelView(view,state,280,400);
    });
    check(ui->find("outline.panel") && ui->find("outline.list"),"right outline builds");
    check(ui->find("outline.list.slot.1.level") && ui->find("outline.list.slot.1.level")->text=="H2",
          "outline exposes heading level separately from title");
    click("outline.list.slot.1.bg");
    check(state.pendingOutlineJumpByte==16 && state.findEditorFocusPending,"outline queues exact source-byte navigation");
    click("outline.collapse.hit");
    check(!state.markdownOutlineOpen,"outline card can collapse itself");
    state.newDocumentLanguage="text";state.markdownOutlineOpen=true;
    check(neo::markdownOutlineWidth(state,1000)==0,"non markdown never reserves outline space");

    auto focusCompose = [&](bool enabled, bool animated) {
        runtime.compose("focus",300,300,[&](core::dsl::Ui& view,const core::dsl::Screen&) {
            ui=&view;
            components::input(view,"focus.input").size(300,300).multiline().wordWrap()
                .fontSize(16).focusLine(enabled,animated).value("first line\nsecond line\nthird line").build();
        });
    };
    focusCompose(true,false);
    auto& focusState=ui->state<Model::InputState>("focus.input");
    Model::moveCursorTo(focusState,12,false);
    focusCompose(true,false);
    check(ui->find("focus.input.focus.0")->opacity==.45f && ui->find("focus.input.focus.1")->opacity==1,
          "focus follows current visual row without changing content");
    check(!ui->find("focus.input.focus.0")->transition.enabled, "focus animation can be disabled");
    Model::moveCursorTo(focusState,1,true);
    focusCompose(true,true);
    check(ui->find("focus.input.focus.0")->opacity==1 && ui->find("focus.input.focus.1")->opacity==1,
          "selected rows remain readable in focus mode");
    focusCompose(false,false);
    check(ui->find("focus.input.focus.2")->opacity==1, "leaving focus restores every row");

    Model::InputState longDoc;
    std::string text;
    for (int i = 0; i < 60; ++i) text += "中文🙂 wrapped words and more words\n";
    Model::loadDocument(longDoc, text);
    longDoc.typewriter = true;
    auto layout = [&] {
        return Model::InputLayout::build(longDoc, 180, 200, 200, 10, 10, 10, 20, "monospace", 16, true);
    };
    Model::moveCursorTo(longDoc, static_cast<int>(text.size()/2), false);
    auto middle = layout();
    const float center = (middle.geometryTable().top(middle.cursorLine) + middle.geometryTable().bottom(middle.cursorLine))*0.5f;
    check(std::fabs(center - longDoc.verticalScroll - 100) < .01f, "typewriter centers wrapped input row");
    longDoc.followCaret = false; longDoc.verticalScroll = 0;
    layout();
    check(longDoc.verticalScroll == 0, "manual scroll stays independent of caret");
    longDoc.selecting = true;
    longDoc.verticalScroll = center - 80;
    Model::syncVerticalScrollTo(longDoc, center - 10, center + 10, 200);
    check(std::fabs(longDoc.verticalScroll - (center - 80)) < .01f,
          "pointer selection does not force recentering inside the viewport");
    longDoc.selecting = false;
    longDoc.followCaret = true;
    Model::moveCursorTo(longDoc, static_cast<int>(text.size()), false);
    auto last = layout();
    check(last.maxVerticalScroll > last.geometryTable().total() - 200, "last line has trailing breathing room");
    check(std::fabs((last.geometryTable().top(last.cursorLine)+last.geometryTable().bottom(last.cursorLine))*0.5f-
                    longDoc.verticalScroll-100) < .01f, "final input row can be centered");
    longDoc.compositionText = "你好🙂";
    auto& preedit = Model::displayState(longDoc, true);
    check(preedit.typewriter && longDoc.text == text, "IME display inherits mode without mutating document");
    const auto preeditLayout = Model::InputLayout::build(preedit, 180, 200, 200, 10, 10, 10, 20, "monospace", 16, true);
    check(std::fabs((preeditLayout.geometryTable().top(preeditLayout.cursorLine) +
                    preeditLayout.geometryTable().bottom(preeditLayout.cursorLine))*0.5f -
                    preedit.verticalScroll - 100) < .01f, "IME display centers its own visual row");
    longDoc.typewriter = false; longDoc.compositionText.clear();
    auto normal = layout();
    check(normal.contentHeight == normal.geometryTable().total() && longDoc.verticalScroll <= normal.maxVerticalScroll,
          "turning off restores normal scroll extent");
    neo::platform::chrome::interactiveRects.clear();
    neo::platform::chrome::exclude(10,0,150,34); // menus
    neo::platform::chrome::exclude(200,4,180,30); // visible tab
    neo::platform::chrome::exclude(880,0,120,34); // window controls
    check(neo::platform::chrome::captionAt(450,18,1000,34) &&
          neo::platform::chrome::captionAt(800,18,1000,34), "all empty titlebar spans drag");
    check(!neo::platform::chrome::captionAt(50,18,1000,34) &&
          !neo::platform::chrome::captionAt(250,18,1000,34) &&
          !neo::platform::chrome::captionAt(920,18,1000,34) &&
          !neo::platform::chrome::captionAt(450,50,1000,34), "controls and editor never drag");
    runtime.compose("window-controls",1000,500,[&](core::dsl::Ui& view,const core::dsl::Screen& screen){
        ui=&view; neo::windowChromeView(view,screen,34);
    });
    const auto* closeHit=ui->find("window.close.bg");
    check(closeHit && std::fabs(closeHit->frame.x+closeHit->frame.width-1000)<.01f &&
          closeHit->frame.y==0 && closeHit->frame.height==34, "close hit covers the top-right client pixel");
    // Exercise the production toolbar, so exclusions track the actual controls
    // for both writing modes instead of a duplicated hard-coded titlebar span.
    for (const char* language : {"markdown", "text"}) {
        state.newDocumentLanguage = language;
        state.markdownImmersive = true;
        for (const float width : {380.0f, 608.0f, 1000.0f}) {
            neo::platform::chrome::beginFrame(width, 44);
            runtime.compose("immersive-window-controls",width,500,[&](core::dsl::Ui& view,const core::dsl::Screen& screen){
                ui=&view;
                neo::markdownToolbarView(view,state,width);
                neo::windowChromeView(view,screen,44);
            });
            const auto caption = [&](float x, float y) {
                return neo::platform::chrome::captionAt(x,y,width,44);
            };
            for (const char* id : {"markdown.typewriter.bg", "markdown.immersive.bg", "window.minimize.bg",
                                   "window.maximize.bg", "window.close.bg"}) {
                const auto* button=ui->find(id);
                check(button && !caption(button->frame.x+1,button->frame.y+1) &&
                      !caption(button->frame.x+button->frame.width-1,button->frame.y+button->frame.height-1),
                      "immersive mode and window buttons retain their complete client hit area");
            }
            check(caption(width-150,22) && caption(width-220,3) && caption(width-170,41),
                  "empty immersive titlebar space between and around controls drags the window");
            check(!caption(width-150,45), "body below immersive titlebar remains client input");
        }
    }
    // Adaptive layout changes visibility only, never the user's view choices.
    state.newDocumentLanguage = "markdown";
    state.path.clear();
    state.markdownImmersive = false;
    state.mode = neo::EditorMode::Vault;
    state.showStatusBar = true;
    state.showLineNumbers = true;
    state.markdownOutlineOpen = true;
    state.autoCompactWindow = true;
    neo::updateCompactWindow(state, 1000);
    check(!state.compactWindow && neo::vaultPanelVisible(state) && neo::statusBarVisible(state),
          "wide layout respects manual visibility choices");
    neo::updateCompactWindow(state, 799);
    compose(799);
    check(state.compactWindow && !neo::vaultPanelVisible(state) && !neo::statusBarVisible(state) &&
          !neo::lineNumbersVisible(state) && neo::markdownToolbarHeight(state, 799) == 0 &&
          neo::markdownOutlineWidth(state, 1000) == 0 &&
          neo::writingCapsuleRailWidth(state, 799, neo::uiMetrics(state)) == 0 &&
          !ui->find("markdown.toolbar"), "narrow layout removes chrome and its occupied space");
    check(state.mode == neo::EditorMode::Vault && state.showStatusBar && state.showLineNumbers &&
          state.markdownOutlineOpen, "auto hiding does not overwrite manual preferences");
    neo::updateCompactWindow(state, 820);
    check(state.compactWindow, "small width oscillations do not restore panels");
    neo::updateCompactWindow(state, 880);
    compose(880);
    check(!state.compactWindow && neo::vaultPanelVisible(state) && neo::statusBarVisible(state) &&
          neo::lineNumbersVisible(state) && ui->find("markdown.toolbar"), "widening restores previous visibility");
    state.mode = neo::EditorMode::Simple;
    state.showStatusBar = false;
    state.showLineNumbers = false;
    state.markdownOutlineOpen = false;
    neo::updateCompactWindow(state, 700);
    neo::updateCompactWindow(state, 1000);
    check(!neo::vaultPanelVisible(state) && !neo::statusBarVisible(state) && !neo::lineNumbersVisible(state) &&
          !state.markdownOutlineOpen, "manual off remains off after a narrow/wide cycle");
    neo::updateCompactWindow(state, 700);
    state.autoCompactWindow = false;
    neo::updateCompactWindow(state, 700);
    check(!state.compactWindow && neo::markdownToolbarHeight(state, 700) > 0,
          "disabling automatic simplification immediately restores normal layout");
    state.autoCompactWindow = true;
    state.markdownImmersive = true;
    neo::updateCompactWindow(state, 700);
    compose(700);
    check(ui->find("markdown.immersive.bg"), "manual immersive mode keeps its exit control in a narrow window");
    runtime.shutdown();
    if (!failures) std::cout << "Markdown toolbar, undo, visibility, focus modes and typewriter geometry PASS\n";
    return failures ? 1 : 0;
}
