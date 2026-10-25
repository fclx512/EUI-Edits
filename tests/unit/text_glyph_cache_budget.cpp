#include "core/render/text.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Exercise live TextPrimitive ownership beyond the shared font-stack LRU.
// These tests use CPU layout only; no window or GPU renderer is required.
extern "C" {
std::size_t neoGlyphAdvanceResidentBytes();
std::size_t neoGlyphAdvanceLimitBytes();
std::size_t neoGlyphAdvanceBudgetDenials();
std::size_t neoGlyphAdvanceAllocationFailures();
void neoGlyphAdvanceResetStats();
void neoTextCacheClear();
void neoTextCacheReloadEnv();
}

namespace {

using Text = core::TextPrimitive;
using Metrics = Text::TextMetrics;

void require(bool condition, const std::string& detail) {
    if (!condition) throw std::runtime_error(detail);
}

void setCacheOff(bool off) {
#ifdef _WIN32
    require(_putenv_s("NEO_TEXT_CACHE_OFF", off ? "1" : "") == 0,
            "failed to set NEO_TEXT_CACHE_OFF");
#else
    require((off ? setenv("NEO_TEXT_CACHE_OFF", "1", 1)
                 : unsetenv("NEO_TEXT_CACHE_OFF")) == 0,
            "failed to set NEO_TEXT_CACHE_OFF");
#endif
    neoTextCacheReloadEnv();
}

bool equalBits(float a, float b) {
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

bool sameMetrics(const Metrics& a, const Metrics& b) {
    if (!equalBits(a.width, b.width) || a.byteIndices != b.byteIndices ||
        a.caretX.size() != b.caretX.size()) return false;
    return a.caretX.empty() ||
           std::memcmp(a.caretX.data(), b.caretX.data(),
                       a.caretX.size() * sizeof(float)) == 0;
}

std::string installedFont() {
    for (const char* path : {"C:/Windows/Fonts/msyh.ttc",
                             "C:/Windows/Fonts/segoeui.ttf",
                             "C:/Windows/Fonts/arial.ttf"}) {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error) && !error) return path;
    }
    throw std::runtime_error("no usable installed font; budget axis NOT_RUN");
}

struct Sample {
    core::TextStyle style;
    Metrics uncached;
    std::string retryText;
    Metrics retryUncached;
};

std::vector<Sample> makeSamples(const std::string& font) {
    std::vector<Sample> result;
    // 48 distinct path#size holder identities exceed both the shared font-stack
    // cache's 16 slots and the 2 MiB / ~66 KiB per-table reservation capacity.
    for (int i = 0; i < 48; ++i) {
        Sample sample;
        sample.style.fontFamily = font;
        sample.style.fontSize = 13.0f + 0.5f * static_cast<float>(i);
        sample.style.fontWeight = 400;
        sample.style.text = "glyph-budget-" + std::to_string(i) + " AWM 019";
        sample.retryText = "glyph-budget-retry-" + std::to_string(i) + " WMA 901";
        result.push_back(std::move(sample));
    }
    return result;
}

void runBudgetOracle(const std::string& font) {
    const std::size_t limit = neoGlyphAdvanceLimitBytes();
    require(limit == 2u * 1024u * 1024u, "unexpected glyph reservation limit");
    setCacheOff(true);
    neoTextCacheClear();
    require(neoGlyphAdvanceResidentBytes() == 0,
            "precondition: live glyph tables before corpus");

    std::vector<Sample> samples = makeSamples(font);
    for (auto& sample : samples) {
        sample.uncached = Text::measureTextMetrics(
            sample.style.text, sample.style.fontFamily,
            sample.style.fontSize, sample.style.fontWeight);
        sample.retryUncached = Text::measureTextMetrics(
            sample.retryText, sample.style.fontFamily,
            sample.style.fontSize, sample.style.fontWeight);
        require(sample.uncached.width > 0, "cache-off reference has zero width");
        require(sample.retryUncached.width > 0, "cache-off retry reference has zero width");
    }
    require(neoGlyphAdvanceResidentBytes() == 0,
            "cache-off created a glyph advance table");

    setCacheOff(false);
    neoTextCacheClear();
    neoGlyphAdvanceResetStats();
    std::vector<std::unique_ptr<Text>> owners;
    owners.reserve(samples.size());
    std::size_t bytesAt16 = 0;
    std::size_t bytesAt17 = 0;
    std::size_t bytesAtFirstDenial = 0;
    std::size_t firstDenialIndex = samples.size();
    bool sawDenial = false;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        auto owner = std::make_unique<Text>();
        owner->setStyle(samples[i].style);
        // This performs CPU layout/shaping and retains fontInfoStorage_. It
        // needs neither initialize()/prepare()/render() nor a GPU window.
        const core::Vec2 size = owner->measuredSize();
        require(equalBits(size.x, samples[i].uncached.width),
                "primitive width differs from cache-off reference at holder " +
                    std::to_string(i));
        require(size.y > 0, "primitive layout has no height at holder " +
                                std::to_string(i));
        owners.push_back(std::move(owner));

        // The primitive already shaped this text, so this full-metrics call may
        // reuse that shape. It still checks every caret against the off result.
        const Metrics current = Text::measureTextMetrics(
            samples[i].style.text, samples[i].style.fontFamily,
            samples[i].style.fontSize, samples[i].style.fontWeight);
        require(sameMetrics(current, samples[i].uncached),
                "full caret metrics differ at holder " + std::to_string(i));

        const std::size_t bytes = neoGlyphAdvanceResidentBytes();
        require(bytes <= limit,
                "global glyph reservation exceeded 2 MiB at holder " +
                    std::to_string(i));
        if (i == 15) bytesAt16 = bytes;
        if (i == 16) bytesAt17 = bytes;
        if (!sawDenial && neoGlyphAdvanceBudgetDenials() > 0) {
            sawDenial = true;
            bytesAtFirstDenial = bytes;
            firstDenialIndex = i;
        }
        if (sawDenial)
            require(bytes == bytesAtFirstDenial,
                    "new table reserved after budget saturation at holder " +
                        std::to_string(i));
    }
    require(bytesAt16 > 0 && bytesAt17 > bytesAt16,
            "external primitive failed to retain holder beyond shared 16 slots");
    require(sawDenial && neoGlyphAdvanceBudgetDenials() > 0,
            "48 distinct live holders never exercised budget rejection");
    require(firstDenialIndex > 0 && firstDenialIndex < owners.size(),
            "cannot identify a retained budget-denied holder");
    require(neoGlyphAdvanceAllocationFailures() == 0,
            "allocator failure confounds budget-denial oracle");

    const std::size_t heldBytes = neoGlyphAdvanceResidentBytes();
    neoTextCacheClear();
    require(neoGlyphAdvanceResidentBytes() == heldBytes,
            "clearing shared cache released externally owned holders");
    Text* deniedOwner = owners[firstDenialIndex].get();
    owners.erase(owners.begin());
    const std::size_t bytesAfterRelease = neoGlyphAdvanceResidentBytes();
    require(bytesAfterRelease < heldBytes,
            "destroying first externally owned holder did not release its table");
    const std::size_t denialsBeforeRetry = neoGlyphAdvanceBudgetDenials();
    deniedOwner->setText(samples[firstDenialIndex].retryText);
    const core::Vec2 retriedSize = deniedOwner->measuredSize();
    require(equalBits(retriedSize.x, samples[firstDenialIndex].retryUncached.width),
            "retained denied holder retry width differs from cache-off reference");
    require(neoGlyphAdvanceResidentBytes() == heldBytes &&
                neoGlyphAdvanceResidentBytes() <= limit,
            "retained denied holder failed to reclaim exactly one freed table");
    require(neoGlyphAdvanceBudgetDenials() == denialsBeforeRetry,
            "retained denied holder was still rejected after one table was freed");
    const Metrics retriedMetrics = Text::measureTextMetrics(
        samples[firstDenialIndex].retryText,
        samples[firstDenialIndex].style.fontFamily,
        samples[firstDenialIndex].style.fontSize,
        samples[firstDenialIndex].style.fontWeight);
    require(sameMetrics(retriedMetrics, samples[firstDenialIndex].retryUncached),
            "retained denied holder retry carets differ from cache-off reference");
    require(neoGlyphAdvanceResidentBytes() <= limit,
            "full retry metrics exceeded global glyph reservation limit");
    owners.clear();
    neoTextCacheClear();
    require(neoGlyphAdvanceResidentBytes() == 0,
            "tables remained resident after all holders were destroyed");

    neoGlyphAdvanceResetStats();
    auto replacement = std::make_unique<Text>();
    replacement->setStyle(samples.front().style);
    const core::Vec2 size = replacement->measuredSize();
    require(equalBits(size.x, samples.front().uncached.width),
            "reallocated holder changed measured width");
    require(neoGlyphAdvanceResidentBytes() > 0 &&
                neoGlyphAdvanceResidentBytes() <= limit,
            "freed budget was not reusable");
    require(neoGlyphAdvanceBudgetDenials() == 0,
            "reallocation still denied after all owners released");
    replacement.reset();
    neoTextCacheClear();
    require(neoGlyphAdvanceResidentBytes() == 0,
            "replacement table was not released");

    std::cout << "[glyph_budget_oracle] PASS holders=" << samples.size()
              << " limit=" << limit << " saturated_bytes=" << heldBytes
              << " external_17_bytes=" << bytesAt17 << '\n';
}

} // namespace

int main() {
    try {
        runBudgetOracle(installedFont());
        setCacheOff(false);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[glyph_budget_oracle] FAIL: " << error.what() << '\n';
        return 1;
    }
}
