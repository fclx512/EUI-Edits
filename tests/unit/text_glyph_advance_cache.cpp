#include "core/render/text.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
std::size_t neoGlyphAdvanceHits();
std::size_t neoGlyphAdvanceMisses();
std::size_t neoGlyphAdvanceCollisions();
void neoGlyphAdvanceResetStats();
std::size_t neoTextCacheShapingMisses();
void neoTextCacheResetStats();
void neoTextCacheClear();
void neoTextCacheReloadEnv();
}

namespace {
using T = core::TextPrimitive;
using Metrics = T::TextMetrics;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void setCacheOff(bool off) {
#ifdef _WIN32
    require(_putenv_s("NEO_TEXT_CACHE_OFF", off ? "1" : "") == 0,
            "cannot set NEO_TEXT_CACHE_OFF");
#else
    if (off) require(setenv("NEO_TEXT_CACHE_OFF", "1", 1) == 0, "cannot set cache-off env");
    else require(unsetenv("NEO_TEXT_CACHE_OFF") == 0, "cannot clear cache-off env");
#endif
    neoTextCacheReloadEnv();
}

bool same(const Metrics& a, const Metrics& b) {
    return std::memcmp(&a.width, &b.width, sizeof(float)) == 0 &&
           a.byteIndices == b.byteIndices &&
           a.caretX.size() == b.caretX.size() &&
           (a.caretX.empty() ||
            std::memcmp(a.caretX.data(), b.caretX.data(),
                        a.caretX.size() * sizeof(float)) == 0);
}

bool valid(const Metrics& metrics, const std::string& text) {
    if (metrics.byteIndices.empty() || metrics.byteIndices.size() != metrics.caretX.size() ||
        metrics.byteIndices.front() != 0 || metrics.caretX.front() != 0.0f ||
        metrics.byteIndices.back() != static_cast<int>(text.size()) || !(metrics.width >= 0.0f))
        return false;
    for (std::size_t i = 1; i < metrics.byteIndices.size(); ++i)
        if (metrics.byteIndices[i] < metrics.byteIndices[i - 1] ||
            metrics.caretX[i] < metrics.caretX[i - 1]) return false;
    return true;
}

Metrics measure(const std::string& text, const std::string& family = {},
                float size = 16.0f, int weight = 400) {
    return T::measureTextMetrics(text, family, size, weight);
}

std::string utf8(unsigned int cp) {
    std::string result;
    if (cp <= 0x7f) result.push_back(static_cast<char>(cp));
    else if (cp <= 0x7ff) {
        result.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        result.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        result.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        result.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
    return result;
}

struct Sample {
    std::string text;
    std::string family;
    float size = 16.0f;
    int weight = 400;
    float scale = 1.0f;
};

void compareCorpus(const std::vector<Sample>& samples, const std::string& label,
                   bool expectGlyphHits, bool expectCollisions) {
    require(!samples.empty(), label + ": empty corpus");
    setCacheOff(true);
    neoTextCacheClear();
    neoGlyphAdvanceResetStats();
    std::vector<Metrics> reference;
    reference.reserve(samples.size());
    for (const auto& sample : samples) {
        T::setLayoutPixelScale(sample.scale);
        auto metrics = measure(sample.text, sample.family, sample.size, sample.weight);
        require(valid(metrics, sample.text), label + ": invalid cache-off metrics");
        reference.push_back(std::move(metrics));
    }
    require(neoGlyphAdvanceHits() == 0 && neoGlyphAdvanceMisses() == 0 &&
            neoGlyphAdvanceCollisions() == 0,
            label + ": cache-off unexpectedly accessed glyph advance cache");

    setCacheOff(false);
    neoTextCacheClear();
    neoTextCacheResetStats();
    neoGlyphAdvanceResetStats();
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto& sample = samples[i];
        T::setLayoutPixelScale(sample.scale);
        const auto current = measure(sample.text, sample.family, sample.size, sample.weight);
        require(same(current, reference[i]), label + ": cache-on differs at sample " +
                std::to_string(i));
    }
    if (expectGlyphHits)
        require(neoGlyphAdvanceHits() > 0, label + ": glyph advance cache never hit");
    if (expectCollisions)
        require(neoGlyphAdvanceCollisions() > 0, label + ": no set-associative replacement observed");
    require(neoTextCacheShapingMisses() >= samples.size() / 2,
            label + ": corpus did not exercise shaping misses");
    std::cout << "[glyph_advance_oracle] " << label << " samples=" << samples.size()
              << " shaping_misses=" << neoTextCacheShapingMisses()
              << " glyph_hits=" << neoGlyphAdvanceHits()
              << " glyph_misses=" << neoGlyphAdvanceMisses()
              << " collisions=" << neoGlyphAdvanceCollisions() << '\n';
}

std::vector<std::string> systemFonts() {
    const char* paths[] = {
        "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/times.ttf"
    };
    std::vector<std::string> found;
    for (const char* path : paths) {
        std::error_code error;
        if (std::filesystem::exists(path, error) && !error) found.emplace_back(path);
    }
    return found;
}

void differentTexts(const std::string& cjkFont) {
    std::vector<Sample> samples;
    samples.reserve(160);
    const std::string body = "Alpha 012345 xyz " + utf8(0x4e2d) + utf8(0x6587) +
                             " " + utf8(0x1f642) + " e" + utf8(0x0301) + "\t";
    for (int i = 0; i < 160; ++i)
        samples.push_back({"line-" + std::to_string(i) + ": " + body +
                           " tail-" + std::to_string(i * 17), cjkFont, 16.0f, 400, 1.0f});
    compareCorpus(samples, "different texts with repeated glyphs", true, false);
}

void mixedFontsAndSpecials(const std::vector<std::string>& fonts) {
    require(fonts.size() >= 2, "need two system fonts for family/weight coverage");
    std::vector<Sample> samples;
    const std::string heartEmoji = utf8(0x2764) + utf8(0xfe0f);
    const std::string family = utf8(0x1f469) + utf8(0x200d) + utf8(0x1f4bb);
    const std::string body = "AV Wi 09 " + utf8(0x4e2d) + utf8(0x56fd) + " " +
                             heartEmoji + " " + family + " e" + utf8(0x0301) + "\tZ";
    for (float scale : {1.0f, 1.25f}) {
        for (const auto& font : {fonts[0], fonts[1]}) {
            for (int weight : {400, 700}) {
                for (int i = 0; i < 8; ++i)
                    samples.push_back({body + " #" + std::to_string(i) + "/" +
                                       std::to_string(weight), font, 15.5f, weight, scale});
            }
        }
    }
    compareCorpus(samples, "font weight DPI emoji combining tab", true, false);
}

void collisionAndCapacity(const std::string& cjkFont) {
    // 4800 distinct CJK codepoints exceed 4096 slots if this installed face maps
    // them to distinct glyphs. The diagnostic counter confirms actual replacement.
    std::vector<Sample> samples;
    samples.reserve(9600);
    for (int pass = 0; pass < 2; ++pass) {
        for (unsigned int i = 0; i < 4800; ++i) {
            const auto cp = utf8(0x4e00 + i);
            samples.push_back({(pass ? "R-" : "L-") + cp + "-" + std::to_string(i),
                               cjkFont, 16.0f, 400, 1.0f});
        }
    }
    compareCorpus(samples, "4096-slot collision and over-capacity", true, true);
}

void cacheOffDoesNotReadOrWrite(const std::string& cjkFont) {
    setCacheOff(false);
    neoTextCacheClear();
    measure("seed aaaa bbbb cccc");
    neoGlyphAdvanceResetStats();
    setCacheOff(true);
    for (int i = 0; i < 8; ++i) {
        const std::string text = "cache-off-" + std::to_string(i) + " abcd " + utf8(0x4e2d);
        require(valid(measure(text), text), "cache-off metrics invalid");
        require(T::measureTextWidth(text) == measure(text).width, "cache-off width/full disagree");
    }
    require(neoGlyphAdvanceHits() == 0 && neoGlyphAdvanceMisses() == 0 &&
            neoGlyphAdvanceCollisions() == 0,
            "cache-off read/wrote or counted holder table");

    // A single cold glyph shaped while disabled must not silently populate the
    // holder table: its first enabled measurement must be a glyph miss.
    neoTextCacheClear();
    const std::string coldGlyph = utf8(0x9f8d);
    require(valid(measure(coldGlyph, cjkFont), coldGlyph), "cache-off cold glyph invalid");
    setCacheOff(false);
    neoGlyphAdvanceResetStats();
    require(valid(measure(coldGlyph, cjkFont), coldGlyph), "cache-on cold glyph invalid");
    require(neoGlyphAdvanceHits() == 0 && neoGlyphAdvanceMisses() == 1,
            "cache-off populated holder glyph table");
}

struct TemporaryFontPath {
    std::filesystem::path dir;
    std::filesystem::path pending;
    TemporaryFontPath() {
        const auto temp = std::filesystem::temp_directory_path();
        dir = temp / ("eui-glyph-advance-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(dir), "cannot create unique font test directory");
        pending = dir / "Pending.ttf";
    }
    ~TemporaryFontPath() {
        std::error_code ignored;
        std::filesystem::remove(pending, ignored);
        std::filesystem::remove(dir, ignored); // nonrecursive, exact test-owned path
    }
};

void fallbackRecovery(const std::vector<std::string>& fonts) {
    TemporaryFontPath temporary;
    const std::string text = "fallback ABC WWW iii " + utf8(0x4e2d) + " 012345";
    const std::string pending = temporary.pending.u8string();
    setCacheOff(true);
    neoTextCacheClear();
    const Metrics fallback = measure(text, pending);
    const Metrics a = measure(text, fonts[0]);
    const Metrics b = measure(text, fonts[1]);
    const std::string donor = !same(a, fallback) ? fonts[0] : fonts[1];
    require(!same(measure(text, donor), fallback), "font donors do not distinguish fallback");
    setCacheOff(false);
    neoTextCacheClear();
    require(same(measure(text, pending), fallback), "warm fallback differs from uncached");
    std::error_code error;
    std::filesystem::copy_file(donor, temporary.pending, error);
    require(!error, "cannot make requested font available: " + error.message());
    const Metrics recovered = measure(text, pending); // no cache clear before recovery
    require(!same(recovered, fallback), "requested font recovery reused old fallback advance");
    setCacheOff(true);
    neoTextCacheClear();
    require(same(recovered, measure(text, pending)), "recovery differs from cache-off reference");
    setCacheOff(false);
    neoTextCacheClear(); // release possible Windows font-file handle before cleanup
}

void defaultFontChangeAndClear(const std::vector<std::string>& fonts) {
    const std::string text = "default WWW iii " + utf8(0x4e2d) + " 24680";
    setCacheOff(true);
    T::setDefaultFontFiles(fonts[0], {});
    const Metrics firstReference = measure(text);
    T::setDefaultFontFiles(fonts[1], {});
    const Metrics secondReference = measure(text);
    require(!same(firstReference, secondReference), "default font references not distinguishable");
    setCacheOff(false);
    T::setDefaultFontFiles(fonts[0], {});
    neoTextCacheClear();
    const Metrics first = measure(text);
    require(same(first, firstReference), "first default font differs");
    T::setDefaultFontFiles(fonts[1], {}); // clears holder and normal text caches
    const Metrics second = measure(text);
    require(same(second, secondReference), "new default font retained old advance");
    neoTextCacheClear();
    neoGlyphAdvanceResetStats();
    const Metrics afterClear = measure(text + " new");
    require(neoGlyphAdvanceMisses() > 0,
            "clear reused a resident glyph table without a cold glyph load");
    setCacheOff(true);
    neoTextCacheClear();
    require(same(afterClear, measure(text + " new")), "clear retained stale advance");
    T::setDefaultFontFiles({}, {});
    setCacheOff(false);
}

void concurrentWidthFullAndClear() {
    setCacheOff(true);
    neoTextCacheClear();
    const std::string text = "concurrent Alpha WWW 123 " + utf8(0x4e2d) +
                             utf8(0x6587) + " " + utf8(0x1f642);
    const Metrics reference = measure(text);
    setCacheOff(false);
    neoTextCacheClear();
    std::atomic<int> ready{0};
    std::atomic<bool> go{false}, failed{false};
    auto worker = [&](bool widthOnly) {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 120; ++i) {
            if (widthOnly) {
                if (T::measureTextWidth(text) != reference.width) failed.store(true);
            } else if (!same(measure(text), reference)) {
                failed.store(true);
            }
            if ((i & 7) == 0) std::this_thread::yield();
        }
    };
    auto clearer = [&] {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 80; ++i) {
            neoTextCacheClear();
            std::this_thread::yield();
        }
    };
    std::thread a(worker, true), b(worker, false), c(clearer);
    while (ready.load(std::memory_order_acquire) != 3) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    a.join(); b.join(); c.join();
    require(!failed.load(), "concurrent width/full/clear changed exact metrics");
}

} // namespace

int main() {
    const float oldScale = T::layoutPixelScale();
    try {
        setCacheOff(false);
        T::setDefaultFontFiles({}, {});
        T::setLayoutPixelScale(1.0f);
        const auto fonts = systemFonts();
        require(fonts.size() >= 2, "two Windows font files required");
        require(std::filesystem::exists("C:/Windows/Fonts/msyh.ttc"),
                "CJK collision fixture requires msyh.ttc");
        differentTexts("C:/Windows/Fonts/msyh.ttc");
        mixedFontsAndSpecials(fonts);
        collisionAndCapacity("C:/Windows/Fonts/msyh.ttc");
        cacheOffDoesNotReadOrWrite("C:/Windows/Fonts/msyh.ttc");
        fallbackRecovery(fonts);
        defaultFontChangeAndClear(fonts);
        T::setLayoutPixelScale(1.0f);
        concurrentWidthFullAndClear();
        T::setLayoutPixelScale(oldScale);
        T::setDefaultFontFiles({}, {});
        setCacheOff(false);
        neoTextCacheClear();
        std::cout << "glyph advance oracle: ALL PASS\n";
        return 0;
    } catch (const std::exception& error) {
        T::setLayoutPixelScale(oldScale);
        T::setDefaultFontFiles({}, {});
        setCacheOff(false);
        neoTextCacheClear();
        std::cerr << "glyph advance oracle: FAIL: " << error.what() << '\n';
        return 1;
    }
}
