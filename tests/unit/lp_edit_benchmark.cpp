// CPU benchmark for the real Live Preview edit chain:
// cachedPlan -> cachedDecorationSnapshot -> ensureLayoutCache.
// Table and non-table documents both require incremental decoration publication.
// Legacy vector publication remains measured as pub-same/pub-diff for comparison;
// snap-same/snap-diff measure immutable snapshots constructed outside the timed loop.
// Cold layout is reported separately and is not improved by snapshot publication.
// NEO_PERF_GUARD=1 applies a same-machine baseline x1.5 guard. No absolute timing
// target is an acceptance criterion; this benchmark excludes drawing, IME and GUI.

#include "components/input.h"
#include "components/input_model.h"
#include "model/lp_decorations.h"
#include "model/lp_plan.h"
#include "model/style_schema.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using Model = components::input_detail::InputModel;
using Decoration = components::input_detail::LineDecoration;
using EditInfo = components::input_detail::DecoratorEditInfo;
using Clock = std::chrono::steady_clock;

constexpr const char* kFontFamily = "monospace";
constexpr const char* kCodeFontFamily = "monospace";
constexpr float kFontSize = 16.0f;
constexpr float kWidth = 800.0f;
constexpr const char* kInsertText = "字";  // 多字节插入，与真实键入同量级

constexpr int kMinIterations = 3;
constexpr double kBudgetMs = 2500.0;  // 单场景时间预算：大 N 少跑几轮，ms/次 不受影响

int g_failures = 0;

void fail(const std::string& message) {
    ++g_failures;
    std::cerr << "[lpbench] FAIL: " << message << "\n";
}

// ── 被测文档：真实 Markdown 结构 ──────────────────────────────────────────────
// 每 20 行一个循环：标题 / 普通段落 / 无序列表 / 引用 / 围栏代码 / 表格 / 行内标记
// / 任务项 / 有序列表，**块与块之间都有空行**。
//
// 空行不是装饰：T5 局部重解析要求"局部起点前、终点后都是空行"（否则块可能跨界，
// 切片无法证明等价）。第一版生成器整篇没有空行，于是局部路径每次都**正当拒收**
// （原因 "局部起点前不是空行"），量出来的是"全量 + 白跑一次判据"——本文件的结构
// 自检就是为抓这种情况而写的。
//
// Keep matching table/non-table variants to verify edits no longer trigger
// whole-document layout simply because an unrelated table exists.
struct Document {
    std::string text;
    int editOffset = 0;  // 选中的插入点：某个"普通段落块"的行首
    int editLine = 0;    // 该插入点的行号（仅供输出核对）
    std::size_t bytes = 0;
};

Document makeMarkdownDocument(int lineCount, bool withTables) {
    Document document;
    std::vector<int> lineStarts;
    lineStarts.reserve(static_cast<std::size_t>(lineCount));
    std::string& text = document.text;
    text.reserve(static_cast<std::size_t>(lineCount) * 64);

    for (int i = 0; i < lineCount; ++i) {
        lineStarts.push_back(static_cast<int>(text.size()));
        const int r = i % 20;
        std::string line;
        if (r == 0) {
            line = "## 小节 " + std::to_string(i);
        } else if (r == 2) {
            line = "普通段落第 " + std::to_string(i) + " 行：用于排版测量的示例文本 sample text。";
        } else if (r == 4) {
            line = "- 项目 " + std::to_string(i) + "：内容 " + std::to_string(i * 31 % 1000);
        } else if (r == 5) {
            line = "- 项目 " + std::to_string(i + 1) + "：同一列表的第二项";
        } else if (r == 7) {
            line = "> 引用 " + std::to_string(i) + " 的正文";
        } else if (r == 8) {
            line = "> 引用 " + std::to_string(i) + " 的续行";
        } else if (r == 10) {
            line = "```cpp";
        } else if (r == 11) {
            line = "int value" + std::to_string(i) + " = " + std::to_string(i) + ";  // 代码行";
        } else if (r == 12) {
            line = "```";
        } else if (withTables && r == 14) {
            line = "| 列A | 列B |";
        } else if (withTables && r == 15) {
            line = "| --- | :-: |";
        } else if (withTables && r == 16) {
            line = "| 值" + std::to_string(i) + " | 值" + std::to_string(i + 1) + " |";
        } else if (r == 18) {
            line = "**粗体 " + std::to_string(i) + "** 与 `code" + std::to_string(i) +
                   "` 与 [链接](https://example.com/" + std::to_string(i) + ")";
        } else if (r == 20) {
            line = "- [ ] 待办 " + std::to_string(i);
        } else if (r == 21) {
            line = "- [x] 已完成 " + std::to_string(i);
        } else if (r == 23) {
            line = "1. 有序项 " + std::to_string(i);
        } else if (r == 24) {
            line = "1. 有序项 " + std::to_string(i + 1);
        }
        // 其余 r 为空行 —— 块边界（无表格变体里 14/15/16 也落到这里）。
        text += line;
        text += '\n';
    }

    // 插入点：取中段最近的"普通段落行"（r == 2）。该行前一行（r == 1）与后一行
    // （r == 3）都是空行，所以受影响范围是一个被空行包住的完整块 —— 这既是真实
    // 文档的形态，也是 T5 局部路径能被证明的前提。
    int target = lineCount / 2;
    while (target < lineCount - 1 && (target % 20) != 2) {
        ++target;
    }
    document.editLine = target;
    document.editOffset = lineStarts[static_cast<std::size_t>(target)];
    document.bytes = text.size();
    return document;
}

EditInfo editInfoOf(const Model::InputState& state) {
    EditInfo info;
    info.textRevision = state.textRevision;
    info.committed = true;
    info.edit = &state.pendingEdit;
    return info;
}

// ── 测量脚手架（与 perf_benchmark.cpp 同口径）────────────────────────────────
struct Measurement {
    std::string key;
    std::size_t bytes = 0;
    int iters = 0;
    double totalMs = 0.0;
    double minMs = 0.0;
    double maxMs = 0.0;

    double msPerOp() const { return iters > 0 ? totalMs / iters : 0.0; }
};

// body(iter)：iter == -1 是不计时的预热轮。**注意场景内部必须自己先把缓存预热好**
// （body 之外显式做一次），否则把预热轮里发生的重建也算进计数器会让自检判错。
template <typename Body>
Measurement measure(const std::string& key, std::size_t bytes, int maxIters, Body&& body) {
    Measurement result;
    result.key = key;
    result.bytes = bytes;
    result.minMs = -1.0;

    body(-1);

    for (int iter = 0; iter < maxIters; ++iter) {
        const Clock::time_point start = Clock::now();
        body(iter);
        const double ms =
            std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        result.totalMs += ms;
        ++result.iters;
        if (result.minMs < 0.0 || ms < result.minMs) {
            result.minMs = ms;
        }
        if (ms > result.maxMs) {
            result.maxMs = ms;
        }
        if (result.iters >= kMinIterations && result.totalMs >= kBudgetMs) {
            break;
        }
    }
    return result;
}

void printMeasurement(const Measurement& measurement) {
    std::cout << "[lpbench] " << measurement.key << "  bytes=" << measurement.bytes
              << "  iters=" << measurement.iters << "  ms/op=" << measurement.msPerOp()
              << "  (min " << measurement.minMs << ", max " << measurement.maxMs << ")\n";
    std::cout << measurement.key << "=" << measurement.msPerOp() << "\n";  // 机器可读行
}

std::string trim(const std::string& value) {
    const std::size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

void parseBaselineEntries(const std::string& blob, std::map<std::string, double>& out) {
    std::size_t begin = 0;
    while (begin <= blob.size()) {
        const std::size_t end = std::min(blob.find_first_of(";\n", begin), blob.size());
        const std::string entry = trim(blob.substr(begin, end - begin));
        if (!entry.empty() && entry[0] != '#') {
            const std::size_t equals = entry.find('=');
            if (equals != std::string::npos) {
                const std::string key = trim(entry.substr(0, equals));
                const std::string value = trim(entry.substr(equals + 1));
                char* parsedEnd = nullptr;
                const double baseline = value.empty() ? 0.0 : std::strtod(value.c_str(), &parsedEnd);
                if (!key.empty() && parsedEnd != value.c_str() && baseline > 0.0) {
                    out[key] = baseline;
                }
            }
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
}

// MSVC 把 getenv 标成 C4996（要求 _dupenv_s）—— 与 perf_benchmark.cpp 同款包一层。
std::string environmentValue(const char* name) {
#ifdef _MSC_VER
    char* buffer = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) {
        return {};
    }
    std::string value(buffer);
    free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

std::map<std::string, double> loadBaselines() {
    std::map<std::string, double> baselines;
    const std::string inlineBaselines = environmentValue("NEO_PERF_BASELINES");
    if (!inlineBaselines.empty()) {
        parseBaselineEntries(inlineBaselines, baselines);
    }
    const std::string baselineFile = environmentValue("NEO_PERF_BASELINE_FILE");
    if (!baselineFile.empty()) {
        std::ifstream stream(baselineFile);
        if (stream) {
            std::string line;
            std::string blob;
            while (std::getline(stream, line)) {
                blob += line;
                blob += '\n';
            }
            parseBaselineEntries(blob, baselines);
        } else {
            std::cout << "[lpbench] 注意：NEO_PERF_BASELINE_FILE 打不开（" << baselineFile
                      << "），该文件里的基线不参与护栏\n";
        }
    }
    return baselines;
}

// ── 场景 ─────────────────────────────────────────────────────────────────────
// 每个场景清空计划与装饰缓存，并按计数器证明走了预期的路径。
std::vector<Measurement> runScenarios(int lineCount, const Document& document,
                                      const components::MarkdownStyle& style,
                                      const neo::EditorColors& colors, const std::string& tag,
                                      bool withTables) {
    const std::string& baseText = document.text;
    const std::string suffix = "/" + std::to_string(lineCount) + tag;
    const unsigned long long themeRev = neo::themeRevision();
    std::vector<Measurement> results;

    // ── plan-hit：未变文本重复取计划（T17 要消掉的整篇判等）──
    {
        neo::lp::invalidatePlanCache();
        std::string text = baseText;
        const EditInfo warm{0, true, nullptr};
        neo::lp::cachedPlan(text, &warm);  // 预热：先建一次，之后的调用都该命中
        if (neo::planDebugStats().full == 0) {
            fail("plan-hit" + suffix + ": 预热没有触发第一次全量重建");
        }
        const std::uint64_t fullBefore = neo::planDebugStats().full;
        const std::uint64_t partialBefore = neo::planDebugStats().partial;
        auto body = [&](int) {
            const neo::LpPlan& plan = neo::lp::cachedPlan(text, &warm);
            if (plan.lines.empty()) {
                fail("plan-hit" + suffix + ": 计划为空");
            }
        };
        results.push_back(measure("plan-hit" + suffix, text.size(), 200, body));
        if (neo::planDebugStats().full != fullBefore ||
            neo::planDebugStats().partial != partialBefore) {
            fail("plan-hit" + suffix + ": 未变文本却重建了计划（量到的不是整篇判等）");
        }
    }

    // ── plan-full：换文本 + 无编辑链 → 全量 buildLpPlan ──
    {
        neo::lp::invalidatePlanCache();
        std::string text = baseText;
        // 只翻首字节：让"整篇判等"在第一个字节就失败，把重建成本与判等成本分开。
        const char original = text.empty() ? '#' : text[0];
        const std::uint64_t fullBefore = neo::planDebugStats().full;
        std::uint64_t calls = 0;
        auto body = [&](int iter) {
            if (iter >= 0) {
                text[0] = (iter % 2 == 0) ? 'H' : original;
            }
            const neo::LpPlan& plan = neo::lp::cachedPlan(text, nullptr);
            ++calls;  // 预热轮与计时轮都会重建（缓存刚被清空）
            if (plan.lines.empty()) {
                fail("plan-full" + suffix + ": 计划为空");
            }
        };
        results.push_back(measure("plan-full" + suffix, text.size(), 20, body));
        if (neo::planDebugStats().full != fullBefore + calls) {
            fail("plan-full" + suffix + ": 全量重建次数与调用次数不符");
        }
        text[0] = original;
    }

    // ── plan-partial：单点插入 + 完整 revision 链 → 局部重解析 ──
    {
        neo::lp::invalidatePlanCache();
        Model::InputState state;
        state.text = baseText;
        state.textRevision = 1;
        Model::moveCursorTo(state, document.editOffset, false);
        {
            // 预热（不计时）：立一次可信基线并让 revision 链对齐（局部路径要求
            // "上一次也是提交文本"，这一条只能由真实的编辑器漏斗产生）。
            const EditInfo info = editInfoOf(state);
            neo::lp::cachedPlan(state.text, &info);
        }
        const std::uint64_t partialBefore = neo::planDebugStats().partial;
        const std::uint64_t fullBefore = neo::planDebugStats().full;
        const std::uint64_t fallbackBefore = neo::planDebugStats().fallback;
        int steps = 0;
        auto body = [&](int iter) {
            if (iter >= 0) {
                Model::insertAtCursor(state, kInsertText);
                ++steps;
            }
            const EditInfo info = editInfoOf(state);
            const neo::LpPlan& plan = neo::lp::cachedPlan(state.text, &info);
            if (plan.lines.empty()) {
                fail("plan-partial" + suffix + ": 计划为空");
            }
        };
        results.push_back(measure("plan-partial" + suffix, baseText.size(), 30, body));
        const neo::LpPlanDebugStats& stats = neo::planDebugStats();
        if (stats.partial != partialBefore + static_cast<std::uint64_t>(steps)) {
            fail("plan-partial" + suffix + ": 局部重解析没命中（回退了全量）—— 拒收原因: " +
                 neo::planRejectReason());
        }
        if (stats.full != fullBefore) {
            fail("plan-partial" + suffix + ": 走了全量 buildLpPlan");
        }
        std::cout << "[lpbench] plan-partial" << suffix
                  << "  局部=" << (stats.partial - partialBefore)
                  << "  回退=" << (stats.fallback - fallbackBefore) << "（回退期望 0）\n";
    }

    // Force a presentation-key miss: cursor changes now use partial rebuilding.
    // dec-full still measures full cache construction, excluding provider/layout.
    {
        neo::lp::invalidatePlanCache();
        std::string text = baseText;
        const EditInfo warm{0, true, nullptr};
        const neo::LpPlan& plan = neo::lp::cachedPlan(text, &warm);
        const unsigned long long planVersion = neo::lp::planCache().version;
        const int cursorA = document.editOffset;
        const int cursorB = static_cast<int>(text.size()) - 1;
        const std::uint64_t fullBefore = neo::lp::decorationDebugStats().full;
        std::uint64_t calls = 0;
        auto body = [&](int iter) {
            const int cursor = (iter % 2 == 0) ? cursorA : cursorB;
            const std::vector<Decoration>& table = neo::lp::cachedDecorations(
                plan, planVersion, cursor, style, kFontFamily, neo::ThemeMode::Dark, {}, nullptr,
                text, &colors, nullptr, themeRev + calls + 1);
            ++calls;
            if (table.size() < plan.lines.size()) {
                fail("dec-full" + suffix + ": 装饰表行数少于计划行数");
            }
        };
        results.push_back(measure("dec-full" + suffix, text.size(), 20, body));
        if (neo::lp::decorationDebugStats().full != fullBefore + calls) {
            fail("dec-full" + suffix + ": presentation-key miss did not trigger full construction");
        }
    }

    // ── dec-inc：单点编辑 + 链完整 → 增量装饰 ──
    {
        neo::lp::invalidatePlanCache();
        Model::InputState state;
        state.text = baseText;
        state.textRevision = 1;
        Model::moveCursorTo(state, document.editOffset, false);
        std::size_t lastPlanLines = 0;
        const auto chain = [&]() {
            const EditInfo info = editInfoOf(state);
            const neo::LpPlan& plan = neo::lp::cachedPlan(state.text, &info);
            lastPlanLines = plan.lines.size();
            return &neo::lp::cachedDecorations(plan, neo::lp::planCache().version, state.cursor,
                                              style, kFontFamily, neo::ThemeMode::Dark, {}, nullptr,
                                              state.text, &colors, &info, themeRev);
        };
        chain();  // 预热：建立"上次建表"的键与链（不计时）
        const std::uint64_t incrementalBefore = neo::lp::decorationDebugStats().incremental;
        const std::uint64_t fallbackBefore = neo::lp::decorationDebugStats().fallback;
        const std::uint64_t planPartialBefore = neo::planDebugStats().partial;
        int steps = 0;
        auto body = [&](int iter) {
            if (iter >= 0) {
                Model::insertAtCursor(state, kInsertText);
                ++steps;
            }
            const std::vector<Decoration>* table = chain();
            if (table == nullptr || table->size() < lastPlanLines) {
                fail("dec-inc" + suffix + ": 装饰表行数少于计划行数");
            }
        };
        results.push_back(measure("dec-inc" + suffix, baseText.size(), 30, body));
        const neo::lp::DecorationDebugStats& stats = neo::lp::decorationDebugStats();
        if (stats.incremental != incrementalBefore + static_cast<std::uint64_t>(steps) ||
            stats.fallback != fallbackBefore) {
            fail("dec-inc" + suffix + ": expected incremental decorations: " + neo::lp::decorationRejectReason());
        }
        std::cout << "[lpbench] dec-inc" << suffix
                  << "  增量=" << (stats.incremental - incrementalBefore)
                  << "  回退=" << (stats.fallback - fallbackBefore)
                  << "  其中计划局部=" << (neo::planDebugStats().partial - planPartialBefore)
                  << "（回退期望 0）\n";
    }

    // ── publish：把装饰表交给输入组件（T8 的两个目标各一档）──
    // setDecorations 是"内容相同就早退"的**整表比较**，内容变了就**整表深拷**进
    // InputState，并在 ensureLayoutCache 内部被真正调用（:1879/:1897/:2138）——
    // 所以这两档量到的正是 T8 说的"整表比较"与"发布复制"。
    {
        neo::lp::invalidatePlanCache();
        std::string text = baseText;
        const EditInfo warm{0, true, nullptr};
        const neo::LpPlan& plan = neo::lp::cachedPlan(text, &warm);
        const std::vector<Decoration>& table = neo::lp::cachedDecorations(
            plan, neo::lp::planCache().version, document.editOffset, style, kFontFamily,
            neo::ThemeMode::Dark, {}, nullptr, text, &colors, nullptr, themeRev);
        if (table.empty()) {
            fail("publish" + suffix + ": 装饰表为空");
        }
        Model::InputState state;
        state.text = text;
        Model::setDecorations(state, table);  // 首次深拷（不计时）
        results.push_back(measure("pub-same" + suffix, text.size(), 200, [&](int) {
            Model::setDecorations(state, table);  // 内容相同 → 比较后早退，不复制
        }));
        auto snapshot = std::make_shared<const components::input_detail::LineDecorationTable>(table);
        Model::setDecorations(state, *snapshot, snapshot);
        results.push_back(measure("snap-same" + suffix, text.size(), 200, [&](int) {
            Model::setDecorations(state, *snapshot, snapshot);
        }));
        std::vector<Decoration> other = table;
        other.back().lineHeight += 0.5f;
        auto alternative = std::make_shared<const components::input_detail::LineDecorationTable>(std::move(other));
        results.push_back(measure("snap-diff" + suffix, text.size(), 200, [&](int iter) {
            const auto& selected = iter % 2 == 0 ? snapshot : alternative;
            Model::setDecorations(state, *selected, selected);
        }));
        if (!state.decorations.empty() || !state.decorationSnapshot) fail("snapshot publication copied the table");
        std::vector<Decoration> variant = table;
        const float baseHeight = variant.back().lineHeight;
        results.push_back(measure("pub-diff" + suffix, text.size(), 20, [&](int iter) {
            if (iter >= 0) {
                // 改**最后一行**：整表比较必须走到末尾才能发现不同，随后整表深拷
                // —— 最坏观察点。
                variant.back().lineHeight = baseHeight + (iter % 2 == 0 ? 0.5f : 0.0f);
            }
            Model::setDecorations(state, variant);
        }));
    }

    // ── lay-full：强制冷排版 → 整篇重排的成本（用来判断"表格回退"那 30~50 倍里，
    //    多少是"整篇重排"本身贵、多少是"表格行贵"）──
    {
        neo::lp::invalidatePlanCache();
        Model::InputState state;
        state.text = baseText;
        state.textRevision = 1;
        Model::moveCursorTo(state, document.editOffset, false);
        const EditInfo warm{0, true, nullptr};
        const neo::LpPlan& plan = neo::lp::cachedPlan(state.text, &warm);
        const std::vector<Decoration>& table = neo::lp::cachedDecorations(
            plan, neo::lp::planCache().version, state.cursor, style, kFontFamily,
            neo::ThemeMode::Dark, {}, nullptr, state.text, &colors, nullptr, themeRev);
        results.push_back(measure("lay-full" + suffix, baseText.size(), 4, [&](int iter) {
            if (iter >= 0) {
                state.layoutCacheValid = false;  // 冷路径：整篇重排
            }
            Model::ensureLayoutCache(state, kFontFamily, kFontSize, kWidth, true, &table);
        }));
        if (state.cachedLines.empty()) {
            fail("lay-full" + suffix + ": 排版后没有行");
        }
    }

    // ── edit：真实一次按键（插入 → 计划 → 装饰 → 排版）──
    {
        neo::lp::invalidatePlanCache();
        Model::InputState state;
        state.text = baseText;
        state.textRevision = 1;
        Model::moveCursorTo(state, document.editOffset, false);
        // 三个阶段分开累计：一次按键的总耗时里，"计划 / 装饰 / 排版"谁是大头决定了
        // 优化该往哪投（B3 的 T17 只动第一步，T8 只动第二步）。
        double planMs = 0.0;
        double decorMs = 0.0;
        double layoutMs = 0.0;
        const auto fullChain = [&]() {
            const EditInfo info = editInfoOf(state);
            const Clock::time_point t0 = Clock::now();
            const neo::LpPlan& plan = neo::lp::cachedPlan(state.text, &info);
            const Clock::time_point t1 = Clock::now();
            const auto table = neo::lp::cachedDecorationSnapshot(
                plan, neo::lp::planCache().version, state.cursor, style, kFontFamily,
                neo::ThemeMode::Dark, {}, nullptr, state.text, &colors, &info, themeRev);
            const Clock::time_point t2 = Clock::now();
            Model::ensureLayoutCache(state, kFontFamily, kFontSize, kWidth, true, table.get(), table);
            const Clock::time_point t3 = Clock::now();
            const auto ms = [](Clock::time_point a, Clock::time_point b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            planMs += ms(t0, t1);
            decorMs += ms(t1, t2);
            layoutMs += ms(t2, t3);
        };
        fullChain();  // 预热（不计时）
        planMs = decorMs = layoutMs = 0.0;
        const std::uint64_t partialBefore = neo::planDebugStats().partial;
        const std::uint64_t incrementalBefore = neo::lp::decorationDebugStats().incremental;
        const std::uint64_t fallbackBefore = neo::lp::decorationDebugStats().fallback;
        const auto layoutBefore = Model::debugLayoutStats();
        int steps = 0;
        int reflows = 0;
        auto body = [&](int iter) {
            const auto previousLineCount = state.cachedLines.size();
            const float previousHeight = state.cachedGeometry.total();
            if (iter >= 0) {
                Model::insertAtCursor(state, kInsertText);
                ++steps;
            }
            fullChain();
            if (iter >= 0 && (previousLineCount != state.cachedLines.size() ||
                              previousHeight != state.cachedGeometry.total())) ++reflows;
            if (state.cachedLines.empty()) {
                fail("edit" + suffix + ": 排版后没有行");
            }
        };
        results.push_back(measure("edit" + suffix, baseText.size(), 30, body));
        if (steps > 0) {
            const double perStep = static_cast<double>(steps);
            std::cout << "[lpbench] edit" << suffix << "  阶段占比（每次按键平均）: 计划 "
                      << planMs / perStep << "ms  装饰 " << decorMs / perStep << "ms  排版 "
                      << layoutMs / perStep << "ms  (iterations=" << steps << ")\n";
        }
        if (neo::planDebugStats().partial != partialBefore + static_cast<std::uint64_t>(steps)) {
            fail("edit" + suffix + ": 计划没走局部重解析 —— 拒收原因: " + neo::planRejectReason());
        }
        if (neo::lp::decorationDebugStats().incremental != incrementalBefore + static_cast<std::uint64_t>(steps) ||
            neo::lp::decorationDebugStats().fallback != fallbackBefore) {
            fail("edit" + suffix + ": expected incremental decorations: " + neo::lp::decorationRejectReason());
        }
        if (Model::debugLayoutStats().full != layoutBefore.full ||
            Model::debugLayoutStats().incremental != layoutBefore.incremental + static_cast<std::uint64_t>(steps)) {
            fail("edit" + suffix + ": expected incremental layout: " + Model::layoutRejectReason());
        }
        if (!withTables) {
            const auto& after = Model::debugLayoutStats();
            // Typing can cross a wrap boundary. Such a real geometry transition
            // must retain the general path; all other edits keep the line storage.
            const auto stableSteps = static_cast<std::uint64_t>(steps - reflows);
            if (after.editPatched != layoutBefore.editPatched + stableSteps ||
                after.editMeasuredRows != layoutBefore.editMeasuredRows + stableSteps ||
                after.geometryRebuilt != layoutBefore.geometryRebuilt + static_cast<std::uint64_t>(reflows))
                fail("edit" + suffix + ": stable edits must measure one row; real reflows rebuild geometry");
            std::cout << "[lpbench] stable-edit" << suffix << " patches="
                      << after.editPatched - layoutBefore.editPatched << " rows="
                      << after.editMeasuredRows - layoutBefore.editMeasuredRows << " geometry="
                      << after.geometryRebuilt - layoutBefore.geometryRebuilt << " real-reflows=" << reflows << "\n";
        }

    }

    // Same-byte replacement keeps absolute source offsets fixed. Use the same
    // generator and real edit funnel in both baseline and candidate binaries.
    {
        neo::lp::invalidatePlanCache();
        Model::InputState state;
        Model::loadDocument(state, baseText);
        const auto chain = [&]() {
            const EditInfo info = editInfoOf(state);
            const auto& plan = neo::lp::cachedPlan(state.text, &info);
            const auto table = neo::lp::cachedDecorationSnapshot(plan,
                neo::lp::planCache().version, state.cursor, style, kFontFamily,
                neo::ThemeMode::Dark, {}, nullptr, state.text, &colors, &info, themeRev);
            Model::ensureLayoutCache(state, kFontFamily, kFontSize, kWidth, true,
                table.get(), table);
        };
        Model::moveCursorTo(state, document.editOffset, false);
        chain();
        results.push_back(measure("replace" + suffix, baseText.size(), 30, [&](int iter) {
            if (iter >= 0) {
                Model::moveCursorTo(state, document.editOffset, false);
                Model::moveCursorTo(state, document.editOffset + 3, true);
                Model::insertAtCursor(state, iter % 2 == 0 ? "常" : "普");
            }
            chain();
            if (state.cachedLines.empty()) fail("replace" + suffix + ": missing layout");
        }));
    }

    // Compare complete cursor refreshes with the former provider-by-value path.
    // The legacy reconstruction keeps vector capacity, as the previous cache did;
    // both paths use the same parser/style/layout implementation in this binary.
    // This paired comparison is not a historical baseline for other changes.
    for (bool snapshots : {false, true}) {
        neo::lp::invalidatePlanCache();
        Model::InputState state;
        state.text = baseText;
        state.textRevision = 1;
        std::vector<Decoration> legacyTable;
        int legacyCursor = -1;
        const auto refresh = [&](int cursor) {
            Model::moveCursorTo(state, cursor, false);
            auto info = editInfoOf(state);
            components::input_detail::LineDecorationChanges changes;
            info.changes = &changes;
            const auto& plan = neo::lp::cachedPlan(state.text, &info);
            if (snapshots) {
                const auto table = neo::lp::cachedDecorationSnapshot(
                    plan, neo::lp::planCache().version, state.cursor, style, kFontFamily,
                    neo::ThemeMode::Dark, {}, nullptr, state.text, &colors, &info, themeRev);
                Model::ensureLayoutCache(state, kFontFamily, kFontSize, kWidth, true, table.get(), table, &changes);
            } else {
                if (legacyCursor != cursor) {
                    neo::lp::buildDecorations(plan, style, cursor, legacyTable, kFontFamily,
                                              {}, nullptr, state.text, &colors);
                    legacyCursor = cursor;
                }
                // The old std::function<vector<Decoration>(...)> provider copied
                // the cache's const-reference result on every frame.
                const std::vector<Decoration> providerResult = legacyTable;
                Model::ensureLayoutCache(state, kFontFamily, kFontSize, kWidth, true, &providerResult);
            }
        };
        refresh(document.editOffset);
        const std::string name = snapshots ? "cursor-snapshot" : "cursor-vector";
        const auto cursorBefore = neo::lp::decorationDebugStats();
        results.push_back(measure(name + suffix, baseText.size(), 20, [&](int iter) {
            refresh(iter % 2 == 0 ? document.editOffset : static_cast<int>(baseText.size()) - 1);
        }));
        const auto cursorAfter = neo::lp::decorationDebugStats();
        if (snapshots && (cursorAfter.full != cursorBefore.full ||
            cursorAfter.cursorPartial <= cursorBefore.cursorPartial ||
            cursorAfter.cursorCopiedRows != cursorBefore.cursorCopiedRows)) {
            fail(name + suffix + ": plain cross-block cursor should share the snapshot");
        }
        refresh(document.editOffset);
        results.push_back(measure((snapshots ? "frame-snapshot" : "frame-vector") + suffix,
                                 baseText.size(), 100, [&](int) { refresh(document.editOffset); }));
        if (snapshots) {
            // Adjacent UTF-8 character boundaries in an ordinary paragraph.
            // Measure the complete provider/layout chain, not just cache lookup.
            refresh(document.editOffset);
            const auto regionDecorBefore = neo::lp::decorationDebugStats();
            const auto regionLayoutBefore = Model::debugLayoutStats();
            results.push_back(measure("cursor-region" + suffix, baseText.size(), 100, [&](int iter) {
                refresh(document.editOffset + (iter % 2 == 0 ? 3 : 6));
            }));
            const auto regionDecorAfter = neo::lp::decorationDebugStats();
            const auto regionLayoutAfter = Model::debugLayoutStats();
            if (regionDecorAfter.full != regionDecorBefore.full ||
                regionDecorAfter.incremental != regionDecorBefore.incremental ||
                regionDecorAfter.cursorSemanticHit <= regionDecorBefore.cursorSemanticHit ||
                regionLayoutAfter.full != regionLayoutBefore.full ||
                regionLayoutAfter.incremental != regionLayoutBefore.incremental) {
                fail("cursor-region" + suffix + ": expected semantic hit without decoration/layout rebuild");
            }
            const auto& cursorPlan = neo::lp::planCache().plan;
            const int markRow = document.editLine + 16;
            const auto& markLine = cursorPlan.lines[static_cast<std::size_t>(markRow)];
            refresh(markLine.srcBeg);
            const auto markBefore = neo::lp::decorationDebugStats();
            const auto markLayoutBefore = Model::debugLayoutStats();
            results.push_back(measure("cursor-mark" + suffix, baseText.size(), 20, [&](int iter) {
                refresh(iter % 2 == 0 ? markLine.srcBeg + 3 : markLine.srcEnd);
            }));
            const auto markAfter = neo::lp::decorationDebugStats();
            const auto markLayoutAfter = Model::debugLayoutStats();
            if (markLayoutAfter.cursorPatched <= markLayoutBefore.cursorPatched ||
                markLayoutAfter.cursorMeasuredRows - markLayoutBefore.cursorMeasuredRows > 21 ||
                markLayoutAfter.geometryRebuilt != markLayoutBefore.geometryRebuilt) {
                fail("cursor-mark" + suffix + ": expected bounded row patches without global geometry rebuild");
            }
            if (markAfter.full != markBefore.full ||
                markAfter.cursorPartial <= markBefore.cursorPartial ||
                markAfter.cursorRebuiltRows - markBefore.cursorRebuiltRows > 21 ||
                markAfter.cursorCopiedRows - markBefore.cursorCopiedRows > 21 * (components::input_detail::LineDecorationTable::pageSize - 1)) {
                fail("cursor-mark" + suffix + ": expected one-row rebuilding with bounded page copies");
            }
            refresh(document.editOffset);
            const auto neighborBefore = neo::lp::decorationDebugStats();
            results.push_back(measure("cursor-neighbor" + suffix, baseText.size(), 20, [&](int iter) {
                refresh(iter % 2 == 0 ? document.editOffset :
                    cursorPlan.lines[static_cast<std::size_t>(document.editLine + 2)].srcBeg);
            }));
            const auto neighborAfter = neo::lp::decorationDebugStats();
            if (neighborAfter.full != neighborBefore.full ||
                neighborAfter.cursorPartial <= neighborBefore.cursorPartial ||
                neighborAfter.cursorRebuiltRows - neighborBefore.cursorRebuiltRows > 21 * 3) {
                fail("cursor-neighbor" + suffix + ": expected bounded old/new block rebuilding");
            }
            std::cout << "[lpbench] cursor-local" << suffix
                      << " mark-rebuilt=" << markAfter.cursorRebuiltRows - markBefore.cursorRebuiltRows
                      << " mark-copied=" << markAfter.cursorCopiedRows - markBefore.cursorCopiedRows
                      << " layout-patched=" << markLayoutAfter.cursorPatched - markLayoutBefore.cursorPatched
                      << " layout-measured=" << markLayoutAfter.cursorMeasuredRows - markLayoutBefore.cursorMeasuredRows
                      << " geometry-rebuilt=" << markLayoutAfter.geometryRebuilt - markLayoutBefore.geometryRebuilt
                      << " neighbor-rebuilt=" << neighborAfter.cursorRebuiltRows - neighborBefore.cursorRebuiltRows
                      << " neighbor-copied=" << neighborAfter.cursorCopiedRows - neighborBefore.cursorCopiedRows << "\n";
            const auto copySource = neo::lp::decorationCache().snapshot;
            std::vector<Decoration> copied;
            results.push_back(measure("dec-copy" + suffix, baseText.size(), 20, [&](int) {
                const components::input_detail::LineDecorationView view(*copySource);
                copied.assign(view.begin(), view.end());
            }));
            if (copied != *copySource) fail("dec-copy: copied table differs");
            components::input_detail::LineDecorationSnapshot freshCopy;
            results.push_back(measure("dec-fresh" + suffix, baseText.size(), 20, [&](int) {
                freshCopy = std::make_shared<const components::input_detail::LineDecorationTable>(copySource->copyRows());
            }));
            if (*freshCopy != *copySource) fail("dec-fresh: copied snapshot differs");

            const auto& boundaries = neo::lp::decorationCache().cursorBoundaries;
            std::cout << "[lpbench] cursor-boundaries" << suffix << " count=" << boundaries.size()
                      << " capacity-bytes=" << boundaries.capacity() * sizeof(int) << "\n";
            const auto& cache = neo::lp::decorationCache();
            std::cout << "[lpbench] cursor-dependencies" << suffix
                      << " spans=" << cache.cursorSpans.size()
                      << " span-capacity-bytes=" << cache.cursorSpans.capacity() * sizeof(neo::lp::CursorSpanDependency)
                      << " fold-capacity-bytes=" << cache.cursorFolds.capacity() * sizeof(neo::lp::CursorFoldSeed) << "\n";
        }
        if (state.cachedLines.empty() || (snapshots && !state.decorations.empty())) {
            fail(name + suffix + ": cursor refresh did not publish the expected layout");
        }
    }
    return results;
}

const Measurement* findResult(const std::vector<Measurement>& results, const std::string& key) {
    for (const Measurement& measurement : results) {
        if (measurement.key == key) {
            return &measurement;
        }
    }
    return nullptr;
}

}  // namespace

// A separate corpus for same-byte edits which actually change decoration values.
// Baseline and candidate compile this identical harness; normal guards are unchanged.
int runEditPageBenchmark() {
    const auto colors = neo::editorColors(neo::ThemeMode::Dark);
    const auto style = neo::markdownStyle(kFontSize, kFontFamily, kCodeFontFamily, colors);
    for (int lines : {80, 2000, 20000}) for (bool code : {false, true}) {
        std::string text;
        for (int row = 0; row < lines; ++row) {
            if (row == lines / 2) text += code ? "```cpp\nint value = 42;\n```\n\n" : "- [x] task 中文 😀\n\n";
            else text += "普通正文 **bold** and 中文 😀 sample\n\n";
        }
        const int pos = static_cast<int>(text.find(code ? "int value" : "[x]")) + (code ? 0 : 1);
        neo::lp::invalidatePlanCache();
        Model::InputState state;
        Model::loadDocument(state, text);
        Model::moveCursorTo(state, pos, false);
        const auto chain = [&]() {
            const auto info = editInfoOf(state);
            const auto& plan = neo::lp::cachedPlan(state.text, &info);
            const auto snapshot = neo::lp::cachedDecorationSnapshot(plan, neo::lp::planCache().version,
                state.cursor, style, kFontFamily, neo::ThemeMode::Dark, {}, nullptr, state.text, &colors, &info);
            Model::ensureLayoutCache(state, kFontFamily, kFontSize, kWidth, true, snapshot.get(), snapshot);
        };
        chain();
        for (int warm = 0; warm < 4; ++warm) {
            Model::moveCursorTo(state, pos, false);
            Model::moveCursorTo(state, pos + (code ? 3 : 1), true);
            Model::insertAtCursor(state, code ? (warm % 2 ? "int" : "abc") : (warm % 2 ? "x" : " "));
            chain();
        }
        const int iterations = lines == 20000 ? 20 : 60;
        const auto start = Clock::now();
        for (int iter = 0; iter < iterations; ++iter) {
            Model::moveCursorTo(state, pos, false);
            Model::moveCursorTo(state, pos + (code ? 3 : 1), true);
            Model::insertAtCursor(state, code ? (iter % 2 ? "int" : "abc") : (iter % 2 ? "x" : " "));
            chain();
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / iterations;
        const auto fullPlan = neo::buildLpPlan(state.text);
        std::vector<Decoration> oracle;
        neo::lp::buildDecorations(fullPlan, style, state.cursor, oracle, kFontFamily, {}, nullptr, state.text, &colors);
        if (!(*state.decorationSnapshot == oracle)) return 1;
        std::cout << "[edit-pages] " << (code ? "code" : "task") << "/" << lines
                  << " bytes=" << text.size() << " iterations=" << iterations << " ms=" << ms << "\n";
    }
    return 0;
}

// Same source-row count, nonzero byte delta: plain versus sparse styled suffix.
// Compiled unchanged against both baseline and candidate decoration builders.
int runOffsetPageBenchmark() {
    const auto colors = neo::editorColors(neo::ThemeMode::Dark);
    const auto style = neo::markdownStyle(kFontSize, kFontFamily, kCodeFontFamily, colors);
    for (int lines : {80, 2000, 20000}) for (bool rich : {false, true}) {
        std::string text;
        for (int row = 0; row < lines; ++row) {
            if (row == lines / 2) text += "EDIT_TARGET 中文 😀\n";
            else if (row % 2) text += "\n";
            else text += rich && row % 40 == 0 ? "正文 **bold** `code` 中文 😀\n" : "普通正文 中文 😀 sample\n";
        }
        const int pos = static_cast<int>(text.find("EDIT_TARGET"));
        neo::lp::invalidatePlanCache();
        Model::InputState state;
        Model::loadDocument(state, text); Model::moveCursorTo(state, pos, false);
        const auto chain = [&]() {
            const auto info = editInfoOf(state);
            const auto& plan = neo::lp::cachedPlan(state.text, &info);
            const auto snapshot = neo::lp::cachedDecorationSnapshot(plan, neo::lp::planCache().version,
                state.cursor, style, kFontFamily, neo::ThemeMode::Dark, {}, nullptr, state.text, &colors, &info);
            Model::ensureLayoutCache(state, kFontFamily, kFontSize, kWidth, true, snapshot.get(), snapshot);
        };
        const auto edit = [&](int iter) {
            Model::moveCursorTo(state, pos, false);
            if (iter % 2) Model::moveCursorTo(state, pos + 7, true);
            if (iter % 2) Model::eraseSelection(state);
            else Model::insertAtCursor(state, "增😀");
            chain();
        };
        chain();
        for (int warm = 0; warm < 4; ++warm) edit(warm);
        const int iterations = lines == 20000 ? 20 : 60;
        const auto start = Clock::now();
        for (int iter = 0; iter < iterations; ++iter) edit(iter);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / iterations;
        const auto plan = neo::buildLpPlan(state.text);
        std::vector<Decoration> oracle;
        neo::lp::buildDecorations(plan, style, state.cursor, oracle, kFontFamily, {}, nullptr, state.text, &colors);
        if (state.text != text || !(*state.decorationSnapshot == oracle)) return 1;
        std::cout << "[offset-pages] " << (rich ? "sparse" : "plain") << "/" << lines
                  << " source-lines=" << plan.lines.size() << " iterations=" << iterations << " ms=" << ms << "\n";
    }
    return 0;
}

int main() {
    if (environmentValue("NEO_OFFSET_PAGE_BENCH") == "1") return runOffsetPageBenchmark();
    if (environmentValue("NEO_EDIT_PAGE_BENCH") == "1") return runEditPageBenchmark();
    const bool guardEnabled = environmentValue("NEO_PERF_GUARD") == "1";
    const std::map<std::string, double> baselines = loadBaselines();

#ifdef NDEBUG
    const char* buildKind = "Release";
#else
    const char* buildKind = "Debug(未优化，数字仅供参考)";
#endif

    std::cout << "[lpbench] === B3 真实 LP 编辑链基准 (" << buildKind << ") ===\n";
    std::cout << "[lpbench] 链：cachedPlan(整篇判等/局部重解析) → cachedDecorations(重建/发布) → "
                 "ensureLayoutCache\n";
    if (guardEnabled) {
        std::cout << "[lpbench] NEO_PERF_GUARD=1：相对护栏开启（基线 ×1.5），已提供基线 "
                  << baselines.size() << " 条\n";
    } else {
        std::cout << "[lpbench] 护栏关闭：只打印 ms/次，不因绝对耗时失败\n";
    }

    std::vector<int> sizes{2000, 20000, 100000};
    const auto requestedLines = environmentValue("NEO_LP_BENCH_LINES");
    if (!requestedLines.empty()) {
        const auto found = std::find_if(sizes.begin(), sizes.end(), [&](int size) {
            return std::to_string(size) == requestedLines;
        });
        if (found == sizes.end()) {
            std::cerr << "NEO_LP_BENCH_LINES must be 2000, 20000 or 100000\n";
            return 2;
        }
        sizes = {*found};
    }
    const neo::EditorColors colors = neo::editorColors(neo::ThemeMode::Dark);
    const components::MarkdownStyle style =
        neo::markdownStyle(kFontSize, kFontFamily, kCodeFontFamily, colors);

    std::vector<Measurement> results;
    for (int lineCount : sizes) {
        // Matching table/non-table samples isolate the cost of table block invalidation.
        for (bool withTables : {true, false}) {
            const std::string tag = withTables ? std::string() : std::string("-notable");
            const Document document = makeMarkdownDocument(lineCount, withTables);
            std::cout << "[lpbench] ---- N=" << lineCount << " 行" << tag << "，bytes="
                      << document.bytes << "，插入点行号=" << document.editLine << " ----\n";
            std::vector<Measurement> perSize =
                runScenarios(lineCount, document, style, colors, tag, withTables);
            results.insert(results.end(), perSize.begin(), perSize.end());
        }
    }

    for (const Measurement& measurement : results) {
        printMeasurement(measurement);
    }

    // ── 汇总：热路径 vs 重建 / 增量 vs 全量 ──
    std::cout << "[lpbench] ---- ms/次 汇总 ----\n";
    std::cout << "[lpbench] scenario";
    for (int lineCount : sizes) {
        std::cout << "            " << lineCount;
    }
    std::cout << "\n";
    for (const char* scenario : {"plan-hit", "plan-partial", "plan-full", "dec-inc", "dec-full",
                                 "edit"}) {
        std::cout << "[lpbench] " << scenario;
        for (int lineCount : sizes) {
            const Measurement* measurement =
                findResult(results, std::string(scenario) + "/" + std::to_string(lineCount));
            std::cout << "    " << (measurement != nullptr ? measurement->msPerOp() : -1.0);
        }
        std::cout << "\n";
    }
    std::cout << "[lpbench] ---- 倍数关系（T17/T8 的收益就在这里）----\n";
    for (int lineCount : sizes) {
        const Measurement* hit = findResult(results, "plan-hit/" + std::to_string(lineCount));
        const Measurement* full = findResult(results, "plan-full/" + std::to_string(lineCount));
        const Measurement* partial =
            findResult(results, "plan-partial/" + std::to_string(lineCount));
        const Measurement* decFull = findResult(results, "dec-full/" + std::to_string(lineCount));
        const Measurement* decInc = findResult(results, "dec-inc/" + std::to_string(lineCount));
        if (hit == nullptr || full == nullptr || partial == nullptr || decFull == nullptr ||
            decInc == nullptr) {
            continue;
        }
        std::cout << "[lpbench] N=" << lineCount << ": 计划 全量/局部="
                  << (partial->msPerOp() > 0.0 ? full->msPerOp() / partial->msPerOp() : 0.0)
                  << "x  局部/热命中="
                  << (hit->msPerOp() > 0.0 ? partial->msPerOp() / hit->msPerOp() : 0.0)
                  << "x  装饰 全量/增量="
                  << (decInc->msPerOp() > 0.0 ? decFull->msPerOp() / decInc->msPerOp() : 0.0)
                  << "x\n";
    }

    std::cout << "[lpbench] ---- 装饰发布（T8）----\n";
    for (int lineCount : sizes) {
        const Measurement* same = findResult(results, "pub-same/" + std::to_string(lineCount));
        const Measurement* diff = findResult(results, "pub-diff/" + std::to_string(lineCount));
        if (same == nullptr || diff == nullptr) {
            continue;
        }
        std::cout << "[lpbench] N=" << lineCount << ": 同表（比较后早退）=" << same->msPerOp()
                  << " ms  变表（比较+整表深拷）=" << diff->msPerOp() << " ms\n";
    }

    std::cout << "[lpbench] ---- 表格的影响（同规模、只差每 20 行一个表格块）----\n";
    for (int lineCount : sizes) {
        const Measurement* withTable = findResult(results, "edit/" + std::to_string(lineCount));
        const Measurement* without =
            findResult(results, "edit/" + std::to_string(lineCount) + "-notable");
        const Measurement* coldTable =
            findResult(results, "lay-full/" + std::to_string(lineCount));
        const Measurement* coldNoTable =
            findResult(results, "lay-full/" + std::to_string(lineCount) + "-notable");
        if (withTable == nullptr || without == nullptr) {
            continue;
        }
        std::cout << "[lpbench] edit N=" << lineCount << ": 含表格=" << withTable->msPerOp()
                  << " ms/按键  不含表格=" << without->msPerOp() << " ms/按键  倍数="
                  << (without->msPerOp() > 0.0 ? withTable->msPerOp() / without->msPerOp() : 0.0)
                  << "x\n";
        if (coldTable != nullptr && coldNoTable != nullptr) {
            std::cout << "[lpbench] 冷排版 N=" << lineCount << "（整篇重排上限）: 含表格="
                      << coldTable->msPerOp() << " ms  不含表格=" << coldNoTable->msPerOp()
                      << " ms  表格倍数="
                      << (coldNoTable->msPerOp() > 0.0
                              ? coldTable->msPerOp() / coldNoTable->msPerOp()
                              : 0.0)
                      << "x\n";
        }
    }

    // ── 相对护栏（仅 NEO_PERF_GUARD=1）──
    int guardFailures = 0;
    int guarded = 0;
    if (guardEnabled) {
        for (const Measurement& measurement : results) {
            const auto baseline = baselines.find(measurement.key);
            if (baseline == baselines.end()) {
                std::cout << "[guard] 跳过 " << measurement.key << "：没有基线\n";
                continue;
            }
            ++guarded;
            const double limit = baseline->second * 1.5;
            const bool ok = measurement.msPerOp() <= limit;
            std::cout << "[guard] " << measurement.key << "  " << measurement.msPerOp()
                      << " ms/op  基线 " << baseline->second << "  上限 ×1.5=" << limit
                      << (ok ? "  OK" : "  超标") << "\n";
            if (!ok) {
                ++guardFailures;
            }
        }
        std::cout << "[guard] 结论：比对 " << guarded << " 条，超标 " << guardFailures << " 条\n";
    }

    std::cout << "[lpbench] 本次基线速贴（NEO_PERF_BASELINES）：";
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (index > 0) {
            std::cout << ";";
        }
        std::cout << results[index].key << "=" << results[index].msPerOp();
    }
    std::cout << "\n";

    if (g_failures > 0) {
        std::cerr << "[lpbench] 结构性自检失败 " << g_failures << " 处\n";
        return 1;
    }
    if (guardFailures > 0) {
        std::cerr << "[lpbench] 相对护栏超标 " << guardFailures << " 处（相对基线 ×1.5）\n";
        return 30;
    }
    return 0;
}
