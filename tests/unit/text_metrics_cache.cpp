// T1 双层文本度量缓存（core/render/text.cpp）结构性单测。
//
// 只做结构性断言，**不拿绝对耗时当判据**（性能验收一律走 T12 perf_benchmark）：
//   1) 缓存开 / 关：width、byteIndices、caretX 逐项等价（关 = NEO_TEXT_CACHE_OFF=1）；
//   2) 重复调用命中：命中 / 未命中计数器按预期推进（仅测试计数器，见 text.cpp 文末钩子）；
//   3) 唯一文本灌过 2048 条之后，两层的条目数与估算字节仍在各自上限内（且确实淘汰过）；
//   4) clear 之后同一条文本重新 miss（外层、内层都是）；
//   5) NEO_TEXT_CACHE_OFF=1 时两层既不读也不写（计数、条目全 0），结果仍等价；
//   6) 并发：4 线程同时测量 + 1 线程清缓存，结果与单线程参考逐项一致（锁真的在起作用）；
//   7) 与 setDefaultFontFiles 并发：4 线程测量 + 1 线程翻默认字体文件，每次结果
//      必须落在「空覆盖 / 字体 A / 字体 B」三个合法候选之内且结构合法（**不断言
//      每轮逐位一致** —— 换字体本来就可能换结果），全程不崩；
//   8) setDefaultFontFiles 换默认字体：两层缓存立即清空（条目 + 字节归零）、
//      同一文本重新从 0 计 miss，且度量换成新字体的结果；
//   9) 错误路径 fallback → 随后路径可用：把真字体复制到一个原先不存在的路径后，
//      不清缓存重测不得返回旧 fallback 字形（外层键 = 实际字体 + 字体栈指纹失效）；
//   10) setDefaultFontFiles 的写侧与测量**共锁**的确定性判定（探针持锁期间写入
//      必须被挡住）—— 压力测试抓不住那么窄的竞态窗口，这条补上硬证据。
//
// 计数器 / 上限 / 清缓存都走 core/render/text.cpp 末尾的 extern "C" 测试钩子 ——
// 钩子不进 text.h，不占公开 API（本文件自己 extern 声明）。
//
// 运行：ctest --test-dir build-win32 -C Release -R text_metrics_cache --output-on-failure

#include "core/render/text.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// 与 core/render/text.cpp 末尾「T1 测试钩子」一一对应。
extern "C" {
std::size_t neoTextCacheMetricEntries();
std::size_t neoTextCacheMetricBytes();
std::size_t neoTextCacheMetricLimitEntries();
std::size_t neoTextCacheMetricLimitBytes();
std::size_t neoTextCacheMetricHits();
std::size_t neoTextCacheMetricMisses();
std::size_t neoTextCacheShapingEntries();
std::size_t neoTextCacheShapingBytes();
std::size_t neoTextCacheShapingLimitEntries();
std::size_t neoTextCacheShapingLimitBytes();
std::size_t neoTextCacheShapingHits();
std::size_t neoTextCacheShapingMisses();
void neoTextCacheResetStats();
void neoTextCacheClear();
void neoTextCacheReloadEnv();
void neoTextCacheDefaultFontPathAfterDelayForTest(int milliseconds,
                                                  std::atomic<bool>* lockAcquired,
                                                  std::string* resolvedPath);
}

using core::TextPrimitive;

namespace {

int g_failures = 0;

void fail(const std::string& message) {
    ++g_failures;
    std::cerr << "[text_metrics_cache] FAIL: " << message << "\n";
}

// Windows 下 CRT 的 getenv 只认 _putenv_s 写过的值（SetEnvironmentVariable 改不动它）；
// 传空串 = 删除该变量。每次都顺带 reload，让 text.cpp 下一次测量重新读环境。
void setCacheEnv(const char* value) {
#ifdef _WIN32
    _putenv_s("NEO_TEXT_CACHE_OFF", value);
#else
    if (value[0] != '\0') {
        setenv("NEO_TEXT_CACHE_OFF", value, 1);
    } else {
        unsetenv("NEO_TEXT_CACHE_OFF");
    }
#endif
    neoTextCacheReloadEnv();
}

bool sameMetrics(const TextPrimitive::TextMetrics& left, const TextPrimitive::TextMetrics& right) {
    // 全程精确比较：两条路（缓存命中 / 现算）必须产出逐位一致的结果。
    return left.width == right.width && left.byteIndices == right.byteIndices &&
           left.caretX == right.caretX;
}

TextPrimitive::TextMetrics measure(const std::string& text,
                                   const std::string& family = {},
                                   float size = 16.0f,
                                   int weight = 400) {
    return TextPrimitive::measureTextMetrics(text, family, size, weight);
}

// 每条都不同（序号打头），且混排中英文：caret 数组两支都非空、条目字节才有代表性。
// 图案固定 20 字节，避免 resize 截断到多字节字符中间。
std::string makeUniqueText(int index, int chunks) {
    const std::string prefix = "L" + std::to_string(index) + ":";
    std::string text = prefix;
    text.reserve(prefix.size() + static_cast<std::size_t>(chunks) * 20u);
    for (int i = 0; i < chunks; ++i) {
        text += "\xe8\xa1\x8c text 0123456789 ";  // "行 text 0123456789 "
    }
    return text;
}

// 产品已不再携带仓库字体（assets 只剩应用图标，界面走 Windows 系统字体），
// 并发/覆盖用例的候选字体改用系统字体文件。返回两份**互不相同**的现有字体
// 绝对路径（度量必然不同），找不到两份时为空。
std::vector<std::string> findSystemFontCandidates() {
    const char* candidates[] = {
        "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/times.ttf", "C:/Windows/Fonts/seguisym.ttf",
    };
    std::vector<std::string> found;
    for (const char* path : candidates) {
        std::error_code error;
        if (!std::filesystem::exists(path, error) || error) {
            continue;
        }
        found.push_back(path);
        if (found.size() == 2) {
            break;
        }
    }
    return found;
}

// 结构合法性：首停在 0、末停在文本末尾、两支数组同长，索引与 caret 都不回退。
// 并发用例拿它兜住「读到撕裂的 override → 结果烂掉」这一类失败。
bool validMetrics(const TextPrimitive::TextMetrics& metrics, const std::string& text) {
    if (metrics.byteIndices.empty() ||
        metrics.byteIndices.size() != metrics.caretX.size()) {
        return false;
    }
    if (metrics.byteIndices.front() != 0 || metrics.caretX.front() != 0.0f) {
        return false;
    }
    if (metrics.byteIndices.back() != static_cast<int>(text.size())) {
        return false;
    }
    if (!(metrics.width >= 0.0f)) {
        return false;
    }
    for (std::size_t index = 1; index < metrics.byteIndices.size(); ++index) {
        if (metrics.byteIndices[index] < metrics.byteIndices[index - 1]) {
            return false;
        }
        if (metrics.caretX[index] < metrics.caretX[index - 1]) {
            return false;
        }
    }
    return true;
}

bool inCandidates(const TextPrimitive::TextMetrics& got,
                  const std::vector<TextPrimitive::TextMetrics>& candidates) {
    for (const TextPrimitive::TextMetrics& candidate : candidates) {
        if (sameMetrics(got, candidate)) {
            return true;
        }
    }
    return false;
}

void checkBounds(const char* where) {
    if (neoTextCacheMetricEntries() > neoTextCacheMetricLimitEntries()) {
        fail(std::string(where) + ": 外层条目数超上限 " +
             std::to_string(neoTextCacheMetricEntries()));
    }
    if (neoTextCacheMetricBytes() > neoTextCacheMetricLimitBytes()) {
        fail(std::string(where) + ": 外层估算字节超预算 " +
             std::to_string(neoTextCacheMetricBytes()));
    }
    if (neoTextCacheShapingEntries() > neoTextCacheShapingLimitEntries()) {
        fail(std::string(where) + ": 内层条目数超上限 " +
             std::to_string(neoTextCacheShapingEntries()));
    }
    if (neoTextCacheShapingBytes() > neoTextCacheShapingLimitBytes()) {
        fail(std::string(where) + ": 内层估算字节超预算 " +
             std::to_string(neoTextCacheShapingBytes()));
    }
}

// ── 1) 缓存开 / 关前后，width + caret 数组逐项等价 ──
void testEquivalence() {
    struct Sample {
        const char* label;
        const char* family;
        float size;
        int weight;
        std::string text;
    };
    const std::vector<Sample> samples = {
        {"空串", "", 16.0f, 400, ""},
        {"ASCII", "", 16.0f, 400, "Hello EUI-Edits, measure twice!"},
        {"CJK", "", 16.0f, 400, "\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe9\x87\x8f\xe6\x96\x87\xe6\x9c\xac"},
        {"中英混排", "", 16.0f, 400, "T1 \xe7\xbc\x93\xe5\xad\x98 abc 0123456789 -- \xe5\x85\x83\xe7\xb4\xa0"},
        {"换行/tab", "", 16.0f, 400, "first line\ttab\nsecond line\r\nthird"},
        {"大字号", "", 40.0f, 400, "size 40 \xe5\xad\x97\xe5\x8f\xb7"},
        {"粗体", "", 16.0f, 700, "bold weight \xe7\xb2\x97\xe4\xbd\x93"},
        {"monospace", "monospace", 16.0f, 400, "const int value = 42; // \xe7\xad\x89\xe5\xae\xbd"},
        {"长段落", "", 16.0f, 400, makeUniqueText(77777, 40)},
    };

    for (const Sample& sample : samples) {
        setCacheEnv("");  // 开：冷读一次，再读一次应命中
        neoTextCacheClear();
        neoTextCacheResetStats();

        const TextPrimitive::TextMetrics cold = measure(sample.text, sample.family, sample.size, sample.weight);
        const TextPrimitive::TextMetrics warm = measure(sample.text, sample.family, sample.size, sample.weight);
        if (!sameMetrics(cold, warm)) {
            fail(std::string(sample.label) + ": 命中路径与冷算结果不一致");
        }

        setCacheEnv("1");  // 关：整条链现算，必须与开的时候逐位一致
        const TextPrimitive::TextMetrics off = measure(sample.text, sample.family, sample.size, sample.weight);
        if (!sameMetrics(off, cold)) {
            fail(std::string(sample.label) + ": NEO_TEXT_CACHE_OFF=1 与缓存开启结果不一致");
        }
        setCacheEnv("");
    }

    // 空串走的是 early return，不进任何一层（计数保持 0）。
    neoTextCacheClear();
    neoTextCacheResetStats();
    measure("");
    if (neoTextCacheMetricHits() != 0 || neoTextCacheMetricMisses() != 0 ||
        neoTextCacheShapingHits() != 0 || neoTextCacheShapingMisses() != 0) {
        fail("空文本不该进任何一层缓存");
    }
}

// ── 2) 重复调用命中：外层由重复 measure 命中，内层由渲染路（TextPrimitive）命中 ──
void testHitCounters() {
    setCacheEnv("");
    neoTextCacheClear();
    neoTextCacheResetStats();

    const std::string text = "T1 cache probe \xe5\xba\x95\xe5\xb1\x82\xe5\x91\xbd\xe4\xb8\xad 0123456789";

    const TextPrimitive::TextMetrics first = measure(text);
    if (neoTextCacheMetricMisses() != 1 || neoTextCacheMetricHits() != 0) {
        fail("首次测量应恰好 1 次外层 miss、0 次 hit");
    }
    const TextPrimitive::TextMetrics second = measure(text);
    if (neoTextCacheMetricHits() != 1 || neoTextCacheMetricMisses() != 1) {
        fail("重复测量应变成 1 次 hit，miss 数不变");
    }
    if (!sameMetrics(first, second)) {
        fail("命中路径返回的 metrics 与冷算不一致");
    }

    // 渲染路（rebuildLayout → shapeText）走的是**内层**塑形缓存：
    // 刚才那次外层 miss 已经把 (holder, text) 写进内层，这里必须命中。
    TextPrimitive primitive;
    if (!primitive.initialize()) {
        fail("TextPrimitive::initialize 失败");
        return;
    }
    primitive.setFontSize(16.0f);
    primitive.setText(text);
    primitive.prepare();
    if (neoTextCacheShapingHits() < 1) {
        fail("内层塑形缓存没有被渲染路命中（shapeText 没走缓存？）");
    }
    if (neoTextCacheMetricHits() != 1) {
        fail("渲染路不该动外层 TextMetrics 缓存");
    }
}

// ── 3) 唯一文本超过 2048 条后，两层的条目 / 字节仍有界（且确实淘汰过）──
void testBounded() {
    setCacheEnv("");
    neoTextCacheClear();
    neoTextCacheResetStats();

    // 每条约 190 字节文本：外层按字节预算（16 MiB）先生效，条目数落在
    // 2048 与注入数之间 —— 正好把"超 2048 之后仍然有界"这件事测实。
    constexpr int kUniqueTexts = 15000;
    constexpr int kChunks = 9;
    for (int index = 0; index < kUniqueTexts; ++index) {
        measure(makeUniqueText(index, kChunks));
        if (index % 2500 == 0) {
            checkBounds("注入中");
        }
    }
    checkBounds("注入后");

    if (neoTextCacheMetricMisses() != static_cast<std::size_t>(kUniqueTexts) ||
        neoTextCacheMetricHits() != 0) {
        fail("全唯一文本应当全 miss、零 hit");
    }
    if (neoTextCacheMetricEntries() >= static_cast<std::size_t>(kUniqueTexts)) {
        fail("外层没有发生淘汰（条目数等于注入数）");
    }
    if (neoTextCacheMetricEntries() < 2048u) {
        fail("外层条目数异常偏低（预算/上限配比可能被改坏了）");
    }
}

// ── 4) clear 之后重新 miss ──
void testClearMiss() {
    setCacheEnv("");
    neoTextCacheClear();
    neoTextCacheResetStats();

    const std::string text = "clear probe \xe6\xb8\x85\xe7\xbc\x93\xe5\xad\x98\xe5\x90\x8e\xe5\xbf\x85\xe9\xa1\xbb miss";
    measure(text);
    measure(text);
    if (neoTextCacheMetricHits() != 1) {
        fail("第二次测量没命中");
    }

    neoTextCacheClear();
    neoTextCacheResetStats();
    if (neoTextCacheMetricEntries() != 0 || neoTextCacheShapingEntries() != 0) {
        fail("clear 之后两层都必须空");
    }
    if (neoTextCacheMetricBytes() != 0 || neoTextCacheShapingBytes() != 0) {
        fail("clear 之后两层字节计数都必须归零");
    }

    measure(text);
    if (neoTextCacheMetricMisses() != 1 || neoTextCacheMetricHits() != 0) {
        fail("clear 之后同一文本必须重新 miss");
    }
    if (neoTextCacheShapingMisses() != 1 || neoTextCacheShapingHits() != 0) {
        fail("clear 之后内层同样必须 miss");
    }
}

// ── 5) NEO_TEXT_CACHE_OFF=1 关掉两层：不读、不写、结果仍等价 ──
void testEnvSwitch() {
    const std::string text = "env switch \xe5\xbc\x80\xe5\x85\xb3 probe 9876543210";

    setCacheEnv("");
    neoTextCacheClear();
    neoTextCacheResetStats();
    const TextPrimitive::TextMetrics on1 = measure(text);
    const TextPrimitive::TextMetrics on2 = measure(text);
    if (neoTextCacheMetricHits() != 1) {
        fail("开关开着时第二次测量应当命中");
    }

    setCacheEnv("1");
    neoTextCacheClear();
    neoTextCacheResetStats();
    const TextPrimitive::TextMetrics off1 = measure(text);
    const TextPrimitive::TextMetrics off2 = measure(text);

    if (neoTextCacheMetricHits() != 0 || neoTextCacheMetricMisses() != 0) {
        fail("NEO_TEXT_CACHE_OFF=1 时外层不该读也不该计数");
    }
    if (neoTextCacheShapingHits() != 0 || neoTextCacheShapingMisses() != 0) {
        fail("NEO_TEXT_CACHE_OFF=1 时内层不该读也不该计数");
    }
    if (neoTextCacheMetricEntries() != 0 || neoTextCacheShapingEntries() != 0) {
        fail("NEO_TEXT_CACHE_OFF=1 时两层都不该写入");
    }
    if (!sameMetrics(off1, on1) || !sameMetrics(off2, on2)) {
        fail("NEO_TEXT_CACHE_OFF=1 前后结果不等价");
    }

    setCacheEnv("");  // 收尾恢复，别把开关状态留给后续用例
}

// ── 6) 并发：多线程测量 + 并发清缓存，结果必须与单线程参考逐项一致 ──
// 锁（core/render/text.cpp 的 textCacheMutex）是 T1 外层 LRU 的线程安全承诺；
// 这里同时压"测量 vs 测量"与"测量 vs 清缓存"两种竞争，只断言结果等价，不断言耗时。
void testConcurrent() {
    setCacheEnv("");
    neoTextCacheClear();

    const std::vector<std::string> texts = {
        "concurrent A \xe5\xb9\xb6\xe5\x8f\x91 0123456789",
        "concurrent B \xe6\xb5\x8b\xe9\x87\x8f xyz XYZ",
        makeUniqueText(1, 9),
        makeUniqueText(2, 9),
        "short",
        std::string(400, 'x') + " \xe9\x95\xbf\xe4\xb8\xb2",
    };

    std::vector<TextPrimitive::TextMetrics> reference;
    reference.reserve(texts.size());
    for (const std::string& text : texts) {
        reference.push_back(measure(text));
    }

    std::atomic<int> mismatches{0};
    std::atomic<int> clearCount{0};
    std::atomic<bool> stopClearer{false};

    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&] {
            for (int round = 0; round < 200; ++round) {
                for (std::size_t index = 0; index < texts.size(); ++index) {
                    const TextPrimitive::TextMetrics got = measure(texts[index]);
                    if (!sameMetrics(got, reference[index])) {
                        mismatches.fetch_add(1);
                    }
                }
            }
        });
    }
    std::thread clearer([&] {
        while (!stopClearer.load()) {
            neoTextCacheClear();
            clearCount.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    for (std::thread& worker : workers) {
        worker.join();
    }
    stopClearer.store(true);
    clearer.join();

    if (mismatches.load() != 0) {
        fail("并发测量结果与单线程参考不一致 x" + std::to_string(mismatches.load()));
    }
    if (clearCount.load() == 0) {
        fail("清缓存线程一轮都没跑（测试写坏了）");
    }

    // 收尾：清干净再测一次，确认并发结束后缓存仍能正常工作。
    neoTextCacheClear();
    neoTextCacheResetStats();
    const TextPrimitive::TextMetrics after = measure(texts[0]);
    if (neoTextCacheMetricMisses() != 1 || !sameMetrics(after, reference[0])) {
        fail("并发收尾：清缓存后重测不正确");
    }
}

// ── 7) 与 setDefaultFontFiles 并发：每次结果必须属于合法候选，且不崩溃 ──
// T1 复审问题 1 的行为面：override 是裸 std::string，写侧（setDefaultFontFiles）
// 不与 measureTextMetrics 共锁，读线程会撞上写到一半的 string（崩溃 / 解析出
// 半截路径 / 结果漂移）。这里 4 个测量线程 + 1 个翻字体线程压同一份数据。
// **刻意不断言每轮逐位一致**：翻到不同字体时结果本来就可能不同，正确判据是
// 每次返回值都落在「空覆盖 / 字体 A / 字体 B」三个单线程参考值之内，外加结构
// 合法（进程崩溃本身就是失败）。锁序证明写在 text.cpp 的 setDefaultFontFiles。
void testConcurrentWithFontOverride() {
    setCacheEnv("");
    const auto systemFonts = findSystemFontCandidates();
    if (systemFonts.size() < 2) {
        fail("并发覆盖：系统字体候选不足两份，测不了翻字体");
        return;
    }
    const std::string fontA = systemFonts[0];
    const std::string fontB = systemFonts[1];
    const std::string text =
        "并发覆盖 \xe5\xad\x97\xe4\xbd\x93 abc 0123456789 \xe5\x9b\xba\xe5\xae\x9a 42";

    // 三个候选各自在「缓存 + 字体栈刚被整层清空」的条件下冷算：每支键对应的
    // holder 都是新建的、只量过这一条文本；并发阶段每次翻字体都会整层清空，
    // holder 重建后同样只量这一条文本 —— 两边字体栈历史一致，结果才可比。
    std::vector<TextPrimitive::TextMetrics> candidates;
    neoTextCacheClear();
    TextPrimitive::setDefaultFontFiles("", "");  // 起点：空覆盖（值相同 → 早返回，不清）
    candidates.push_back(measure(text));
    TextPrimitive::setDefaultFontFiles(fontA, "");
    candidates.push_back(measure(text));
    TextPrimitive::setDefaultFontFiles(fontB, "");
    candidates.push_back(measure(text));
    TextPrimitive::setDefaultFontFiles("", "");  // 回到起点，翻字体线程只在 A/B 间切
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (!validMetrics(candidates[index], text)) {
            fail("并发覆盖：候选 " + std::to_string(index) + " 结构非法");
        }
    }

    std::atomic<int> invalid{0};
    std::atomic<int> unknown{0};
    std::atomic<int> flips{0};
    std::atomic<bool> stop{false};

    // 测量线程按时间连跑（不是固定轮数）：每次 measure 都会读一次 override，
    // 读得越密，撞上写窗口的机会越大；每轮仍只做「结构合法 + 属于候选集」两项判定。
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&] {
            int measures = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const TextPrimitive::TextMetrics got = measure(text);
                if (!validMetrics(got, text)) {
                    invalid.fetch_add(1);
                } else if (!inCandidates(got, candidates)) {
                    unknown.fetch_add(1);
                }
                // 每 8 次测量歇一小会儿，把锁让出来给翻字体线程：Windows 的
                // CRITICAL_SECTION 不公平，4 个线程把 textCacheMutex 抓死的话
                // 写侧会被饥饿，"并发换字体"就名存实亡（读侧窗口照样密）。
                if ((++measures & 7) == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(300));
                }
            }
        });
    }

    // 翻字体线程：翻够 200 次或跑满 2.5s 就停（写侧高频）。每次翻转都要等
    // 4 个测量线程把手上的 FreeType 字体加载做完才拿得到锁，所以翻得快慢取决于
    // 机器快慢 —— 断言只取一个很稳的下限（本机 ~160 次 / 2.5s，留 5 倍余量），
    // 次数本身打出来供复审参考。
    // 抓不抓得到竞态窗口不作断言（窗口只有几十纳秒，见 text.cpp 里问题 1 的锁序
    // 证明 / 用例 10 的确定性判定），这里要的是持续压力 + 结果合法性。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    bool useFontA = true;
    while (flips.load() < 200 && std::chrono::steady_clock::now() < deadline) {
        TextPrimitive::setDefaultFontFiles(useFontA ? fontA : fontB, "");
        useFontA = !useFontA;
        flips.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    stop.store(true);
    for (std::thread& worker : workers) {
        worker.join();
    }
    std::cout << "[text_metrics_cache] 并发覆盖：翻字体 " << flips.load()
              << " 次，非法结构 " << invalid.load() << "，候选外结果 " << unknown.load() << "\n";

    if (invalid.load() != 0) {
        fail("并发覆盖：出现结构非法的结果 x" + std::to_string(invalid.load()));
    }
    if (unknown.load() != 0) {
        fail("并发覆盖：出现候选集之外的结果 x" + std::to_string(unknown.load()) +
             "（override 读写没共锁？）");
    }
    if (flips.load() < 30) {
        fail("翻字体线程翻得太少（" + std::to_string(flips.load()) +
             " 次，写侧没被真正并发压到）");
    }

    // 收尾：恢复空覆盖并整层清空，确认并发结束后缓存与默认字体都正常。
    TextPrimitive::setDefaultFontFiles("", "");
    neoTextCacheClear();
    neoTextCacheResetStats();
    const TextPrimitive::TextMetrics after = measure(text);
    if (neoTextCacheMetricMisses() != 1 || !sameMetrics(after, candidates[0])) {
        fail("并发覆盖收尾：恢复默认字体后的测量不正确");
    }
}

// ── 8) setDefaultFontFiles 换默认字体：两层清空、重新从 0 计 miss ──
// T1 验收 C（问题 1 的写侧在锁内 → 换字体与测量互斥）。覆盖一变，旧的
// (默认字体, 文本) 度量必须整层作废：同一文本重新 miss，且结果换成新字体的度量。
void testOverrideClearsCache() {
    setCacheEnv("");
    const auto systemFonts = findSystemFontCandidates();
    if (systemFonts.empty()) {
        fail("覆盖清缓存：系统里找不到候选字体");
        return;
    }
    const std::string fontA = systemFonts[0];
    const std::string text = "覆盖清缓存 \xe5\xad\x97\xe4\xbd\x93 abc 0123456789 \xe9\x87\x8d\xe6\x96\xb0\xe8\xae\xa1 miss";

    neoTextCacheClear();
    neoTextCacheResetStats();
    const TextPrimitive::TextMetrics before = measure(text);
    measure(text);
    if (neoTextCacheMetricHits() != 1 || neoTextCacheMetricMisses() != 1) {
        fail("覆盖清缓存：前置应为 1 次 miss + 1 次 hit");
    }

    // 新字体单独冷算一份参考：先整层清空，保证 fontA 的 holder 是新建的、只量过
    // 这一条文本；后面覆盖生效后的那次测量（setDefaultFontFiles 会再清一次）与
    // 它同历史，结果才逐位可比。
    neoTextCacheClear();
    const TextPrimitive::TextMetrics reference = measure(text, fontA);

    TextPrimitive::setDefaultFontFiles(fontA, "");
    if (neoTextCacheMetricEntries() != 0 || neoTextCacheShapingEntries() != 0 ||
        neoTextCacheMetricBytes() != 0 || neoTextCacheShapingBytes() != 0) {
        fail("setDefaultFontFiles 之后两层缓存没有清空");
    }

    neoTextCacheResetStats();  // 重新从 0 计数
    const TextPrimitive::TextMetrics after = measure(text);
    if (neoTextCacheMetricMisses() != 1 || neoTextCacheMetricHits() != 0) {
        fail("覆盖之后同一文本必须重新计 1 次 miss、0 次 hit");
    }
    if (!sameMetrics(after, reference)) {
        fail("覆盖之后的度量不是新字体的度量");
    }
    const TextPrimitive::TextMetrics afterWarm = measure(text);
    if (neoTextCacheMetricHits() != 1 || !sameMetrics(afterWarm, after)) {
        fail("覆盖之后第二次测量应当命中且结果一致");
    }
    // 覆盖确实换了结果；候选字体与默认字体度量恰好相同就跳过这条，只报 NOTE。
    if (!sameMetrics(reference, before)) {
        if (sameMetrics(after, before)) {
            fail("覆盖之后度量与覆盖前逐位相同（override 没生效？）");
        }
    } else {
        std::cerr << "[text_metrics_cache] NOTE: 候选字体与默认字体度量相同，覆盖差异断言跳过\n";
    }

    // 恢复空覆盖：值又变了，同样要触发一次整层清空。
    TextPrimitive::setDefaultFontFiles("", "");
    if (neoTextCacheMetricEntries() != 0 || neoTextCacheShapingEntries() != 0) {
        fail("恢复默认字体之后两层缓存没有清空");
    }
}

// ── 9) 错误路径 fallback → 随后路径可用：不得返回旧字形 ──
// T1 复审问题 2：外层键曾是**请求**路径。请求的字体文件加载失败、
// loadSharedFontStack fallback 到默认字体之后，这条 fallback 度量被写在请求
// 路径名下；等请求路径真的可用，请求键一命中就把旧字形还回来。
// 修法两半：① 落键用 holder 的实际字体路径（fallback 结果绝不落回请求路径）；
// ② 字体栈条目按请求路径当时的文件指纹（存在性 / 大小 / mtime）判定过期 →
// 丢弃重载 holder，实际字体路径这才真的换成新文件。
// 构造「动态字体文件」：拿一个**尚不存在**的临时路径当 fontFamily（解析会原样
// 返回它）→ 量到 fallback 结果；把一份真字体复制到该路径 → **不清缓存**重测。
void testFallbackRecovery() {
    setCacheEnv("");
    neoTextCacheClear();
    neoTextCacheResetStats();

    const std::string text =
        "fallback \xe6\x81\xa2\xe5\xa4\x8d abc 0123456789 \xe8\xb7\xaf\xe5\xbe\x84\xe5\x8f\xaf\xe7\x94\xa8";

    std::error_code error;
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path(error);
    if (error) {
        fail("fallback 恢复：取不到临时目录 " + error.message());
        return;
    }
    static long long uniqueCounter = 0;
    const std::filesystem::path pending = tempDir /
        ("neo_text_metrics_cache_pending_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "_" + std::to_string(++uniqueCounter) + ".ttf");
    error.clear();
    std::filesystem::remove(pending, error);  // 起点必须是「不存在」
    error.clear();

    const std::string pendingPath = pending.string();
    const TextPrimitive::TextMetrics fallback = measure(text, pendingPath);
    if (!validMetrics(fallback, text)) {
        fail("fallback 恢复：请求路径缺失时的 fallback 结果结构非法");
        return;
    }

    // 挑一份度量与 fallback **不同**的真字体当「稍后到位」的那份文件（这样
    // "换没换结果" 才判得出来）。
    std::string donor;
    TextPrimitive::TextMetrics donorReference;
    for (const std::string& candidate : findSystemFontCandidates()) {
        const TextPrimitive::TextMetrics metrics = measure(text, candidate);
        if (!sameMetrics(metrics, fallback)) {
            donor = candidate;
            donorReference = metrics;
            break;
        }
    }
    if (donor.empty()) {
        // 构造不出来时不硬断言：明确记「未测」并把现场清干净。
        std::cerr << "[text_metrics_cache] NOTE: fallback→路径可用 未测"
                     "（候选字体与 fallback 度量相同，换不出差异）\n";
        std::filesystem::remove(pending, error);
        return;
    }

    std::filesystem::copy_file(donor, pending,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        fail("fallback 恢复：复制字体到临时路径失败 " + error.message());
        std::filesystem::remove(pending, error);
        return;
    }

    // 关键：**不清缓存**（只重置计数）—— 旧的 fallback 条目必须靠失效策略自己
    // 让位；清了缓存这道题就白出（哪版实现都能过）。
    neoTextCacheResetStats();
    const TextPrimitive::TextMetrics recovered = measure(text, pendingPath);
    if (sameMetrics(recovered, fallback)) {
        fail("fallback 恢复：路径可用后仍返回旧 fallback 字形");
    }
    if (!sameMetrics(recovered, donorReference)) {
        fail("fallback 恢复：路径可用后的度量与该字体单独量出的结果不一致");
    }
    if (neoTextCacheMetricMisses() != 1 || neoTextCacheMetricHits() != 0) {
        fail("fallback 恢复：恢复那次测量应当恰好 1 次外层 miss（旧条目不得直接命中）");
    }
    const TextPrimitive::TextMetrics recoveredWarm = measure(text, pendingPath);
    if (neoTextCacheMetricHits() != 1 || !sameMetrics(recoveredWarm, recovered)) {
        fail("fallback 恢复：恢复之后第二次测量应当命中且结果一致");
    }

    // 收尾：先清缓存（释放仍持着临时文件句柄的 holder），Windows 下句柄没关掉
    // 文件删不掉。删不掉只记 NOTE，判定已经结束。
    neoTextCacheClear();
    neoTextCacheResetStats();
    error.clear();
    std::filesystem::remove(pending, error);
    if (error || std::filesystem::exists(pending)) {
        std::cerr << "[text_metrics_cache] NOTE: 临时字体文件未能删除：" << pending.string() << "\n";
    }
}

// ── 10) setDefaultFontFiles 的写侧与测量共锁（问题 1 的确定性判定）────────
// 用例 7 是压力测试：竞态窗口只有几十纳秒，跑一秒钟也未必撞得上，"没崩"并不
// 等于"锁对了"。这里用测试钩子把判定变成时序上确定的一次比较：
//   探针线程先**持锁** 250ms，再在锁内读一次解析后的默认字体路径；
//   主线程确认探针确实拿到锁之后，发起 setDefaultFontFiles(fontA)。
//   * 写侧在锁内（修复后）：换字体被挡到锁释放之后 → 探针读到**旧**路径；
//   * 写侧在锁外（修复前）：sleep 期间新值已写进探针的读取点 → 读到**新**路径。
// 两个路径必然不同（fontA 是存在的绝对路径，空覆盖解析到仓库自带默认字体）。
void testOverrideWriteSharesLock() {
    setCacheEnv("");
    const auto systemFonts = findSystemFontCandidates();
    if (systemFonts.empty()) {
        fail("写侧共锁：系统里找不到候选字体");
        return;
    }
    const std::string fontA = systemFonts[0];

    TextPrimitive::setDefaultFontFiles("", "");  // 起点：空覆盖
    std::string baseline;
    neoTextCacheDefaultFontPathAfterDelayForTest(0, nullptr, &baseline);
    if (baseline.empty()) {
        fail("写侧共锁：空覆盖下解析不到默认字体路径");
        return;
    }
    if (baseline == fontA) {
        fail("写侧共锁：候选字体与默认字体解析成同一路径，测不出差异");
        return;
    }

    std::atomic<bool> probeHolding{false};
    std::string duringLock;
    std::thread probe([&] {
        neoTextCacheDefaultFontPathAfterDelayForTest(250, &probeHolding, &duringLock);
    });

    // 等探针真的把锁拿住再发起写入（否则线程调度慢一拍会把用例判成假失败）。
    const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!probeHolding.load() && std::chrono::steady_clock::now() < waitDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool gotLock = probeHolding.load();
    if (gotLock) {
        TextPrimitive::setDefaultFontFiles(fontA, "");  // 共锁 → 这里会阻塞到探针放锁
    }
    probe.join();
    if (!gotLock) {
        fail("写侧共锁：探针线程 5s 内没拿到 textCacheMutex（锁被别人占死？）");
        return;
    }

    if (duringLock != baseline) {
        fail("setDefaultFontFiles 在别人持锁期间就改掉了 override —— 写侧没与测量共锁"
             "（探针读到 " + duringLock + "）");
    }
    std::string after;
    neoTextCacheDefaultFontPathAfterDelayForTest(0, nullptr, &after);
    if (after != fontA) {
        fail("写侧共锁：锁释放后换字体没有生效（解析到 " + after + "）");
    }

    TextPrimitive::setDefaultFontFiles("", "");  // 恢复空覆盖
}

// ── 11) width-only 度量不污染完整 caret metrics 缓存 ──────────────────────
// 宽度 API 和完整 metrics 必须给出同一结果，但仅量宽不能把 byteIndices / caretX
// 留空后塞进共享的完整 metrics 缓存；随后完整调用必须仍得到可用 caret 停靠点。
void testWidthOnlyMetrics() {
    struct Sample {
        const char* label;
        std::string family;
        float size;
        int weight;
        std::string text;
    };
    const auto uniqueStamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path missingFont = std::filesystem::temp_directory_path() /
        ("EUI-Edits-missing-width-only-font-" + std::to_string(uniqueStamp) + ".ttf");
    std::error_code error;
    std::filesystem::remove(missingFont, error);
    const std::vector<Sample> samples = {
        {"空串", {}, 16.0f, 400, ""},
        {"ASCII", {}, 14.0f, 400, "width-only ASCII 0123456789"},
        {"中文", {}, 16.0f, 400, "中文宽度与光标停靠"},
        {"emoji", {}, 18.0f, 400, "emoji 😀 🧪🚀"},
        {"粗体", {}, 16.0f, 700, "Bold 中文 42"},
        {"缺失字体fallback", missingFont.string(), 16.0f, 400, "fallback 字宽 😀"},
    };
    const float previousScale = TextPrimitive::layoutPixelScale();
    constexpr float scales[] = {1.0f, 1.25f, 1.5f};

    for (float scale : scales) {
        TextPrimitive::setLayoutPixelScale(scale);
        for (const Sample& sample : samples) {
            // Cold width-first call must not publish an incomplete metrics entry.
            setCacheEnv("");
            neoTextCacheClear();
            neoTextCacheResetStats();
            const float widthFirst = TextPrimitive::measureTextWidth(
                sample.text, sample.family, sample.size, sample.weight);
            if (!sample.text.empty() && neoTextCacheMetricEntries() != 0) {
                fail(std::string(sample.label) + " @" + std::to_string(scale) +
                     "x: width-only call inserted an incomplete metrics entry");
            }
            const TextPrimitive::TextMetrics afterWidth =
                measure(sample.text, sample.family, sample.size, sample.weight);
            if (widthFirst != afterWidth.width) {
                fail(std::string(sample.label) + " @" + std::to_string(scale) +
                     "x: width-first value differs from complete metrics width");
            }
            if (!validMetrics(afterWidth, sample.text)) {
                fail(std::string(sample.label) + " @" + std::to_string(scale) +
                     "x: full metrics after width-only has invalid/empty caret arrays");
            }
            if (!sample.text.empty() && neoTextCacheMetricEntries() == 0) {
                fail(std::string(sample.label) + " @" + std::to_string(scale) +
                     "x: complete metrics were not cached after width-only");
            }

            // Reverse order: width-only must preserve a previously cached complete entry.
            neoTextCacheClear();
            neoTextCacheResetStats();
            const TextPrimitive::TextMetrics metricsFirst =
                measure(sample.text, sample.family, sample.size, sample.weight);
            if (!validMetrics(metricsFirst, sample.text)) {
                fail(std::string(sample.label) + ": cold complete metrics are malformed");
            }
            const std::size_t entriesBeforeWidth = neoTextCacheMetricEntries();
            const float metricsFirstWidth = TextPrimitive::measureTextWidth(
                sample.text, sample.family, sample.size, sample.weight);
            const TextPrimitive::TextMetrics afterMetricsWidth =
                measure(sample.text, sample.family, sample.size, sample.weight);
            if (metricsFirstWidth != metricsFirst.width ||
                !sameMetrics(metricsFirst, afterMetricsWidth) ||
                neoTextCacheMetricEntries() != entriesBeforeWidth) {
                fail(std::string(sample.label) + " @" + std::to_string(scale) +
                     "x: metrics-first width call changed the complete cache entry");
            }

            // With both caches disabled, the two public APIs must remain equivalent
            // and no partial/full result may be written into either cache.
            setCacheEnv("1");
            neoTextCacheClear();
            neoTextCacheResetStats();
            const float uncachedWidth = TextPrimitive::measureTextWidth(
                sample.text, sample.family, sample.size, sample.weight);
            const TextPrimitive::TextMetrics uncachedMetrics =
                measure(sample.text, sample.family, sample.size, sample.weight);
            if (uncachedWidth != uncachedMetrics.width || uncachedWidth != widthFirst ||
                !sameMetrics(uncachedMetrics, afterWidth) || !validMetrics(uncachedMetrics, sample.text) ||
                neoTextCacheMetricEntries() != 0 || neoTextCacheShapingEntries() != 0 ||
                neoTextCacheMetricHits() != 0 || neoTextCacheMetricMisses() != 0 ||
                neoTextCacheShapingHits() != 0 || neoTextCacheShapingMisses() != 0) {
                fail(std::string(sample.label) + " @" + std::to_string(scale) +
                     "x: cache-off width/metrics equivalence or no-write invariant failed");
            }
            setCacheEnv("");
        }
    }

    TextPrimitive::setLayoutPixelScale(previousScale);
    setCacheEnv("");
    // This is the final cache test: leave neither its counters nor entries behind
    // for any later test in the same process.
    neoTextCacheClear();
    neoTextCacheResetStats();
}

}  // namespace

int main() {
    // 进程可能带着外部的 NEO_TEXT_CACHE_OFF 起来，先归零保证用例确定性。
    setCacheEnv("");

    testEquivalence();
    testHitCounters();
    testBounded();
    testClearMiss();
    testEnvSwitch();
    testConcurrent();
    // 新增四项按用例编号顺序跑。前 7/8/9 三项开头都会整层清缓存（neoTextCacheClear
    // 或 setDefaultFontFiles 自带的那次），字体栈 holder 历史互不污染，参考值
    // 才能逐位对比；用例 10 只比路径字符串，不依赖缓存状态。
    testConcurrentWithFontOverride();
    testOverrideClearsCache();
    testFallbackRecovery();
    testOverrideWriteSharesLock();
    testWidthOnlyMetrics();

    setCacheEnv("");

    if (g_failures > 0) {
        std::cerr << "[text_metrics_cache] " << g_failures << " 处失败\n";
        return 1;
    }
    std::printf("text metrics cache: ALL PASS\n");
    return 0;
}
