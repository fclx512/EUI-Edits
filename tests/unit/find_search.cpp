#include "apps/neo_editor/model/find_search.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string>
#include <vector>

namespace allocation_probe {
struct alignas(std::max_align_t) Header {
    std::size_t size;
    bool counted;
};
std::atomic<bool> enabled{false};
std::atomic<std::size_t> live{0};
std::atomic<std::size_t> peak{0};

void add(std::size_t size) noexcept {
    const auto current = live.fetch_add(size, std::memory_order_relaxed) + size;
    auto observed = peak.load(std::memory_order_relaxed);
    while (observed < current && !peak.compare_exchange_weak(
               observed, current, std::memory_order_relaxed)) {}
}
}

void* operator new(std::size_t size) {
    const auto actual = size ? size : 1;
    auto* header = static_cast<allocation_probe::Header*>(
        std::malloc(sizeof(allocation_probe::Header) + actual));
    if (!header) throw std::bad_alloc();
    header->size = actual;
    header->counted = allocation_probe::enabled.load(std::memory_order_relaxed);
    if (header->counted) allocation_probe::add(actual);
    return header + 1;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept {
    if (!memory) return;
    auto* header = static_cast<allocation_probe::Header*>(memory) - 1;
    if (header->counted) allocation_probe::live.fetch_sub(header->size, std::memory_order_relaxed);
    std::free(header);
}
void operator delete[](void* memory) noexcept { ::operator delete(memory); }
void operator delete(void* memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "find_search: " << message << '\n';
    return condition;
}
}

int main() {
    using neo::search::Options;
    using neo::search::matches;
    bool ok = true;

    {
        const auto found = matches("Alpha alpha ALPHA", "aLpHa");
        ok &= expect(found.size() == 3 && found[0].begin == 0 && found[1].begin == 6 &&
                     found[2].begin == 12, "ASCII case-insensitive search");
        const auto exact = matches("Alpha alpha", "alpha", Options{true, false});
        ok &= expect(exact.size() == 1 && exact[0].begin == 6, "case-sensitive search");
    }
    {
        const std::string upperSharpS = "\xE1\xBA\x9E"; // U+1E9E
        const std::string sharpS = "\xC3\x9F";          // U+00DF
        const auto found = matches(upperSharpS, sharpS);
        ok &= expect(found.size() == 1 && found[0].begin == 0 &&
                     found[0].end == static_cast<int>(upperSharpS.size()),
                     "Unicode simple fold preserves original byte span");
        const auto greek = matches("\xCE\x91\xCE\x98\xCE\x97\xCE\x9D\xCE\x91", // ΑΘΗΝΑ
                                   "\xCE\xB1\xCE\xB8\xCE\xB7\xCE\xBD\xCE\xB1"); // αθηνα
        ok &= expect(greek.size() == 1, "Greek case-insensitive search");
        const auto sigma = matches("\xCE\x9F\xCE\xA3", "\xCE\xBF\xCF\x82"); // ΟΣ / ος
        ok &= expect(sigma.size() == 1, "Greek final sigma folds with sigma");
        const auto cyrillic = matches("\xD0\x9F\xD0\xA0\xD0\x98\xD0\x92\xD0\x95\xD0\xA2", // ПРИВЕТ
                                      "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"); // привет
        ok &= expect(cyrillic.size() == 1, "Cyrillic case-insensitive search");
    }
    {
        const std::string text = "中文搜索结果，搜索！ （搜索）";
        const auto whole = matches(text, "搜索", Options{false, true});
        ok &= expect(whole.size() == 2 && text.substr(whole[0].begin, whole[0].end - whole[0].begin) == "搜索",
                     "CJK neighbors are word characters, punctuation is a boundary");
        const auto fullwidth = matches("（搜索）", "搜索", Options{false, true});
        ok &= expect(fullwidth.size() == 1, "fullwidth punctuation forms a boundary");
        const auto underscore = matches("a_search_", "search", Options{false, true});
        ok &= expect(underscore.empty(), "underscore is a word character");
        const auto asciiBoundary = matches("(search),", "search", Options{false, true});
        ok &= expect(asciiBoundary.size() == 1, "ASCII punctuation forms a boundary");
        const auto arabic = matches("\xD8\xA7\xD9\x84word", "word", Options{false, true}); // Arabic letters before word
        ok &= expect(arabic.empty(), "Arabic letters are word characters");
        const auto hebrew = matches("\xD7\x90word", "word", Options{false, true});
        ok &= expect(hebrew.empty(), "Hebrew letters are word characters");
    }
    {
        const auto found = matches("aaaaa", "aaa");
        ok &= expect(found.size() == 1 && found[0].begin == 0 && found[0].end == 3,
                     "matches are non-overlapping");
        std::string replaced = "xx xx xx";
        const auto spans = matches(replaced, "xx");
        for (auto it = spans.rbegin(); it != spans.rend(); ++it)
            replaced.replace(static_cast<std::size_t>(it->begin),
                             static_cast<std::size_t>(it->end - it->begin), "Y");
        ok &= expect(replaced == "Y Y Y", "byte spans can be consumed for replacement");
    }
    {
        const std::string malformed = std::string("x\xFFy", 3);
        const std::string badByte = std::string("\xFF", 1);
        const auto found = matches(malformed, badByte);
        ok &= expect(found.size() == 1 && found[0].begin == 1 && found[0].end == 2,
                     "malformed UTF-8 is handled as opaque bytes");
        ok &= expect(matches("anything", "").empty(), "empty query has no matches");
    }
    {
        constexpr std::size_t documentSize = 8u * 1024u * 1024u;
        const std::string document(documentSize, 'a');
        const std::string query = "zzzz-not-present";
        allocation_probe::live.store(0, std::memory_order_relaxed);
        allocation_probe::peak.store(0, std::memory_order_relaxed);
        allocation_probe::enabled.store(true, std::memory_order_relaxed);
        const auto start = std::chrono::steady_clock::now();
        const auto found = matches(document, query);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        allocation_probe::enabled.store(false, std::memory_order_relaxed);
        const auto peakBytes = allocation_probe::peak.load(std::memory_order_relaxed);
        ok &= expect(found.empty(), "large ASCII performance probe has no matches");
        ok &= expect(peakBytes < documentSize / 16,
                     "large ASCII search temporary heap stays independent of document size");
        std::cout << "find_search 8MiB ASCII: " << elapsed << " ms, peak temporary heap "
                  << peakBytes << " bytes (query length " << query.size() << ")\n";
    }
    return ok ? 0 : 1;
}
