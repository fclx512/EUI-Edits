#include "core/render/text.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

extern "C" {
std::size_t neoFontResolutionScopeEntries();
std::size_t neoFontResolutionScopeBytes();
void neoTextCacheClear();
void neoTextCacheReloadEnv();
}
using T = core::TextPrimitive;
using Scope = T::FontResolutionScope;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool same(const T::TextMetrics& a, const T::TextMetrics& b) {
    return a.width == b.width && a.byteIndices == b.byteIndices && a.caretX == b.caretX;
}
T::TextMetrics measure(const std::string& family = {}, int weight = 400) {
    return T::measureTextMetrics("中文 😀 scope iii WWW 012345", family, 16, weight);
}

int main() {
    try {
#ifdef _WIN32
        _putenv_s("NEO_TEXT_CACHE_OFF", "");
#else
        unsetenv("NEO_TEXT_CACHE_OFF");
#endif
        neoTextCacheReloadEnv();
        const std::string fontA = "C:/Windows/Fonts/msyh.ttc";
        const std::string fontB = "C:/Windows/Fonts/consola.ttf";
        require(std::filesystem::exists(fontA) && std::filesystem::exists(fontB),
                "required Windows fonts missing; test cannot pass without coverage");
        const auto a = measure(fontA), b = measure(fontB);
        require(!same(a, b), "font-switch reference must distinguish the fonts");
        const auto plain = measure("Microsoft YaHei"), bold = measure("Microsoft YaHei", 700);
        const auto mono = measure("monospace");
        {
            Scope outer;
            require(same(plain, measure("Microsoft YaHei")), "regular metrics changed");
            require(same(bold, measure("Microsoft YaHei", 700)), "weight key/metrics changed");
            require(same(mono, measure("monospace")), "family key/metrics changed");
            require(neoFontResolutionScopeEntries() == 3, "scope must separate family and weight");
            {
                Scope nested;
                require(neoFontResolutionScopeEntries() == 0, "nested scope inherited entries");
                measure(fontB);
                require(neoFontResolutionScopeEntries() == 1, "nested scope did not cache");
            }
            require(neoFontResolutionScopeEntries() == 3, "nested destruction lost parent entries");
            std::atomic<bool> isolated{false};
            std::thread worker([&] {
                const bool empty = neoFontResolutionScopeEntries() == 0;
                Scope scope;
                measure(fontA);
                isolated = empty && neoFontResolutionScopeEntries() == 1;
            });
            worker.join();
            require(isolated && neoFontResolutionScopeEntries() == 3, "scope is not thread-local");
            T::setDefaultFontFiles(fontA, {});
            require(same(a, measure()), "font switch failed inside live scope");
            std::thread setter([&] { T::setDefaultFontFiles(fontB, {}); });
            setter.join();
            require(same(b, measure()), "cross-thread font switch reused old path");
            neoTextCacheClear();
            measure(fontA);
            require(neoFontResolutionScopeEntries() == 1, "clear did not invalidate active scope");
            for (int i = 0; i < 80; ++i) measure(fontA, 300 + i);
            require(neoFontResolutionScopeEntries() == 32, "entry bound failed");
            require(neoFontResolutionScopeBytes() <= 16 * 1024, "byte budget failed");
        }
        require(neoFontResolutionScopeEntries() == 0, "scope leaked after return");
        T::setDefaultFontFiles({}, {});
        {
            Scope scope;
            measure(std::string(9000, 'q'));
            measure(std::string(9000, 'r'));
            require(neoFontResolutionScopeEntries() == 1 &&
                    neoFontResolutionScopeBytes() <= 16 * 1024, "large-family byte bound failed");
        }
        {
            Scope scope;
#ifdef _WIN32
            _putenv_s("NEO_TEXT_CACHE_OFF", "1");
#else
            setenv("NEO_TEXT_CACHE_OFF", "1", 1);
#endif
            neoTextCacheReloadEnv();
            require(same(plain, measure("Microsoft YaHei")) &&
                    neoFontResolutionScopeEntries() == 0, "cache-off scope bypass failed");
#ifdef _WIN32
            _putenv_s("NEO_TEXT_CACHE_OFF", "");
#else
            unsetenv("NEO_TEXT_CACHE_OFF");
#endif
            neoTextCacheReloadEnv();
        }
        // A newly available bold sibling must change resolution in the next scope.
        // Removal/arrival is confined to a unique test-owned directory.
        const auto dir = std::filesystem::temp_directory_path() /
            ("eui-font-scope-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(dir), "temporary directory already exists");
        struct Cleanup {
            std::filesystem::path dir;
            ~Cleanup() { std::error_code e; std::filesystem::remove_all(dir, e); }
        } cleanup{dir};
        const auto regular = dir / "Scope.ttf", sibling = dir / "Scope-Bold.ttf";
        std::filesystem::copy_file(fontA, regular);
        const auto family = regular.u8string();
        {
            Scope scope;
            require(same(a, measure(family, 700)), "missing bold sibling fallback changed");
        }
        std::filesystem::copy_file(fontB, sibling);
        {
            Scope scope;
            require(same(b, measure(family, 700)), "new bold file invisible in next scope");
        }
        std::filesystem::remove(sibling);
        {
            Scope scope;
            require(same(a, measure(family, 700)), "deleted bold file reused across scopes");
        }
        const auto pending = (dir / "Pending.ttf").u8string();
        T::TextMetrics fallback;
        { Scope scope; fallback = measure(pending); }
        const auto replacement = same(fallback, a) ? fontB : fontA;
        const auto expected = same(fallback, a) ? b : a;
        std::filesystem::copy_file(replacement, std::filesystem::u8path(pending));
        { Scope scope; require(same(expected, measure(pending)), "missing font recovery failed"); }
        const auto defaultPending = (dir / "Default.ttf").u8string();
        T::setDefaultFontFiles(defaultPending, {});
        { Scope scope; fallback = measure(); }
        const bool useB = same(fallback, a);
        std::filesystem::copy_file(useB ? fontB : fontA, std::filesystem::u8path(defaultPending));
        { Scope scope; require(same(useB ? b : a, measure()), "missing default override recovery failed"); }
        T::setDefaultFontFiles({}, {});
        const float oldScale = T::layoutPixelScale();
        T::setLayoutPixelScale(1.5f);
        const auto scaled = measure(fontA);
        { Scope scope; require(same(scaled, measure(fontA)), "scope altered DPI metrics"); }
        T::setLayoutPixelScale(oldScale);
        std::cout << "font resolution scope: ALL PASS\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
