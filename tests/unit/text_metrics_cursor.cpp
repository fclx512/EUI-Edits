#include "core/render/text.h"

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" {
void neoTextCacheClear();
void neoTextCacheReloadEnv();
}

namespace {
using Text = core::TextPrimitive;
using Cursor = Text::MetricsCursor;
using Metrics = Text::TextMetrics;
using Clock = std::chrono::steady_clock;

static_assert(!std::is_copy_constructible<Cursor>::value, "metrics cursor must not copy live shaping state");
static_assert(!std::is_copy_assignable<Cursor>::value, "metrics cursor must not copy live shaping state");
static_assert(std::is_move_constructible<Cursor>::value, "metrics cursor must transfer its state");
static_assert(std::is_move_assignable<Cursor>::value, "metrics cursor must support move assignment");

int failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::uint32_t bits(float value) {
    std::uint32_t result = 0;
    static_assert(sizeof(result) == sizeof(value), "float must have 32-bit bit pattern");
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

bool exact(const Metrics& a, const Metrics& b) {
    if (bits(a.width) != bits(b.width) || a.byteIndices != b.byteIndices ||
        a.caretX.size() != b.caretX.size()) return false;
    for (std::size_t i = 0; i < a.caretX.size(); ++i)
        if (bits(a.caretX[i]) != bits(b.caretX[i])) return false;
    return true;
}

void cacheMode(bool enabled) {
#ifdef _WIN32
    _putenv_s("NEO_TEXT_CACHE_OFF", enabled ? "" : "1");
#else
    setenv("NEO_TEXT_CACHE_OFF", enabled ? "" : "1", 1);
#endif
    neoTextCacheReloadEnv();
    neoTextCacheClear();
}

std::string invalidUtf8Fixture() {
    std::string text = "A中文🙂\t";
    text.push_back(static_cast<char>(0xF0));
    text.push_back(static_cast<char>(0x28));
    text.push_back(static_cast<char>(0x8C));
    text.push_back(static_cast<char>(0x28));
    text += " B";
    text.push_back(static_cast<char>(0xC0));
    text.push_back(static_cast<char>(0xAF));
    text += "\tend";
    return text;
}

std::string emojiLookaheadFixture() {
    std::string text;
    for (int i = 0; i < 65; ++i) text += "中🙂";
    // With a one-codepoint budget, U+FE0F is on a later step than U+2764.
    text += "❤\xEF\xB8\x8F";
    for (int i = 0; i < 70; ++i) text += "A中🙂\t";
    return text;
}

std::string combiningFixture() {
    std::string text = "e\xCC\x81 A\t中🙂 "; // e + combining acute, then a tab.
    text += "a\xCC\x81\xCC\xA7";
    text += "\n";
    text += invalidUtf8Fixture();
    return text;
}

std::string uniqueLongFixture() {
    std::string text;
    for (int i = 0; i < 80; ++i)
        text += "segment-" + std::to_string(i) + "-中文🙂❤\xEF\xB8\x8F ";
    return text;
}

void compareOne(const std::string& text, const std::string& family,
                float fontSize, int weight, std::size_t budget,
                const std::string& label) {
    neoTextCacheClear();
    const Metrics reference = Text::measureTextMetrics(text, family, fontSize, weight);
    // The cursor must do fresh work. A warm whole-string cache could hide a
    // broken batch boundary or make a one-op test pass without shaping.
    neoTextCacheClear();
    Cursor cursor(text, family, fontSize, weight);
    check(!cursor.complete() && !cursor.cancelled(), label + ": initial cursor is live");
    check(!cursor.step(0, Clock::time_point::max()), label + ": zero budget does not finish");
    check(!cursor.complete() && !cursor.cancelled(), label + ": zero budget keeps cursor live");
    check(!cursor.step(budget, Clock::time_point::min()), label + ": expired deadline does not finish");
    check(!cursor.complete() && !cursor.cancelled(), label + ": expired deadline keeps cursor live");
    std::size_t calls = 0;
    bool terminal = false;
    for (; calls < text.size() + 1000 && !terminal; ++calls)
        terminal = cursor.step(budget, Clock::time_point::max());
    check(terminal && cursor.complete() && !cursor.cancelled(), label + ": cursor completes without cancellation");
    if (!cursor.complete()) return;
    if (budget == 1 && text.size() > 128)
        check(calls > 2, label + ": one-codepoint budget requires multiple fresh batches");
    const Metrics actual = cursor.takeMetrics();
    check(exact(actual, reference), label + ": width, every caret float bit, and UTF-8 byte indices equal synchronous oracle");
}

void testEquivalenceMatrix() {
    const std::vector<std::pair<std::string, std::string>> fixtures{
        {emojiLookaheadFixture(), "emoji VS16 and batch boundary"},
        {combiningFixture(), "combining tab invalid UTF-8"},
        {uniqueLongFixture(), "uncached unique long tail"}
    };
    for (bool cacheOn : {true, false}) {
        cacheMode(cacheOn);
        for (float dpi : {1.0f, 1.25f, 1.5f, 2.0f}) {
            Text::setLayoutPixelScale(dpi);
            for (std::size_t budget : {std::size_t{1}, std::size_t{7}, std::size_t{128}}) {
                for (const auto& fixture : fixtures) {
                    const std::string label = std::string(cacheOn ? "cache-on " : "cache-off ") +
                        "dpi " + std::to_string(dpi) + " budget " + std::to_string(budget) +
                        " " + fixture.second;
                    compareOne(fixture.first, "Microsoft YaHei", 16.0f, 400, budget, label);
                }
            }
        }
    }
    Text::setLayoutPixelScale(1.0f);
    cacheMode(true);
}

void testCacheModeAgreement() {
    Text::setLayoutPixelScale(1.25f);
    for (const auto& fixture : {emojiLookaheadFixture(), combiningFixture(), uniqueLongFixture()}) {
        cacheMode(true);
        const Metrics cached = Text::measureTextMetrics(fixture, "Microsoft YaHei", 16.0f, 400);
        cacheMode(false);
        const Metrics uncached = Text::measureTextMetrics(fixture, "Microsoft YaHei", 16.0f, 400);
        check(exact(cached, uncached), "cache enabled and disabled synchronous metrics agree bit for bit");
    }
    Text::setLayoutPixelScale(1.0f);
    cacheMode(true);
}

void testMoveAndEarlyDestruction() {
    const std::string text = emojiLookaheadFixture();
    neoTextCacheClear();
    const Metrics reference = Text::measureTextMetrics(text, "monospace", 17.0f, 700);
    neoTextCacheClear();
    Cursor first(text, "monospace", 17.0f, 700);
    check(!first.step(1, Clock::time_point::max()), "move fixture remains partial after one codepoint");
    Cursor second(std::move(first));
    Cursor third("unused", "monospace", 17.0f);
    third = std::move(second);
    bool terminal = false;
    for (std::size_t i = 0; i < text.size() + 1000 && !terminal; ++i)
        terminal = third.step(7, Clock::time_point::max());
    check(terminal && third.complete() && exact(third.takeMetrics(), reference),
          "move construction and assignment preserve partial cursor state exactly");
    {
        Cursor abandoned(std::string(4096, 'Q'), "monospace", 16.0f);
        abandoned.step(1, Clock::time_point::max());
    }
    Cursor fresh("after abandoned cursor 中文🙂", "monospace", 16.0f);
    bool freshTerminal = false;
    for (int i = 0; i < 1000 && !freshTerminal; ++i)
        freshTerminal = fresh.step(7, Clock::time_point::max());
    check(freshTerminal && fresh.complete(), "early destruction releases state for a fresh cursor");
}

void testContextCancellation() {
    const std::string text = emojiLookaheadFixture();
    const float previousDpi = Text::layoutPixelScale();
    Text::setLayoutPixelScale(1.0f);
    Cursor dpiCursor(text, "monospace", 16.0f);
    check(!dpiCursor.step(1, Clock::time_point::max()), "DPI cancellation fixture starts partial");
    Text::setLayoutPixelScale(1.25f);
    check(dpiCursor.step(7, Clock::time_point::max()) && dpiCursor.cancelled() && !dpiCursor.complete(),
          "DPI change cancels partial cursor before publication");
    Text::setLayoutPixelScale(previousDpi);

    const std::string fontA = "C:/Windows/Fonts/msyh.ttc";
    const std::string fontB = "C:/Windows/Fonts/consola.ttf";
    check(std::filesystem::exists(fontA) && std::filesystem::exists(fontB),
          "font cancellation fixture requires two real Windows fonts");
    if (!std::filesystem::exists(fontA) || !std::filesystem::exists(fontB)) return;
    Text::setDefaultFontFiles(fontA, {});
    const std::uint64_t generationA = Text::fontConfigurationGeneration();
    Cursor fontCursor(text, "", 16.0f);
    check(!fontCursor.step(1, Clock::time_point::max()), "font cancellation fixture starts partial");
    Text::setDefaultFontFiles(fontB, {});
    check(Text::fontConfigurationGeneration() != generationA,
          "default font switch advances configuration generation");
    check(fontCursor.step(7, Clock::time_point::max()) && fontCursor.cancelled() && !fontCursor.complete(),
          "default font switch cancels partial cursor despite unchanged family string");
    Text::setDefaultFontFiles("", "");
}
} // namespace

int main() {
    testEquivalenceMatrix();
    testCacheModeAgreement();
    testMoveAndEarlyDestruction();
    testContextCancellation();
    return failures ? 1 : 0;
}
