// T12 无头性能基准（给 T1/T4/T5 等性能项提供"尺子"）。
//
// 三个场景（见 docs/归档/NeoEditor-修复任务清单-2026-09-25.md 的 T12）：
//   full   — N 行文档全量 ensureLayoutCache：每轮先把排版缓存打冷（layoutCacheValid=false），
//            测"从零重排整篇"的现状基线；
//   decor  — 文本不动、光标换块 100 次：装饰在几个停靠行之间来回点亮/熄灭，走
//            updateChangedDecorations 增量路（只该重测装饰变了的行）；
//   resize — viewportWidth（= 组件的 textWidth）在 720/760 两个宽度间来回切，模拟拖拽
//            窗口时每帧 InputLayout::build 的全量重排（T1 塑形缓存的对照就在这里）。
//
// 规模 2k / 20k / 100k 行。shaping 结果缓存容量是 2048（core/render/text.cpp 的
// kShapingCacheCapacity）：2000 行（N<=2048）稳态全命中、20000/100000 行（N>2048）
// 每轮全量都要逐条触发淘汰（O(2048) 的 min_element 扫描）—— 汇总里的
// "N<=2048 vs N>2048" 就是给这个断崖留的对照。
//
// 判定：
//   * 默认只打印 ms/次，**不因绝对耗时失败**。返回非 0 只可能是结构性自检挂了
//     （增量路重测了不该动的行 / 宽度没生效 / 缓存打冷了却没整份重建）。
//   * NEO_PERF_GUARD=1 时才启用相对护栏：ms/次 > 基线 × 1.5 判失败。基线来自
//     NEO_PERF_BASELINES（`key=ms;key=ms`）或 NEO_PERF_BASELINE_FILE（每行 key=ms，
//     `#` 起头为注释）。没有基线的 key 打印"跳过"、不判失败 —— 基线与机器强相关，
//     仓库里不存绝对值，因此**不硬编码任何毫秒阈值**。
//   * key 形如 `full/2000`、`decor/100000`、`resize/20000`。
//
// 无头写法参考 tests/unit/input_model.cpp（InputState + InputLayout::build）与
// 参考/tools/lp_probe/lp_plan_test.cpp。注意 InputLayout 只是持有 state.cachedLines
// 内部指针的轻量视图：**不要跨状态变更持有它** —— 本文件里它只在单次 build 的
// 作用域内使用，出作用域即弃。
//
// 运行：
//   ctest --test-dir build-win32 -C Release -L performance --output-on-failure
//   build-win32/Release/perf_benchmark.exe                     # 直接看数字
//   NEO_PERF_GUARD=1 NEO_PERF_BASELINES="full/2000=2.0;..." ... # 相对护栏

#include "components/input.h"
#include "components/input_model.h"

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
using Clock = std::chrono::steady_clock;

// ── 几何参数（全部无头：不需要窗口、不需要 GPU，测量只依赖 FreeType 字面）──
constexpr const char* kFontFamily = "monospace";
constexpr float kFontSize = 16.0f;
constexpr float kLineHeight = 19.2f;
constexpr float kInset = 10.0f;
constexpr float kViewportHeight = 600.0f;
constexpr float kWidth = 800.0f;         // full / decor 场景的视口宽
constexpr float kResizeWidthA = 720.0f;  // resize 场景在两个宽度间来回切
constexpr float kResizeWidthB = 760.0f;

constexpr int kDecorIterations = 100;    // 文档口径：光标换块 100 次
constexpr int kFullMaxIterations = 50;
constexpr int kResizeMaxIterations = 100;
constexpr int kMinIterations = 3;
constexpr double kBudgetMs = 2500.0;     // 单格时间预算（大 N 少跑几轮，ms/次 不受影响）

constexpr std::size_t kProbeLine = 1;    // 结构自检取样行（永远不会被点亮，也非标题行）

int g_failures = 0;

void fail(const std::string& message) {
    ++g_failures;
    std::cerr << "[perf] FAIL: " << message << "\n";
}

double msSince(Clock::time_point timePoint) {
    return std::chrono::duration<double, std::milli>(Clock::now() - timePoint).count();
}

// 每行内容都不同：shaping 缓存按文本做 key，行文重复会把 N>2048 的淘汰代价测没了。
// 行宽按最坏字体（0.6em ASCII + 1.125 倍标题字号）也压在 620px 以内，两个 resize
// 宽度下都不软换行 —— 行数恒等于 lineCount，自检才有确定的基准。
std::string makeDocument(int lineCount) {
    std::string text;
    text.reserve(static_cast<std::size_t>(lineCount) * 56);
    for (int i = 0; i < lineCount; ++i) {
        text += std::to_string(i);
        text += " 行示例：sample line for layout measure ";
        text += std::to_string(i * 31 % 100000);
        if (i + 1 < lineCount) {
            text += '\n';
        }
    }
    return text;
}

// 逐行装饰：每 10 行一个"标题块"（字号/行高/底色同时变），让全量与增量两条路
// 都有真实的逐行差异可算（光标换块在真实 Live Preview 里就是这种块级差异）。
std::vector<Decoration> makeDecorations(int lineCount) {
    std::vector<Decoration> decorations(static_cast<std::size_t>(lineCount));
    for (int i = 0; i < lineCount; i += 10) {
        Decoration& heading = decorations[static_cast<std::size_t>(i)];
        heading.fontSize = 18.0f;
        heading.lineHeight = 27.0f;
        heading.box.background = core::Color{0.12f, 0.18f, 0.30f, 0.35f};
    }
    return decorations;
}

// 取样行的 caret 缓冲地址（转成整数立刻脱手）：增量路靠 move 复用旧 TextLine（值不变），
// 全量路新分配（值必变 —— 旧缓冲在 measureLines 返回前一直活着）。
// 存整数而不是 const float*：InputLayout/InputState 的内部缓冲随状态变更即失效，
// 只把地址当"身份令牌"比较、从不解引用 —— 绝不跨状态持有内部指针。
std::uintptr_t caretToken(const Model::InputState& state, std::size_t line) {
    if (line >= state.cachedLines.size()) {
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(state.cachedLines[line].metrics.caretX.data());
}

struct Measurement {
    std::string key;
    int lines = 0;
    int iters = 0;
    double totalMs = 0.0;
    double minMs = 0.0;
    double maxMs = 0.0;

    double msPerOp() const { return iters > 0 ? totalMs / iters : 0.0; }
};

// body(iter)：iter == -1 是不计时的预热轮（字体加载 + 首轮 shaping 不该进基线），
// iter >= 0 为计时轮。每轮内部允许有纳秒~亚微秒级的准备动作（翻一个标志位、换一处
// 装饰、切一个宽度），相对毫秒级的排版开销可忽略，统一计入。
template <typename Body>
Measurement measure(const std::string& key, int lines, int minIters, int maxIters,
                    double budgetMs, Body&& body) {
    Measurement result;
    result.key = key;
    result.lines = lines;
    result.minMs = -1.0;

    body(-1);

    for (int iter = 0; iter < maxIters; ++iter) {
        const Clock::time_point start = Clock::now();
        body(iter);
        const double ms = msSince(start);
        result.totalMs += ms;
        ++result.iters;
        if (result.minMs < 0.0 || ms < result.minMs) {
            result.minMs = ms;
        }
        if (ms > result.maxMs) {
            result.maxMs = ms;
        }
        if (result.iters >= minIters && result.totalMs >= budgetMs) {
            break;
        }
    }
    return result;
}

void printMeasurement(const Measurement& measurement) {
    std::cout << "[perf] " << measurement.key << "  lines=" << measurement.lines
              << "  iters=" << measurement.iters
              << "  total_ms=" << measurement.totalMs
              << "  ms/op=" << measurement.msPerOp()
              << "  (min " << measurement.minMs << ", max " << measurement.maxMs << ")\n";
    // 机器可读行：直接贴进 NEO_PERF_BASELINES 就是基线。
    std::cout << measurement.key << "=" << measurement.msPerOp() << "\n";
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

// MSVC 把 getenv 标成 C4996（要求 _dupenv_s），GCC/Clang 没这个问题；
// 这里包一层，避免在 CMake 里加 _CRT_SECURE_NO_WARNINGS 去污染整个目标的编译选项。
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
            std::cout << "[perf] 注意：NEO_PERF_BASELINE_FILE 打不开（" << baselineFile
                      << "），该文件里的基线不参与护栏\n";
        }
    }
    return baselines;
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

int main() {
    const bool guardEnabled = environmentValue("NEO_PERF_GUARD") == "1";
    const std::map<std::string, double> baselines = loadBaselines();

#ifdef NDEBUG
    const char* buildKind = "Release";
#else
    const char* buildKind = "Debug(未优化，数字仅供参考)";
#endif

    std::cout << "[perf] === T12 无头性能基准 (" << buildKind << ") ===\n";
    if (guardEnabled) {
        std::cout << "[perf] NEO_PERF_GUARD=1：相对护栏开启（阈值 = 基线 × 1.5），已提供基线 "
                  << baselines.size() << " 条\n";
        if (baselines.empty()) {
            std::cout << "[perf] 注意：一条基线都没给（NEO_PERF_BASELINES / "
                         "NEO_PERF_BASELINE_FILE），护栏本次不会拦任何东西\n";
        }
    } else {
        std::cout << "[perf] 护栏关闭：只打印 ms/次，不因绝对耗时失败"
                     "（NEO_PERF_GUARD=1 才按基线 × 1.5 判失败）\n";
    }
    std::cout << "[perf] 规模 2000 / 20000 / 100000 行；shaping 缓存容量 2048 → "
                 "2000 属 N<=2048，其余属 N>2048\n";

    const std::vector<int> sizes{2000, 20000, 100000};
    std::vector<Measurement> results;

    for (int lineCount : sizes) {
        const std::string text = makeDocument(lineCount);
        const std::vector<Decoration> baseDecorations = makeDecorations(lineCount);

        // ── 场景 full：打冷缓存 → 全量 ensureLayoutCache ──
        {
            Model::InputState state;
            state.text = text;
            state.textRevision = 1;
            std::uintptr_t lastProbe = 0;
            auto body = [&](int iter) {
                state.layoutCacheValid = false;  // 冷路径：强制整份重排
                Model::ensureLayoutCache(state, kFontFamily, kFontSize, kWidth, true,
                                         &baseDecorations);
                if (iter == -1 && state.cachedLines.size() < static_cast<std::size_t>(lineCount)) {
                    fail("full/" + std::to_string(lineCount) + ": 行数少于源行（丢了行）");
                }
                const std::uintptr_t probe = caretToken(state, kProbeLine);
                if (probe == 0) {
                    fail("full: 取样行没有 caretX，无法做结构自检");
                } else if (iter >= 0 && probe == lastProbe) {
                    fail("full/" + std::to_string(lineCount) +
                         ": 缓存打冷后取样行 caretX 地址没变，全量重排没有真正发生");
                }
                lastProbe = probe;
            };
            results.push_back(measure("full/" + std::to_string(lineCount), lineCount,
                                      kMinIterations, kFullMaxIterations, kBudgetMs, body));
        }

        // ── 场景 decor：光标换块 → updateChangedDecorations 增量路 ──
        {
            Model::InputState state;
            state.text = text;
            state.textRevision = 1;
            std::vector<Decoration> decorations = baseDecorations;  // 可变工作副本
            Model::ensureLayoutCache(state, kFontFamily, kFontSize, kWidth, true, &decorations);
            if (state.cachedLines.size() < static_cast<std::size_t>(lineCount)) {
                fail("decor/" + std::to_string(lineCount) + ": 行数少于源行（丢了行）");
            }
            std::uintptr_t lastProbe = 0;
            // 停靠行：远隔开、避开取样行 1；碰上标题行也没关系（高亮盖在标题底色上）。
            const std::vector<std::size_t> stops{
                static_cast<std::size_t>(lineCount / 4),
                static_cast<std::size_t>(lineCount / 2),
                static_cast<std::size_t>(lineCount * 3 / 4),
                static_cast<std::size_t>(lineCount - 1),
            };
            constexpr std::size_t kNoStop = static_cast<std::size_t>(-1);
            std::size_t litStop = kNoStop;
            std::size_t nextStop = 0;
            const core::Color highlight{0.2f, 0.45f, 0.9f, 0.35f};
            auto body = [&](int iter) {
                if (litStop != kNoStop) {
                    decorations[litStop] = baseDecorations[litStop];  // 熄灭上一处
                }
                const std::size_t stop = stops[nextStop];
                nextStop = (nextStop + 1) % stops.size();
                decorations[stop] = baseDecorations[stop];
                decorations[stop].box.background = highlight;  // 点亮下一处
                litStop = stop;

                Model::ensureLayoutCache(state, kFontFamily, kFontSize, kWidth, true,
                                         &decorations);
                const std::uintptr_t probe = caretToken(state, kProbeLine);
                if (probe == 0) {
                    fail("decor: 取样行没有 caretX，无法做结构自检");
                } else if (iter >= 0 && probe != lastProbe) {
                    // 增量路对未变行是 std::move 复用（地址不变）；地址变了说明掉进了
                    // 全量重排（updateChangedDecorations 返回 false 的回退），路径测错了。
                    fail("decor/" + std::to_string(lineCount) +
                         ": 取样行 caretX 地址变了 —— 增量路没有生效（疑似回退全量）");
                }
                lastProbe = probe;
            };
            results.push_back(measure("decor/" + std::to_string(lineCount), lineCount,
                                      kDecorIterations, kDecorIterations,
                                      1e9, body));
        }

        // ── 场景 resize：textWidth 连续变化 → 每帧 InputLayout::build ──
        {
            Model::InputState state;
            state.text = text;
            state.textRevision = 1;
            std::uintptr_t lastProbe = 0;
            auto body = [&](int iter) {
                const float width = (iter % 2 == 0) ? kResizeWidthA : kResizeWidthB;
                // layout 只在本作用域内用：它持 state.cachedLines 的内部指针，
                // 下一轮 state 一变就是悬垂指针 —— 绝不带出作用域。
                const Model::InputLayout layout = Model::InputLayout::build(
                    state, width, kViewportHeight, width, kInset, kInset, kInset, kLineHeight,
                    kFontFamily, kFontSize, true, &baseDecorations);
                if (iter == -1) {
                    if (state.cachedLines.size() < static_cast<std::size_t>(lineCount)) {
                        fail("resize/" + std::to_string(lineCount) + ": 行数少于源行（丢了行）");
                    }
                    lastProbe = caretToken(state, kProbeLine);
                    return;
                }
                if (std::fabs(state.cachedViewportWidth - width) > 0.001f) {
                    fail("resize/" + std::to_string(lineCount) + ": 宽度没生效");
                }
                const std::uintptr_t probe = caretToken(state, kProbeLine);
                if (probe == 0) {
                    fail("resize: 取样行没有 caretX，无法做结构自检");
                } else if (probe == lastProbe) {
                    fail("resize/" + std::to_string(lineCount) +
                         ": 宽度变了取样行地址却没变，全量重排没有真正发生");
                }
                lastProbe = probe;
                if (!(layout.contentHeight > 0.0f)) {
                    fail("resize/" + std::to_string(lineCount) + ": contentHeight 异常");
                }
            };
            results.push_back(measure("resize/" + std::to_string(lineCount), lineCount,
                                      kMinIterations, kResizeMaxIterations, kBudgetMs, body));
        }
    }

    for (const Measurement& measurement : results) {
        printMeasurement(measurement);
    }

    // ── 汇总 ──
    std::cout << "[perf] ---- ms/次 汇总 ----\n";
    std::cout << "[perf] scenario    2000        20000       100000\n";
    for (const char* scenario : {"full", "decor", "resize"}) {
        std::cout << "[perf] " << scenario;
        for (int lineCount : sizes) {
            const Measurement* measurement =
                findResult(results, std::string(scenario) + "/" + std::to_string(lineCount));
            if (measurement != nullptr) {
                std::cout << "    " << measurement->msPerOp();
            } else {
                std::cout << "    -";
            }
        }
        std::cout << "\n";
    }

    // ── N<=2048 vs N>2048 对比（shaping 缓存容量 2048 的断崖）──
    std::cout << "[perf] ---- N<=2048 vs N>2048（shaping 缓存容量 2048）----\n";
    for (const char* scenario : {"full", "decor", "resize"}) {
        const Measurement* at2k = findResult(results, std::string(scenario) + "/2000");
        const Measurement* at20k = findResult(results, std::string(scenario) + "/20000");
        const Measurement* at100k = findResult(results, std::string(scenario) + "/100000");
        if (at2k == nullptr || at20k == nullptr || at100k == nullptr) {
            continue;
        }
        std::cout << "[perf] " << scenario << ": 2000(N<=2048)=" << at2k->msPerOp()
                  << " ms | 20000(N>2048)=" << at20k->msPerOp() << " ms (x"
                  << at20k->msPerOp() / at2k->msPerOp() << ")"
                  << " | 100000(N>2048)=" << at100k->msPerOp() << " ms (x"
                  << at100k->msPerOp() / at2k->msPerOp() << ")\n";
    }

    // ── 相对护栏（仅 NEO_PERF_GUARD=1）──
    int guardFailures = 0;
    int guarded = 0;
    if (guardEnabled) {
        for (const Measurement& measurement : results) {
            const auto baseline = baselines.find(measurement.key);
            if (baseline == baselines.end()) {
                std::cout << "[guard] 跳过 " << measurement.key
                          << "：没有基线（NEO_PERF_BASELINES / NEO_PERF_BASELINE_FILE）\n";
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

    // ── 基线速贴：把本次数字串起来，可直接当 NEO_PERF_BASELINES 用 ──
    std::cout << "[perf] 本次基线速贴（NEO_PERF_BASELINES）：";
    for (std::size_t index = 0; index < results.size(); ++index) {
        if (index > 0) {
            std::cout << ";";
        }
        std::cout << results[index].key << "=" << results[index].msPerOp();
    }
    std::cout << "\n";

    if (g_failures > 0) {
        std::cerr << "[perf] 结构性自检失败 " << g_failures << " 处\n";
        return 1;
    }
    if (guardFailures > 0) {
        std::cerr << "[perf] 相对护栏超标 " << guardFailures << " 处（相对基线 ×1.5）\n";
        return 30;
    }
    return 0;
}
