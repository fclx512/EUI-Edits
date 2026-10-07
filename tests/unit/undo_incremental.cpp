// T3 增量撤销 + T11 组件级 loadDocument 的定向测试（无头、纯文本，不碰排版/渲染）。
//
// 判据分组：
//   ① 随机编辑序列 vs **朴素全文快照 oracle**：插入 / 删除 / 选择替换 / 格式 span
//      （applyInlineFormat 的两点修改）/ undo-redo 交错，逐步逐字段比对；
//   ② 1MB 文档 × 200 次单字符编辑：撤销历史持有的文本字节 < 2× 文档（只读统计
//      InputModel::undoHistoryBytes），且层数不越过 kMaxUndoDepth；
//   ③ 切文档（loadDocument）之后 Ctrl+Z 不恢复上一篇；
//   ④ loadDocument 与旧 resetEditorInputState 逐字段等价（旧实现原样复刻在本文件里）；
//   ⑤ same/no-op 不入栈、新编辑清 redo、深度淘汰最老、undo/redo 自己不生记录、
//      textRevision 每次实际改动恰好 bump 一次。
//   ⑥ 组合 / 分散改动只出一条记录（整篇捕获、多行标题前缀、单字节任务翻转）。
//   ⑦ abortEdit 的漏 endEdit 兜底：同长度替换 / 追加都要认出来并补 bump，
//      no-op 一律不 bump，兜底之后的新编辑是独立的一条记录。
//   ⑧ applyEditorCommand 的回写判据：revision 快路径 + abort / 长度差兜底
//      （纯光标命令不因无谓的全文比较而回写）。
//
// 构建：随 tests/unit/*.cpp 的 GLOB 自动接线（label = unit），需要 apps/neo_editor
// 的 include 目录才能看到 state/app_state.h（见 CMakeLists.txt 的最小接线块）。

#include "components/input.h"
#include "state/app_state.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <vector>

// 单测的链接缝：本目标没编入 model/settings.cpp（CMakeLists 只给 settings_atomic /
// theme_loader 两个目标接了它），而 applyEditorCommand 的公共尾巴会经 maybeWriteRecovery
// 引到 settings::writeRecovery。这里给一个"什么都不写"的定义满足链接 —— 测试里再用
// AppState::lastRecoveryWrite 的 1.5s 节流挡板双保险，单测一次都不会落盘。
namespace neo::settings {
bool writeRecovery(const std::string&, const std::string&, const textfile::Document*) { return true; }
}  // namespace neo::settings

namespace app {
void requestUpdate() {}
} // namespace app


namespace {

using Model = components::input_detail::InputModel;
using State = Model::InputState;

int gFailures = 0;
int gChecks = 0;

void check(bool ok, const std::string& what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

int pick(std::mt19937& rng, int count) {
    return static_cast<int>(rng() % static_cast<unsigned int>(count));
}

// 随机位置必须落在 UTF-8 边界上：undo/redo 恢复光标时会 clampUtf8Boundary，
// 边界上的值夹逼是恒等变换，oracle 与实现才可逐字段比对。
int randomPosition(std::mt19937& rng, const std::string& text) {
    if (text.empty()) {
        return 0;
    }
    return Model::clampUtf8Boundary(text, pick(rng, static_cast<int>(text.size()) + 1));
}

// ── 朴素 oracle ─────────────────────────────────────────────────────────────
// 全文快照 + 与改造前 EditSnapshot 完全同口径的撤销/重做：每条编辑前存一份全文，
// undo 把它整份放回、redo 反向。它不知道增量记录的存在，因此能独立证伪实现。
struct OracleSnapshot {
    std::string text;
    int cursor = 0;
    int selectionStart = 0;
    int selectionEnd = 0;
};

struct Oracle {
    std::string text;
    int cursor = 0;
    int selectionStart = 0;
    int selectionEnd = 0;
    std::vector<OracleSnapshot> undo;
    std::vector<OracleSnapshot> redo;
    OracleSnapshot pending;
    std::size_t pushed = 0;     // 历史上共入账多少条
    std::size_t discarded = 0;  // 深度淘汰 + 新编辑清 redo 丢掉的条数（历史是否还线性）

    OracleSnapshot capture() const { return OracleSnapshot{text, cursor, selectionStart, selectionEnd}; }

    void noteBefore() {
        pending = capture();
    }

    void noteAfter() {
        const OracleSnapshot after = capture();
        if (after.text == pending.text) {
            return;  // no-op 不入栈
        }
        undo.push_back(pending);
        if (undo.size() > Model::kMaxUndoDepth) {
            undo.erase(undo.begin());  // 淘汰最老
            ++discarded;
        }
        ++pushed;
        discarded += redo.size();
        redo.clear();
    }

    void applyUndo() {
        redo.push_back(capture());
        const OracleSnapshot snapshot = undo.back();
        undo.pop_back();
        text = snapshot.text;
        cursor = snapshot.cursor;
        selectionStart = snapshot.selectionStart;
        selectionEnd = snapshot.selectionEnd;
    }

    void applyRedo() {
        undo.push_back(capture());
        const OracleSnapshot snapshot = redo.back();
        redo.pop_back();
        text = snapshot.text;
        cursor = snapshot.cursor;
        selectionStart = snapshot.selectionStart;
        selectionEnd = snapshot.selectionEnd;
    }
};

void compare(const State& actual, const Oracle& oracle, const std::string& tag) {
    std::string message = tag + ": ";
    bool ok = true;
    if (actual.text != oracle.text) {
        ok = false;
        message += "text(impl=" + std::to_string(actual.text.size()) + "B oracle=" +
                   std::to_string(oracle.text.size()) + "B) ";
    }
    if (actual.cursor != oracle.cursor) {
        ok = false;
        message += "cursor(" + std::to_string(actual.cursor) + " vs " + std::to_string(oracle.cursor) + ") ";
    }
    if (actual.selectionStart != oracle.selectionStart || actual.selectionEnd != oracle.selectionEnd) {
        ok = false;
        message += "selection(" + std::to_string(actual.selectionStart) + "," +
                   std::to_string(actual.selectionEnd) + " vs " + std::to_string(oracle.selectionStart) + "," +
                   std::to_string(oracle.selectionEnd) + ") ";
    }
    if (actual.undoStack.size() != oracle.undo.size()) {
        ok = false;
        message += "undoDepth(" + std::to_string(actual.undoStack.size()) + " vs " +
                   std::to_string(oracle.undo.size()) + ") ";
    }
    if (actual.redoStack.size() != oracle.redo.size()) {
        ok = false;
        message += "redoDepth(" + std::to_string(actual.redoStack.size()) + " vs " +
                   std::to_string(oracle.redo.size()) + ") ";
    }
    if (!ok) {
        std::cerr << "FAIL: " << message << "\n";
    }
    ++gChecks;
    if (!ok) {
        ++gFailures;
    }
}

// 与 applyInlineFormat 等价、但用**字符串拼接**独立实现的镜像（oracle 侧）。
void oracleInlineFormat(Oracle& oracle, const std::string& marker) {
    const int beg = std::min(oracle.selectionStart, oracle.selectionEnd);
    const int end = std::max(oracle.selectionStart, oracle.selectionEnd);
    const int size = static_cast<int>(marker.size());
    const bool wrapped =
        beg >= size && end + size <= static_cast<int>(oracle.text.size()) &&
        oracle.text.compare(static_cast<std::size_t>(beg - size), static_cast<std::size_t>(size), marker) == 0 &&
        oracle.text.compare(static_cast<std::size_t>(end), static_cast<std::size_t>(size), marker) == 0;
    int newBeg = beg;
    int newEnd = end;
    if (wrapped) {
        oracle.text = oracle.text.substr(0, static_cast<std::size_t>(beg - size)) +
                      oracle.text.substr(static_cast<std::size_t>(beg), static_cast<std::size_t>(end - beg)) +
                      oracle.text.substr(static_cast<std::size_t>(end + size));
        newBeg = beg - size;
        newEnd = end - size;
    } else {
        oracle.text = oracle.text.substr(0, static_cast<std::size_t>(beg)) + marker +
                      oracle.text.substr(static_cast<std::size_t>(beg), static_cast<std::size_t>(end - beg)) +
                      marker + oracle.text.substr(static_cast<std::size_t>(end));
        newBeg = beg + size;
        newEnd = end + size;
    }
    oracle.selectionStart = newBeg;
    oracle.selectionEnd = newEnd;
    oracle.cursor = newEnd;
}

// 把实现侧的光标/选区同步给 oracle（光标移动不入撤销历史，两侧必须同源）。
void syncCursor(State& actual, Oracle& oracle) {
    oracle.cursor = actual.cursor;
    oracle.selectionStart = actual.selectionStart;
    oracle.selectionEnd = actual.selectionEnd;
}

void randomizeSelection(State& actual, Oracle& oracle, std::mt19937& rng) {
    const int a = randomPosition(rng, actual.text);
    const int b = randomPosition(rng, actual.text);
    const bool forward = (rng() & 1u) != 0u;
    actual.selectionStart = forward ? a : b;
    actual.selectionEnd = forward ? b : a;
    actual.cursor = actual.selectionEnd;
    syncCursor(actual, oracle);
}

// ── ④ 旧 resetEditorInputState 的原样复刻（改造前的逐字段实现）──────────────
void legacyResetEditorInputState(State& input, const std::string& text) {
    input.text = text;
    ++input.textRevision;
    input.cursor = 0;
    input.selectionStart = 0;
    input.selectionEnd = 0;
    input.dragAnchor = 0;
    input.selecting = false;
    input.hasPreferredCursorX = false;
    input.followCaret = true;
    input.preferredCursorX = 0.0f;
    input.horizontalScroll = 0.0f;
    input.verticalScroll = 0.0f;
    input.compositionText.clear();
    input.preedit.reset();
    input.undoStack.clear();
    input.redoStack.clear();
    input.layoutCacheValid = false;
    input.cachedTextRevision = static_cast<unsigned long long>(-1);
    input.cachedLayoutText.clear();
    input.decorations.clear();
    ++input.decorationRevision;
}

// 把 InputState 的每个字段都拧到"非默认"，用来逐字段比对两份实现。
void dirtyState(State& input, const std::string& text) {
    input.text = text;
    input.compositionText = "拼音";
    input.cursor = 7;
    input.selectionStart = 2;
    input.selectionEnd = 9;
    input.dragAnchor = 2;
    input.selecting = true;
    input.cursorBeforePress = 4;
    input.hasPreferredCursorX = true;
    input.followCaret = false;
    input.preferredCursorX = 123.5f;
    input.horizontalScroll = 40.0f;
    input.verticalScroll = 88.0f;
    input.scrollbarDragOffset = 11.0f;
    input.scrollbarDragScale = 2.5f;
    input.textRevision = 41;
    input.compositionRevision = 7;
    input.lastBounds = core::Rect{1.0f, 2.0f, 3.0f, 4.0f};
    input.cachedTextRevision = 5;
    input.cachedFontFamily = "Consolas";
    input.cachedFontSize = 13.0f;
    input.cachedViewportWidth = 500.0f;
    input.cachedMultiline = true;
    input.cachedLayoutText = "旧布局文本";
    input.cachedTextWidth = 321.0f;
    input.layoutCacheValid = true;
    input.decorations.push_back(components::input_detail::LineDecoration{});
    input.decorationRevision = 3;
    input.pointerHoverValid = true;
    input.pointerHoverY = 66.0f;
    input.pointerHoverLine = 3;
    input.caretBlinkVisible = false;
    input.caretBlinkPendingReset = true;
    input.caretBlinkInitialized = true;
    input.caretBlinkCursor = 7;
    input.caretBlinkSelectionStart = 2;
    input.caretBlinkSelectionEnd = 9;
    input.caretBlinkTextRevision = 41;
    input.caretBlinkCompositionRevision = 7;
    // 一段撤销历史 + 一个未提交的捕获，验证 loadDocument 会把新结构一起清掉。
    input.undoStack.push_back(Model::EditRecord{});
    input.undoStack.back().beg = 3;
    input.undoStack.back().removed = "ab";
    input.undoStack.back().inserted = "cd";
    input.redoStack.push_back(Model::EditRecord{});
    input.editDepth = 1;
    input.editText = "残留捕获";
    input.editBeg = 1;
    input.editEnd = 5;
    input.editFullSize = 99;
    input.editCursorBefore = 6;
    input.editSelectionStartBefore = 1;
    input.editSelectionEndBefore = 8;
    input.preedit = std::make_unique<State>();
    input.preedit->text = "预编辑副本";
}

std::string describeScalars(const State& input) {
    std::string out;
    const auto add = [&out](const char* name, const std::string& value) {
        out += name;
        out += '=';
        out += value;
        out += ';';
    };
    add("text", input.text);
    add("cursor", std::to_string(input.cursor));
    add("selStart", std::to_string(input.selectionStart));
    add("selEnd", std::to_string(input.selectionEnd));
    add("dragAnchor", std::to_string(input.dragAnchor));
    add("selecting", input.selecting ? "1" : "0");
    add("cursorBeforePress", std::to_string(input.cursorBeforePress));
    add("hasPreferredCursorX", input.hasPreferredCursorX ? "1" : "0");
    add("followCaret", input.followCaret ? "1" : "0");
    add("preferredCursorX", std::to_string(input.preferredCursorX));
    add("horizontalScroll", std::to_string(input.horizontalScroll));
    add("verticalScroll", std::to_string(input.verticalScroll));
    add("scrollbarDragOffset", std::to_string(input.scrollbarDragOffset));
    add("scrollbarDragScale", std::to_string(input.scrollbarDragScale));
    add("textRevision", std::to_string(input.textRevision));
    add("compositionRevision", std::to_string(input.compositionRevision));
    add("lastBounds", std::to_string(input.lastBounds.x) + "," + std::to_string(input.lastBounds.y) + "," +
                          std::to_string(input.lastBounds.width) + "," + std::to_string(input.lastBounds.height));
    add("cachedTextRevision", std::to_string(input.cachedTextRevision));
    add("cachedFontFamily", input.cachedFontFamily);
    add("cachedFontSize", std::to_string(input.cachedFontSize));
    add("cachedViewportWidth", std::to_string(input.cachedViewportWidth));
    add("cachedMultiline", input.cachedMultiline ? "1" : "0");
    add("cachedLayoutText", input.cachedLayoutText);
    add("cachedTextWidth", std::to_string(input.cachedTextWidth));
    add("layoutCacheValid", input.layoutCacheValid ? "1" : "0");
    add("decorations", std::to_string(input.decorations.size()));
    add("decorationRevision", std::to_string(input.decorationRevision));
    add("cachedLines", std::to_string(input.cachedLines.size()));
    add("cachedTables", std::to_string(input.cachedTables.size()));
    add("cachedMetricsWidth", std::to_string(input.cachedMetrics.width));
    add("pointerHoverValid", input.pointerHoverValid ? "1" : "0");
    add("pointerHoverY", std::to_string(input.pointerHoverY));
    add("pointerHoverLine", std::to_string(input.pointerHoverLine));
    add("caretBlinkVisible", input.caretBlinkVisible ? "1" : "0");
    add("caretBlinkPendingReset", input.caretBlinkPendingReset ? "1" : "0");
    add("caretBlinkInitialized", input.caretBlinkInitialized ? "1" : "0");
    add("caretBlinkCursor", std::to_string(input.caretBlinkCursor));
    add("caretBlinkSelectionStart", std::to_string(input.caretBlinkSelectionStart));
    add("caretBlinkSelectionEnd", std::to_string(input.caretBlinkSelectionEnd));
    add("caretBlinkTextRevision", std::to_string(input.caretBlinkTextRevision));
    add("caretBlinkCompositionRevision", std::to_string(input.caretBlinkCompositionRevision));
    add("undoDepth", std::to_string(input.undoStack.size()));
    add("redoDepth", std::to_string(input.redoStack.size()));
    add("preedit", input.preedit ? "1" : "0");
    return out;
}

std::string describeCapture(const State& input) {
    std::string out;
    out += "editDepth=" + std::to_string(input.editDepth);
    out += ";editTextSize=" + std::to_string(input.editText.size());
    out += ";editBeg=" + std::to_string(input.editBeg);
    out += ";editEnd=" + std::to_string(input.editEnd);
    out += ";editFullSize=" + std::to_string(input.editFullSize);
    out += ";editCursorBefore=" + std::to_string(input.editCursorBefore);
    out += ";editSelectionStartBefore=" + std::to_string(input.editSelectionStartBefore);
    out += ";editSelectionEndBefore=" + std::to_string(input.editSelectionEndBefore);
    return out;
}

std::string randomText(std::mt19937& rng) {
    static const char* kPieces[] = {"alpha ", "beta\n", "中文段落", "🙂", " gamma", "\n", "**粗**", "x"};
    std::string text;
    const int count = 6 + pick(rng, 40);
    for (int i = 0; i < count; ++i) {
        text += kPieces[pick(rng, static_cast<int>(sizeof(kPieces) / sizeof(kPieces[0])))];
    }
    return text;
}

// ── ① 随机编辑序列 vs 朴素全文快照 oracle ───────────────────────────────────
// allowUndoRedo = false 的序列不丢任何记录（既不淘汰也不清 redo），因此撤销到底
// 必须精确回到起点；带 undo/redo 交错的序列则以"与 oracle 逐字段一致"为准。
void runSequence(unsigned seed, int steps, bool allowUndoRedo, const std::string& label) {
    std::mt19937 rng(seed);
    State state;
    Oracle oracle;
    const std::string startText = randomText(rng);
    state.text = startText;
    oracle.text = state.text;
    state.cursor = Model::clampUtf8Boundary(state.text, static_cast<int>(state.text.size()) / 2);
    Model::clearSelection(state);
    syncCursor(state, oracle);

    static const char* kMarkers[] = {"**", "*", "`", "=="};
    static const char* kInserts[] = {"a", "中文\n", "🙂z", "lorem ipsum "};
    const int opRange = allowUndoRedo ? 14 : 12;

    for (int step = 0; step < steps; ++step) {
        const int op = pick(rng, opRange);
        const std::string tag = label + " step " + std::to_string(step);
        if (op <= 4) {
            // 插入（无选区）。
            const std::string value = kInserts[pick(rng, 4)];
            state.cursor = randomPosition(rng, state.text);
            Model::clearSelection(state);
            syncCursor(state, oracle);
            oracle.noteBefore();
            Model::insertAtCursor(state, value);
            const int at = std::min(oracle.cursor, static_cast<int>(oracle.text.size()));
            oracle.text.insert(static_cast<std::size_t>(at), value);
            oracle.cursor = at + static_cast<int>(value.size());
            oracle.selectionStart = oracle.cursor;
            oracle.selectionEnd = oracle.cursor;
            oracle.noteAfter();
        } else if (op <= 7) {
            // 删除选区（覆盖"删除"与"选择替换"里的删字部分）。
            randomizeSelection(state, oracle, rng);
            if (state.selectionStart == state.selectionEnd) {
                continue;
            }
            const int a = std::min(oracle.selectionStart, oracle.selectionEnd);
            const int b = std::max(oracle.selectionStart, oracle.selectionEnd);
            oracle.noteBefore();
            Model::eraseSelection(state);
            oracle.text.erase(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a));
            oracle.cursor = a;
            oracle.selectionStart = a;
            oracle.selectionEnd = a;
            oracle.noteAfter();
        } else if (op <= 9) {
            // 选择替换（有选区的 insertAtCursor）。
            randomizeSelection(state, oracle, rng);
            const std::string value = kInserts[pick(rng, 4)];
            const int a = std::min(oracle.selectionStart, oracle.selectionEnd);
            const int b = std::max(oracle.selectionStart, oracle.selectionEnd);
            oracle.noteBefore();
            Model::insertAtCursor(state, value);
            oracle.text.replace(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a), value);
            oracle.cursor = a + static_cast<int>(value.size());
            oracle.selectionStart = oracle.cursor;
            oracle.selectionEnd = oracle.cursor;
            oracle.noteAfter();
        } else if (op <= 11) {
            // 格式 span：两点修改（包住 / 解开）由 applyInlineFormat 自己记账。
            randomizeSelection(state, oracle, rng);
            const std::string marker = kMarkers[pick(rng, 4)];
            oracle.noteBefore();
            neo::applyInlineFormat(state, marker.c_str());
            oracleInlineFormat(oracle, marker);
            oracle.noteAfter();
        } else if (op == 12) {
            // undo / redo 交错。
            const bool canUndo = !oracle.undo.empty();
            const bool didUndo = Model::undoEdit(state);
            check(didUndo == canUndo, tag + ": undo 可用性与 oracle 不一致");
            if (didUndo) {
                oracle.applyUndo();
                compare(state, oracle, tag + " after undo");
            }
        } else {
            const bool canRedo = !oracle.redo.empty();
            const bool didRedo = Model::redoEdit(state);
            check(didRedo == canRedo, tag + ": redo 可用性与 oracle 不一致");
            if (didRedo) {
                oracle.applyRedo();
                compare(state, oracle, tag + " after redo");
            }
        }
        compare(state, oracle, tag);
        check(state.editDepth == 0, tag + ": 捕获未提交（editDepth 残留）");
        check(state.editText.empty(), tag + ": 捕获文本未释放");
    }

    // 收尾：全部撤销。层数上限淘汰 + "新编辑清 redo"会让历史不再线性（设计内行为），
    // 所以只有在一条记录都没被丢掉时才要求精确回到起点；否则与 oracle 逐字段一致即为通过。
    while (!oracle.undo.empty()) {
        check(Model::undoEdit(state), label + ": 收尾撤销应成功");
        oracle.applyUndo();
    }
    compare(state, oracle, label + " 收尾撤销");
    if (oracle.discarded == 0) {
        check(state.text == startText, label + ": 未发生淘汰时，撤销到底应回到起点文本");
    } else {
        std::cout << label << ": " << oracle.pushed << " records pushed, " << oracle.discarded
                  << " discarded (depth cap / redo reset) — 收尾按 oracle 对齐\n";
    }
    check(state.editDepth == 0 && state.editText.empty(), label + ": 收尾后不应有打开的捕获");
}

void testRandomSequenceAgainstOracle() {
    // 短序列（不淘汰、不清 redo）：撤销到底必须精确回到起点文本。
    runSequence(20260925u, 60, false, "linear");
    // 长序列 + undo/redo 交错：以与朴素全文快照 oracle 逐字段一致为准。
    runSequence(1u, 900, true, "interleaved");
}

// ── ② 1MB 文档 × 200 次单字符编辑的内存 ────────────────────────────────────
void testHistoryMemoryOnOneMegabyteDocument() {
    State big;
    const std::string line = "The quick brown fox jumps over the lazy dog 0123456789 中文行内容与排版测试 🙂\n";
    while (big.text.size() < (1u << 20)) {
        big.text += line;
    }
    ++big.textRevision;
    const std::size_t documentBytes = big.text.size();
    const std::string original = big.text;
    std::string after72Edits;

    std::mt19937 rng(7u);
    for (int i = 0; i < 200; ++i) {
        if (i % 2 == 0) {
            big.cursor = randomPosition(rng, big.text);
            Model::clearSelection(big);
            Model::insertAtCursor(big, "x");
        } else {
            // 找一个纯 ASCII 字节做单字符删除（保证 [p, p+1) 正好是一个字符）。
            int p = randomPosition(rng, big.text);
            while (p < static_cast<int>(big.text.size()) &&
                   static_cast<unsigned char>(big.text[static_cast<std::size_t>(p)]) >= 0x80) {
                ++p;
            }
            if (p >= static_cast<int>(big.text.size())) {
                p = 0;
            }
            big.cursor = p;
            big.selectionStart = p;
            big.selectionEnd = p + 1;
            Model::eraseSelection(big);
        }
        if (i == 71) {
            after72Edits = big.text;  // 保留的 128 条记录正好覆盖第 73..200 次编辑
        }
    }

    const std::size_t historyBytes = Model::undoHistoryBytes(big);
    std::cout << "1MB doc (" << documentBytes << " B): 200 single-char edits, undo history holds "
              << historyBytes << " B in " << big.undoStack.size() << " records\n";
    check(big.undoStack.size() <= Model::kMaxUndoDepth, "撤销层数不得超过 kMaxUndoDepth");
    check(historyBytes < 2 * documentBytes, "撤销历史持有字节必须 < 2× 文档大小");
    check(historyBytes < 4096, "200 次单字符编辑的历史应当只有 KB 量级");

    // 淘汰语义：留下的 128 条恰好是第 73..200 次编辑，全撤销应回到第 72 次编辑后的样子。
    int undos = 0;
    while (Model::undoEdit(big)) {
        ++undos;
    }
    check(undos == static_cast<int>(Model::kMaxUndoDepth), "应能撤销 kMaxUndoDepth 次");
    check(big.text == after72Edits, "淘汰最老后，撤销到底应回到第 72 次编辑后的文本");
    check(big.text.size() != original.size() || big.text != original, "前提自检：200 次编辑确实改过文本");
}

// ── ③ 切文档后 Ctrl+Z 不恢复上一篇 ──────────────────────────────────────────
void testDocumentSwitchClearsHistory() {
    State state;
    Model::loadDocument(state, "第一篇的内容：甲乙丙丁");
    state.cursor = 5;
    Model::clearSelection(state);
    Model::insertAtCursor(state, "（第一篇的编辑）");
    check(!state.undoStack.empty(), "第一篇应有可撤销记录");

    Model::loadDocument(state, "第二篇的内容：戊己庚辛");
    check(state.undoStack.empty() && state.redoStack.empty(), "换文档必须清空撤销/重做");
    check(!Model::undoEdit(state), "换文档后 Ctrl+Z 应无事可做");
    check(state.text == "第二篇的内容：戊己庚辛", "Ctrl+Z 不得恢复上一篇");
    check(!Model::redoEdit(state), "换文档后同样没有重做");

    // 新文档里编辑 → 撤销只回到"第二篇的初始态"，不牵连上一篇。
    state.cursor = 3;
    Model::clearSelection(state);
    Model::insertAtCursor(state, "XX");
    check(Model::undoEdit(state), "新文档的撤销应生效");
    check(state.text == "第二篇的内容：戊己庚辛", "撤销只应退回本篇装载时的文本");
}

// ── ④ loadDocument 与旧 resetEditorInputState 逐字段等价 ────────────────────
void testLoadDocumentMatchesLegacyReset() {
    const std::string payload = "换进来的新文档\n第二行 🙂";
    State legacy;
    State modern;
    dirtyState(legacy, "旧的文档内容");
    dirtyState(modern, "旧的文档内容");
    // 两侧 textRevision / decorationRevision 必须从同一个起点出发（都已 dirty 成 41 / 3）。
    legacy.textRevision = modern.textRevision;
    legacy.decorationRevision = modern.decorationRevision;

    legacyResetEditorInputState(legacy, payload);
    Model::loadDocument(modern, payload);

    check(describeScalars(legacy) == describeScalars(modern),
          "loadDocument 必须与旧 resetEditorInputState 逐字段等价");
    if (describeScalars(legacy) != describeScalars(modern)) {
        std::cerr << "  legacy: " << describeScalars(legacy) << "\n";
        std::cerr << "  modern: " << describeScalars(modern) << "\n";
    }
    // 旧实现不认识的新撤销结构：必须被清干净（否则上一篇的捕获会算到新文档上）。
    check(describeCapture(modern).find("editDepth=0;editTextSize=0") == 0,
          "loadDocument 应清掉未提交的捕获，实际：" + describeCapture(modern));
    // 装载本身不算一次"编辑"：它自己 bump 一次 revision，但不产生撤销记录。
    check(modern.undoStack.empty() && modern.redoStack.empty(), "loadDocument 不得留下撤销记录");
    check(modern.editDepth == 0 && modern.editText.empty(), "loadDocument 后不应有打开的捕获");
    // 悬停 / 光标闪烁按旧口径保持（旧实现不清它们，新实现也不许清）。
    check(modern.pointerHoverValid && modern.pointerHoverLine == 3, "悬停行应与旧实现一样保持");
    check(!modern.caretBlinkVisible && modern.caretBlinkCursor == 7, "光标闪烁签名应与旧实现一样保持");
}

// ── ⑤ no-op / redo 失效 / 深度淘汰 / revision 计数 ──────────────────────────
void testNoOpRedoAndRevisionSemantics() {
    State state;
    state.text = "hello world";
    state.cursor = 5;
    Model::clearSelection(state);

    // no-op：空插入 / 空区间删除都不入栈、不 bump revision。
    const unsigned long long baseRevision = state.textRevision;
    Model::insertAtCursor(state, "");
    Model::eraseRange(state, 4, 4);
    Model::eraseSelection(state);  // 无选区
    check(state.undoStack.empty(), "no-op 不得入栈");
    check(state.textRevision == baseRevision, "no-op 不得 bump textRevision");

    // 真实编辑：一次组合操作（选区擦除 + 插入）只 bump 一次。
    state.selectionStart = 0;
    state.selectionEnd = 5;
    state.cursor = 5;
    Model::insertAtCursor(state, "HI");
    check(state.text == "HI world", "选区替换应生效");
    check(state.textRevision == baseRevision + 1, "一次选区替换只应 bump 一次 textRevision");
    check(state.undoStack.size() == 1, "一次选区替换只应产生一条记录（不许拆成两条）");

    // undo / redo 各 bump 一次，且不产生新记录。
    const std::size_t depthBeforeUndo = state.undoStack.size();
    check(Model::undoEdit(state), "撤销应生效");
    check(state.text == "hello world" && state.cursor == 5, "撤销应回到编辑前");
    check(state.textRevision == baseRevision + 2, "撤销应 bump 一次");
    check(state.undoStack.size() == depthBeforeUndo - 1, "撤销不得产生新撤销记录");
    check(state.redoStack.size() == 1, "撤销应把记录放进重做栈");
    check(Model::redoEdit(state), "重做应生效");
    check(state.text == "HI world", "重做应恢复编辑后");
    check(state.textRevision == baseRevision + 3, "重做应 bump 一次");
    check(state.undoStack.size() == depthBeforeUndo && state.redoStack.empty(), "重做后应回到原栈形");

    // 撤销之后来一次真实编辑 → redo 清空；来一次 no-op → redo 保留。
    check(Model::undoEdit(state), "再撤销一次");
    check(state.redoStack.size() == 1, "此时应有重做记录");
    state.cursor = 6;
    Model::clearSelection(state);
    Model::insertAtCursor(state, "");          // no-op
    check(state.redoStack.size() == 1, "no-op 不得清掉重做链");
    Model::insertAtCursor(state, "!");         // 真实编辑
    check(state.redoStack.empty(), "新的真实编辑必须清掉重做链");

    // 深度淘汰：连做 kMaxUndoDepth + 8 次编辑，只留 kMaxUndoDepth 条。
    State deep;
    deep.text = "";
    for (int i = 0; i < static_cast<int>(Model::kMaxUndoDepth) + 8; ++i) {
        deep.cursor = static_cast<int>(deep.text.size());
        Model::clearSelection(deep);
        Model::insertAtCursor(deep, "n");
    }
    check(deep.undoStack.size() == Model::kMaxUndoDepth, "达到上限应淘汰最老、保持 kMaxUndoDepth 条");
    int undos = 0;
    while (Model::undoEdit(deep)) {
        ++undos;
    }
    check(undos == static_cast<int>(Model::kMaxUndoDepth), "只能撤销被保留的层数");
    check(deep.text == "nnnnnnnn", "淘汰后撤销到底应停在第 8 次编辑之后（8 = 多出来的那 8 条）");
}

// ── ⑥ 组合 / 分散改动只出一条记录（防双 push）──────────────────────────────
void testSingleRecordForCompoundEdits() {
    // 区间未知的整篇捕获：一个"命令"在两处分散改动，收成一条记录、撤销一次全回退。
    State bracketed;
    bracketed.text = "aaa bbb ccc";
    bracketed.cursor = 0;
    Model::clearSelection(bracketed);
    const unsigned long long baseRevision = bracketed.textRevision;
    Model::beginEdit(bracketed);  // 整篇捕获（区间由命令内部决定时的逃生口）
    bracketed.text.replace(0, 3, "AAA");
    bracketed.text.replace(8, 3, "CCC");
    bracketed.cursor = 4;
    Model::endEdit(bracketed);
    check(bracketed.text == "AAA bbb CCC", "分散改动应生效");
    check(bracketed.undoStack.size() == 1, "整篇捕获下的分散改动应只产生一条记录");
    check(bracketed.textRevision == baseRevision + 1, "整篇捕获下的分散改动只应 bump 一次");
    check(Model::undoEdit(bracketed), "撤销应生效");
    check(bracketed.text == "aaa bbb ccc" && bracketed.cursor == 0, "一次撤销应回到命令前");
    check(Model::redoEdit(bracketed), "重做应生效");
    check(bracketed.text == "AAA bbb CCC" && bracketed.cursor == 4, "重做应回到命令后");

    // 多行标题前缀：连续 before span（整段行首到行尾）里的多处编辑收成一条记录。
    State heading;
    heading.text = "甲\n乙\n丙";
    heading.selectionStart = 2;
    heading.selectionEnd = 4;
    heading.cursor = 4;
    check(neo::applyLinePrefix(heading, "# ", true), "多行前缀命令应成功");
    check(heading.text == "# 甲\n# 乙\n丙", "多行前缀应逐行生效，实际 \"" + heading.text + "\"");
    check(heading.undoStack.size() == 1,
          "多行前缀应只产生一条记录（不许逐行一条），实际 " + std::to_string(heading.undoStack.size()) +
              " 条，depth=" + std::to_string(heading.editDepth) + " captured=" +
              std::to_string(heading.editText.size()));
    check(Model::undoEdit(heading), "多行前缀可撤销");
    check(heading.text == "甲\n乙\n丙", "一次撤销应回到命令前的多行文本");

    // 任务复选框翻转：单字节 span，连续两次翻转 = 两条记录，撤销一次回一步。
    State flip;
    flip.text = "- [ ] 待办";
    flip.textRevision = 1;
    check(neo::flipTaskCheckboxAt(flip, 3), "翻转 [ ] 应成功");
    check(flip.text == "- [x] 待办" && flip.undoStack.size() == 1, "单字节翻转应产生一条记录");
    check(flip.textRevision == 2, "单字节翻转只应 bump 一次");
    check(Model::undoEdit(flip) && flip.text == "- [ ] 待办", "翻转可撤销");
}

// ── ⑦ abortEdit 的漏 endEdit 兜底（T3 复审）─────────────────────────────────
// beginEdit 之后文本被改、调用方却漏了 endEdit：捕获必须被**真实文本对照**认出来
// （同长度替换不能只比 size）、补 bump 一次 revision、作废 pendingEdit 与排版判等，
// 但不入撤销栈、不动 redo、不回滚文本；no-op 一律什么都不做；兜底之后的新编辑是
// 独立的一条记录。
void testAbortEditDetectsUncommittedChange() {
    // (a) 整篇捕获 + 同长度替换：只比 size 的实现会把这类改动整个漏掉。
    State same;
    same.text = "hello world";
    same.textRevision = 7;
    same.layoutCacheValid = true;
    same.cachedTextRevision = same.textRevision;  // 布局"有效"，判等只看 revision
    Model::beginEdit(same);
    same.text.replace(6, 5, "WOrld");
    check(same.text.size() == 11, "前提：同长度替换后大小不变");
    check(Model::abortEdit(same), "同长度替换必须被 abortEdit 认出来");
    check(same.textRevision == 8, "认出改动必须补 bump 一次 revision");
    check(same.cachedTextRevision != same.textRevision, "补 bump 必须让排版缓存判等失效");
    check(!same.pendingEdit.valid, "abort 必须作废编辑区间（T4 A1）");
    check(same.undoStack.empty() && same.redoStack.empty(), "abort 不得产生撤销/重做记录");
    check(same.editDepth == 0 && same.editText.empty(), "abort 后捕获必须归零并释放");
    check(same.text == "hello WOrld", "abort 丢的是捕获，不回滚命令已经改出来的字节");
    check(!Model::undoEdit(same), "没有记录就不该撤销出东西来");

    // (b) 区间捕获 + 窗口内的同长度替换。
    State interval;
    interval.text = "aaa bbb ccc";
    interval.textRevision = 3;
    Model::beginEdit(interval, 4, 7);
    interval.text.replace(4, 3, "BBB");
    check(Model::abortEdit(interval), "区间内的同长度替换必须被认出来");
    check(interval.textRevision == 4, "区间内同长度替换只补 bump 一次");

    // (b') 已知触发边界：区间捕获下**窗口之外**的同长度改写认不出来 —— 那违反
    // "这次编辑必须全部落在 [beg,end) 内"的约定。写成断言是把边界钉在纸面上。
    State outside;
    outside.text = "aaa bbb ccc";
    outside.textRevision = 5;
    Model::beginEdit(outside, 4, 7);
    outside.text[9] = 'X';
    check(!Model::abortEdit(outside), "窗口外的同长度改写是文档写明的边界（不认）");
    check(outside.textRevision == 5, "边界情形不得凭空 bump revision");

    // (c) 追加：整篇长度变了 → O(1) 那一层接住（空窗口的"光标处追加"预告也一样）。
    State append;
    append.text = "line\n";
    append.textRevision = 1;
    Model::beginEdit(append, 0, 0);
    append.text += "next";
    check(Model::abortEdit(append), "追加必须被长度差认出来");
    check(append.textRevision == 2, "追加补 bump 一次");
    check(append.text == "line\nnext", "同样不回滚文本");
    check(append.undoStack.empty(), "追加的兜底也不产生撤销记录");

    // (d) no-op：捕获后一字未动 → 不 bump、不动栈；改了又改回去同理。
    State noop;
    noop.text = "hello";
    noop.cursor = 2;
    Model::clearSelection(noop);
    Model::insertAtCursor(noop, "!");
    check(Model::undoEdit(noop), "前置：撤销应生效");
    const unsigned long long revision = noop.textRevision;
    const std::size_t undoDepth = noop.undoStack.size();
    const std::size_t redoDepth = noop.redoStack.size();
    Model::beginEdit(noop);
    check(!Model::abortEdit(noop), "no-op abort 必须报告未改动");
    check(noop.textRevision == revision, "no-op abort 不得 bump revision");
    check(noop.undoStack.size() == undoDepth && noop.redoStack.size() == redoDepth,
          "no-op abort 不得动撤销/重做栈（尤其不许清 redo）");
    Model::beginEdit(noop);
    noop.text.replace(0, 5, "HELLO");
    noop.text.replace(0, 5, "hello");
    check(!Model::abortEdit(noop), "改回原样应按 no-op 处理");
    check(noop.textRevision == revision, "改回原样不得 bump revision");
    check(noop.editDepth == 0 && noop.editText.empty(), "两种 abort 都要把捕获清干净");

    // (e) 兜底之后的新编辑：独立的一条记录，undo/redo 各回一步。
    State next;
    next.text = "abc";
    next.textRevision = 1;
    Model::beginEdit(next);
    next.text = "abd";  // 漏 endEdit 的同长度替换
    check(Model::abortEdit(next), "漏 endEdit 的改动必须被认出");
    check(next.textRevision == 2, "兜底补 bump 一次");
    check(next.undoStack.empty(), "被丢弃的改动不得留下撤销记录");
    next.cursor = 3;
    Model::clearSelection(next);
    Model::insertAtCursor(next, "!");
    check(next.text == "abd!" && next.undoStack.size() == 1, "后续编辑应是独立的一条记录");
    check(next.textRevision == 3, "abort 一次 + 新编辑一次 = bump 两次");
    check(Model::undoEdit(next) && next.text == "abd", "撤销应回到兜底后的文本，不牵连被丢的那次");
    check(Model::redoEdit(next) && next.text == "abd!", "重做应回到新编辑后");
    check(next.textRevision == 5, "undo / redo 各 bump 一次");
}

// ── ⑧ applyEditorCommand 的回写判据（revision 快路径 + 两道 O(1) 兜底）────────
// 模拟"app command 文本同步"：漏 endEdit 的改动、无捕获的裸改都必须让 doc.text
// 跟上组件文本；纯光标命令在没有改动时不许做无谓的回写（也就没有全文比较）。
void testApplyEditorCommandSyncsDivergedText() {
    eui::Ui ui;
    neo::AppState appState;
    // maybeWriteRecovery 的节流挡板：单测不写应急副本文件。
    appState.lastRecoveryWrite = std::chrono::steady_clock::now();
    auto& input = ui.state<State>(neo::kEditorInputId);
    input.text = "- [ ] 待办";
    appState.doc.text = input.text;
    const unsigned long long baseRevision = appState.revision;

    // 场景一：命令开着捕获改了文本却漏了 endEdit，随后来一条纯光标命令（SelectAll
    // 不碰文本）→ abortEdit 认出改动、补 bump，公共尾巴据此回写 doc.text。
    Model::beginEdit(input);
    input.text[3] = 'x';  // 同长度改写，且没有 endEdit
    appState.pendingEditorCommand = neo::EditorCommand::SelectAll;
    neo::applyEditorCommand(ui, appState);
    check(input.text == "- [x] 待办", "组件文本保持命令改出来的样子");
    check(appState.doc.text == input.text, "漏 endEdit 后 doc.text 必须与组件文本同步");
    check(appState.revision == baseRevision + 1, "文本真的变了 → app revision 推进一次");
    check(input.selectionEnd == static_cast<int>(input.text.size()), "SelectAll 本身照常生效");
    check(input.editDepth == 0, "兜底必须把捕获深度收回 0");
    check(input.undoStack.empty(), "失衡的改动不产生撤销记录（最多丢一条）");

    // 场景二：纯光标命令 + 没有任何改动 → 快路径短路，revision 与 doc 都不动。
    const unsigned long long afterAbort = appState.revision;
    appState.pendingEditorCommand = neo::EditorCommand::SelectAll;
    neo::applyEditorCommand(ui, appState);
    check(appState.revision == afterAbort, "无改动的命令不得推进 app revision");
    check(appState.doc.text == input.text, "无改动时两份文本本来就相等");

    // 场景三：连捕获都没有的裸改（长度变了）→ abortEdit 无从得知，靠长度差兜底回写。
    input.text += "！";
    appState.pendingEditorCommand = neo::EditorCommand::SelectAll;
    neo::applyEditorCommand(ui, appState);
    check(appState.doc.text == input.text, "长度差必须触发回写");
    check(appState.revision == afterAbort + 1, "长度差回写同样推进一次 revision");

    // 场景四：正常配对的命令路径一字不变 —— 一次真实编辑、一条记录、一次回写。
    appState.pendingTaskByte = 3;
    appState.pendingEditorCommand = neo::EditorCommand::ToggleTask;
    neo::applyEditorCommand(ui, appState);
    check(input.text == "- [ ] 待办！", "任务翻转应生效");
    check(appState.doc.text == input.text, "正常命令同样回写 doc.text");
    check(appState.revision == afterAbort + 2, "正常路径一次改动推进一次 revision");
    check(input.undoStack.size() == 1, "正常命令产生一条撤销记录");
    check(appState.pendingTaskByte == -1, "ToggleTask 载荷应被消费");
}

void testTaskTogglePreservesViewport() {
    eui::Ui ui;
    neo::AppState appState;
    appState.lastRecoveryWrite = std::chrono::steady_clock::now();
    auto& input = ui.state<State>(neo::kEditorInputId);
    input.text = "- [ ] task\n";
    for (int i = 0; i < 100; ++i) input.text += "body\n";
    appState.doc.text = input.text;
    input.cursor = static_cast<int>(input.text.size());
    Model::clearSelection(input);
    input.dragAnchor = input.cursor;
    input.followCaret = false;
    input.verticalScroll = 40.0f;
    auto build = [&] {
        return Model::InputLayout::build(input, 400.0f, 100.0f, 420.0f,
                                        10.0f, 10.0f, 10.0f, 20.0f,
                                        "monospace", 16.0f, true);
    };
    build();
    const int oldCursor = input.cursor;
    const auto revision = input.textRevision;
    for (const char expected : {'x', ' '}) {
        appState.pendingTaskByte = 3;
        appState.pendingEditorCommand = neo::EditorCommand::ToggleTask;
        neo::applyEditorCommand(ui, appState);
        build();
        check(input.text[3] == expected && appState.doc.text == input.text,
              "task toggle must synchronize text");
        check(input.cursor == oldCursor && input.selectionStart == oldCursor &&
                  input.selectionEnd == oldCursor && input.dragAnchor == oldCursor,
              "task toggle must preserve caret and selection");
        check(!input.followCaret && input.verticalScroll == 40.0f,
              "task toggle and the following layout must preserve a scrolled viewport");
    }
    check(input.textRevision == revision + 2 && input.undoStack.size() == 2,
          "two task toggles must produce two reversible edits");
    appState.pendingEditorCommand = neo::EditorCommand::Undo;
    neo::applyEditorCommand(ui, appState);
    build();
    check(input.text[3] == 'x' && input.followCaret && input.verticalScroll > 40.0f,
          "undo must still follow the restored caret");
    input.followCaret = false;
    appState.pendingEditorCommand = neo::EditorCommand::Redo;
    neo::applyEditorCommand(ui, appState);
    check(input.text[3] == ' ' && input.followCaret,
          "redo must still restore text and enable caret following");
    appState.pendingTaskByte = 3;
    appState.pendingEditorCommand = neo::EditorCommand::ToggleTask;
    neo::applyEditorCommand(ui, appState);
    check(input.followCaret, "task toggle must also preserve enabled caret following");
}

void testImageCommandRejectsPlainDocuments() {
    eui::Ui ui;
    neo::AppState appState;
    appState.lastRecoveryWrite = std::chrono::steady_clock::now();
    auto& input = ui.state<State>(neo::kEditorInputId);
    input.text = "original";
    input.cursor = 8;
    Model::clearSelection(input);
    appState.doc.text = input.text;
    appState.path = "plain.txt";
    const auto revision = input.textRevision;
    appState.pendingImageLink = "![](<D:/image.png>)";
    appState.pendingEditorCommand = neo::EditorCommand::InsertImageLink;
    neo::applyEditorCommand(ui, appState);
    check(input.text == "original" && appState.doc.text == input.text &&
              input.textRevision == revision && input.undoStack.empty(),
          "queued image insertion must not modify a plain document");
    check(appState.toastVisible && appState.pendingImageLink.empty(),
          "rejected image command must show a reason and consume its payload");
    appState.path = "note.md";
    appState.pendingImageLink = "![](<D:/image.png>)";
    appState.pendingEditorCommand = neo::EditorCommand::InsertImageLink;
    neo::applyEditorCommand(ui, appState);
    const std::string imageBlock = "original\n![](<D:/image.png>)\n";
    check(input.text == imageBlock && appState.doc.text == input.text &&
              input.cursor == static_cast<int>(imageBlock.size()),
          "Markdown image insertion must create a standalone block and leave the caret after it");
    appState.pendingEditorCommand = neo::EditorCommand::Undo;
    neo::applyEditorCommand(ui, appState);
    check(input.text == "original" && appState.doc.text == input.text,
          "image reference insertion must undo in one step");
    appState.pendingEditorCommand = neo::EditorCommand::Redo;
    neo::applyEditorCommand(ui, appState);
    check(input.text == imageBlock && appState.doc.text == input.text &&
              input.cursor == static_cast<int>(imageBlock.size()),
          "redo must restore the standalone image block and its post-image caret");
}

void testImageCommandSeparatesParagraphAndReplacesSelection() {
    const std::string link = "![](<D:/image.png>)";
    {
        eui::Ui ui;
        neo::AppState appState;
        appState.lastRecoveryWrite = std::chrono::steady_clock::now();
        auto& input = ui.state<State>(neo::kEditorInputId);
        input.text = "left right";
        input.cursor = 5;
        Model::clearSelection(input);
        appState.doc.text = input.text;
        appState.path = "note.md";
        appState.pendingImageLink = link;
        appState.pendingEditorCommand = neo::EditorCommand::InsertImageLink;
        neo::applyEditorCommand(ui, appState);
        const std::string expected = "left \n" + link + "\n\nright";
        check(input.text == expected && appState.doc.text == expected,
              "image insertion in paragraph text must split it around a standalone block");
        check(input.cursor == static_cast<int>(("left \n" + link + "\n").size()),
              "paragraph split must leave the caret on the blank line after the image");
        appState.pendingEditorCommand = neo::EditorCommand::Undo;
        neo::applyEditorCommand(ui, appState);
        check(input.text == "left right" && appState.doc.text == input.text,
              "paragraph image insertion must remain one undoable edit");
    }
    {
        eui::Ui ui;
        neo::AppState appState;
        appState.lastRecoveryWrite = std::chrono::steady_clock::now();
        auto& input = ui.state<State>(neo::kEditorInputId);
        input.text = "hello replace tail";
        input.cursor = 6;
        input.selectionStart = 6;
        input.selectionEnd = 13;
        appState.doc.text = input.text;
        appState.path = "note.md";
        appState.pendingImageLink = link;
        appState.pendingEditorCommand = neo::EditorCommand::InsertImageLink;
        neo::applyEditorCommand(ui, appState);
        const std::string expected = "hello \n" + link + "\n\n tail";
        check(input.text == expected && appState.doc.text == expected,
              "image insertion must replace only the selected text and keep surrounding text");
        check(input.cursor == static_cast<int>(("hello \n" + link + "\n").size()),
              "selection replacement must leave the caret after the image block");
        appState.pendingEditorCommand = neo::EditorCommand::Undo;
        neo::applyEditorCommand(ui, appState);
        check(input.text == "hello replace tail" && appState.doc.text == input.text,
              "selection replacement and image block must undo together");
    }
}

}  // namespace

int main() {
    testRandomSequenceAgainstOracle();
    testHistoryMemoryOnOneMegabyteDocument();
    testDocumentSwitchClearsHistory();
    testLoadDocumentMatchesLegacyReset();
    testNoOpRedoAndRevisionSemantics();
    testSingleRecordForCompoundEdits();
    testAbortEditDetectsUncommittedChange();
    testApplyEditorCommandSyncsDivergedText();
    testTaskTogglePreservesViewport();
    testImageCommandRejectsPlainDocuments();
    testImageCommandSeparatesParagraphAndReplacesSelection();

    if (gFailures != 0) {
        std::cerr << gFailures << " / " << gChecks << " checks failed\n";
        return 1;
    }
    std::cout << gChecks << " checks passed\n";
    return 0;
}
