#include "ui/theme_workbench.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
int failures = 0;
int closeRequests = 0;
int toastRequests = 0;
int settingsWrites = 0;
void check(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
}

// Exercise the production DSL and callbacks without a native window or input
// injection. Keep application notifications and settings writes observable.
namespace app {
void requestUpdate() {}
void requestClose() { ++closeRequests; }
namespace detail { void requestFullPaint() {} }
}
namespace neo {
void showToast(AppState&, std::string, std::string) { ++toastRequests; }
void persistSettings(const AppState&) { ++settingsWrites; }
float effectiveWindowScale() { return 1.0f; }
app::DslAppConfig& mutableAppConfig() { static app::DslAppConfig value; return value; }
}

namespace {
namespace wb = neo::theme_workbench_ui;
namespace model = neo::themeworkbench;
namespace fs = std::filesystem;

struct Fixture {
    neo::AppState state;
    eui::Ui ui;

    Fixture() {
        neo::themeloader::reset();
        wb::session() = wb::Session{};
        state.theme = neo::ThemeMode::Light;
        state.themeFile.clear();
        neo::openThemeWorkbench(state);
    }
    void compose(float width = 1000, float height = 720) {
        ui.begin("theme-workbench-headless");
        neo::themeWorkbenchOverlay(ui, state, eui::Screen{width, height});
        ui.end();
        ui.layout(width, height);
    }
    core::dsl::Element* element(const std::string& id) {
        auto* value = ui.find(id);
        if (!value) { ++failures; std::cerr << "FAIL: missing UI element " << id << '\n'; }
        return value;
    }
};

void testCandidateCallbacks() {
    Fixture f;
    check(wb::session().drafts.size() == 1 && !wb::anyDirty(), "opening the current theme starts with one clean draft");
    const auto before = wb::bytes(wb::draft());
    const auto baseline = neo::themeloader::serializeWorkbench(wb::draft().baseline, wb::draft().baseline, "");
    f.compose();
    auto* candidate = f.element("themeWorkbench.candidate.1.bg");
    if (!candidate || !candidate->onHoverChanged || !candidate->onClick) return;
    candidate->onHoverChanged(true);
    check(wb::session().trial == 1 && wb::bytes(wb::draft()) == before && wb::draft().undoStack.empty(),
          "hovering a candidate previews it without writing or creating undo history");
    candidate->onHoverChanged(false);
    check(wb::session().trial == -1 && wb::bytes(wb::draft()) == before,
          "leaving a candidate returns to the exact unchanged draft");
    candidate->onClick();
    check(wb::bytes(wb::draft()) != before && wb::draft().undoStack.size() == 1 && wb::anyDirty(),
          "explicit candidate adoption updates the draft in one undo step");
    check(neo::themeloader::serializeWorkbench(wb::draft().baseline, wb::draft().baseline, "") == baseline,
          "candidate adoption preserves the original restore baseline");
    const auto adopted = wb::bytes(wb::draft());
    wb::session().compare = true;
    f.compose();
    candidate = f.element("themeWorkbench.candidate.2.bg");
    if (candidate && candidate->onClick) candidate->onClick();
    check(wb::bytes(wb::draft()) == adopted, "before-comparison mode cannot adopt a candidate");
    check(model::undo(wb::draft()) && wb::bytes(wb::draft()) == before && !wb::anyDirty(),
          "undoing the only change returns the draft to clean state");
}

void testPickerGestureCallbacks() {
    Fixture f;
    wb::select("accent");
    f.compose();
    const auto before = wb::bytes(wb::draft());
    auto* hit = f.element("themeWorkbench.color.picker.0.hit");
    if (!hit || !hit->onPress || !hit->onDrag || !hit->onRelease) return;
    const core::Rect frame{hit->frame.x, hit->frame.y, hit->frame.width, hit->frame.height};
    core::PointerEvent press;
    press.button = core::PointerButton::Left;
    press.x = frame.x + frame.width * .2f;
    press.y = frame.y + frame.height * .5f;
    hit->onPress(press, frame);
    for (int step = 0; step < 140; ++step) {
        core::dsl::DragEvent drag;
        drag.button = core::PointerButton::Left;
        drag.x = frame.x + frame.width * (.2f + .6f * step / 139.0f);
        drag.y = press.y;
        hit->onDrag(drag);
    }
    hit->onRelease(press, frame);
    const auto after = wb::bytes(wb::draft());
    check(after != before && wb::draft().undoStack.size() == 1 &&
              model::isLocked(wb::draft(), true, "accent") && !wb::session().dragging,
          "real picker callbacks group 140 drag updates into one edit and release capture state");
    check(model::undo(wb::draft()) && wb::bytes(wb::draft()) == before,
          "picker gesture undo restores color and automatic lock together");
    check(model::redo(wb::draft()) && wb::bytes(wb::draft()) == after,
          "picker gesture redo restores the final edited state");
}

void testAllDraftsCloseProtection() {
    Fixture f;
    model::setColor(wb::draft(), true, "heading", neo::rgba(.7f, .2f, .3f));
    const auto dirtyFirst = wb::bytes(wb::draft());
    auto& s = wb::session();
    s.drafts.push_back(model::makeDraft(neo::ThemeFileData{}));
    s.active = 1;
    s.cleanState[1] = wb::bytes(wb::draft());
    wb::prepareDraft();
    check(!wb::dirty(1) && wb::dirty(0), "fixture has an inactive dirty draft and an active clean draft");
    check(!neo::allowThemeWorkbenchWindowClose(f.state) && s.open && s.closeDialog && s.windowClosePending,
          "window close checks inactive drafts instead of silently losing them");
    neo::handleThemeWorkbenchEscape(f.state);
    check(!s.closeDialog && !s.windowClosePending && s.open && wb::bytes(s.drafts[0]) == dirtyFirst,
          "Escape cancels application close without modifying any draft");
    neo::requestThemeWorkbenchClose(f.state);
    check(s.closeDialog && !s.windowClosePending, "returning to the library uses the distinct leave confirmation");
    wb::leave(f.state);
    check(!s.open && s.drafts.size() == 2 && wb::bytes(s.drafts[0]) == dirtyFirst,
          "leaving the workbench retains every session draft");
    check(!neo::allowThemeWorkbenchWindowClose(f.state) && s.open && s.windowClosePending,
          "retained drafts remain protected after returning to the library");
    const auto closeCount = closeRequests;
    wb::finishWindowClose();
    check(closeRequests == closeCount + 1 && !s.open && neo::allowThemeWorkbenchWindowClose(f.state),
          "explicit close approval hides the workbench for ordinary document confirmation");
    check(!s.windowCloseApproved && !neo::allowThemeWorkbenchWindowClose(f.state),
          "theme close approval is consumed and cannot authorize a later independent close");
}

std::size_t fileCount(const fs::path& directory) {
    std::size_t count = 0;
    if (fs::exists(directory)) for (const auto& item : fs::directory_iterator(directory))
        if (item.is_regular_file() && item.path().extension() == ".json") ++count;
    return count;
}

void testSaveAndImport(const fs::path& root) {
    Fixture f;
    auto& d = wb::draft();
    d.current.name = "Headless workspace theme";
    model::setColor(d, true, "heading", neo::rgba(.7f, .2f, .3f));
    const auto original = model::colors(d, true, true).heading;
    const auto themes = neo::textfile::pathFromUtf8(neo::settings::configDirectory()) / "themes";
    const auto countBefore = fileCount(themes);
    check(wb::save(f.state, false) && !wb::anyDirty() && fileCount(themes) == countBefore + 1,
          "saving a draft creates one file and records clean content");
    const auto savedPath = d.savedPath;
    check(wb::save(f.state, true) && fileCount(themes) == countBefore + 1 &&
              f.state.themeFile == savedPath && settingsWrites > 0,
          "applying an unchanged saved draft reuses the same file");
    neo::ThemeFileData loaded;
    std::string error;
    check(neo::themeloader::read(savedPath, loaded, error), "saved UI draft is readable by the startup loader");
    const auto reopened = model::makeDraft(loaded, savedPath);
    check(model::sameColor(model::colors(reopened, true, true).heading, original) &&
              model::isLocked(reopened, true, "heading"),
          "save/read preserves the original restore color and manual lock");

    const std::string external = "{\"name\":\"Externally changed\"}";
    { std::ofstream out(neo::textfile::pathFromUtf8(savedPath), std::ios::binary); out << external; }
    check(wb::save(f.state, true) && d.savedPath != savedPath && fileCount(themes) == countBefore + 2,
          "an externally changed receipt file is never reused as the current draft");
    check(neo::textfile::load(savedPath).document.text == external,
          "saving again preserves the external file byte for byte");

    const auto before = wb::bytes(d);
    const auto count = wb::session().drafts.size();
    const auto toasts = toastRequests;
    check(!wb::reviewFile(f.state, neo::textfile::pathToUtf8(root / "missing.css")) &&
              toastRequests == toasts + 1 && wb::bytes(wb::draft()) == before &&
              wb::session().drafts.size() == count,
          "failed import surfaces an error and preserves the existing draft");
    const auto css = root / "partial.css";
    { std::ofstream out(css); out << ".theme-light { --text-normal: #112233; }"; }
    check(wb::reviewFile(f.state, neo::textfile::pathToUtf8(css)) && wb::session().reviewingImport &&
              wb::session().drafts.size() == count && wb::bytes(wb::draft()) == before,
          "successful import review does not create or replace a draft before confirmation");
    neo::handleThemeWorkbenchEscape(f.state);
    check(!wb::session().reviewingImport && wb::bytes(wb::draft()) == before,
          "cancelling import review preserves the exact prior draft");
    check(wb::reviewFile(f.state, neo::textfile::pathToUtf8(css)), "review can reopen after cancellation");
    f.compose();
    auto* confirm = f.element("themeWorkbench.report.confirm.bg");
    if (confirm && confirm->onClick) confirm->onClick();
    check(!wb::session().reviewingImport && wb::session().drafts.size() == count + 1 &&
              wb::bytes(wb::session().drafts[0]) == before && wb::dirty(wb::session().active),
          "confirming import creates an unsaved independent draft and preserves the prior draft");
}

void testSaveAllDraftsBeforeClosing() {
    Fixture f;
    model::setColor(wb::draft(), true, "heading", neo::rgba(.6f, .2f, .3f));
    auto& s = wb::session();
    s.drafts.push_back(model::makeDraft(neo::ThemeFileData{}));
    s.active = 1;
    wb::prepareDraft();
    check(!neo::allowThemeWorkbenchWindowClose(f.state), "two dirty drafts delay application close");
    const int beforeClose = closeRequests;
    for (int remaining = 1; remaining >= 0; --remaining) {
        f.compose();
        auto* chooseSave = f.element("themeWorkbench.close.save.bg");
        if (!chooseSave || !chooseSave->onClick) return;
        chooseSave->onClick();
        check(s.saveDialog && wb::dirty(s.active), "close dialog chooses a dirty draft for saving");
        f.compose();
        auto* saveOnly = f.element("themeWorkbench.save.only.bg");
        if (!saveOnly || !saveOnly->onClick) return;
        saveOnly->onClick();
        if (remaining)
            check(s.closeDialog && !s.saveDialog && s.windowClosePending && closeRequests == beforeClose,
                  "saving one of two drafts returns to the close decision without closing the app");
    }
    check(!wb::anyDirty() && !s.open && closeRequests == beforeClose + 1,
          "saving the last dirty draft requests ordinary document close exactly once");
}

void testTypographySaveRange() {
    Fixture f;
    wb::session().layer = wb::Layer::Type;
    f.compose();
    auto* plus = f.element("themeWorkbench.type.field.6.plus.bg");
    if (!plus || !plus->onClick) return;
    for (int step = 0; step < 40; ++step) plus->onClick();
    const auto value = wb::draft().current.light.typography.codeSizeFactor;
    check(value && *value <= 2 && !wb::draft().current.dark.typography.codeSizeFactor,
          "code-size controls stay within the file-format range and leave the other side unchanged");
    check(wb::save(f.state, false), "maximum UI code size saves successfully");
    neo::ThemeFileData loaded;
    std::string error;
    check(neo::themeloader::read(wb::draft().savedPath, loaded, error) &&
              loaded.light.typography.codeSizeFactor == value,
          "maximum UI code size survives loading without rejection or fallback");
}

void testSupportedLayoutBounds() {
    Fixture f;
    const auto right = [](const core::dsl::Element* e) { return e->frame.x + e->frame.width; };
    const auto bottom = [](const core::dsl::Element* e) { return e->frame.y + e->frame.height; };
    for (const float font : {12.0f, 14.0f, 18.0f}) {
        f.state.uiFontSize = font;
        for (const auto screen : {eui::Screen{608, 384}, eui::Screen{896, 608}, eui::Screen{1280, 800}}) {
            f.compose(screen.width, screen.height);
            const auto* preview = f.element("themeWorkbench.preview.viewport");
            const auto* panel = f.element("themeWorkbench.panel.surface");
            const auto* footer = f.element("themeWorkbench.footer");
            const auto* sidebar = f.element("themeWorkbench.preview.sidebar.viewport");
            const auto* status = f.element("themeWorkbench.preview.status");
            const auto* document = f.element("themeWorkbench.preview.document");
            const auto* create = f.element("themeWorkbench.preview.new");
            if (!preview || !panel || !footer || !sidebar || !status || !document || !create) return;
            check(preview->clip && right(preview) <= panel->frame.x && bottom(preview) <= footer->frame.y,
                  "the preview stays clipped within its own left pane at supported sizes and fonts");
            check(sidebar->clip && bottom(sidebar) <= status->frame.y + .1f &&
                      document->clip && bottom(document) <= status->frame.y + .1f,
                  "sample sidebar and document cannot draw or accept input over the status bar");
            for (const auto* id : {"themeWorkbench.preview.tab", "themeWorkbench.preview.tool", "themeWorkbench.preview.pressed"})
                if (const auto* button = f.ui.find(id))
                    check(right(button) + 4 <= create->frame.x, "sample toolbar controls do not overlap the new-note button");
            if (screen.height == 384)
                check(sidebar->scrollMaxOffset > 0 && static_cast<bool>(sidebar->onScrollOffsetChanged),
                      "all five sample files remain reachable by scrolling in a short window");

            wb::openSave();
            f.compose(screen.width, screen.height);
            const auto* card = f.element("themeWorkbench.dialog.card");
            const auto* save = f.element("themeWorkbench.save.apply.bg");
            if (!card || !save) return;
            check(card->frame.x >= 0 && card->frame.y >= 0 && right(card) <= screen.width && bottom(card) <= screen.height &&
                      save->frame.x >= card->frame.x && right(save) <= right(card) && bottom(save) <= bottom(card),
                  "save dialog and its primary action remain inside the supported window");
            neo::handleThemeWorkbenchEscape(f.state);
        }
    }
}

void testDynamicFieldTranslations() {
    for (const auto* language : {"zh-CN", "en"}) {
        neo::i18n::initialize(language);
        for (const auto& field : model::fields()) {
            check(wb::fieldLabel(field.name) != std::string("theme_wb.field.") + field.name,
                  "every editable color has a translated label");
            check(wb::fieldAffects(field.name) != std::string("theme_wb.affect.") + field.name,
                  "every editable color has a translated explanation");
        }
        for (int group = 0; group < 4; ++group)
            check(std::string(wb::groupLabel(group)).rfind("theme_wb.", 0) != 0, "every group has a translated label");
    }
    neo::i18n::initialize("zh-CN");
}
}

int main() {
    const auto root = fs::temp_directory_path() / ("eui_theme_workbench_ui_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(root)) return 1;
#if defined(_WIN32)
    _wputenv_s(L"APPDATA", root.wstring().c_str());
#else
    setenv("XDG_CONFIG_HOME", root.string().c_str(), 1);
#endif
    neo::i18n::initialize("zh-CN");
    testCandidateCallbacks();
    testPickerGestureCallbacks();
    testAllDraftsCloseProtection();
    testSaveAndImport(root);
    testSaveAllDraftsBeforeClosing();
    testTypographySaveRange();
    testSupportedLayoutBounds();
    testDynamicFieldTranslations();
    neo::themeloader::reset();
    wb::session() = wb::Session{};
    std::error_code error;
    fs::remove_all(root, error);
    if (!failures) std::cout << "theme_workbench_ui: headless DSL and callback checks PASS (no native window)\n";
    return failures ? 1 : 0;
}
