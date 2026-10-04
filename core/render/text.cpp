#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "core/render/text.h"
#include "core/render/render_backend.h"
#include "core/platform/bundled_resources.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace core {

namespace {

constexpr const char* kDefaultUiFontFile = "JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf";
constexpr const char* kDefaultIconFontFile = "Font Awesome 7 Free-Solid-900.otf";
constexpr int kGrayAtlasInitialSize = 512;
constexpr int kGrayAtlasMaxSize = 2048;
constexpr int kColorAtlasSize = 1024;
constexpr FT_Int32 kGlyphLoadFlags = FT_LOAD_DEFAULT | FT_LOAD_COLOR | FT_LOAD_NO_SVG | FT_LOAD_TARGET_LIGHT;

struct FontFace {
    std::string path;
    FT_Face face = nullptr;
    std::shared_ptr<std::vector<unsigned char>> fileBytes;
    float size = 16.0f;
    float ascent = 0.0f;
    float descent = 0.0f;
    float lineGap = 0.0f;
    float glyphScale = 1.0f;
    bool colored = false;

    FontFace() = default;
    FontFace(const FontFace&) = delete;
    FontFace& operator=(const FontFace&) = delete;

    FontFace(FontFace&& other) noexcept {
        *this = std::move(other);
    }

    FontFace& operator=(FontFace&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (face) {
            FT_Done_Face(face);
        }
        path = std::move(other.path);
        face = other.face;
        fileBytes = std::move(other.fileBytes);
        size = other.size;
        ascent = other.ascent;
        descent = other.descent;
        lineGap = other.lineGap;
        glyphScale = other.glyphScale;
        colored = other.colored;
        other.face = nullptr;
        return *this;
    }

    ~FontFace() {
        if (face) {
            FT_Done_Face(face);
        }
    }
};

struct FontInfoHolder {
    std::vector<FontFace> faces;
    std::vector<std::string> lazyFallbackPaths;
};

constexpr std::size_t kFontStackCacheCapacity = 16;
constexpr std::size_t kTextSizeCacheCapacity = 1024;

// ─────────────────────────────────────────────────────────────────────────────
// T1 双层文本度量缓存（清单 T1「逐行塑形缓存」）
//
// 层次（外 → 内），两层都挂在 measureTextMetrics 这条路上：
//   ① TextMetricsCache —— 最外层：键 = **解析后的字体文件路径**（= 字体栈身份，
//      与 loadSharedFontStack 的 path#size 同口径）+ 字号 + 文本，**不含
//      wrap width / maxWidth / lineHeight**（塑形与宽度无关，软换行只是对
//      caretX 做前缀和切分）；值 = 最终 TextMetrics（width + byteIndices +
//      caretX）。命中直接按值返回 —— FreeType、塑形、makeTextMetrics 一步不走。
//   ② ShapingCache —— 内层：键 = holder 指针 + 文本，值 = ShapedGlyph 数组。
//      结构与语义原样保留，只把 min_element 全表扫描淘汰换成 O(1) LRU，
//      并补上按估算字节的上限。
//
// 两层共同约定：
//   * 有界：估算字节预算 + 条目数上限两道闸，先到先限；
//   * O(1) LRU：侵入式双向链表 + unordered_map —— map 是节点式容器，rehash
//     只重排桶、**元素地址不变**，所以 entry 里存的 key 指针 / 链表指针在插入
//     后一直有效，淘汰时不必扫表（原 min_element 是 O(条目数)）；
//   * 线程安全：与字体栈 / TextSizeCache 共用 textCacheMutex()（递归锁，
//     measureTextSize 外层持锁时会回调 measureTextMetrics）；
//   * NEO_TEXT_CACHE_OFF=1 → 两层都不读不写（跑"无缓存基线"用）；
//   * clearSharedFontStackCache() → 两层连同字体栈 / TextSizeCache 一起清空。
// ─────────────────────────────────────────────────────────────────────────────

// 两层**总**预算（估算字节）：调这一个常数即可整体调参，按 2:1 切给外层 / 内层。
constexpr std::size_t kTextCacheBudgetBytes = 24ull * 1024ull * 1024ull;             // 合计 24 MiB
constexpr std::size_t kTextMetricsCacheBudgetBytes = kTextCacheBudgetBytes / 3 * 2;  // 外层 16 MiB
constexpr std::size_t kShapingCacheBudgetBytes = kTextCacheBudgetBytes / 3;          // 内层 8 MiB
// 条目数上限：键极小时字节预算到不了，条数必须自己封顶（第二道闸）。
constexpr std::size_t kTextMetricsCacheMaxEntries = 65536;
constexpr std::size_t kShapingCacheCapacity = 2048;  // 沿用原值：perf_benchmark 的注释引用它

// 四层缓存共用一把可重入锁：measureTextMetrics 外层持锁时还要走字体栈与塑形，
// measureTextSize 持锁时会回调 measureTextMetrics —— 单锁 + recursive 无锁序问题。
std::recursive_mutex& textCacheMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

// NEO_TEXT_CACHE_OFF 读取：首次用到时读一次并缓存（逐行测量是热路径，每次
// 都去查环境变量不划算）；进程内切换走 reloadTextCacheEnv()（见文末测试钩子）。
bool readTextCacheOffEnv() {
#ifdef _MSC_VER
    char* buffer = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&buffer, &length, "NEO_TEXT_CACHE_OFF") != 0 || buffer == nullptr) {
        return false;
    }
    const bool off = std::string(buffer) == "1";
    free(buffer);
    return off;
#else
    const char* value = std::getenv("NEO_TEXT_CACHE_OFF");
    return value != nullptr && std::string(value) == "1";
#endif
}

std::atomic<int>& textCacheOffFlag() {
    static std::atomic<int> flag{-1};  // -1 = 还没读过；1 = 关；0 = 开
    return flag;
}

bool textCacheDisabled() {
    int cached = textCacheOffFlag().load(std::memory_order_relaxed);
    if (cached < 0) {
        cached = readTextCacheOffEnv() ? 1 : 0;
        textCacheOffFlag().store(cached, std::memory_order_relaxed);
    }
    return cached == 1;
}

void reloadTextCacheEnv() {
    textCacheOffFlag().store(-1, std::memory_order_relaxed);
}

// 侵入式 LRU 链（环形哨兵）：入队 / 摘链 / 取最老都是 O(1)。
template <typename Owner>
struct LruNode {
    Owner* owner = nullptr;
    LruNode* prev = nullptr;
    LruNode* next = nullptr;
};

template <typename Owner>
class LruList {
public:
    LruList() { head_.prev = head_.next = &head_; }
    LruList(const LruList&) = delete;
    LruList& operator=(const LruList&) = delete;

    // 节点必须处于已摘链状态（prev/next 为空）；owner 由调用方在入链前设好。
    void pushFront(LruNode<Owner>& node) {
        node.next = head_.next;
        node.prev = &head_;
        head_.next->prev = &node;
        head_.next = &node;
    }

    void unlink(LruNode<Owner>& node) {
        node.prev->next = node.next;
        node.next->prev = node.prev;
        node.prev = node.next = nullptr;
    }

    Owner* back() const {
        return head_.prev == &head_ ? nullptr : head_.prev->owner;
    }

    // 整链作废：只断哨兵，节点随 entry 一起被 clear 掉。
    void reset() { head_.prev = head_.next = &head_; }

private:
    LruNode<Owner> head_{};
};

// 请求路径的**文件身份快照**：fallback holder 的失效判定用（见 loadSharedFontStack）。
// 用「存在性 + 字节数 + mtime」而不是只看存在性：路径原先是坏文件（能 stat 但
// FT_New_Face 打不开）后来被换成好文件时，存在性没变，只有 size/mtime 变了。
// 三项都取不到时保持默认值 —— 查询失败一律往"不相等"的方向退（宁可多重载一次，
// 也不能留着旧 fallback 不放）。这是纯只读探测，不写任何缓存状态。
struct FontFileFingerprint {
    bool exists = false;
    std::uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};

    bool operator==(const FontFileFingerprint& other) const {
        return exists == other.exists && size == other.size && mtime == other.mtime;
    }
};

FontFileFingerprint fingerprintFontFile(const std::string& path) {
    FontFileFingerprint fingerprint;
    if (path == core::platform::kEuiEditsFontAwesomeResourcePath) {
        const auto resource = core::platform::bundledResource(
            core::platform::BundledResourceId::EuiEditsFontAwesome);
        fingerprint.exists = static_cast<bool>(resource);
        fingerprint.size = resource.size;
        return fingerprint;
    }
    const auto native = std::filesystem::u8path(path);
    std::error_code error;
    const bool exists = std::filesystem::exists(native, error);
    if (error || !exists) {
        return fingerprint;  // 不存在 / 查询失败 → exists=false（见上：偏向"不相等"）
    }
    fingerprint.exists = true;
    std::error_code sizeError;
    fingerprint.size = std::filesystem::file_size(native, sizeError);  // 失败 → 0
    std::error_code mtimeError;
    fingerprint.mtime = std::filesystem::last_write_time(native, mtimeError);  // 失败 → 默认值
    return fingerprint;
}

struct FontStackCacheEntry {
    std::shared_ptr<FontInfoHolder> holder;
    std::uint64_t lastUsed = 0;

    // ── fallback 条目的失效判定（安全路径身份的另一半）──────────────────────
    // 请求的字体文件没能成为 faces.front()（loadSharedFontStack 落到了默认 /
    // 系统字体）时，记下请求路径**当时**的文件身份；下次命中先比对，变了就丢弃
    // 重载。否则「路径暂时不可用 → 后来可用」会永远拿到旧 fallback holder，
    // 连带 T1 外层也一直回旧字形（measureTextMetrics 的实际字体键依赖这里
    // 把 holder 纠正过来）。
    // 加载成功的条目（faces.front().path == 请求路径）不做任何文件系统查询：
    // 正常路径保持零额外 syscall；「同路径换字体文件内容」沿用 T1 既定语义
    // （整个字体栈 / 度量缓存都不监听文件变化，换文件走 setDefaultFontFiles 的
    // 清缓存或重启进程）—— 只有 fallback 这条非常态路径才值得每次多花几次 stat。
    bool loadedAsRequested = true;
    FontFileFingerprint requestedAtLoad{};
};

struct FontStackCache {
    std::unordered_map<std::string, FontStackCacheEntry> entries;
    std::uint64_t accessTick = 0;
};

FontStackCache& sharedFontStackCache() {
    static FontStackCache cache;
    return cache;
}

struct TextSizeCacheKey {
    std::string text;
    std::string fontFamily;
    float fontSize = 0.0f;
    float maxWidth = 0.0f;
    float lineHeight = 0.0f;
    int fontWeight = 0;
    bool wrap = false;

    bool operator==(const TextSizeCacheKey& other) const {
        return text == other.text &&
               fontFamily == other.fontFamily &&
               fontSize == other.fontSize &&
               maxWidth == other.maxWidth &&
               lineHeight == other.lineHeight &&
               fontWeight == other.fontWeight &&
               wrap == other.wrap;
    }
};

struct TextSizeCacheKeyHash {
    std::size_t operator()(const TextSizeCacheKey& key) const {
        std::size_t value = std::hash<std::string>{}(key.text);
        const auto combine = [&](std::size_t part) {
            value ^= part + 0x9e3779b9u + (value << 6u) + (value >> 2u);
        };
        combine(std::hash<std::string>{}(key.fontFamily));
        combine(std::hash<float>{}(key.fontSize));
        combine(std::hash<float>{}(key.maxWidth));
        combine(std::hash<float>{}(key.lineHeight));
        combine(std::hash<int>{}(key.fontWeight));
        combine(std::hash<bool>{}(key.wrap));
        return value;
    }
};

struct TextSizeCacheEntry {
    Vec2 size;
    std::uint64_t lastUsed = 0;
};

struct TextSizeCache {
    std::unordered_map<TextSizeCacheKey, TextSizeCacheEntry, TextSizeCacheKeyHash> entries;
    std::uint64_t accessTick = 0;
};

TextSizeCache& sharedTextSizeCache() {
    static TextSizeCache cache;
    return cache;
}

// ── 塑形结果缓存（T1 内层）──────────────────────────────────────────────────
// resize / 滚动时同一段文本一帧内会被 shape 两遍以上（组件的测量路径 + 图元的
// 渲染路径），编辑器全文档逐段 shape 是 resize 卡顿的大头。key 挂 holder 指针
// （每个字体栈×字号一份 face 布局，faceIndex 不会因兜底字体懒加载而漂移 ——
// faces 只增不动）；owner 用 weak_ptr，holder 被字体栈 LRU 淘汰后条目自动作废。
struct ShapingCacheKey {
    const void* holder = nullptr;
    std::string text;

    bool operator==(const ShapingCacheKey& other) const {
        return holder == other.holder && text == other.text;
    }
};

struct ShapingCacheKeyHash {
    std::size_t operator()(const ShapingCacheKey& key) const {
        std::size_t seed = std::hash<const void*>{}(key.holder);
        seed ^= std::hash<std::string>{}(key.text) + 0x9e3779b9U + (seed << 6) + (seed >> 2);
        return seed;
    }
};

struct ShapingCacheEntry {
    std::vector<TextPrimitive::ShapedGlyph> shaped;
    std::weak_ptr<FontInfoHolder> owner;
    const ShapingCacheKey* keyRef = nullptr;  // 指向 map 里那份 key（节点地址稳定）
    LruNode<ShapingCacheEntry> lru;
    std::size_t bytes = 0;                    // 估算占用，见 estimateShapingBytes
};

// 估算条目占用（不是精确值，只求"涨了能看见、封顶封得住"）：
// map 节点里那份 key + entry 自身（含 vector/weak_ptr 对象）+ 塑形缓冲。
std::size_t estimateShapingBytes(const ShapingCacheKey& key, const ShapingCacheEntry& entry) {
    return sizeof(ShapingCacheEntry) + sizeof(ShapingCacheKey) + key.text.size() +
           entry.shaped.capacity() * sizeof(TextPrimitive::ShapedGlyph);
}

struct ShapingCache {
    std::unordered_map<ShapingCacheKey, ShapingCacheEntry, ShapingCacheKeyHash> entries;
    LruList<ShapingCacheEntry> lru;
    std::size_t bytes = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;

    // 命中：摘下来放队首（O(1)），返回条目里的塑形结果。holder 指针对不上
    // （旧 holder 已析构、地址被复用）时条目作废，按 miss 处理。
    const std::vector<TextPrimitive::ShapedGlyph>* find(const ShapingCacheKey& key,
                                                        const std::shared_ptr<FontInfoHolder>& holder) {
        const auto it = entries.find(key);
        if (it == entries.end()) {
            ++misses;
            return nullptr;
        }
        if (it->second.owner.lock() != holder) {
            remove(it);
            ++misses;
            return nullptr;
        }
        lru.unlink(it->second.lru);
        lru.pushFront(it->second.lru);
        ++hits;
        return &it->second.shaped;
    }

    void insert(ShapingCacheKey key,
                const std::vector<TextPrimitive::ShapedGlyph>& shaped,
                const std::shared_ptr<FontInfoHolder>& holder) {
        ShapingCacheEntry entry;
        entry.shaped = shaped;  // 缓存放副本，调用方那份原样返回（与旧行为一致）
        entry.owner = holder;
        entry.bytes = estimateShapingBytes(key, entry);
        const auto inserted = entries.emplace(std::move(key), std::move(entry));
        if (!inserted.second) {
            return;  // 防御：同键已在（单锁内不该发生），不重复入链 / 重复计字节
        }
        ShapingCacheEntry& stored = inserted.first->second;
        stored.keyRef = &inserted.first->first;
        stored.lru.owner = &stored;
        lru.pushFront(stored.lru);
        bytes += stored.bytes;
        evict();
    }

    void clear() {
        lru.reset();
        entries.clear();
        bytes = 0;
        // hits / misses 归 neoTextCacheResetStats 管，clear 不动
    }

private:
    void remove(std::unordered_map<ShapingCacheKey, ShapingCacheEntry,
                                   ShapingCacheKeyHash>::iterator it) {
        lru.unlink(it->second.lru);
        bytes -= it->second.bytes;
        entries.erase(it);
    }

    // 淘汰到条数、字节都在限内：从最老一条开始，逐条 O(1) 摘除
    // （原来是整表 min_element 扫 lastUsed —— N>2048 时每次插入都 O(2048)，
    // 正是 perf_benchmark 里 N>2048 断崖的主因）。
    void evict() {
        while (entries.size() > kShapingCacheCapacity || bytes > kShapingCacheBudgetBytes) {
            ShapingCacheEntry* victim = lru.back();
            if (victim == nullptr) {
                break;
            }
            const auto it = entries.find(*victim->keyRef);
            if (it == entries.end()) {
                break;  // 不该发生
            }
            remove(it);
        }
    }
};

ShapingCache& sharedShapingCache() {
    static ShapingCache cache;
    return cache;
}

// ── 最终 TextMetrics 缓存（T1 外层）─────────────────────────────────────────
// 键用**解析后的字体路径**而不是 fontFamily 名：同族不同字样（粗体 / 系统回退）
// 会解析到不同文件，用名字会串；换默认字体文件（setDefaultFontFiles）会改解析
// 结果，而那条路正好调 clearSharedFontStackCache() 把本层一起清掉。
// holder 身份 = (path, size)：loadSharedFontStack 的 key 就是 path#round(size*64)，
// 本键即 holder 身份 —— 刻意不存 holder 裸指针，免得字体栈淘汰一次整层陪着 miss。
// 字号存调用方原值（已 clamp 到 ≥1）：同一输入必得同键。
//
// **路径身份是两支的，不是一个**（T1 复审问题 2）：请求的字体文件可能加载失败，
// loadSharedFontStack 会 fallback 到默认 / 系统字体，此时 metrics 是**实际字体**
// 算出来的。因此：
//   * 落键永远用 holder->faces.front().path（实际参与塑形的字体）—— metrics 只由
//     (实际字体, 字号, 文本) 决定，同一 faces[0] 必得同一结果；
//   * 只有「请求路径 == 实际路径」时才把条目落在请求路径那支键上，fallback 结果
//     绝不落回请求路径 —— 否则原路径一旦可用、holder 重载成真字体，请求路径那支
//     键还会命中旧 fallback 字形；
//   * 查询分两支（见 Impl::measureTextMetrics）：先查请求路径（常规情形，零额外
//     成本），miss 且 holder 已在手时再按实际路径查。刻意不把整次查询挪到
//     loadSharedFontStack 之后 —— 那样每次命中都要走字体栈（16 格 LRU，被淘汰
//     就重开 FreeType face），holder 一换内层键也跟着换、还得重新 shape。
//   * 失效：请求路径不可用期间积累的 fallback 结果由字体栈条目的文件指纹判定
//     过期（见 FontStackCacheEntry::requestedAtLoad），holder 一纠正，实际路径键
//     自然取到真字体；setDefaultFontFiles 换默认字体则整层清空。
struct TextMetricsCacheKey {
    std::string fontPath;
    std::string text;
    float fontSize = 0.0f;

    bool operator==(const TextMetricsCacheKey& other) const {
        return fontSize == other.fontSize && fontPath == other.fontPath && text == other.text;
    }
};

struct TextMetricsCacheKeyHash {
    std::size_t operator()(const TextMetricsCacheKey& key) const {
        std::size_t seed = std::hash<std::string>{}(key.fontPath);
        const auto combine = [&](std::size_t part) {
            seed ^= part + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
        };
        combine(std::hash<float>{}(key.fontSize));
        combine(std::hash<std::string>{}(key.text));
        return seed;
    }
};

struct TextMetricsCacheEntry {
    TextPrimitive::TextMetrics metrics;
    const TextMetricsCacheKey* keyRef = nullptr;  // 指向 map 里那份 key
    LruNode<TextMetricsCacheEntry> lru;
    std::size_t bytes = 0;                       // 估算占用，见 estimateTextMetricsBytes
};

// 估算条目占用：map 节点里那份 key + entry 自身 + 两支 caret 索引缓冲。
std::size_t estimateTextMetricsBytes(const TextMetricsCacheKey& key,
                                     const TextPrimitive::TextMetrics& metrics) {
    return sizeof(TextMetricsCacheEntry) + sizeof(TextMetricsCacheKey) + key.fontPath.size() +
           key.text.size() + metrics.byteIndices.capacity() * sizeof(int) +
           metrics.caretX.capacity() * sizeof(float);
}

struct TextMetricsCache {
    std::unordered_map<TextMetricsCacheKey, TextMetricsCacheEntry, TextMetricsCacheKeyHash> entries;
    LruList<TextMetricsCacheEntry> lru;
    std::size_t bytes = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;

    // 命中：摘下来放队首，返回条目的 TextMetrics（调用方按值拷走 —— 公开 API
    // 是按值签名，拷两支 vector 是有意保留的代价，换掉的是整条塑形链）。
    const TextPrimitive::TextMetrics* find(const TextMetricsCacheKey& key) {
        const auto it = entries.find(key);
        if (it == entries.end()) {
            ++misses;
            return nullptr;
        }
        lru.unlink(it->second.lru);
        lru.pushFront(it->second.lru);
        ++hits;
        return &it->second.metrics;
    }

    void insert(TextMetricsCacheKey key, TextPrimitive::TextMetrics metrics) {
        TextMetricsCacheEntry entry;
        entry.bytes = estimateTextMetricsBytes(key, metrics);
        entry.metrics = std::move(metrics);
        const auto inserted = entries.emplace(std::move(key), std::move(entry));
        if (!inserted.second) {
            return;  // 防御：同键已在（单锁内不该发生），不重复入链 / 重复计字节
        }
        TextMetricsCacheEntry& stored = inserted.first->second;
        stored.keyRef = &inserted.first->first;
        stored.lru.owner = &stored;
        lru.pushFront(stored.lru);
        bytes += stored.bytes;
        evict();
    }

    void clear() {
        lru.reset();
        entries.clear();
        bytes = 0;
    }

private:
    void remove(std::unordered_map<TextMetricsCacheKey, TextMetricsCacheEntry,
                                   TextMetricsCacheKeyHash>::iterator it) {
        lru.unlink(it->second.lru);
        bytes -= it->second.bytes;
        entries.erase(it);
    }

    void evict() {
        while (entries.size() > kTextMetricsCacheMaxEntries || bytes > kTextMetricsCacheBudgetBytes) {
            TextMetricsCacheEntry* victim = lru.back();
            if (victim == nullptr) {
                break;
            }
            const auto it = entries.find(*victim->keyRef);
            if (it == entries.end()) {
                break;  // 不该发生
            }
            remove(it);
        }
    }
};

TextMetricsCache& sharedTextMetricsCache() {
    static TextMetricsCache cache;
    return cache;
}

// 清掉全部共享文本缓存。setDefaultFontFiles 换默认字体后，字体路径解析结果全变，
// 字体栈 / TextSizeCache / T1 两层都可能指向旧身份，必须一起清（T1 验收 C）。
void clearSharedFontStackCache() {
    std::scoped_lock lock(textCacheMutex());

    FontStackCache& cache = sharedFontStackCache();
    cache.entries.clear();
    cache.accessTick = 0;

    TextSizeCache& sizeCache = sharedTextSizeCache();
    sizeCache.entries.clear();
    sizeCache.accessTick = 0;

    sharedShapingCache().clear();    // T1 内层
    sharedTextMetricsCache().clear();  // T1 外层
}

struct AtlasPage {
    int width = 0;
    int height = 0;
    int channels = 1;
    int x = 1;
    int y = 1;
    int rowHeight = 0;
    std::uint64_t generation = 0;
    std::vector<unsigned char> pixels;
    std::unordered_map<std::string, TextPrimitive::Glyph> glyphs;
};

struct SharedTextAtlas {
    AtlasPage gray;
    AtlasPage color;
    int references = 0;
    std::uint64_t grayGrowthCount = 0;
    std::uint64_t grayOverflowResetCount = 0;
    std::uint64_t overflowResetCount = 0;
};

unsigned int readUtf8Codepoint(const std::string& text, size_t& index);

FT_Library sharedFreeTypeLibrary() {
    static FT_Library library = [] {
        FT_Library created = nullptr;
        return FT_Init_FreeType(&created) == 0 ? created : nullptr;
    }();
    return library;
}

SharedTextAtlas& sharedTextAtlas() {
    static SharedTextAtlas atlas;
    return atlas;
}

std::uint64_t& textAtlasGenerationCounter() {
    static std::uint64_t generation = 0;
    return generation;
}

// 图集布局"纪元"：页重置或灰度页扩容改变归一化 UV 时 +1。图元发现自己落后，
// 会重新构建布局，从共享页索引重新取 UV；图元几何和字体度量不因此改变。
std::uint64_t& textAtlasResetEpoch() {
    static std::uint64_t epoch = 1;
    return epoch;
}

// 图集放不下新字形时整页作废：清掉 UV 索引与像素（缓冲保留，避免反复分配），
// 下一帧所有文本重新栅格化。之前的行为是"静默缓存一个空字形"——图集一旦满，
// 之后每个新字形都永久空白（长驻图元让引用计数永远不归零，图集再也出不来），
// 这就是"字用着用着缺字、支持的字形也画不出来"的根源。
void resetAtlasPageForOverflow(AtlasPage& page) {
    if (page.pixels.empty()) {
        return;
    }
    SharedTextAtlas& atlas = sharedTextAtlas();
    ++atlas.overflowResetCount;
    if (&page == &atlas.gray) {
        ++atlas.grayOverflowResetCount;
    }
    std::fill(page.pixels.begin(), page.pixels.end(), 0);
    page.x = 1;
    page.y = 1;
    page.rowHeight = 0;
    page.glyphs.clear();
    page.generation = ++textAtlasGenerationCounter();
    ++textAtlasResetEpoch();
}

bool ensureAtlasPage(AtlasPage& page, int width, int height, int channels) {
    if (!page.pixels.empty()) {
        return true;
    }

    page.width = width;
    page.height = height;
    page.channels = channels;
    page.x = 1;
    page.y = 1;
    page.rowHeight = 0;
    page.generation = ++textAtlasGenerationCounter();
    page.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(channels), 0);
    return !page.pixels.empty();
}

int grayAtlasInitialSize() {
    // Diagnostic-only paired-measurement switch. A separate process using the same
    // executable can restore the old eager 2048² footprint without UI/settings state.
    static const int size = [] {
        const char* value = std::getenv("NEO_GRAY_ATLAS_INITIAL_SIZE");
        return value != nullptr && std::string(value) == "2048"
                   ? kGrayAtlasMaxSize
                   : kGrayAtlasInitialSize;
    }();
    return size;
}

bool ensureGrayAtlasPage() {
    const int size = grayAtlasInitialSize();
    return ensureAtlasPage(sharedTextAtlas().gray, size, size, 1);
}

bool appendToAtlas(AtlasPage& page,
                   const unsigned char* pixels,
                   int width,
                   int height,
                   int channels,
                   TextPrimitive::Glyph& glyph);

bool growGrayAtlasPage() {
    SharedTextAtlas& atlas = sharedTextAtlas();
    AtlasPage& page = atlas.gray;
    if (page.pixels.empty() || page.width >= kGrayAtlasMaxSize || page.height >= kGrayAtlasMaxSize ||
        page.width != page.height) {
        return false;
    }

    const int oldSize = page.width;
    const int newSize = std::min(kGrayAtlasMaxSize, oldSize * 2);
    if (newSize <= oldSize) {
        return false;
    }

    std::vector<unsigned char> grown(static_cast<std::size_t>(newSize) * newSize, 0);
    for (int row = 0; row < oldSize; ++row) {
        const unsigned char* source = page.pixels.data() + static_cast<std::size_t>(row) * oldSize;
        unsigned char* destination = grown.data() + static_cast<std::size_t>(row) * newSize;
        std::copy_n(source, oldSize, destination);
    }

    // Pixel coordinates are unchanged, but normalized UVs are not. Rebase the shared
    // glyph index before publishing the larger page so other primitives can re-layout
    // by looking up their glyphs instead of rasterizing duplicate copies.
    const float scale = static_cast<float>(oldSize) / static_cast<float>(newSize);
    for (auto& [key, glyph] : page.glyphs) {
        (void)key;
        glyph.u0 *= scale;
        glyph.u1 *= scale;
        glyph.v0 *= scale;
        glyph.v1 *= scale;
    }

    page.pixels.swap(grown);
    page.width = newSize;
    page.height = newSize;
    page.generation = ++textAtlasGenerationCounter();
    ++atlas.grayGrowthCount;
    ++textAtlasResetEpoch();
    return true;
}

bool appendGrayGlyph(const unsigned char* pixels,
                     int width,
                     int height,
                     TextPrimitive::Glyph& glyph) {
    SharedTextAtlas& atlas = sharedTextAtlas();
    AtlasPage& page = atlas.gray;
    if (!ensureGrayAtlasPage()) {
        return false;
    }
    if (appendToAtlas(page, pixels, width, height, 1, glyph)) {
        return true;
    }

    while (page.width < kGrayAtlasMaxSize) {
        if (!growGrayAtlasPage()) {
            break;
        }
        if (appendToAtlas(page, pixels, width, height, 1, glyph)) {
            return true;
        }
    }

    // Preserve the established maximum-size policy: at 2048², discard the page and
    // restart packing. prepare() detects this epoch change and retries a bounded time.
    resetAtlasPageForOverflow(page);
    return appendToAtlas(page, pixels, width, height, 1, glyph);
}

bool retainSharedTextAtlas() {
    SharedTextAtlas& atlas = sharedTextAtlas();
    ++atlas.references;
    if (ensureGrayAtlasPage()) {
        return true;
    }
    atlas.references = std::max(0, atlas.references - 1);
    return false;
}

void releaseAtlasPage(AtlasPage& page) {
    page = {};
}

void releaseSharedTextAtlas() {
    SharedTextAtlas& atlas = sharedTextAtlas();
    atlas.references = std::max(0, atlas.references - 1);
    if (atlas.references > 0) {
        return;
    }

    releaseAtlasPage(atlas.gray);
    releaseAtlasPage(atlas.color);
}

std::uint64_t makeGlyphKey(size_t faceIndex, unsigned int glyphIndex) {
    return ((static_cast<std::uint64_t>(faceIndex) + 1ULL) << 32) | static_cast<std::uint64_t>(glyphIndex);
}

size_t faceIndexFromGlyphKey(std::uint64_t key) {
    return static_cast<size_t>((key >> 32) - 1ULL);
}

unsigned int glyphIndexFromGlyphKey(std::uint64_t key) {
    return static_cast<unsigned int>(key & 0xffffffffULL);
}

std::string glyphCacheKey(const FontFace& face, float fontSize, unsigned int glyphIndex, bool colored) {
    return face.path + "#" +
           std::to_string(static_cast<int>(std::round(fontSize * 64.0f))) + "#" +
           (colored ? "color#" : "gray#") +
           std::to_string(glyphIndex);
}

std::string existingPath(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        return path.u8string();
    }
    return {};
}

std::string firstExistingPath(std::initializer_list<const char*> paths) {
    for (const char* path : paths) {
        if (path == nullptr || path[0] == '\0') {
            continue;
        }
        if (const std::string existing = existingPath(path); !existing.empty()) {
            return existing;
        }
    }
    return {};
}

std::string& defaultUiFontFileOverride() {
    static std::string value;
    return value;
}

std::string& defaultIconFontFileOverride() {
    static std::string value;
    return value;
}

std::string resolveFontFilePath(const std::string& path);

std::string resolveSystemUiFontPath() {
#ifdef _WIN32
    return firstExistingPath({
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/arial.ttf"
    });
#elif defined(__APPLE__)
    return firstExistingPath({
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf"
    });
#else
    return firstExistingPath({
        // Debian / Ubuntu layout
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        // Fedora / RHEL family (static packages)
        "/usr/share/fonts/google-noto-sans/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto-sans-fonts/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-sans-cjk/NotoSansCJK-Regular.ttc",
        // Fedora 38+ variable-font packages
        "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf",
        "/usr/share/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf"
    });
#endif
}

std::string resolveSystemIconFontPath() {
#ifdef _WIN32
    return firstExistingPath({
        "C:/Windows/Fonts/seguisym.ttf",
        "C:/Windows/Fonts/SegMDL2.ttf",
        "C:/Windows/Fonts/seguiemj.ttf",
        "C:/Windows/Fonts/segoeui.ttf"
    });
#elif defined(__APPLE__)
    return firstExistingPath({
        "/System/Library/Fonts/Apple Symbols.ttf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/System/Library/Fonts/Helvetica.ttc"
    });
#else
    return firstExistingPath({
        // Debian / Ubuntu layout
        "/usr/share/fonts/fontawesome/fa-solid-900.ttf",
        "/usr/share/fonts/TTF/fa-solid-900.ttf",
        "/usr/share/fonts/truetype/font-awesome/fa-solid-900.ttf",
        "/usr/share/fonts/opentype/font-awesome/Font Awesome 6 Free-Solid-900.otf",
        "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansSymbols-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        // Fedora / RHEL family (static packages)
        "/usr/share/fonts/google-noto-sans-symbols2/NotoSansSymbols2-Regular.ttf",
        "/usr/share/fonts/google-noto-sans-symbols-fonts/NotoSansSymbols-Regular.ttf",
        "/usr/share/fonts/google-noto-sans-symbols/NotoSansSymbols-Regular.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        // Fedora 38+ variable-font packages
        "/usr/share/fonts/google-noto-vf/NotoSansSymbols[wght].ttf"
    });
#endif
}

std::string resolveSystemEmojiFontPath() {
#ifdef _WIN32
    return firstExistingPath({
        "C:/Windows/Fonts/seguiemj.ttf",
        "C:/Windows/Fonts/seguisym.ttf"
    });
#elif defined(__APPLE__)
    return firstExistingPath({
        "/System/Library/Fonts/Apple Color Emoji.ttc"
    });
#else
    return firstExistingPath({
        // Debian / Ubuntu layout
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/noto/NotoColorEmoji.ttf",
        "/usr/share/fonts/google-noto/NotoColorEmoji.ttf",
        // Fedora / RHEL family
        "/usr/share/fonts/google-noto-emoji/NotoColorEmoji.ttf",
        "/usr/share/fonts/google-noto-color-emoji-fonts/Noto-COLRv1.ttf"
    });
#endif
}

std::string resolveSystemMonospaceFontPath() {
#ifdef _WIN32
    // Consolas 优先于 Cascadia：CascadiaMono.ttf 是可变字体（wght 200-700），
    // 本管线的 FreeType 路径（整数 ppem + LIGHT hinting + linearHoriAdvance）渲染它时
    // 标点字形会整体缩小并下沉（`;` 只剩字母 1/4 高，实测 2026-09-25），代码块里
    // 引号/分号看起来挤作一团；静态字体的 Consolas 完全正常。谁先谁后只影响
    // "monospace" 预设的默认观感，用户仍可在设置里自选其它等宽字体。
    return firstExistingPath({
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/CascadiaMono.ttf",
        "C:/Windows/Fonts/CascadiaCode.ttf",
        "C:/Windows/Fonts/cour.ttf"
    });
#elif defined(__APPLE__)
    return firstExistingPath({
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Supplemental/Menlo.ttc",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Monaco.ttf"
    });
#else
    return firstExistingPath({
        // Debian / Ubuntu layout
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/TTF/Hack-Regular.ttf",
        // Fedora / RHEL family (static packages)
        "/usr/share/fonts/google-noto-sans-mono/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/google-noto-sans-mono-fonts/NotoSansMono-Regular.ttf",
        // Fedora 38+ variable-font packages
        "/usr/share/fonts/google-noto-vf/NotoSansMono[wght].ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf",
        "/usr/share/fonts/source-foundry-hack-fonts/Hack-Regular.ttf"
    });
#endif
}

std::filesystem::path executableDirectory() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD length = 0;
    while (true) {
        length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            break;
        }
        if (buffer.size() >= 32768u) return {};
        buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768u));
    }
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
#elif defined(__APPLE__)
    std::vector<char> buffer(4096);
    uint32_t size = static_cast<uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            return {};
        }
    }
    std::error_code error;
    return std::filesystem::absolute(std::filesystem::path(buffer.data()), error).parent_path();
#elif defined(__linux__)
    std::vector<char> buffer(4096);
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) {
        return {};
    }
    buffer[static_cast<size_t>(length)] = '\0';
    std::error_code error;
    return std::filesystem::absolute(std::filesystem::path(buffer.data()), error).parent_path();
#else
    return {};
#endif
}

std::string resolveProjectAssetPath(const std::string& filename) {
    const std::filesystem::path exeDir = executableDirectory();
    const std::filesystem::path candidates[] = {
        exeDir / "assets" / filename,
        std::filesystem::path("assets") / filename,
        std::filesystem::path("..") / "assets" / filename,
        std::filesystem::path("..") / ".." / "assets" / filename
    };

    for (const auto& candidate : candidates) {
        if (const std::string path = existingPath(candidate); !path.empty()) {
            return path;
        }
    }
    return {};
}

std::string resolveDefaultUiFontPath() {
    // override 的读侧必须与 setDefaultFontFiles 的写侧同一把锁（T1 复审问题 1）：
    // 这里返回的是拷贝，临界区只包住「读一次 string」，锁一释放拷贝就归本线程。
    // 调用方大多已经持锁（measureTextMetrics / loadSharedFontStack）—— 递归锁
    // 同线程重入，不产生新的锁序（单锁，见 setDefaultFontFiles 的证明）。
    std::scoped_lock lock(textCacheMutex());
    const std::string& override = defaultUiFontFileOverride();
    const std::string path = override.empty() ? resolveProjectAssetPath(kDefaultUiFontFile) : resolveFontFilePath(override);
    if (const std::string existing = existingPath(std::filesystem::u8path(path)); !existing.empty()) {
        return existing;
    }
    return resolveSystemUiFontPath();
}

std::string resolveDefaultIconFontPath() {
    // 与 resolveDefaultUiFontPath 同理：icon override 的读侧也取同一把锁。
    std::scoped_lock lock(textCacheMutex());
    const std::string& override = defaultIconFontFileOverride();
    if (override.empty() && core::platform::bundledResource(
            core::platform::BundledResourceId::EuiEditsFontAwesome)) {
        return core::platform::kEuiEditsFontAwesomeResourcePath;
    }
    const std::string path = override.empty() ? resolveProjectAssetPath(kDefaultIconFontFile) : resolveFontFilePath(override);
    if (const std::string existing = existingPath(std::filesystem::u8path(path)); !existing.empty()) {
        return existing;
    }
    return resolveSystemIconFontPath();
}

std::string resolveFontFilePath(const std::string& path) {
    const std::filesystem::path raw = std::filesystem::u8path(path);
    if (const std::string existing = existingPath(raw); !existing.empty()) {
        return existing;
    }

    const std::filesystem::path exeDir = executableDirectory();
    const std::filesystem::path candidates[] = {
        exeDir / "assets" / raw.filename(),
        exeDir / raw,
        std::filesystem::path("assets") / raw.filename(),
        std::filesystem::path("..") / "assets" / raw.filename(),
        std::filesystem::path("..") / ".." / "assets" / raw.filename()
    };

    for (const auto& candidate : candidates) {
        if (const std::string existing = existingPath(candidate); !existing.empty()) {
            return existing;
        }
    }
    return path;
}

bool isEmojiFontPath(const std::string& path) {
    const std::string filename = std::filesystem::u8path(path).filename().u8string();
    return filename.find("Emoji") != std::string::npos ||
           filename.find("emoji") != std::string::npos ||
           filename.find("seguiemj") != std::string::npos;
}

// FreeType's Windows file API interprets narrow filenames using CP_ACP.
// Keep Unicode font data alive for the face, sharing it across ppem sizes.
FT_Error openFontFace(FT_Library library, const std::string& path, FT_Face* face,
                     std::shared_ptr<std::vector<unsigned char>>& storage) {
    std::scoped_lock lock(textCacheMutex());
    if (path == core::platform::kEuiEditsFontAwesomeResourcePath) {
        const auto resource = core::platform::bundledResource(
            core::platform::BundledResourceId::EuiEditsFontAwesome);
        if (!resource || resource.size > static_cast<std::size_t>(std::numeric_limits<FT_Long>::max())) {
            return FT_Err_Cannot_Open_Resource;
        }
        storage.reset();
        return FT_New_Memory_Face(library, resource.data, static_cast<FT_Long>(resource.size), 0, face);
    }
#ifdef _WIN32
    const bool unicode = std::any_of(path.begin(), path.end(), [](unsigned char c) { return c >= 128; });
    if (unicode) {
        static std::unordered_map<std::string, std::weak_ptr<std::vector<unsigned char>>> memo;
        storage = memo[path].lock();
        if (!storage) {
            std::error_code error;
            const auto native = std::filesystem::u8path(path);
            const auto size = std::filesystem::file_size(native, error);
            if (error || size == 0 || size > 128u * 1024u * 1024u) return FT_Err_Cannot_Open_Resource;
            auto bytes = std::make_shared<std::vector<unsigned char>>(static_cast<std::size_t>(size));
            std::ifstream input(native, std::ios::binary);
            if (!input || !input.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(size)))
                return FT_Err_Cannot_Open_Resource;
            storage = std::move(bytes);
            if (memo.size() > 128) {
                for (auto it = memo.begin(); it != memo.end();) {
                    if (it->second.expired()) it = memo.erase(it);
                    else ++it;
                }
            }
            memo[path] = storage;
        }
        return FT_New_Memory_Face(library, storage->data(), static_cast<FT_Long>(storage->size()), 0, face);
    }
#endif
    return FT_New_Face(library, path.c_str(), 0, face);
}

bool loadFontFace(const std::string& path, float fontSize, FontFace& face) {
    FT_Library library = sharedFreeTypeLibrary();
    if (!library) {
        return false;
    }

    FT_Face loadedFace = nullptr;
    std::shared_ptr<std::vector<unsigned char>> storage;
    if (openFontFace(library, path, &loadedFace, storage) != 0 || !loadedFace) {
        return false;
    }

    const bool emojiFont = isEmojiFontPath(path);
    // 字号 -> em 的换算：**字号就是 em**（与 CSS / 浏览器同口径）。
    //
    // 这段改过两次，两次的毛病是同一个——"字号"渲染出来的字形比别的应用小：
    //   ① 最早是 upem/(ascender-descender)：同字号下各字体的视觉大小完全对不上，
    //      实测 (ascender-descender)/upem 从 1.04（等线/Deng）到 1.45（微软雅黑），
    //      黑体与 Consolas 恰好 1.00 —— 换一次字体字形能差 45%；
    //   ② 2026-09-22 改成固定基准 1.30（em = 字号/1.30，"字号 = 行高"语义）：
    //      字体之间齐了，但比别家一律小 30% —— 实测"设置里 16"只画出 em 12.3px，
    //      而 ZCode/Obsidian/浏览器里 16px 就是 em 16px。用户因此必须在应用内额外
    //      把缩放调到 125% 才能对上别家的观感（定位过程见
    //      参考/NeoEditor-S3计划-2026-09-23.md §1）。
    // 2026-09-23 起直接对齐 CSS 口径：em = 字号。
    //
    // **行高不在这里决定**：默认 lineHeight = 字号 × 1.2（见 Impl::measureTextSize 与
    // 渲染路径里的同名默认值），要更大行距的调用方显式给 lineHeight
    // （markdown 正文 1.75em、h1 2.5em…）。
    // 下面的行盒归一（boxNorm）仍保持"ascent - descent = 字号"：下游 maxGlyphHeight 与
    // 字形 yOffset 拿它当 em 高度用，改后它恰好等于真正的 em，语义反而更正。
    const float scaledFontSize = std::max(1.0f, fontSize);

    FT_Error sizeError = 1;
    float glyphScale = 1.0f;
    if ((emojiFont || FT_HAS_COLOR(loadedFace)) && loadedFace->num_fixed_sizes > 0) {
        const float targetPpem = scaledFontSize * 64.0f;
        int bestStrike = 0;
        float bestDistance = std::numeric_limits<float>::max();
        for (int i = 0; i < loadedFace->num_fixed_sizes; ++i) {
            const float strikePpem = static_cast<float>(loadedFace->available_sizes[i].y_ppem);
            const float distance = std::fabs(strikePpem - targetPpem);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestStrike = i;
            }
        }
        sizeError = FT_Select_Size(loadedFace, bestStrike);
        const float strikePpem = static_cast<float>(loadedFace->available_sizes[bestStrike].y_ppem) / 64.0f;
        if (strikePpem > 0.0f) {
            glyphScale = fontSize / strikePpem;
        }
    }
    if (sizeError != 0) {
        // 整数 ppem 光栅化：小数 ppem（dpi 1.25 × uiScale 0.05 步进之后到处都是）
        // 会让轻 hinting 的字干落在半像素上，同一个字忽清忽糊。对齐到整数像素后
        // 字形大小误差被限制在 ±0.5px，而且各字体仍然一致（同一个 ppem）。
        const float rasterPpem = std::max(1.0f, std::round(scaledFontSize));
        sizeError = FT_Set_Char_Size(
            loadedFace,
            0,
            static_cast<FT_F26Dot6>(rasterPpem * 64.0f),
            72,
            72);
        if (sizeError != 0) {
            const FT_UInt pixelSize = static_cast<FT_UInt>(std::max(1.0f, std::round(scaledFontSize)));
            sizeError = FT_Set_Pixel_Sizes(loadedFace, 0, pixelSize);
        }
    }
    if (sizeError != 0) {
        FT_Done_Face(loadedFace);
        return false;
    }

    face.path = path;
    face.face = loadedFace;
    face.fileBytes = std::move(storage);
    face.size = fontSize;
    face.glyphScale = glyphScale;
    face.colored = emojiFont || FT_HAS_COLOR(loadedFace);
    if (loadedFace->size && loadedFace->size->metrics.y_ppem > 0) {
        const float rawAscent =
            static_cast<float>(loadedFace->size->metrics.ascender) / 64.0f * glyphScale;
        const float rawDescent =
            static_cast<float>(loadedFace->size->metrics.descender) / 64.0f * glyphScale;
        // 行盒归一：让 ascender-descender 恒等于字号。字体的行盒设计值差异同样很大
        // （等线 1.04 em、Consolas 1.17 em、微软雅黑 1.45 em），不归一的话换字体
        // 时段落疏密也跟着变。归一化基准落在 ascender-descender 上而不是 height 上：
        // 下游 maxGlyphHeight、字形 yOffset 都拿 ascent_-descent_ 当"em 高度"用，
        // 这条保持不变才不会有第二处连锁反应。默认字体这一系数恰好是 1。
        const float rawBox = rawAscent - rawDescent;
        const float boxNorm = rawBox > 0.0f ? fontSize / rawBox : 1.0f;
        face.ascent = rawAscent * boxNorm;
        face.descent = rawDescent * boxNorm;
        // lineGap 归零：行高就是字号。字体的额外行距设计值同样因字体而异
        // （黑体 +36/256 em、Consolas +350/2048 em），留着就等于换字体变行距。
        face.lineGap = 0.0f;
    } else {
        face.ascent = fontSize * 0.8f;
        face.descent = -fontSize * 0.2f;
        face.lineGap = 0.0f;
    }
    return true;
}

std::string fontStackCacheKey(const std::string& fontPath, float fontSize) {
    return fontPath + "#" + std::to_string(static_cast<int>(std::round(fontSize * 64.0f)));
}

std::shared_ptr<FontInfoHolder> loadSharedFontStack(const std::string& fontPath, float fontSize) {
    std::scoped_lock lock(textCacheMutex());  // 与 T1 两层共用一把锁（递归，可被外层重入）

    const std::string cacheKey = fontStackCacheKey(fontPath, fontSize);
    FontStackCache& cache = sharedFontStackCache();
    const auto existing = cache.entries.find(cacheKey);
    if (existing != cache.entries.end()) {
        FontStackCacheEntry& entry = existing->second;
        // 短路顺序是刻意的：加载成功的条目（loadedAsRequested）一次文件系统查询
        // 都不做；只有 fallback 条目才付一次 stat（存在性翻转 / 换内容 → 丢弃重载）。
        if (entry.loadedAsRequested || fingerprintFontFile(fontPath) == entry.requestedAtLoad) {
            entry.lastUsed = ++cache.accessTick;
            return entry.holder;
        }
        // 请求路径的文件身份变了（原来缺的现在有了 / 原来在的没了或换了内容）：
        // 这条 holder 已经不代表 fontPath，删掉往下重载。
        cache.entries.erase(existing);
    }

    auto holder = std::make_shared<FontInfoHolder>();

    auto addLoadedFace = [&](const std::string& path) {
        if (path.empty()) {
            return false;
        }
        const bool alreadyLoaded = std::any_of(holder->faces.begin(), holder->faces.end(),
                                               [&](const FontFace& loadedFace) {
                                                   return loadedFace.path == path;
                                               });
        if (alreadyLoaded) {
            return true;
        }
        FontFace face;
        if (!loadFontFace(path, fontSize, face)) {
            return false;
        }
        holder->faces.push_back(std::move(face));
        return true;
    };

    if (!addLoadedFace(fontPath) &&
        !addLoadedFace(resolveDefaultUiFontPath()) &&
        !addLoadedFace(resolveSystemUiFontPath())) {
        return {};
    }

    auto addLazyFallback = [&](const std::string& fallbackPath) {
        if (fallbackPath.empty() || fallbackPath == fontPath) {
            return;
        }
        if (std::find(holder->lazyFallbackPaths.begin(), holder->lazyFallbackPaths.end(), fallbackPath) == holder->lazyFallbackPaths.end()) {
            holder->lazyFallbackPaths.push_back(fallbackPath);
        }
    };

    addLazyFallback(resolveDefaultUiFontPath());
    addLazyFallback(resolveDefaultIconFontPath());
    addLazyFallback(resolveSystemUiFontPath());
    addLazyFallback(resolveSystemIconFontPath());
    addLazyFallback(resolveSystemEmojiFontPath());

#ifdef _WIN32
    addLazyFallback("C:/Windows/Fonts/seguiemj.ttf");
    addLazyFallback("C:/Windows/Fonts/seguisym.ttf");
    addLazyFallback("C:/Windows/Fonts/msyh.ttc");
    addLazyFallback("C:/Windows/Fonts/simhei.ttf");
#elif defined(__APPLE__)
    addLazyFallback("/System/Library/Fonts/Apple Color Emoji.ttc");
    addLazyFallback("/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
    addLazyFallback("/System/Library/Fonts/Supplemental/Arial.ttf");
#else
    // Debian / Ubuntu layout
    addLazyFallback("/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf");
    addLazyFallback("/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc");
    addLazyFallback("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    // Fedora / RHEL family
    addLazyFallback("/usr/share/fonts/google-noto-emoji/NotoColorEmoji.ttf");
    addLazyFallback("/usr/share/fonts/google-noto-color-emoji-fonts/Noto-COLRv1.ttf");
    addLazyFallback("/usr/share/fonts/google-noto-sans-cjk/NotoSansCJK-Regular.ttc");
    addLazyFallback("/usr/share/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc");
    addLazyFallback("/usr/share/fonts/dejavu/DejaVuSans.ttf");
#endif

    if (cache.entries.size() >= kFontStackCacheCapacity) {
        const auto oldest = std::min_element(cache.entries.begin(), cache.entries.end(),
                                             [](const auto& left, const auto& right) {
                                                 return left.second.lastUsed < right.second.lastUsed;
                                             });
        if (oldest != cache.entries.end()) {
            cache.entries.erase(oldest);
        }
    }

    FontStackCacheEntry entry{holder, ++cache.accessTick};
    // 失效判定的记录点：请求字体没成为首面 = 这是 fallback holder，记下请求
    // 路径此刻的文件身份，供下次命中比对（见 FontStackCacheEntry::requestedAtLoad）。
    entry.loadedAsRequested = !holder->faces.empty() && holder->faces.front().path == fontPath;
    if (!entry.loadedAsRequested) {
        entry.requestedAtLoad = fingerprintFontFile(fontPath);
    }
    const auto inserted = cache.entries.emplace(cacheKey, std::move(entry));
    return inserted.first->second.holder;
}

bool isCombiningMark(unsigned int codepoint) {
    return (codepoint >= 0x0300 && codepoint <= 0x036F) ||
           (codepoint >= 0x1AB0 && codepoint <= 0x1AFF) ||
           (codepoint >= 0x1DC0 && codepoint <= 0x1DFF) ||
           (codepoint >= 0x20D0 && codepoint <= 0x20FF) ||
           (codepoint >= 0xFE20 && codepoint <= 0xFE2F);
}

bool isEmojiCodepoint(unsigned int codepoint) {
    return (codepoint >= 0x1F000 && codepoint <= 0x1FAFF) ||
           (codepoint >= 0x2600 && codepoint <= 0x27BF);
}

bool nextCodepointIsEmojiPresentation(const std::string& text, size_t index) {
    if (index >= text.size()) {
        return false;
    }
    const size_t saved = index;
    const unsigned int next = readUtf8Codepoint(text, index);
    (void)saved;
    return next == 0xFE0F;
}

unsigned int readUtf8Codepoint(const std::string& text, size_t& index) {
    const unsigned char first = static_cast<unsigned char>(text[index++]);
    if (first < 0x80) {
        return first;
    }
    if ((first >> 5) == 0x6 && index < text.size()) {
        return ((first & 0x1F) << 6) | (static_cast<unsigned char>(text[index++]) & 0x3F);
    }
    if ((first >> 4) == 0xE && index + 1 < text.size()) {
        unsigned int cp = (first & 0x0F) << 12;
        cp |= (static_cast<unsigned char>(text[index++]) & 0x3F) << 6;
        cp |= static_cast<unsigned char>(text[index++]) & 0x3F;
        return cp;
    }
    if ((first >> 3) == 0x1E && index + 2 < text.size()) {
        unsigned int cp = (first & 0x07) << 18;
        cp |= (static_cast<unsigned char>(text[index++]) & 0x3F) << 12;
        cp |= (static_cast<unsigned char>(text[index++]) & 0x3F) << 6;
        cp |= static_cast<unsigned char>(text[index++]) & 0x3F;
        return cp;
    }
    return '?';
}

constexpr std::size_t kNoFaceFound = static_cast<std::size_t>(-1);

// 码点 -> 能显示它的系统字体路径（空串 = 全系统都没有）。全局备忘：
// 一次运行里系统字体集合不变，跨字号/跨字体栈复用扫描结果，目录最多扫一遍一个码点。
std::unordered_map<unsigned int, std::string>& systemFontScanMemo() {
    static std::unordered_map<unsigned int, std::string> memo;
    return memo;
}

#ifdef _WIN32
size_t scanSystemFontsForCodepoint(FontInfoHolder& holder, unsigned int codepoint, float fontSize) {
    constexpr const char* kFontsDir = "C:/Windows/Fonts";
    auto& memo = systemFontScanMemo();
    if (const auto it = memo.find(codepoint); it != memo.end()) {
        if (it->second.empty()) {
            return kNoFaceFound;
        }
        for (size_t i = 0; i < holder.faces.size(); ++i) {
            if (holder.faces[i].path == it->second) {
                return i;
            }
        }
        FontFace face;
        if (!loadFontFace(it->second, fontSize, face)) {
            return kNoFaceFound;
        }
        holder.faces.push_back(std::move(face));
        return holder.faces.size() - 1;
    }

    FT_Library library = sharedFreeTypeLibrary();
    if (!library) {
        return kNoFaceFound;
    }

    std::vector<std::filesystem::path> candidates;
    std::error_code error;
    for (std::filesystem::directory_iterator it(kFontsDir, error), end; it != end; it.increment(error)) {
        if (error) {
            break;
        }
        const auto& path = it->path();
        const std::string extension = path.extension().string();
        std::string lowered;
        lowered.reserve(extension.size());
        for (char c : extension) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (lowered == ".ttf" || lowered == ".ttc" || lowered == ".otf") {
            candidates.push_back(path);
        }
    }
    std::sort(candidates.begin(), candidates.end());

    for (const std::filesystem::path& path : candidates) {
        const std::string pathString = path.u8string();
        const bool alreadyInStack = std::any_of(holder.faces.begin(), holder.faces.end(),
                                                [&](const FontFace& loaded) {
                                                    return loaded.path == pathString;
                                                });
        if (alreadyInStack) {
            continue;
        }
        FT_Face probe = nullptr;
        std::shared_ptr<std::vector<unsigned char>> probeStorage;
        if (openFontFace(library, pathString, &probe, probeStorage) != 0 || !probe) {
            continue;
        }
        const bool hasGlyph = FT_Get_Char_Index(probe, codepoint) != 0;
        FT_Done_Face(probe);
        if (!hasGlyph) {
            continue;
        }
        FontFace face;
        if (!loadFontFace(pathString, fontSize, face)) {
            continue;
        }
        holder.faces.push_back(std::move(face));
        memo.emplace(codepoint, pathString);
        return holder.faces.size() - 1;
    }

    memo.emplace(codepoint, std::string());
    return kNoFaceFound;
}
#endif

size_t findFaceForCodepoint(FontInfoHolder& holder, unsigned int codepoint, float fontSize) {
    if (holder.faces.empty()) {
        return 0;
    }
    if (codepoint == ' ' || codepoint == '\t' || codepoint == 0) {
        return 0;
    }

    for (size_t i = 0; i < holder.faces.size(); ++i) {
        if (FT_Get_Char_Index(holder.faces[i].face, codepoint) != 0) {
            return i;
        }
    }

    for (const std::string& fallbackPath : holder.lazyFallbackPaths) {
        if (fallbackPath.empty()) {
            continue;
        }

        const bool alreadyLoaded = std::any_of(holder.faces.begin(), holder.faces.end(),
                                               [&](const FontFace& loadedFace) {
                                                   return loadedFace.path == fallbackPath;
                                               });
        if (alreadyLoaded) {
            continue;
        }

        FontFace fallback;
        if (!loadFontFace(fallbackPath, fontSize, fallback)) {
            continue;
        }

        const bool hasGlyph = FT_Get_Char_Index(fallback.face, codepoint) != 0;
        holder.faces.push_back(std::move(fallback));
        if (hasGlyph) {
            return holder.faces.size() - 1;
        }
    }

#ifdef _WIN32
    // 既定字体栈与回退链都缺这个码点：扫一遍系统字体目录，找出第一份
    // cmap 里真有此字形的预装字体挂进字体栈。面向"生僻字/符号在自选字体
    // 与常用兜底字体里都没有，但系统里其实装了能显示它的字体"的场景。
    // 结果全局备忘（码点 -> 字体路径），跨字号只扫一次目录。
    if (const size_t scanned = scanSystemFontsForCodepoint(holder, codepoint, fontSize);
        scanned != kNoFaceFound) {
        return scanned;
    }
#endif

    return 0;
}

size_t findEmojiFaceForCodepoint(FontInfoHolder& holder, unsigned int codepoint, float fontSize) {
    for (size_t i = 0; i < holder.faces.size(); ++i) {
        if (holder.faces[i].colored && FT_Get_Char_Index(holder.faces[i].face, codepoint) != 0) {
            return i;
        }
    }

    for (const std::string& fallbackPath : holder.lazyFallbackPaths) {
        if (fallbackPath.empty() || !isEmojiFontPath(fallbackPath)) {
            continue;
        }

        const auto loaded = std::find_if(holder.faces.begin(), holder.faces.end(),
                                         [&](const FontFace& loadedFace) {
                                             return loadedFace.path == fallbackPath;
                                         });
        if (loaded != holder.faces.end()) {
            if (FT_Get_Char_Index(loaded->face, codepoint) != 0) {
                return static_cast<size_t>(std::distance(holder.faces.begin(), loaded));
            }
            continue;
        }

        FontFace fallback;
        if (!loadFontFace(fallbackPath, fontSize, fallback)) {
            continue;
        }

        const bool hasGlyph = FT_Get_Char_Index(fallback.face, codepoint) != 0;
        holder.faces.push_back(std::move(fallback));
        if (hasGlyph) {
            return holder.faces.size() - 1;
        }
    }

    return findFaceForCodepoint(holder, codepoint, fontSize);
}

float loadGlyphAdvance(const FontFace& face,
                       unsigned int glyphIndex,
                       unsigned int codepoint,
                       float fontSize,
                       float maxGlyphHeight) {
    if (codepoint == '\t') {
        return fontSize * 4.0f;
    }
    if (isCombiningMark(codepoint)) {
        return 0.0f;
    }
    if (FT_Load_Glyph(face.face, glyphIndex, kGlyphLoadFlags) != 0) {
        return fontSize * 0.5f;
    }

    float glyphScale = face.glyphScale;
    const float glyphHeight = static_cast<float>(face.face->glyph->metrics.height) / 64.0f;
    if (face.colored && glyphHeight > 0.0f) {
        glyphScale = std::min(glyphScale, maxGlyphHeight / glyphHeight);
    }
    // 推进量必须描述**马上要被栅格化的那枚字形**，否则相邻字形的落点与它自己的
    // 墨迹宽度对不上：
    //   · kGlyphLoadFlags 开着 hinting（FT_LOAD_TARGET_LIGHT）+ 整数 ppem，FreeType 会把
    //     字形的左右边距 snap 到整数像素，`advance.x` 才是这枚字形实际的格宽；
    //   · `linearHoriAdvance` 是**未 hint** 的线性推进量，与上面那枚已 hint 的字形无关。
    // 14px 下两者能差 0.3px：Consolas 线性 7.70 / hint 后 8.00，Cascadia 线性 8.20 / hint 后
    // 8.00。混用等于每字符固定错位 0.3px —— 线性值偏小就一路压着重叠（Consolas 每个 M 的
    // 墨迹宽 8px 却只前进 7.70px，符号糊成一片），偏大就一路拉开缝（Cascadia），而且 caret /
    // 选区矩形是按同一个推进量累加的，于是选框与字形整体错开。逐字符对照图见
    // docs/渲染问题标本-2026-09-26.md「补充四」。
    // 注意：若将来把 kGlyphLoadFlags 改成 FT_LOAD_NO_HINTING，这里必须换回
    // linearHoriAdvance（那时栅格化的字形才是未 hint 的）。
    return static_cast<float>(face.face->glyph->advance.x) / 64.0f * glyphScale;
}

std::vector<TextPrimitive::ShapedGlyph> shapeWithFallback(FontInfoHolder& holder,
                                                          const std::string& text,
                                                          float fontSize) {
    std::vector<TextPrimitive::ShapedGlyph> shaped;
    const float maxGlyphHeight = holder.faces.front().ascent - holder.faces.front().descent;
    size_t index = 0;
    while (index < text.size()) {
        const size_t start = index;
        const unsigned int codepoint = readUtf8Codepoint(text, index);
        const bool preferEmoji = isEmojiCodepoint(codepoint) || nextCodepointIsEmojiPresentation(text, index);
        const size_t faceIndex = preferEmoji
            ? findEmojiFaceForCodepoint(holder, codepoint, fontSize)
            : findFaceForCodepoint(holder, codepoint, fontSize);
        const FontFace& face = holder.faces[faceIndex];
        const unsigned int glyphIndex = codepoint == '\t' ? 0 : FT_Get_Char_Index(face.face, codepoint);
        const float advance = loadGlyphAdvance(face, glyphIndex, codepoint, fontSize, maxGlyphHeight);
        shaped.push_back({glyphIndex == 0 && codepoint == '\t' ? 0 : makeGlyphKey(faceIndex, glyphIndex),
                          codepoint,
                          static_cast<int>(start),
                          static_cast<int>(index),
                          advance,
                          0.0f,
                          0.0f});
    }
    return shaped;
}

// 塑形缓存的结构、预算与淘汰策略都在文件开头的「T1 双层文本度量缓存」区块里，
// 这里只剩查询入口：命中走 LRU（O(1)），关闭开关时整层不读不写。
std::vector<TextPrimitive::ShapedGlyph> shapeTextWithFontStack(const std::shared_ptr<FontInfoHolder>& holder,
                                                               const std::string& text,
                                                               float fontSize) {
    // 先取锁：下面无论是查缓存、走兜底字体扫描（会写 holder->faces 与
    // systemFontScanMemo），还是算完回填，都要在同一临界区里。
    std::scoped_lock lock(textCacheMutex());

    if (!holder || holder->faces.empty() || text.empty()) {
        return holder ? shapeWithFallback(*holder, text, fontSize)
                      : std::vector<TextPrimitive::ShapedGlyph>{};
    }

    ShapingCache& cache = sharedShapingCache();
    const bool cacheOn = !textCacheDisabled();
    const ShapingCacheKey key{holder.get(), text};
    if (cacheOn) {
        if (const std::vector<TextPrimitive::ShapedGlyph>* cached = cache.find(key, holder)) {
            return *cached;  // 按值拷贝给调用方（公开签名不变，缓存里保留一份）
        }
    }

    std::vector<TextPrimitive::ShapedGlyph> shaped = shapeWithFallback(*holder, text, fontSize);
    if (cacheOn) {
        cache.insert(key, shaped, holder);
    }
    return shaped;
}

TextPrimitive::TextMetrics makeTextMetrics(const std::string& text,
                                           const std::vector<TextPrimitive::ShapedGlyph>& shaped) {
    TextPrimitive::TextMetrics metrics;
    metrics.byteIndices.reserve(shaped.size() + 1);
    metrics.caretX.reserve(shaped.size() + 1);
    auto addStop = [&](int byteIndex, float x) {
        byteIndex = std::clamp(byteIndex, 0, static_cast<int>(text.size()));
        if (!metrics.byteIndices.empty() && metrics.byteIndices.back() == byteIndex) {
            metrics.caretX.back() = x;
            return;
        }
        metrics.byteIndices.push_back(byteIndex);
        metrics.caretX.push_back(x);
    };

    addStop(0, 0.0f);

    float cursorX = 0.0f;
    for (const TextPrimitive::ShapedGlyph& glyph : shaped) {
        const float startX = cursorX;
        cursorX += glyph.advance;
        addStop(glyph.byteStart, startX);
        addStop(glyph.byteEnd, cursorX);
    }

    metrics.width = cursorX;
    addStop(static_cast<int>(text.size()), metrics.width);
    return metrics;
}

bool appendToAtlas(AtlasPage& page,
                   const unsigned char* pixels,
                   int width,
                   int height,
                   int channels,
                   TextPrimitive::Glyph& glyph) {
    if (width <= 0 || height <= 0 || pixels == nullptr || channels <= 0 || page.pixels.empty()) {
        return true;
    }
    if (width + 2 >= page.width || height + 2 >= page.height) {
        return false;
    }

    if (page.x + width + 1 >= page.width) {
        page.x = 1;
        page.y += page.rowHeight + 1;
        page.rowHeight = 0;
    }
    if (page.y + height + 1 >= page.height) {
        return false;
    }

    const int copyChannels = std::min(channels, page.channels);
    for (int row = 0; row < height; ++row) {
        const auto dstOffset = (static_cast<std::size_t>(page.y + row) * static_cast<std::size_t>(page.width) +
                                static_cast<std::size_t>(page.x)) *
                               static_cast<std::size_t>(page.channels);
        const auto srcOffset = static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * static_cast<std::size_t>(channels);
        unsigned char* dst = page.pixels.data() + dstOffset;
        const unsigned char* src = pixels + srcOffset;
        for (int x = 0; x < width; ++x) {
            std::copy(src + static_cast<std::size_t>(x) * static_cast<std::size_t>(channels),
                      src + static_cast<std::size_t>(x) * static_cast<std::size_t>(channels) + copyChannels,
                      dst + static_cast<std::size_t>(x) * static_cast<std::size_t>(page.channels));
        }
    }

    glyph.u0 = static_cast<float>(page.x) / static_cast<float>(page.width);
    glyph.v0 = static_cast<float>(page.y) / static_cast<float>(page.height);
    glyph.u1 = static_cast<float>(page.x + width) / static_cast<float>(page.width);
    glyph.v1 = static_cast<float>(page.y + height) / static_cast<float>(page.height);

    page.x += width + 1;
    page.rowHeight = std::max(page.rowHeight, height);
    page.generation = ++textAtlasGenerationCounter();
    return true;
}

std::vector<unsigned char> copyGrayBitmap(const FT_Bitmap& bitmap) {
    if (bitmap.width == 0 || bitmap.rows == 0 || !bitmap.buffer ||
        std::abs(bitmap.pitch) < static_cast<int>(bitmap.width)) {
        return {};
    }
    std::vector<unsigned char> compact(static_cast<size_t>(bitmap.width) * bitmap.rows);
    const int pitch = std::abs(bitmap.pitch);
    const unsigned char* base = bitmap.pitch >= 0
        ? bitmap.buffer
        : bitmap.buffer + static_cast<ptrdiff_t>(bitmap.rows - 1) * pitch;
    for (unsigned int row = 0; row < bitmap.rows; ++row) {
        const unsigned char* source = bitmap.pitch >= 0
            ? base + static_cast<ptrdiff_t>(row) * pitch
            : base - static_cast<ptrdiff_t>(row) * pitch;
        std::copy(source, source + bitmap.width, compact.begin() + static_cast<ptrdiff_t>(row) * bitmap.width);
    }
    return compact;
}

std::vector<unsigned char> copyBgraBitmapAsRgba(const FT_Bitmap& bitmap) {
    if (bitmap.width == 0 || bitmap.rows == 0 || !bitmap.buffer ||
        std::abs(bitmap.pitch) < static_cast<int>(bitmap.width * 4)) {
        return {};
    }
    std::vector<unsigned char> compact(static_cast<size_t>(bitmap.width) * bitmap.rows * 4);
    const int pitch = std::abs(bitmap.pitch);
    const unsigned char* base = bitmap.pitch >= 0
        ? bitmap.buffer
        : bitmap.buffer + static_cast<ptrdiff_t>(bitmap.rows - 1) * pitch;
    for (unsigned int row = 0; row < bitmap.rows; ++row) {
        const unsigned char* source = bitmap.pitch >= 0
            ? base + static_cast<ptrdiff_t>(row) * pitch
            : base - static_cast<ptrdiff_t>(row) * pitch;
        unsigned char* target = compact.data() + static_cast<ptrdiff_t>(row) * bitmap.width * 4;
        for (unsigned int x = 0; x < bitmap.width; ++x) {
            target[x * 4 + 0] = source[x * 4 + 2];
            target[x * 4 + 1] = source[x * 4 + 1];
            target[x * 4 + 2] = source[x * 4 + 0];
            target[x * 4 + 3] = source[x * 4 + 3];
        }
    }
    return compact;
}

} // namespace

struct TextPrimitive::Impl {
    struct LaidOutGlyph {
        Glyph glyph;
        float x = 0.0f;
        float y = 0.0f;
    };

    struct Line {
        std::vector<LaidOutGlyph> glyphs;
        float width = 0.0f;
        float inkTop = 0.0f;
        float inkBottom = 0.0f;
        bool hasInk = false;
    };

    Impl() = default;
    Impl(float x, float y) : position_{x, y} {}

    bool initialize();
    void destroy();

    void setPosition(float x, float y);
    void setText(const std::string& text);
    void setFontFamily(const std::string& fontFamily);
    void setFontSize(float fontSize);
    void setFontWeight(int fontWeight);
    void setColor(const Color& color);
    void setMaxWidth(float maxWidth);
    void setWrap(bool wrap);
    void setHorizontalAlign(HorizontalAlign align);
    void setVerticalAlign(VerticalAlign align);
    void setLineHeight(float lineHeight);
    void setStyle(const TextStyle& style);
    void setVisualScale(float originX, float originY, float scale);
    void setTransform(const Transform& transform, const Rect& frame);
    void setTransformMatrix(const TransformMatrix& matrix);

    const TextStyle& style() const;
    Vec2 position() const;
    Vec2 measuredSize();
    static float measureTextWidth(const std::string& text,
                                  const std::string& fontFamily = {},
                                  float fontSize = 16.0f,
                                  int fontWeight = 400);
    static TextMetrics measureTextMetrics(const std::string& text,
                                          const std::string& fontFamily = {},
                                          float fontSize = 16.0f,
                                          int fontWeight = 400,
                                          float* widthOnly = nullptr);
    static Vec2 measureTextSize(const TextStyle& style);
    static void setDefaultFontFiles(const std::string& textFontFile, const std::string& iconFontFile);

    void prepare();
    void render(int windowWidth, int windowHeight);

    bool loadFont();
    bool ensureGlyph(const ShapedGlyph& shaped);
    Glyph* findGlyph(std::uint64_t key);
    void cacheGlyph(std::uint64_t key, const Glyph& glyph);
    bool rasterizeTofuGlyph(const ShapedGlyph& shaped, Glyph& glyph);
    void invalidateLayout();
    void invalidateVertices();
    void rebuildLayout();
    void rebuildVertices();
    std::vector<ShapedGlyph> shapeText(const std::string& text);
    void appendShapedGlyphToLine(Line& line, const ShapedGlyph& shaped, float& cursorX);

    static unsigned int readCodepoint(const std::string& text, size_t& index);
    static std::string resolveFontPath(const std::string& fontFamily, int fontWeight);
    static std::string resolveRegularFontPath(const std::string& fontFamily);
    static std::string resolveBoldFontPath(const std::string& fontFamily,
                                           const std::string& regularPath,
                                         int fontWeight);
    static std::string resolveItalicFontPath(const std::string& fontFamily);
    // 度量路径的塑形入口：与 measureTextMetrics 相同的字体栈解析（两端都走各自
    // 的缓存），但要拿回字形级推进量给 wrapLineStarts 用 —— TextMetrics 的
    // caretX 是按字节停靠点合并过的，不够用。
    static std::vector<ShapedGlyph> shapedForMeasure(const std::string& fontFamily,
                                                     float fontSize,
                                                     int fontWeight,
                                                     const std::string& text);

    Vec2 position_;
    Vec2 visualScaleOrigin_;
    float visualScale_ = 1.0f;
    Transform transform_;
    Rect transformFrame_;
    TransformMatrix transformMatrix_;
    bool hasTransformMatrix_ = false;
    TextStyle style_;
    std::shared_ptr<void> fontInfoStorage_;
    float scale_ = 1.0f;
    float ascent_ = 0.0f;
    float descent_ = 0.0f;
    float lineGap_ = 0.0f;

    std::unordered_map<std::uint64_t, Glyph> glyphs_;
    std::uint64_t layoutEpoch_ = 0;

    std::vector<Line> lines_;
    std::vector<float> vertices_;
    Vec2 measuredSize_;
    bool layoutDirty_ = true;
    bool verticesDirty_ = true;
    bool fontDirty_ = true;
};

bool TextPrimitive::Impl::initialize() {
    if (!loadFont() || !retainSharedTextAtlas()) {
        return false;
    }

    return true;
}

void TextPrimitive::Impl::destroy() {
    releaseSharedTextAtlas();
    fontInfoStorage_.reset();
    glyphs_.clear();
    glyphs_.rehash(0);
    lines_.clear();
    vertices_.clear();
    measuredSize_ = {};
    layoutDirty_ = true;
    verticesDirty_ = true;
    fontDirty_ = true;
}

void TextPrimitive::Impl::setPosition(float x, float y) {
    if (position_.x == x && position_.y == y) {
        return;
    }
    position_ = {x, y};
    invalidateVertices();
}

void TextPrimitive::Impl::setText(const std::string& text) {
    if (style_.text == text) {
        return;
    }
    if (style_.text.capacity() / 4u > text.size()) {
        std::string compactText = text;
        style_.text.swap(compactText);
    } else {
        style_.text = text;
    }
    // 虚拟列表会复用 TextPrimitive。glyph cache 仅服务于当前内容；共享 atlas
    // 仍保留栅格化结果，因此清理实例缓存不会重复分配 GPU atlas。
    glyphs_.clear();
    glyphs_.rehash(0);
    if (lines_.capacity() > 256u) {
        std::vector<Line>().swap(lines_);
    }
    // 长文本产生的顶点容量不能跟随 slot 永久保留；普通文本更新仍复用小缓存。
    constexpr std::size_t kMinimumRetainedVertexFloats = 32u * 1024u;
    const std::size_t desiredCapacity = std::max(
        kMinimumRetainedVertexFloats,
        text.size() <= std::numeric_limits<std::size_t>::max() / 48u
            ? text.size() * 48u
            : std::numeric_limits<std::size_t>::max());
    if (vertices_.capacity() > desiredCapacity) {
        std::vector<float>().swap(vertices_);
    }
    invalidateLayout();
}

void TextPrimitive::Impl::setFontFamily(const std::string& fontFamily) {
    if (style_.fontFamily == fontFamily) {
        return;
    }
    style_.fontFamily = fontFamily;
    fontDirty_ = true;
    invalidateLayout();
}

void TextPrimitive::Impl::setFontSize(float fontSize) {
    if (style_.fontSize == fontSize) {
        return;
    }
    style_.fontSize = fontSize;
    fontDirty_ = true;
    invalidateLayout();
}

void TextPrimitive::Impl::setFontWeight(int fontWeight) {
    if (style_.fontWeight == fontWeight) {
        return;
    }
    style_.fontWeight = fontWeight;
    fontDirty_ = true;
    invalidateLayout();
}

void TextPrimitive::Impl::setColor(const Color& color) {
    style_.color = color;
}

void TextPrimitive::Impl::setMaxWidth(float maxWidth) {
    if (style_.maxWidth == maxWidth) {
        return;
    }
    style_.maxWidth = maxWidth;
    invalidateLayout();
}

void TextPrimitive::Impl::setWrap(bool wrap) {
    if (style_.wrap == wrap) {
        return;
    }
    style_.wrap = wrap;
    invalidateLayout();
}

void TextPrimitive::Impl::setHorizontalAlign(HorizontalAlign align) {
    if (style_.horizontalAlign == align) {
        return;
    }
    style_.horizontalAlign = align;
    invalidateVertices();
}

void TextPrimitive::Impl::setVerticalAlign(VerticalAlign align) {
    if (style_.verticalAlign == align) {
        return;
    }
    style_.verticalAlign = align;
    invalidateVertices();
}

void TextPrimitive::Impl::setLineHeight(float lineHeight) {
    if (style_.lineHeight == lineHeight) {
        return;
    }
    style_.lineHeight = lineHeight;
    invalidateLayout();
}

void TextPrimitive::Impl::setVisualScale(float originX, float originY, float scale) {
    const float nextScale = std::max(0.01f, scale);
    if (visualScaleOrigin_.x == originX && visualScaleOrigin_.y == originY && visualScale_ == nextScale) {
        return;
    }
    visualScaleOrigin_ = {originX, originY};
    visualScale_ = nextScale;
    invalidateVertices();
}

void TextPrimitive::Impl::setTransform(const Transform& transform, const Rect& frame) {
    auto close = [](float left, float right) {
        return std::fabs(left - right) <= 0.0001f;
    };
    auto closeVec = [&](const Vec2& left, const Vec2& right) {
        return close(left.x, right.x) && close(left.y, right.y);
    };
    const bool sameTransform =
        closeVec(transform_.translate, transform.translate) &&
        close(transform_.translateZ, transform.translateZ) &&
        closeVec(transform_.scale, transform.scale) &&
        close(transform_.rotate, transform.rotate) &&
        close(transform_.rotateX, transform.rotateX) &&
        close(transform_.rotateY, transform.rotateY) &&
        closeVec(transform_.origin, transform.origin) &&
        close(transform_.perspective, transform.perspective);
    const bool sameFrame =
        close(transformFrame_.x, frame.x) &&
        close(transformFrame_.y, frame.y) &&
        close(transformFrame_.width, frame.width) &&
        close(transformFrame_.height, frame.height);
    if (sameTransform && sameFrame) {
        return;
    }
    transform_ = transform;
    transformFrame_ = frame;
    hasTransformMatrix_ = false;
    invalidateVertices();
}

void TextPrimitive::Impl::setTransformMatrix(const TransformMatrix& matrix) {
    auto close = [](float left, float right) {
        return std::fabs(left - right) <= 0.0001f;
    };
    const bool same =
        close(transformMatrix_.m00, matrix.m00) &&
        close(transformMatrix_.m01, matrix.m01) &&
        close(transformMatrix_.tx, matrix.tx) &&
        close(transformMatrix_.m10, matrix.m10) &&
        close(transformMatrix_.m11, matrix.m11) &&
        close(transformMatrix_.ty, matrix.ty) &&
        close(transformMatrix_.px, matrix.px) &&
        close(transformMatrix_.py, matrix.py) &&
        close(transformMatrix_.pw, matrix.pw) &&
        hasTransformMatrix_;
    if (same) {
        return;
    }
    transformMatrix_ = matrix;
    hasTransformMatrix_ = true;
    invalidateVertices();
}

void TextPrimitive::Impl::setStyle(const TextStyle& style) {
    const bool fontChanged = style.fontFamily != style_.fontFamily ||
                             style.fontSize != style_.fontSize ||
                             style.fontWeight != style_.fontWeight;
    style_ = style;
    fontDirty_ = fontDirty_ || fontChanged;
    invalidateLayout();
}

const TextStyle& TextPrimitive::Impl::style() const {
    return style_;
}

Vec2 TextPrimitive::Impl::position() const {
    return position_;
}

Vec2 TextPrimitive::Impl::measuredSize() {
    if (layoutDirty_) {
        rebuildLayout();
    }
    return measuredSize_;
}

float TextPrimitive::Impl::measureTextWidth(const std::string& text,
                                      const std::string& fontFamily,
                                      float fontSize,
                                      int fontWeight) {
    float width = 0.0f;
    measureTextMetrics(text, fontFamily, fontSize, fontWeight, &width);
    return width;
}

TextPrimitive::TextMetrics TextPrimitive::Impl::measureTextMetrics(const std::string& text,
                                                             const std::string& fontFamily,
                                                             float fontSize,
                                                             int fontWeight,
                                                             float* widthOnly) {
    TextMetrics empty;
    empty.byteIndices = {0};
    empty.caretX = {0.0f};
    if (text.empty()) {
        return empty;  // 空串不进缓存（与旧行为一致，也不必白占预算）
    }

    // T1 外层缓存。锁是递归的：本函数持锁期间 loadSharedFontStack /
    // shapeTextWithFontStack 会重入同一把锁。开关关掉时两层都不读不写，
    // 锁照拿 —— 字体栈缓存本身也在这把锁里。
    std::scoped_lock lock(textCacheMutex());
    const bool cacheOn = !textCacheDisabled();

    const float size = std::max(1.0f, fontSize);
    const std::string fontPath = resolveFontPath(fontFamily, fontWeight);

    // ── 第 1 支（快路径）：键 = 请求路径 ────────────────────────────────────
    // 字体文件正常可加载时实际字体就是请求路径，这一支命中直接按值返回 ——
    // 不碰字体栈、不 shape，热路径成本与 T1 原实现完全相同。键只有在真要查
    // 缓存时才构造（关掉开关时别平白复制两支 string）。
    TextMetricsCacheKey requestedKey;
    if (cacheOn) {
        requestedKey = TextMetricsCacheKey{fontPath, text, size};
        if (const TextMetrics* cached = sharedTextMetricsCache().find(requestedKey)) {
            // 命中：只拷 width + 两支 caret 数组 —— FreeType、塑形、
            // ShapedText 重建、O(条目数) 淘汰扫描全都不走（T1 验收 D）。
            if (widthOnly) { *widthOnly = cached->width; return {}; }
            return *cached;
        }
    }

    auto holder = loadSharedFontStack(fontPath, size);
    if (!holder || holder->faces.empty()) {
        return empty;
    }

    // ── 第 2 支 + 落键：键 = 实际加载的字体路径 ─────────────────────────────
    // 请求的字体文件加载失败时 loadSharedFontStack 会 fallback 到默认 / 系统
    // 字体，此刻 faces.front() 才是真正参与塑形的那份字体，metrics 只由它决定。
    // 若仍拿请求路径当键写进去，等原路径可用、holder 被指纹判定过期重载成真
    // 字体之后，第 1 支还会命中这条旧 fallback 字形（T1 复审问题 2）。
    // 反过来也不能"把整次查缓存挪到 load 之后"：那样每次命中都得先走字体栈
    // （LRU 只有 16 格，被淘汰就重开 FreeType face），holder 一换内层键跟着换、
    // 还得重新 shape。所以两支分工 —— 第 1 支覆盖常规情形且零额外成本；只有
    // 第 1 支 miss、holder 已经在手时才做第 2 支（命中照样不 shape，代价只是
    // 一次 map 查找；冷 fallback 度量因此记 2 次 miss）。
    // 失效策略在 loadSharedFontStack：fallback 条目按请求路径当时的文件指纹
    // （存在性 / 大小 / mtime）判定是否过期，过期即丢弃重载 —— 请求路径的"身份"
    // 只认文件系统指纹，绝不复用语义已经变了的旧 holder。
    TextMetricsCacheKey actualKey;
    if (cacheOn) {
        const std::string& actualPath = holder->faces.front().path;
        if (actualPath == fontPath) {
            // 实际就是请求的那支字体：第 1 支刚查过同一支键（这里刻意不再 find，
            // 免得一次冷查询记两次 miss），直接复用那份 key 落盘。
            actualKey = std::move(requestedKey);
        } else {
            actualKey = TextMetricsCacheKey{actualPath, text, size};
            if (const TextMetrics* cached = sharedTextMetricsCache().find(actualKey)) {
                if (widthOnly) { *widthOnly = cached->width; return {}; }
                return *cached;
            }
        }
    }

    const auto shaped = shapeTextWithFontStack(holder, text, size);
    if (widthOnly) {
        // The geometry pass needs the same shaped advances, but no per-character
        // caret arrays. Never publish an incomplete entry in the metrics cache.
        for (const auto& glyph : shaped) *widthOnly += glyph.advance;
        return {};
    }
    TextMetrics metrics = makeTextMetrics(text, shaped);
    if (cacheOn) {
        sharedTextMetricsCache().insert(std::move(actualKey), metrics);  // 按值拷一份进缓存
    }
    if (widthOnly) { *widthOnly = metrics.width; return {}; }
    return metrics;
}

// CJK 码点之间（以及 CJK 与西文之间）允许直接断行，不需要空格。
bool isCjkBreakCodepoint(unsigned int codepoint) {
    return (codepoint >= 0x3000 && codepoint <= 0x30FF) ||   // CJK 标点 + 假名
           (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||   // 扩展 A
           (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||   // CJK 统一表意文字
           (codepoint >= 0xAC00 && codepoint <= 0xD7AF) ||   // 谚文音节
           (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||   // 兼容表意文字
           (codepoint >= 0xFF00 && codepoint <= 0xFFEF);     // 全角形式
}

// 贪心换行的**唯一决策点**：给定段内字形序列，返回每行第一个字形的下标。
// 渲染（rebuildLayout）与度量（measureTextSize）都必须走这里，两边行数才会
// 一致。溢出时回退到行内最后一个可断缝隙（空格之后，或任一侧是 CJK 码点的
// 缝隙），缝隙前的行尾空白归入丢弃区；行内没有可断缝隙、或断点前全是空白时
// 退回在溢出字形处硬断（超长英文单词的兜底）。
std::vector<size_t> wrapLineStarts(const std::vector<TextPrimitive::ShapedGlyph>& shaped, float maxWidth) {
    std::vector<size_t> starts{0};
    if (maxWidth <= 0.0f || shaped.empty()) {
        return starts;
    }
    float cursorX = 0.0f;
    size_t lineStart = 0;
    for (size_t i = 0; i < shaped.size(); ++i) {
        if (cursorX > 0.0f && cursorX + shaped[i].advance > maxWidth) {
            long brk = -1;
            for (long j = static_cast<long>(i); j > static_cast<long>(lineStart); --j) {
                const TextPrimitive::ShapedGlyph& prev = shaped[static_cast<size_t>(j) - 1];
                const TextPrimitive::ShapedGlyph& next = shaped[static_cast<size_t>(j)];
                if (prev.codepoint == ' ' || prev.codepoint == '\t' ||
                    isCjkBreakCodepoint(prev.codepoint) || isCjkBreakCodepoint(next.codepoint)) {
                    brk = j;
                    break;
                }
            }
            size_t nextStart = brk >= 0 ? static_cast<size_t>(brk) : i;
            if (brk >= 0) {
                size_t keepEnd = nextStart;
                while (keepEnd > lineStart &&
                       (shaped[keepEnd - 1].codepoint == ' ' || shaped[keepEnd - 1].codepoint == '\t')) {
                    --keepEnd;
                }
                if (keepEnd == lineStart) {
                    nextStart = i;   // 断点前全是空白：退回硬断，避免空行死循环
                }
            }
            starts.push_back(nextStart);
            lineStart = nextStart;
            cursorX = 0.0f;
            for (size_t k = nextStart; k < i; ++k) {
                cursorX += shaped[k].advance;
            }
        }
        cursorX += shaped[i].advance;
    }
    return starts;
}

// 行内最后一个非空白字形的下一个下标：换行时行尾空白不渲染也不计宽。
size_t lineInkEnd(const std::vector<TextPrimitive::ShapedGlyph>& shaped, size_t begin, size_t end) {
    while (end > begin &&
           (shaped[end - 1].codepoint == ' ' || shaped[end - 1].codepoint == '\t')) {
        --end;
    }
    return end;
}

std::vector<TextPrimitive::ShapedGlyph> TextPrimitive::Impl::shapedForMeasure(const std::string& fontFamily,
                                                                              float fontSize,
                                                                              int fontWeight,
                                                                              const std::string& text) {
    const float size = std::max(1.0f, fontSize);
    const std::string fontPath = resolveFontPath(fontFamily, fontWeight);
    auto holder = loadSharedFontStack(fontPath, size);
    if (!holder || holder->faces.empty()) {
        return {};
    }
    return shapeTextWithFontStack(holder, text, size);
}

Vec2 TextPrimitive::Impl::measureTextSize(const TextStyle& style) {
    // 递归锁：下面整段（含逐段回调 measureTextMetrics）都在临界区内。
    std::scoped_lock lock(textCacheMutex());

    TextSizeCache& cache = sharedTextSizeCache();
    TextSizeCacheKey cacheKey{
        style.text,
        style.fontFamily,
        style.fontSize,
        style.wrap ? style.maxWidth : 0.0f,
        style.lineHeight,
        style.fontWeight,
        style.wrap
    };
    const auto cached = cache.entries.find(cacheKey);
    if (cached != cache.entries.end()) {
        cached->second.lastUsed = ++cache.accessTick;
        return cached->second.size;
    }

    const float lineHeight = style.lineHeight > 0.0f ? style.lineHeight : style.fontSize * 1.2f;
    const float maxWidth = style.wrap && style.maxWidth > 0.0f ? style.maxWidth : 0.0f;
    float measuredWidth = 0.0f;
    int lineCount = 0;

    size_t paragraphStart = 0;
    while (paragraphStart <= style.text.size()) {
        const size_t newline = style.text.find('\n', paragraphStart);
        const size_t paragraphEnd = newline == std::string::npos ? style.text.size() : newline;
        std::string paragraph = style.text.substr(paragraphStart, paragraphEnd - paragraphStart);
        if (!paragraph.empty() && paragraph.back() == '\r') {
            paragraph.pop_back();
        }

        // 与 rebuildLayout 完全同一套断行决策（wrapLineStarts），度量与渲染的
        // 行数/行宽才不会因断词差异各说各话。
        const std::vector<TextPrimitive::ShapedGlyph> shaped =
            shapedForMeasure(style.fontFamily, style.fontSize, style.fontWeight, paragraph);        const std::vector<size_t> starts = wrapLineStarts(shaped, maxWidth);
        lineCount += static_cast<int>(starts.size());
        for (size_t lineIdx = 0; lineIdx < starts.size(); ++lineIdx) {
            const size_t begin = starts[lineIdx];
            const size_t end = lineIdx + 1 < starts.size() ? starts[lineIdx + 1] : shaped.size();
            float lineWidth = 0.0f;
            const size_t inkEnd = lineInkEnd(shaped, begin, end);
            for (size_t k = begin; k < inkEnd; ++k) {
                lineWidth += shaped[k].advance;
            }
            measuredWidth = std::max(measuredWidth, lineWidth);
        }

        if (newline == std::string::npos) {
            break;
        }
        paragraphStart = newline + 1;
    }

    const Vec2 measuredSize{measuredWidth, static_cast<float>(lineCount) * lineHeight};
    if (cache.entries.size() >= kTextSizeCacheCapacity) {
        const auto oldest = std::min_element(cache.entries.begin(), cache.entries.end(),
                                             [](const auto& left, const auto& right) {
                                                 return left.second.lastUsed < right.second.lastUsed;
                                             });
        if (oldest != cache.entries.end()) {
            cache.entries.erase(oldest);
        }
    }
    cache.entries.emplace(std::move(cacheKey), TextSizeCacheEntry{measuredSize, ++cache.accessTick});
    return measuredSize;
}

void TextPrimitive::Impl::setDefaultFontFiles(const std::string& textFontFile, const std::string& iconFontFile) {
    // T1 复审问题 1：两个 override 都是裸 std::string，写侧必须与读侧
    // （measureTextMetrics → resolveFontPath → resolveDefaultUiFontPath，以及
    // loadSharedFontStack 里那几处）共用同一把锁，否则读线程能撞上写到一半的
    // string（SSO/堆指针撕裂 → 崩溃或解析出半个路径）。从函数入口就把锁拿满：
    // 下面的**比较、两次赋值、clearSharedFontStackCache 全程**都在临界区内，
    // 中间不放锁 —— 「读到新 override 但缓存还是旧的」这个窗口一并堵死。
    //
    // 锁序 / 死锁证明：本子系统只有 textCacheMutex 这**一把**锁（递归）。它的一
    // 个持有者在临界区内只会再取它自己（clearSharedFontStackCache /
    // loadSharedFontStack / shapeTextWithFontStack 都是同线程重入），从不取第二把
    // 互斥量，也不回调外部代码（只有 FreeType、文件系统与纯计算）—— 单锁没有
    // 顺序可言，A→B / B→A 的环无从构成，因此这里持锁期间重入 clear 不可能死锁。
    // 反方向（外部线程先拿自己的锁再进来取 textCacheMutex）也不会成环：text.cpp
    // 从不持 textCacheMutex 等待外部锁。
    std::scoped_lock lock(textCacheMutex());
    if (defaultUiFontFileOverride() == textFontFile &&
        defaultIconFontFileOverride() == iconFontFile) {
        return;
    }
    defaultUiFontFileOverride() = textFontFile;
    defaultIconFontFileOverride() = iconFontFile;
    clearSharedFontStackCache();  // 递归重入同一把锁，见上
}

void TextPrimitive::Impl::prepare() {
    // Gray-page growth preserves pixels but changes normalized UVs. A rebuild that
    // triggered growth can therefore contain glyphs using both old and new UV scales;
    // rerun until the atlas epoch is stable (at most two growth steps from 512 to 2048).
    // Overflow at maximum size retains its reset policy and aborts this prepare after
    // one pass; a later frame gets a fresh bounded attempt rather than looping forever.
    constexpr int kMaxLayoutAttempts = 3;
    SharedTextAtlas& atlas = sharedTextAtlas();
    for (int attempt = 0; attempt < kMaxLayoutAttempts; ++attempt) {
        const std::uint64_t epochBefore = textAtlasResetEpoch();
        const std::uint64_t overflowResetsBefore = atlas.overflowResetCount;
        if (layoutDirty_ || layoutEpoch_ != epochBefore) {
            rebuildLayout();
        }
        const std::uint64_t epochAfter = textAtlasResetEpoch();
        if (layoutEpoch_ == epochAfter && epochBefore == epochAfter) {
            if (verticesDirty_) {
                rebuildVertices();
            }
            return;
        }

        if (atlas.overflowResetCount != overflowResetsBefore) {
            // This document exceeded the maximum page during the rebuild. Keep its
            // measured layout for callers, but never submit vertices whose UVs point
            // into a page that was cleared halfway through the pass.
            vertices_.clear();
            verticesDirty_ = true;
            layoutDirty_ = true;
            return;
        }

        layoutDirty_ = true;
        verticesDirty_ = true;
    }

    // A concurrent page change or an unexpected series of epoch changes must not
    // publish mixed UVs. The next prepare retries with the current generation.
    vertices_.clear();
    verticesDirty_ = true;
    layoutDirty_ = true;
}

void TextPrimitive::Impl::render(int windowWidth, int windowHeight) {
    core::render::RenderBackend* backend = core::render::activeRenderBackend();
    if (backend == nullptr || windowWidth <= 0 || windowHeight <= 0) {
        return;
    }

    prepare();

    if (vertices_.empty()) {
        return;
    }

    const SharedTextAtlas& atlas = sharedTextAtlas();
    core::render::TextDrawCommand command{};
    command.vertices = vertices_.data();
    command.vertexFloatCount = vertices_.size();
    command.color = style_.color;
    command.grayAtlas = {
        core::render::TextAtlasPageKind::Gray,
        atlas.gray.width,
        atlas.gray.height,
        atlas.gray.channels,
        atlas.gray.generation,
        atlas.gray.pixels.empty() ? nullptr : atlas.gray.pixels.data()
    };
    command.colorAtlas = {
        core::render::TextAtlasPageKind::Color,
        atlas.color.width,
        atlas.color.height,
        atlas.color.channels,
        atlas.color.generation,
        atlas.color.pixels.empty() ? nullptr : atlas.color.pixels.data()
    };
    backend->drawText(command, windowWidth, windowHeight);
}

bool TextPrimitive::Impl::loadFont() {
    const std::string fontPath = resolveFontPath(style_.fontFamily, style_.fontWeight);
    auto holder = loadSharedFontStack(fontPath, style_.fontSize);
    if (!holder || holder->faces.empty()) {
        return false;
    }

    fontInfoStorage_ = holder;
    scale_ = 1.0f;
    ascent_ = holder->faces.front().ascent;
    descent_ = holder->faces.front().descent;
    lineGap_ = holder->faces.front().lineGap;

    glyphs_.clear();

    fontDirty_ = false;
    return true;
}

bool TextPrimitive::Impl::ensureGlyph(const ShapedGlyph& shaped) {
    if (shaped.key == 0) {
        return true;
    }
    if (const Glyph* cached = findGlyph(shaped.key)) {
        if (cached->atlasEpoch == textAtlasResetEpoch()) {
            return true;
        }
        // 图集被整页重置过：缓存里的 UV 指向已清空的像素，作废重栅格化。
        glyphs_.erase(shaped.key);
    }

    if (fontDirty_ && !loadFont()) {
        return false;
    }

    auto holder = std::static_pointer_cast<FontInfoHolder>(fontInfoStorage_);
    if (!holder || holder->faces.empty()) {
        return false;
    }

    const size_t faceIndex = faceIndexFromGlyphKey(shaped.key);
    const unsigned int glyphIndex = glyphIndexFromGlyphKey(shaped.key);
    if (faceIndex >= holder->faces.size()) {
        return false;
    }

    FontFace& face = holder->faces[faceIndex];
    Glyph glyph;
    glyph.advance = shaped.advance;
    glyph.colored = face.colored;

    if (shaped.codepoint == ' ' || shaped.codepoint == '\t') {
        cacheGlyph(shaped.key, glyph);
        return true;
    }

    if (glyphIndex == 0) {
        // 码点在整个字体栈（含系统兜底链、目录扫描）里都没有：
        // 画一个豆腐块而不是空白，保证"看得出这里有个字符"。
        return rasterizeTofuGlyph(shaped, glyph);
    }

    if (FT_Load_Glyph(face.face, glyphIndex, kGlyphLoadFlags) != 0) {
        return rasterizeTofuGlyph(shaped, glyph);
    }

    FT_GlyphSlot slot = face.face->glyph;
    glyph.advance = shaped.advance;

    if (slot->format != FT_GLYPH_FORMAT_BITMAP) {
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) {
            cacheGlyph(shaped.key, glyph);
            return true;
        }
    }

    const FT_Bitmap& bitmap = slot->bitmap;
    if (bitmap.width > 4096 || bitmap.rows > 4096 ||
        static_cast<std::uint64_t>(bitmap.width) * bitmap.rows > 4u * 1024u * 1024u) {
        return rasterizeTofuGlyph(shaped, glyph);
    }
    const bool colorBitmap = bitmap.pixel_mode == FT_PIXEL_MODE_BGRA;
    float glyphScale = face.glyphScale;
    if (colorBitmap && bitmap.rows > 0) {
        const float emHeight = ascent_ - descent_;
        glyphScale = std::min(glyphScale, emHeight / static_cast<float>(bitmap.rows));
    }
    glyph.colored = colorBitmap;
    glyph.xOffset = static_cast<float>(slot->bitmap_left) * glyphScale;
    glyph.yOffset = ascent_ - static_cast<float>(slot->bitmap_top) * glyphScale;
    glyph.width = static_cast<float>(bitmap.width) * glyphScale;
    glyph.height = static_cast<float>(bitmap.rows) * glyphScale;
    if (colorBitmap) {
        glyph.yOffset = ascent_ - descent_ - glyph.height;
    }

    if (bitmap.width == 0 || bitmap.rows == 0 || !bitmap.buffer) {
        cacheGlyph(shaped.key, glyph);
        return true;
    }

    const std::string cacheKey = glyphCacheKey(face, style_.fontSize, glyphIndex, colorBitmap);
    SharedTextAtlas& atlas = sharedTextAtlas();
    AtlasPage& page = colorBitmap ? atlas.color : atlas.gray;
    if (const auto cached = page.glyphs.find(cacheKey); cached != page.glyphs.end()) {
        glyph.u0 = cached->second.u0;
        glyph.v0 = cached->second.v0;
        glyph.u1 = cached->second.u1;
        glyph.v1 = cached->second.v1;
        cacheGlyph(shaped.key, glyph);
        return true;
    }

    if (colorBitmap) {
        if (!ensureAtlasPage(atlas.color, kColorAtlasSize, kColorAtlasSize, 4)) {
            cacheGlyph(shaped.key, glyph);
            return true;
        }
        std::vector<unsigned char> rgba = copyBgraBitmapAsRgba(bitmap);
        if (rgba.empty()) {
            cacheGlyph(shaped.key, glyph);
            return true;
        }
        if (!appendToAtlas(atlas.color, rgba.data(), static_cast<int>(bitmap.width), static_cast<int>(bitmap.rows), 4, glyph)) {
            resetAtlasPageForOverflow(atlas.color);
            if (!appendToAtlas(atlas.color, rgba.data(), static_cast<int>(bitmap.width), static_cast<int>(bitmap.rows), 4, glyph)) {
                cacheGlyph(shaped.key, glyph);
                return true;
            }
        }
        atlas.color.glyphs[cacheKey] = glyph;
    } else if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
        std::vector<unsigned char> gray = copyGrayBitmap(bitmap);
        if (gray.empty()) {
            cacheGlyph(shaped.key, glyph);
            return true;
        }
        if (!appendGrayGlyph(gray.data(), static_cast<int>(bitmap.width), static_cast<int>(bitmap.rows), glyph)) {
            cacheGlyph(shaped.key, glyph);
            return true;
        }
        atlas.gray.glyphs[cacheKey] = glyph;
    } else {
        cacheGlyph(shaped.key, glyph);
        return true;
    }

    cacheGlyph(shaped.key, glyph);
    return true;
}

// 字体栈彻底缺字时的兜底：合成一个"豆腐块"轮廓位图放进图集，让"这里有个字符"
// 看得见。之前的行为是缓存一枚空字形——用户看到的就是"直接渲染空白"。
// 位图坐标系直接用逻辑像素（写入图集时不做 face 缩放），大小随当前字号走。
bool TextPrimitive::Impl::rasterizeTofuGlyph(const ShapedGlyph& shaped, Glyph& glyph) {
    const float em = std::max(1.0f, style_.fontSize);
    const int w = std::max(4, static_cast<int>(std::round(em * 0.58f)));
    const int h = std::max(6, static_cast<int>(std::round(em * 0.64f)));
    // 描边至少 2 逻辑像素：1px 描边在非整数像素相位会被抗锯齿摊到看不清
    // （实测 125% 缩放下右移半个像素就只剩底边可见）。兜底符号宁可粗一点。
    const int stroke = std::max(2, static_cast<int>(std::round(em * 0.09f)));
    std::vector<unsigned char> box(static_cast<std::size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (x < stroke || x >= w - stroke || y < stroke || y >= h - stroke) {
                box[static_cast<std::size_t>(y) * w + x] = 255;
            }
        }
    }

    glyph.width = static_cast<float>(w);
    glyph.height = static_cast<float>(h);
    glyph.xOffset = 0.0f;
    glyph.yOffset = ascent_ - glyph.height;
    glyph.colored = false;

    SharedTextAtlas& atlas = sharedTextAtlas();
    if (ensureGrayAtlasPage()) {
        // 同字号共用一枚豆腐；glyphIndex 用一个真实字形到不了的水位避免撞 key。
        const std::string tofuKey = glyphCacheKey(FontFace{}, style_.fontSize, 0x7FFFFFFFu, false);
        if (const auto cached = atlas.gray.glyphs.find(tofuKey); cached != atlas.gray.glyphs.end()) {
            glyph.u0 = cached->second.u0;
            glyph.v0 = cached->second.v0;
            glyph.u1 = cached->second.u1;
            glyph.v1 = cached->second.v1;
        } else if (appendGrayGlyph(box.data(), w, h, glyph)) {
            atlas.gray.glyphs[tofuKey] = glyph;
        }
    }

    cacheGlyph(shaped.key, glyph);
    return true;
}

TextPrimitive::Glyph* TextPrimitive::Impl::findGlyph(std::uint64_t key) {
    const auto it = glyphs_.find(key);
    return it == glyphs_.end() ? nullptr : &it->second;
}

void TextPrimitive::Impl::cacheGlyph(std::uint64_t key, const Glyph& glyph) {
    if (key == 0) {
        return;
    }
    Glyph stored = glyph;
    stored.atlasEpoch = textAtlasResetEpoch();
    glyphs_[key] = std::move(stored);
}

void TextPrimitive::Impl::invalidateLayout() {
    layoutDirty_ = true;
    invalidateVertices();
}

void TextPrimitive::Impl::rebuildLayout() {
    if (fontDirty_ && !loadFont()) {
        layoutDirty_ = false;
        return;
    }

    layoutEpoch_ = textAtlasResetEpoch();
    lines_.clear();
    measuredSize_ = {};

    Line currentLine;
    float cursorX = 0.0f;
    const float lineHeight = style_.lineHeight > 0.0f ? style_.lineHeight : style_.fontSize * 1.2f;
    const float maxWidth = style_.maxWidth > 0.0f ? style_.maxWidth : 0.0f;

    size_t paragraphStart = 0;
    while (paragraphStart <= style_.text.size()) {
        const size_t newline = style_.text.find('\n', paragraphStart);
        const size_t paragraphEnd = newline == std::string::npos ? style_.text.size() : newline;
        std::string paragraph = style_.text.substr(paragraphStart, paragraphEnd - paragraphStart);
        if (!paragraph.empty() && paragraph.back() == '\r') {
            paragraph.pop_back();
        }

        const std::vector<ShapedGlyph> shaped = shapeText(paragraph);
        const std::vector<size_t> starts = wrapLineStarts(shaped, style_.wrap ? maxWidth : 0.0f);
        for (size_t lineIdx = 0; lineIdx < starts.size(); ++lineIdx) {
            const size_t begin = starts[lineIdx];
            const size_t end = lineIdx + 1 < starts.size() ? starts[lineIdx + 1] : shaped.size();
            const size_t inkEnd = lineInkEnd(shaped, begin, end);
            for (size_t k = begin; k < inkEnd; ++k) {
                appendShapedGlyphToLine(currentLine, shaped[k], cursorX);
            }
            measuredSize_.x = std::max(measuredSize_.x, currentLine.width);
            lines_.push_back(currentLine);
            currentLine = Line{};
            cursorX = 0.0f;
        }

        if (newline == std::string::npos) {
            break;
        }

        paragraphStart = newline + 1;
    }

    measuredSize_.y = lines_.empty() ? 0.0f : static_cast<float>(lines_.size()) * lineHeight;
    layoutDirty_ = false;
    invalidateVertices();
}

void TextPrimitive::Impl::invalidateVertices() {
    verticesDirty_ = true;
}

void TextPrimitive::Impl::rebuildVertices() {
    vertices_.clear();
    const float lineHeight = style_.lineHeight > 0.0f ? style_.lineHeight : style_.fontSize * 1.2f;
    float blockYOffset = 0.0f;
    if (style_.verticalAlign == VerticalAlign::Center) {
        float inkTop = std::numeric_limits<float>::max();
        float inkBottom = std::numeric_limits<float>::lowest();
        for (size_t lineIndex = 0; lineIndex < lines_.size(); ++lineIndex) {
            const Line& line = lines_[lineIndex];
            if (!line.hasInk) {
                continue;
            }
            const float lineY = static_cast<float>(lineIndex) * lineHeight;
            inkTop = std::min(inkTop, lineY + line.inkTop);
            inkBottom = std::max(inkBottom, lineY + line.inkBottom);
        }
        if (inkTop <= inkBottom) {
            blockYOffset = -(inkTop + inkBottom) * 0.5f;
        } else {
            blockYOffset = -measuredSize_.y * 0.5f;
        }
    } else if (style_.verticalAlign == VerticalAlign::Bottom) {
        blockYOffset = -measuredSize_.y;
    }

    for (size_t lineIndex = 0; lineIndex < lines_.size(); ++lineIndex) {
        const Line& line = lines_[lineIndex];
        float lineX = position_.x;
        if (style_.horizontalAlign == HorizontalAlign::Center) {
            lineX -= line.width * 0.5f;
        } else if (style_.horizontalAlign == HorizontalAlign::Right) {
            lineX -= line.width;
        }

        const float lineY = position_.y + blockYOffset + static_cast<float>(lineIndex) * lineHeight;
        for (const LaidOutGlyph& laidOut : line.glyphs) {
            const Glyph& glyph = laidOut.glyph;
            const float x0 = lineX + laidOut.x + glyph.xOffset;
            const float y0 = lineY + laidOut.y + glyph.yOffset;
            const float x1 = x0 + glyph.width;
            const float y1 = y0 + glyph.height;
            const float colored = glyph.colored ? 1.0f : 0.0f;
            Vec2 p0{x0, y0};
            Vec2 p1{x1, y0};
            Vec2 p2{x1, y1};
            Vec2 p3{x0, y1};

            if (hasTransformMatrix_) {
                p0 = core::transformPoint(transformMatrix_, p0.x, p0.y);
                p1 = core::transformPoint(transformMatrix_, p1.x, p1.y);
                p2 = core::transformPoint(transformMatrix_, p2.x, p2.y);
                p3 = core::transformPoint(transformMatrix_, p3.x, p3.y);
            } else if (std::fabs(transform_.translate.x) > 0.0001f ||
                std::fabs(transform_.translate.y) > 0.0001f ||
                std::fabs(transform_.scale.x - 1.0f) > 0.0001f ||
                std::fabs(transform_.scale.y - 1.0f) > 0.0001f ||
                std::fabs(transform_.rotate) > 0.0001f) {
                const Vec2 origin{
                    transformFrame_.x + transformFrame_.width * transform_.origin.x,
                    transformFrame_.y + transformFrame_.height * transform_.origin.y
                };
                const float cosine = std::cos(transform_.rotate);
                const float sine = std::sin(transform_.rotate);
                auto transformPoint = [&](Vec2 point) {
                    const float scaledX = (point.x - origin.x) * transform_.scale.x;
                    const float scaledY = (point.y - origin.y) * transform_.scale.y;
                    return Vec2{
                        origin.x + scaledX * cosine - scaledY * sine + transform_.translate.x,
                        origin.y + scaledX * sine + scaledY * cosine + transform_.translate.y
                    };
                };
                p0 = transformPoint(p0);
                p1 = transformPoint(p1);
                p2 = transformPoint(p2);
                p3 = transformPoint(p3);
            }

            if (!hasTransformMatrix_ && std::fabs(visualScale_ - 1.0f) > 0.0001f) {
                auto scalePoint = [&](Vec2 point) {
                    return Vec2{
                        visualScaleOrigin_.x + (point.x - visualScaleOrigin_.x) * visualScale_,
                        visualScaleOrigin_.y + (point.y - visualScaleOrigin_.y) * visualScale_
                    };
                };
                p0 = scalePoint(p0);
                p1 = scalePoint(p1);
                p2 = scalePoint(p2);
                p3 = scalePoint(p3);
            }

            // 落点强制对齐整数像素。这里把矩形四角平移同一个 (offsetX, offsetY)，
            // 是纯平移，任何变换下都安全。旧条件只在"矩阵平移已是整数"时才取整，
            // 恰好把滚动容器（renderTransform 的 tx/ty 是小数）排除在外——
            // 结果越常滚动的文本越发虚。不对齐时 GL_LINEAR 会在相邻像素间插值发虚。
            // 彩色字形（emoji 位图）保持亚像素落点，不硬跳。
            if (!glyph.colored) {
                const float offsetX = std::round(p0.x) - p0.x;
                const float offsetY = std::round(p0.y) - p0.y;
                p0 = {p0.x + offsetX, p0.y + offsetY};
                p1 = {p1.x + offsetX, p1.y + offsetY};
                p2 = {p2.x + offsetX, p2.y + offsetY};
                p3 = {p3.x + offsetX, p3.y + offsetY};
            }

            vertices_.insert(vertices_.end(), {
                p0.x, p0.y, glyph.u0, glyph.v0, colored,
                p1.x, p1.y, glyph.u1, glyph.v0, colored,
                p2.x, p2.y, glyph.u1, glyph.v1, colored,
                p0.x, p0.y, glyph.u0, glyph.v0, colored,
                p2.x, p2.y, glyph.u1, glyph.v1, colored,
                p3.x, p3.y, glyph.u0, glyph.v1, colored
            });
        }
    }
    verticesDirty_ = false;
}

std::vector<TextPrimitive::ShapedGlyph> TextPrimitive::Impl::shapeText(const std::string& text) {
    if (fontDirty_ && !loadFont()) {
        return {};
    }
    auto holder = std::static_pointer_cast<FontInfoHolder>(fontInfoStorage_);
    if (!holder || holder->faces.empty()) {
        return {};
    }
    return shapeTextWithFontStack(holder, text, std::max(1.0f, style_.fontSize));
}

void TextPrimitive::Impl::appendShapedGlyphToLine(Line& line, const ShapedGlyph& shaped, float& cursorX) {
    if (!ensureGlyph(shaped)) {
        cursorX += shaped.advance;
        line.width = cursorX;
        return;
    }

    const Glyph* glyph = findGlyph(shaped.key);
    if (glyph && shaped.key != 0 && shaped.codepoint != ' ' && shaped.codepoint != '\t') {
        line.glyphs.push_back({*glyph, cursorX + shaped.xOffset, shaped.yOffset});
        const float top = shaped.yOffset + glyph->yOffset;
        const float bottom = top + glyph->height;
        if (!line.hasInk) {
            line.inkTop = top;
            line.inkBottom = bottom;
            line.hasInk = true;
        } else {
            line.inkTop = std::min(line.inkTop, top);
            line.inkBottom = std::max(line.inkBottom, bottom);
        }
    }
    cursorX += shaped.advance;
    line.width = cursorX;
}

unsigned int TextPrimitive::Impl::readCodepoint(const std::string& text, size_t& index) {
    return readUtf8Codepoint(text, index);
}

// 打开候选字体看它到底是不是斜体（style_flags 的 ITALIC 位，或 style 名带 Italic/Oblique）。
// 按命名约定猜出来的候选可能撞到同名前缀的非斜体文件（字体名以 i/it 结尾很常见），
// 这道校验是必须的 —— 只认文件名会让"同族另一个字体"被当成斜体挂上去。
bool faceLooksItalic(const std::string& path) {
    std::scoped_lock lock(textCacheMutex());
    FT_Library library = sharedFreeTypeLibrary();
    if (library == nullptr) {
        return false;
    }
    FT_Face face = nullptr;
    std::shared_ptr<std::vector<unsigned char>> storage;
    if (openFontFace(library, path, &face, storage) != 0 || face == nullptr) {
        return false;
    }
    bool italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    if (!italic && face->style_name != nullptr) {
        const std::string name = face->style_name;
        italic = name.find("talic") != std::string::npos || name.find("blique") != std::string::npos;
    }
    FT_Done_Face(face);
    return italic;
}

// 粗体字面：本框架不做仿粗（合成粗体要改字形位图，属于渲染管线改动），所以"加粗"
// 必须落到**另一份字体文件**上。找不到粗体文件就回落常规字面——与改造前的行为完全一致，
// 因此"粗体只在系统里真有这份字体时才看得出差别"。
std::string TextPrimitive::Impl::resolveBoldFontPath(const std::string& fontFamily,
                                                     const std::string& regularPath,
                                                     int fontWeight) {
    (void) fontWeight;
#ifdef _WIN32
    if (fontFamily == "Microsoft YaHei" || fontFamily == "YaHei") {
        if (const std::string path = existingPath("C:/Windows/Fonts/msyhbd.ttc"); !path.empty()) {
            return path;
        }
    }
    if (fontFamily == "SimHei" || fontFamily == "SimSun" || fontFamily == "NSimSun") {
        // 黑体/宋体在系统里只有常规字面，回落到常规路径（黑体本身就偏粗）。
        return regularPath;
    }
#endif
    // 斜体字面上再要粗体（`***粗斜***`）：常规的 "-Bold" 后缀在斜体文件名上不存在
    // （ariali-Bold.ttf 没有），先按"粗斜体"命名找（ariali → arialbi、Roboto-Italic →
    // Roboto-BoldItalic）。找不到就落到下面的常规粗体搜索，多半再回落成"只有斜、没有粗"。
    if (!regularPath.empty()) {
        const std::size_t slash = regularPath.find_last_of("/\\");
        const std::size_t dot = regularPath.find_last_of('.');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            const std::string stem = regularPath.substr(0, dot);
            const std::string ext = regularPath.substr(dot);
            std::string base;
            for (const char* tail : {"-Italic", "_Italic", " Italic", "Italic",
                                     "-Oblique", "_Oblique", " Oblique", "Oblique", "oblique",
                                     "it", "i"}) {
                const std::string suffix(tail);
                if (stem.size() > suffix.size() &&
                    stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    base = stem.substr(0, stem.size() - suffix.size());
                    break;
                }
            }
            if (!base.empty()) {
                for (const char* biSuffix : {"bi", "bz", "BoldItalic", "-BoldItalic", "_BoldItalic",
                                             " BoldItalic", "BoldOblique", "-BoldOblique"}) {
                    const std::string path = existingPath(std::filesystem::u8path(base + biSuffix + ext));
                    if (!path.empty() && faceLooksItalic(path)) {
                        return path;
                    }
                }
            }
        }
    }

    // 自选字体文件：按常见命名约定找同族粗体（X.ttf → X-Bold.ttf / XBold.ttf / Xbd.ttf …）。
    if (!regularPath.empty()) {
        const std::size_t slash = regularPath.find_last_of("/\\");
        const std::size_t dot = regularPath.find_last_of('.');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
            const std::string stem = regularPath.substr(0, dot);
            const std::string ext = regularPath.substr(dot);
            for (const char* suffix : {"-Bold", "Bold", "bd", "-bd", "-SemiBold", "SB"}) {
                if (const std::string path = existingPath(std::filesystem::u8path(stem + suffix + ext)); !path.empty()) {
                    return path;
                }
            }
        }
    }
    return regularPath;
}

// 斜体字面（S3d 降级版）：与粗体同理，只认"另一份字体文件"，不做合成斜体。
// 候选按命名约定生成（arial.ttf → ariali.ttf / arial-Italic.ttf；Roboto-Regular.ttf →
// Roboto-Italic.ttf），逐个用 faceLooksItalic 校验后才认。找不到返回**空串** ——
// 调用方（装饰层）据此不生成样式段，让文字保持原样，而不是造一个假斜体。
// 按常规字面路径 memo（空结果也记）：装饰表每次重建都会来问一次。
std::string TextPrimitive::Impl::resolveItalicFontPath(const std::string& fontFamily) {
    const std::string regular = resolveRegularFontPath(fontFamily);
    if (regular.empty()) {
        return {};
    }
    static std::unordered_map<std::string, std::string> italicMemo;
    const auto cached = italicMemo.find(regular);
    if (cached != italicMemo.end()) {
        return cached->second;
    }

    std::string found;
    const std::size_t slash = regular.find_last_of("/\\");
    const std::size_t dot = regular.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        const std::string stem = regular.substr(0, dot);
        const std::string ext = regular.substr(dot);
        std::vector<std::string> candidates;
        for (const char* suffix : {"i", "it", "Italic", "-Italic", "_Italic", " Italic",
                                   "Oblique", "-Oblique", "_Oblique", " Oblique"}) {
            candidates.push_back(stem + suffix + ext);
        }
        // Family-Style 命名：把结尾的 Regular 换成 Italic（Roboto-Regular.ttf → Roboto-Italic.ttf）。
        for (const char* tail : {"-Regular", "_Regular", " Regular", "Regular"}) {
            const std::string baseTail(tail);
            if (stem.size() > baseTail.size() &&
                stem.compare(stem.size() - baseTail.size(), baseTail.size(), baseTail) == 0) {
                const std::string base = stem.substr(0, stem.size() - baseTail.size());
                for (const char* suffix : {"Italic", "-Italic", "_Italic", " Italic",
                                           "Oblique", "-Oblique"}) {
                    candidates.push_back(base + suffix + ext);
                }
                break;
            }
        }
        for (const std::string& candidate : candidates) {
            if (candidate == regular) {
                continue;
            }
            const std::string path = existingPath(std::filesystem::u8path(candidate));
            if (!path.empty() && faceLooksItalic(path)) {
                found = path;
                break;
            }
        }
    }
    italicMemo[regular] = found;
    return found;
}

std::string TextPrimitive::Impl::resolveRegularFontPath(const std::string& fontFamily) {
    if (!fontFamily.empty() && fontFamily.find('.') != std::string::npos) {
        return resolveFontFilePath(fontFamily);
    }

    if (fontFamily == "YouSheBiaoTiHei" || fontFamily == "YouShe") {
        return resolveFontFilePath("YouSheBiaoTiHei-2.ttf");
    }

    if (fontFamily == "Title" || fontFamily == "PingFang" || fontFamily == "PingFang SC") {
        return resolveDefaultUiFontPath();
    }

    if (fontFamily == "FontAwesome" || fontFamily == "Font Awesome" ||
        fontFamily == "Font Awesome 7 Free" || fontFamily == "Icon") {
        return resolveDefaultIconFontPath();
    }

#ifdef _WIN32
    if (fontFamily == "monospace" || fontFamily == "Mono" ||
        fontFamily == "Cascadia Mono" || fontFamily == "Cascadia Code") {
        if (const std::string path = resolveSystemMonospaceFontPath(); !path.empty()) {
            return path;
        }
    }
    if (fontFamily == "Microsoft YaHei" || fontFamily == "YaHei") {
        return "C:/Windows/Fonts/msyh.ttc";
    }
    if (fontFamily == "Segoe UI Emoji" || fontFamily == "Emoji") {
        if (const std::string path = resolveSystemEmojiFontPath(); !path.empty()) {
            return path;
        }
        return resolveDefaultUiFontPath();
    }
    if (fontFamily == "SimHei") {
        return "C:/Windows/Fonts/simhei.ttf";
    }
    return resolveDefaultUiFontPath();
#elif defined(__APPLE__)
    if (fontFamily == "monospace" || fontFamily == "Mono" ||
        fontFamily == "SF Mono" || fontFamily == "Menlo" || fontFamily == "Monaco") {
        if (const std::string path = resolveSystemMonospaceFontPath(); !path.empty()) {
            return path;
        }
    }
    if (fontFamily == "Noto Color Emoji" || fontFamily == "Apple Color Emoji" || fontFamily == "Emoji") {
        if (const std::string path = resolveSystemEmojiFontPath(); !path.empty()) {
            return path;
        }
        return resolveDefaultUiFontPath();
    }
    return resolveDefaultUiFontPath();
#else
    if (fontFamily == "monospace" || fontFamily == "Mono") {
        if (const std::string path = resolveSystemMonospaceFontPath(); !path.empty()) {
            return path;
        }
        return resolveDefaultUiFontPath();
    }
    if (fontFamily == "Noto Color Emoji" || fontFamily == "Emoji") {
        if (const std::string path = resolveSystemEmojiFontPath(); !path.empty()) {
            return path;
        }
        return resolveDefaultUiFontPath();
    }
    return resolveDefaultUiFontPath();
#endif
}

// 常规字面 + "字重 >= 600 时换粗体字样" 这一层非常薄，但把字重这条信息真正落到了字体文件上：
// 改造前 fontWeight 只参与缓存键，两支都返回同一个文件，等于字重不起作用。
std::string TextPrimitive::Impl::resolveFontPath(const std::string& fontFamily, int fontWeight) {
    const std::string regular = resolveRegularFontPath(fontFamily);
    if (fontWeight < 600) {
        return regular;
    }
    return resolveBoldFontPath(fontFamily, regular, fontWeight);
}

TextPrimitive::TextPrimitive()
    : impl_(std::make_unique<Impl>()) {}

TextPrimitive::TextPrimitive(float x, float y)
    : impl_(std::make_unique<Impl>(x, y)) {}

TextPrimitive::~TextPrimitive() = default;
TextPrimitive::TextPrimitive(TextPrimitive&&) noexcept = default;
TextPrimitive& TextPrimitive::operator=(TextPrimitive&&) noexcept = default;

bool TextPrimitive::initialize() { return impl_->initialize(); }
void TextPrimitive::destroy() { impl_->destroy(); }
void TextPrimitive::setPosition(float x, float y) { impl_->setPosition(x, y); }
void TextPrimitive::setText(const std::string& text) { impl_->setText(text); }
void TextPrimitive::setFontFamily(const std::string& fontFamily) { impl_->setFontFamily(fontFamily); }
void TextPrimitive::setFontSize(float fontSize) { impl_->setFontSize(fontSize); }
void TextPrimitive::setFontWeight(int fontWeight) { impl_->setFontWeight(fontWeight); }
void TextPrimitive::setColor(const Color& color) { impl_->setColor(color); }
void TextPrimitive::setMaxWidth(float maxWidth) { impl_->setMaxWidth(maxWidth); }
void TextPrimitive::setWrap(bool wrap) { impl_->setWrap(wrap); }
void TextPrimitive::setHorizontalAlign(HorizontalAlign align) { impl_->setHorizontalAlign(align); }
void TextPrimitive::setVerticalAlign(VerticalAlign align) { impl_->setVerticalAlign(align); }
void TextPrimitive::setLineHeight(float lineHeight) { impl_->setLineHeight(lineHeight); }
void TextPrimitive::setStyle(const TextStyle& style) { impl_->setStyle(style); }
void TextPrimitive::setVisualScale(float originX, float originY, float scale) { impl_->setVisualScale(originX, originY, scale); }
void TextPrimitive::setTransform(const Transform& transform, const Rect& frame) { impl_->setTransform(transform, frame); }
void TextPrimitive::setTransformMatrix(const TransformMatrix& matrix) { impl_->setTransformMatrix(matrix); }
const TextStyle& TextPrimitive::style() const { return impl_->style(); }
Vec2 TextPrimitive::position() const { return impl_->position(); }
Vec2 TextPrimitive::measuredSize() { return impl_->measuredSize(); }
void TextPrimitive::prepare() { impl_->prepare(); }
// 排版层的设备缩放：见 text.h 的说明。默认 1.0 = 不做换算。
namespace {
float g_layoutPixelScale = 1.0f;
}

float TextPrimitive::measureTextWidth(const std::string& text,
                                      const std::string& fontFamily,
                                      float fontSize,
                                      int fontWeight) {
    // 与 measureTextMetrics 同一套换算（见 setLayoutPixelScale）：宽度也要按渲染的
    // ppem 量，否则"只量宽度"的调用方（复选框/开关/只读预览这些）会与旁边的排版层
    // 差出同样的几个百分点。
    const float scale = g_layoutPixelScale;
    if (scale == 1.0f) {
        return Impl::measureTextWidth(text, fontFamily, fontSize, fontWeight);
    }
    return Impl::measureTextWidth(text, fontFamily, fontSize * scale, fontWeight) / scale;
}

void TextPrimitive::setLayoutPixelScale(float scale) {
    g_layoutPixelScale = scale > 0.0f ? scale : 1.0f;
}

float TextPrimitive::layoutPixelScale() {
    return g_layoutPixelScale;
}

TextPrimitive::TextMetrics TextPrimitive::measureTextMetrics(const std::string& text,
                                                             const std::string& fontFamily,
                                                             float fontSize,
                                                             int fontWeight) {
    const float scale = g_layoutPixelScale;
    if (scale == 1.0f) {
        return Impl::measureTextMetrics(text, fontFamily, fontSize, fontWeight);
    }
    // 借设备单位量一遍再除回逻辑单位：量的时候用与渲染完全相同的字号，
    // 于是命中同一份字体栈（fontStackCacheKey 按字号取键）与同一套 hint 量化结果。
    TextMetrics metrics = Impl::measureTextMetrics(text, fontFamily, fontSize * scale, fontWeight);
    metrics.width /= scale;
    for (float& x : metrics.caretX) {
        x /= scale;
    }
    return metrics;
}

Vec2 TextPrimitive::measureTextSize(const TextStyle& style) {
    const float scale = g_layoutPixelScale;
    if (scale == 1.0f) {
        return Impl::measureTextSize(style);
    }
    // 折行判定与行高都在逻辑空间做，所以把参与测量的三项一起放大、量完再除回来：
    // 字号 / 最大宽度 / 行高（行高决定行数，不放大就会把设备宽度的行误判成溢出）。
    TextStyle scaled = style;
    scaled.fontSize = style.fontSize * scale;
    scaled.lineHeight = style.lineHeight > 0.0f ? style.lineHeight * scale : 0.0f;
    scaled.maxWidth = style.maxWidth > 0.0f ? style.maxWidth * scale : 0.0f;
    const Vec2 size = Impl::measureTextSize(scaled);
    return Vec2{size.x / scale, size.y / scale};
}

TextAtlasDebugStats TextPrimitive::debugAtlasStats() {
    const SharedTextAtlas& atlas = sharedTextAtlas();
    TextAtlasDebugStats stats;
    stats.grayWidth = atlas.gray.width;
    stats.grayHeight = atlas.gray.height;
    stats.grayCapacityBytes = atlas.gray.pixels.capacity();
    stats.grayGlyphs = atlas.gray.glyphs.size();
    stats.grayGeneration = atlas.gray.generation;
    stats.grayGrowthCount = atlas.grayGrowthCount;
    stats.grayOverflowResetCount = atlas.grayOverflowResetCount;
    stats.colorWidth = atlas.color.width;
    stats.colorHeight = atlas.color.height;
    stats.colorCapacityBytes = atlas.color.pixels.capacity();
    stats.colorGlyphs = atlas.color.glyphs.size();
    stats.colorGeneration = atlas.color.generation;
    stats.overflowResetCount = atlas.overflowResetCount;
    return stats;
}

void TextPrimitive::setDefaultFontFiles(const std::string& textFontFile, const std::string& iconFontFile) {
    Impl::setDefaultFontFiles(textFontFile, iconFontFile);
}
std::string TextPrimitive::resolveItalicFontPath(const std::string& fontFamily) {
    return Impl::resolveItalicFontPath(fontFamily);
}
void TextPrimitive::render(int windowWidth, int windowHeight) { impl_->render(windowWidth, windowHeight); }

// ── T1 测试钩子 ──────────────────────────────────────────────────────────────
// tests/unit/text_metrics_cache.cpp 用 extern "C" 声明后直接调用。刻意不进
// core/render/text.h：纯测试入口，不占公开 API（extern "C" 的名字不随命名空间
// 变化，测试侧全局声明即可链接）。计数器只在这几个入口上读写，全部取同一把锁。
extern "C" {

std::size_t neoTextCacheMetricEntries() {
    std::scoped_lock lock(textCacheMutex());
    return sharedTextMetricsCache().entries.size();
}

std::size_t neoTextCacheMetricBytes() {
    std::scoped_lock lock(textCacheMutex());
    return sharedTextMetricsCache().bytes;
}

std::size_t neoTextCacheMetricLimitEntries() { return kTextMetricsCacheMaxEntries; }

std::size_t neoTextCacheMetricLimitBytes() { return kTextMetricsCacheBudgetBytes; }

std::size_t neoTextCacheMetricHits() {
    std::scoped_lock lock(textCacheMutex());
    return static_cast<std::size_t>(sharedTextMetricsCache().hits);
}

std::size_t neoTextCacheMetricMisses() {
    std::scoped_lock lock(textCacheMutex());
    return static_cast<std::size_t>(sharedTextMetricsCache().misses);
}

std::size_t neoTextCacheShapingEntries() {
    std::scoped_lock lock(textCacheMutex());
    return sharedShapingCache().entries.size();
}

std::size_t neoTextCacheShapingBytes() {
    std::scoped_lock lock(textCacheMutex());
    return sharedShapingCache().bytes;
}

std::size_t neoTextCacheShapingLimitEntries() { return kShapingCacheCapacity; }

std::size_t neoTextCacheShapingLimitBytes() { return kShapingCacheBudgetBytes; }

std::size_t neoTextCacheShapingHits() {
    std::scoped_lock lock(textCacheMutex());
    return static_cast<std::size_t>(sharedShapingCache().hits);
}

std::size_t neoTextCacheShapingMisses() {
    std::scoped_lock lock(textCacheMutex());
    return static_cast<std::size_t>(sharedShapingCache().misses);
}

// 只清命中/未命中计数，缓存内容不动。
void neoTextCacheResetStats() {
    std::scoped_lock lock(textCacheMutex());
    sharedTextMetricsCache().hits = 0;
    sharedTextMetricsCache().misses = 0;
    sharedShapingCache().hits = 0;
    sharedShapingCache().misses = 0;
}

// 清空全部共享文本缓存（字体栈 / TextSizeCache / T1 两层）。
void neoTextCacheClear() { clearSharedFontStackCache(); }

// 强制下一次测量重新读取 NEO_TEXT_CACHE_OFF（正常路径只在首次读一次）。
void neoTextCacheReloadEnv() { reloadTextCacheEnv(); }

// 「写侧与读者互斥」的确定性探针（T1 复审问题 1）：拿到锁后立刻置 lockAcquired
// （调用方据此确认"锁真的在我手里"再发起写入），持锁 sleep 毫秒，再在锁内读一次
// 解析后的默认 UI 字体路径返回。测试拿它判断 setDefaultFontFiles 的两次赋值到底
// 在不在锁内 —— 共锁时，另一线程在 sleep 期间发起的换字体会被挡到锁释放之后，
// 探针读到的仍是旧 override；写侧若在锁外（修复前），sleep 期间新值就已经写进
// 来了，探针会读到新路径。并发 stress 抓不住这么窄的竞态窗口，这个钩子把判定
// 变成时序上确定的一次比较。
// 注意 extern "C" 只约束符号名不约束形参 —— 调用侧（tests/unit/text_metrics_cache.cpp）
// 的声明必须与这里逐参一致（返回值走 out 参数，别返回 std::string：C 链接下会
// 触发 C4190，也更容易和调用侧对不上）。
void neoTextCacheDefaultFontPathAfterDelayForTest(int milliseconds,
                                                  std::atomic<bool>* lockAcquired,
                                                  std::string* resolvedPath) {
    std::scoped_lock lock(textCacheMutex());
    if (lockAcquired != nullptr) {
        lockAcquired->store(true, std::memory_order_release);
    }
    if (milliseconds > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
    if (resolvedPath != nullptr) {
        *resolvedPath = resolveDefaultUiFontPath();  // 重入同一把锁（递归）
    }
}

}  // extern "C"

} // namespace core
