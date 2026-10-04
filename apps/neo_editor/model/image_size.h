#pragma once

// 图片文件头解析（S3f 批次 B）：只读文件头拿固有宽高，不碰解码器。
//
// 为什么自己解析：`ImagePrimitive` 没有任何尺寸回传 API（全仓无 naturalSize，见
// S3f 规划 §1.2），`.size()` 又是必填的 —— 而 PNG/JPEG/GIF/BMP 的文件头里本来
// 就有宽高，纯逻辑 ~80 行、可无头单测。SVG / 远程 / 解析失败一律返回空，
// 调用方退回 `[image: alt]` 文本占位（不比现状差，S3f 规划 §3 的底线）。
//
// 记忆化（T16：给驻留加上限）：按 (mtime, size) 缓存每个路径的结果。装饰表只在
// 文本/光标/主题变化时重建（不是每帧），一次 filesystem::stat 可以接受；文件被替换
// 后 stamp 变化会重读。表本身是"条目数 ≤ kMaxMemoEntries **且** 估算字节 ≤
// kMaxMemoBytes"的 O(1) LRU，任一预算先到就从最旧端淘汰：
//   * 解析成功的条目入表；
//   * **已存在但解析失败**的条目以 ok=false 入表，与成功项一样计预算、参与淘汰
//     （一堆坏图撑不垮表）；
//   * **路径不存在 / 不可达的不入表**（stat 失败就地返回，见 readImageExtent）——
//     这类失败根本没有条目，也正因如此 missing→appears 一出现就能读到。
//
// T7 另有一张**路径解析**记忆化（resolveImagePathMemo，见文件末尾）：缓存的是
// resolveLocalImageSrc 每次都要做的 exists + weakly_canonical，键是「文档内写法 +
// 文档目录」而不是绝对路径 —— 与上面这张头解析表**分工不同、表也不共用**，各受
// 同一组双预算约束，互不拷键、互不计数（详见 resolveImagePathMemo 处的注释）。
//
// 本头文件只依赖标准库（UTF-8 路径转换直接用 std::filesystem::u8path，与
// textfile::pathFromUtf8 是同一实现），因此可以被 tests/unit 无链接依赖地单测；
// stats/reset 钩子放在 image_size_detail，不进 neo::lp 的公开 API。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neo {
namespace lp {

struct ImageExtent {
    int width = 0;
    int height = 0;
};

namespace image_size_detail {

// JPEG 的 SOF 可能排在 EXIF/ICC 之后，读整个文件最稳；上限防御异常大文件。
constexpr std::size_t kMaxReadBytes = 8u * 1024u * 1024u;

inline std::uint32_t be32(const std::string& b, std::size_t i) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(b[i])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[i + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[i + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[i + 3]));
}

inline std::uint16_t be16(const std::string& b, std::size_t i) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<unsigned char>(b[i])) << 8) |
        static_cast<std::uint16_t>(static_cast<unsigned char>(b[i + 1])));
}

inline std::uint16_t le16(const std::string& b, std::size_t i) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(static_cast<unsigned char>(b[i])) |
        (static_cast<std::uint16_t>(static_cast<unsigned char>(b[i + 1])) << 8));
}

inline std::uint32_t le32(const std::string& b, std::size_t i) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[i])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[i + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[i + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b[i + 3])) << 24);
}

inline std::optional<ImageExtent> parseJpeg(const std::string& b) {
    // 从 SOI 之后逐 marker 找 SOF0-15（排除 DHT/C8/CC）。布局：
    // FF <marker> len(2) ... SOF: precision(1) height(2) width(2)
    std::size_t pos = 2;
    while (pos + 1 < b.size()) {
        if (static_cast<unsigned char>(b[pos]) != 0xFF) {
            return std::nullopt;  // 结构坏了就放弃，不猜
        }
        while (pos < b.size() && static_cast<unsigned char>(b[pos]) == 0xFF) {
            ++pos;
        }
        if (pos >= b.size()) {
            return std::nullopt;
        }
        const unsigned char marker = static_cast<unsigned char>(b[pos]);
        ++pos;
        if (marker == 0xD9 || marker == 0xDA) {
            return std::nullopt;  // EOI/SOS 之前没有 SOF
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;  // 无长度字段的 marker
        }
        if (pos + 2 > b.size()) {
            return std::nullopt;
        }
        const std::uint16_t segLen = be16(b, pos);
        const bool isSof = marker >= 0xC0 && marker <= 0xCF &&
                           marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (isSof) {
            if (pos + 7 > b.size()) {
                return std::nullopt;
            }
            const int height = be16(b, pos + 3);
            const int width = be16(b, pos + 5);
            if (width <= 0 || height <= 0) {
                return std::nullopt;
            }
            return ImageExtent{width, height};
        }
        if (segLen < 2) {
            return std::nullopt;
        }
        pos += segLen;  // 段长含自身 2 字节
    }
    return std::nullopt;
}

inline std::optional<ImageExtent> parseHeader(const std::string& b) {
    if (b.size() >= 24 && b.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0 &&
        b.compare(12, 4, "IHDR") == 0) {
        const int width = static_cast<int>(be32(b, 16));
        const int height = static_cast<int>(be32(b, 20));
        return width > 0 && height > 0 ? std::optional<ImageExtent>(ImageExtent{width, height})
                                       : std::nullopt;
    }
    if (b.size() >= 10 &&
        (b.compare(0, 6, "GIF87a") == 0 || b.compare(0, 6, "GIF89a") == 0)) {
        const int width = le16(b, 6);
        const int height = le16(b, 8);
        return width > 0 && height > 0 ? std::optional<ImageExtent>(ImageExtent{width, height})
                                       : std::nullopt;
    }
    if (b.size() >= 26 && b.compare(0, 2, "BM") == 0) {
        const int width = static_cast<int>(le32(b, 18));
        const int height = std::abs(static_cast<int>(le32(b, 22)));  // 底朝上位图高度为负
        return width > 0 && height > 0 ? std::optional<ImageExtent>(ImageExtent{width, height})
                                       : std::nullopt;
    }
    if (b.size() >= 4 && static_cast<unsigned char>(b[0]) == 0xFF &&
        static_cast<unsigned char>(b[1]) == 0xD8) {
        return parseJpeg(b);
    }
    return std::nullopt;
}

// ---- 有界记忆化（T16 驻留上限）------------------------------------------
// 双预算：条目数与"键字节 + 定长 Entry 估算"，先到先淘汰。两个常量只给实现与
// 单测断言用，不进 neo::lp 的公开 API。
constexpr std::size_t kMaxMemoEntries = 512u;
constexpr std::size_t kMaxMemoBytes = 256u * 1024u;
// 单条目的固定开销估算（hash 节点 + MemoEntry + LRU 链表节点），键字符另行计入。
constexpr std::size_t kMemoEntryOverheadBytes = 128u;

// LRU 序列只存**指向 map 键的指针**：unordered_map 是节点式容器，rehash 不移动键
// 对象的地址，条目在表内存续期间指针稳定 —— 省掉一份键拷贝，字节账本也只算一份。
using MemoKeyList = std::list<const std::string*>;

struct MemoEntry {
    std::filesystem::file_time_type mtime;
    std::uintmax_t bytes = 0;   // 文件大小（stamp 用，与下面的预算字节无关）
    ImageExtent extent;         // 解析成功时有效
    bool ok = false;
    MemoKeyList::iterator lru;  // 在表内时有效：本条目在 LRU 序列中的位置
};

using MemoMap = std::unordered_map<std::string, MemoEntry>;

inline MemoMap& memo() {
    static MemoMap map;
    return map;
}

inline MemoKeyList& memoOrder() {
    static MemoKeyList order;
    return order;
}

// 当前估算占用字节 + 累计计数（活账本；对外只经 memoStats() 只读快照暴露）。
inline std::size_t& memoUsedBytes() {
    static std::size_t bytes = 0;
    return bytes;
}

struct MemoCounters {
    std::size_t hits = 0;       // 累计：stamp 命中，直接复用表内结果（含 ok=false）
    std::size_t misses = 0;     // 累计：表内没有或 stamp 过期，重读了文件头
    std::size_t evictions = 0;  // 累计：按预算淘汰的条目数
};

inline MemoCounters& memoCounters() {
    static MemoCounters counters;
    return counters;
}

inline std::size_t memoEntryBytes(const std::string& key) {
    return key.size() + kMemoEntryOverheadBytes;
}

// 命中 / 刷新后挪到最新端：list::splice 是 O(1)，不分配、不失效任何迭代器。
//
// LRU 三件套（挪最新端 / 删任意条目 / 删最旧条目 / 压回预算）按「map + order + 字节
// 账本 + 计数」模板化：头解析 memo 与 T7 的路径解析 memo 各持有一套**独立**状态
// （表、LRU 序、账本、计数全部分开），只共用这一份实现 —— 两张表互不拷键、互不计数，
// 就不存在"两份缓存把同一批路径无界复制两遍"的问题（各自都压在双预算内）。
template <class Map, class List>
inline void lruTouch(List& order, const typename Map::iterator& item) {
    order.splice(order.begin(), order, item->second.lru);
}

template <class Map, class List>
inline void lruErase(Map& map, List& order, std::size_t& usedBytes,
                     const typename Map::iterator& item) {
    // 先扣字节、再按迭代器销毁节点 —— 键引用之后不再解引用。
    usedBytes -= memoEntryBytes(item->first);
    order.erase(item->second.lru);
    map.erase(item);
}

template <class Map, class List>
inline void lruEraseOldest(Map& map, List& order, std::size_t& usedBytes,
                           MemoCounters& counters) {
    const std::string* key = order.back();  // 指向最旧条目的键（map 节点内）
    usedBytes -= memoEntryBytes(*key);
    map.erase(map.find(*key));  // 先按迭代器销毁节点，键引用之后不再解引用
    order.pop_back();
    ++counters.evictions;
}

template <class Map, class List>
inline void lruEvictToBudget(Map& map, List& order, std::size_t& usedBytes,
                             MemoCounters& counters) {
    while (order.size() > 1 && (map.size() > kMaxMemoEntries || usedBytes > kMaxMemoBytes)) {
        lruEraseOldest(map, order, usedBytes, counters);
    }
}

inline void memoTouch(const MemoMap::iterator& item) {
    lruTouch<MemoMap>(memoOrder(), item);
}

inline void memoEraseOldest() {
    lruEraseOldest(memo(), memoOrder(), memoUsedBytes(), memoCounters());
}

// 预算淘汰：条目数或估算字节任一超限就从最旧端丢弃。始终保留最新一条 —— 单条目
// 不可能靠继续淘汰满足预算，把它也丢掉等于每次重读，纯属浪费。
inline void memoEvictToBudget() {
    lruEvictToBudget(memo(), memoOrder(), memoUsedBytes(), memoCounters());
}

// 只读统计快照（测试钩子）：留在 image_size_detail，不污染 neo::lp 公开 API。
struct MemoStats {
    std::size_t entries = 0;    // 当前在表条目（成功 + 已存在但解析失败者）
    std::size_t bytes = 0;      // 当前估算占用字节（键字节 + 定长开销）
    std::size_t hits = 0;       // 累计命中（见 MemoCounters）
    std::size_t misses = 0;     // 累计重读
    std::size_t evictions = 0;  // 累计淘汰
};

inline MemoStats memoStats() {
    MemoStats snapshot;
    snapshot.entries = memo().size();
    snapshot.bytes = memoUsedBytes();
    snapshot.hits = memoCounters().hits;
    snapshot.misses = memoCounters().misses;
    snapshot.evictions = memoCounters().evictions;
    return snapshot;
}

// 测试钩子：清空表、LRU 序列与全部计数。
inline void resetMemo() {
    memo().clear();
    memoOrder().clear();
    memoUsedBytes() = 0;
    memoCounters() = MemoCounters{};
}

// ---- T7：本地图片路径解析的记忆化（exists + weakly_canonical）----------------
// 与头解析 memo 的分工：
//   * 这张表的键 = 文档内的**相对写法**（UTF-8 src）+ '\x1f' + 文档目录（UTF-8
//     docDir），值 = 解析出的绝对路径；省掉的是 fs::exists + fs::weakly_canonical
//     （后者最坏要逐级 stat 到根）。同一写法换个目录解析结果就不同 —— 目录必须进键。
//   * 头解析 memo 的键 = 绝对路径，值 = (mtime,size) → 宽高，省掉的是读文件头。
// 两表键空间不同（相对写法 vs 绝对路径），**互不拷键、互不计数**，各受同一组
// kMaxMemoEntries / kMaxMemoBytes 约束 —— 加起来最多两张有界小表，不会出现
// "两份缓存把同一批路径无界复制"。
// 失效与负缓存口径（与头解析 memo 同一课）：
//   * 命中一律复核**源路径目标**的 (mtime, size)：文件换了 / 没了 / 内容变了 →
//     丢条目重解析，绝不拿旧路径糊弄；
//   * 源路径不存在、weakly_canonical 失败 → **不入表**：零负缓存，missing→appears
//     下一次调用当场解析出来，"解析失败"也绝不会变成永久失败。
struct ResolveEntry {
    std::string resolved;                     // 上次解析出的绝对路径（UTF-8）
    std::filesystem::file_time_type mtime;    // 源路径目标的修改时间（stamp）
    std::uintmax_t bytes = 0;                 // 源路径目标的字节数（stamp）
    MemoKeyList::iterator lru;                // 在表内时有效
};

using ResolveMap = std::unordered_map<std::string, ResolveEntry>;

inline ResolveMap& resolveMap() {
    static ResolveMap map;
    return map;
}

inline MemoKeyList& resolveOrder() {
    static MemoKeyList order;
    return order;
}

inline std::size_t& resolveUsedBytes() {
    static std::size_t bytes = 0;
    return bytes;
}

inline MemoCounters& resolveCounters() {
    static MemoCounters counters;
    return counters;
}

// 只读统计快照（测试钩子）：与头解析的 MemoStats 同构，计数各自独立。
inline MemoStats resolveMemoStats() {
    MemoStats snapshot;
    snapshot.entries = resolveMap().size();
    snapshot.bytes = resolveUsedBytes();
    snapshot.hits = resolveCounters().hits;
    snapshot.misses = resolveCounters().misses;
    snapshot.evictions = resolveCounters().evictions;
    return snapshot;
}

// 测试钩子：清空解析表、LRU 序列与它的计数（不影响头解析那张表）。
inline void resetResolveMemo() {
    resolveMap().clear();
    resolveOrder().clear();
    resolveUsedBytes() = 0;
    resolveCounters() = MemoCounters{};
}

}  // namespace image_size_detail

// 读文件头得到固有宽高；文件不存在 / 不是四种位图之一 / 头损坏 → 空。
inline std::optional<ImageExtent> readImageExtent(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code error;
    // UTF-8 → 宽字符路径，中文路径才打得开（与 textfile::pathFromUtf8 同实现，
    // 就地用标准库是为了让本头文件不依赖 .cpp 符号，可独立单测）。
    const fs::path file = fs::u8path(path);
    const fs::file_time_type mtime = fs::last_write_time(file, error);
    if (error) {
        // 不存在 / 不可达：**不入表**。这里若记一条失败，missing→appears 之后
        // 负缓存会一直把新文件挡在外面；不记则是每次调用一次 stat 换来的刷新保证。
        return std::nullopt;
    }
    const std::uintmax_t bytes = fs::file_size(file, error);
    if (error) {
        return std::nullopt;
    }

    image_size_detail::MemoMap& map = image_size_detail::memo();
    const auto found = map.find(path);
    if (found != map.end() && found->second.mtime == mtime && found->second.bytes == bytes) {
        // stamp 未变：命中（成功与 ok=false 失败项一视同仁），顺手挪到最新端。
        image_size_detail::memoTouch(found);
        ++image_size_detail::memoCounters().hits;
        return found->second.ok ? std::optional<ImageExtent>(found->second.extent)
                                : std::nullopt;
    }
    ++image_size_detail::memoCounters().misses;  // 表内没有，或 stamp 已过期

    // 窄字符 fopen 走 ANSI 代码页，中文路径打不开；ifstream 接 fs::path 才是宽字符打开。
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        // 打不开（权限 / 被占用）不入表：既读不出结果，也无从判断内容，下次照常重试。
        return std::nullopt;
    }
    std::string content(bytes > image_size_detail::kMaxReadBytes
                             ? image_size_detail::kMaxReadBytes
                             : bytes,
                         '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    content.resize(static_cast<std::size_t>(input.gcount()));

    const std::optional<ImageExtent> extent = image_size_detail::parseHeader(content);
    image_size_detail::MemoEntry entry;
    entry.mtime = mtime;
    entry.bytes = bytes;
    entry.ok = extent.has_value();
    if (extent.has_value()) {
        entry.extent = *extent;
    }

    if (found != map.end()) {
        // stamp 过期的既有条目：原地刷新（键不变 → 预算字节不变），挪到最新端。
        const auto position = found->second.lru;
        found->second = entry;
        found->second.lru = position;
        image_size_detail::memoTouch(found);
    } else {
        const auto inserted = map.emplace(path, entry).first;
        image_size_detail::MemoKeyList& order = image_size_detail::memoOrder();
        inserted->second.lru = order.insert(order.begin(), &inserted->first);
        image_size_detail::memoUsedBytes() += image_size_detail::memoEntryBytes(path);
    }
    image_size_detail::memoEvictToBudget();
    return extent;
}

// T7：本地图片路径解析（供 lp_decorations 的 resolveLocalImageSrc 调用）。
//
// 干的事与 resolveLocalImageSrc 改造前逐字同义：UTF-8 → path（u8path，避开 ANSI
// 代码页）、相对路径按 docDir 补基准、exists → weakly_canonical；区别只是结果按
// (UTF-8 src + '\x1f' + UTF-8 docDir) 记进有界 memo，命中时先复核源路径目标的
// (mtime, size) stamp。语义保证：
//   * 源路径不存在 → 空串且**不入表**（零负缓存，missing→appears 当场生效）；
//   * weakly_canonical 失败 → 按字面归一返回但**不入表**（解析失败绝不永久化）；
//   * 已入表的条目在 stamp 变化 / 文件消失时丢弃重解析，mtime 或 size 任一变都刷新。
inline std::string resolveImagePathMemo(const std::string& raw, const std::string& docDir) {
    namespace fs = std::filesystem;
    if (raw.empty()) {
        return {};
    }
    // 键按调用方原样存（不做规范化）：`./a.png` 与 `a.png` 是两条键 —— 宁可多算一次，
    // 也不能因为规范化规则变化把不同写法 / 不同目录的结果串到一起。目录进键是因为
    // 相对写法的解析基准就是它。'\x1f'（ASCII Unit Separator）做分隔符不会出现在
    // 合法路径里，"src 结尾 + 目录"与"另一个 src"不可能拼出同一个键。
    const std::string key = raw + '\x1f' + docDir;
    fs::path path = fs::u8path(raw);
    if (!path.is_absolute()) {
        path = fs::u8path(docDir) / path;
    }

    image_size_detail::ResolveMap& map = image_size_detail::resolveMap();
    image_size_detail::MemoKeyList& order = image_size_detail::resolveOrder();
    std::size_t& usedBytes = image_size_detail::resolveUsedBytes();
    image_size_detail::MemoCounters& counters = image_size_detail::resolveCounters();

    // stamp：读源路径（跟随符号链接到最终目标）的 mtime + size。解析结果真正依赖的
    // 就是它 —— 换文件、换目标、父链改指别处，都会让这两个值对不上。
    std::error_code stampError;
    const fs::file_time_type mtime = fs::last_write_time(path, stampError);
    if (stampError) {
        // 源路径不存在 / 不可达：不入表；表里若有上一次的旧条目也一并丢掉 ——
        // 上次解析出的绝对路径已经不代表这个写法了，留着就是拿旧结果糊弄下一次。
        const auto stale = map.find(key);
        if (stale != map.end()) {
            image_size_detail::lruErase(map, order, usedBytes, stale);
        }
        return {};
    }
    std::error_code sizeError;
    const std::uintmax_t bytes = fs::file_size(path, sizeError);
    const bool stampUsable = !sizeError;  // 目录这类拿不到 size 的：能解析，但不入表

    auto found = map.find(key);
    if (found != map.end()) {
        if (stampUsable && found->second.mtime == mtime && found->second.bytes == bytes) {
            image_size_detail::lruTouch<image_size_detail::ResolveMap>(order, found);
            ++counters.hits;
            return found->second.resolved;
        }
        // stamp 变了（文件被替换 / 大小或时间变了）：丢掉旧条目，下面重新解析。
        image_size_detail::lruErase(map, order, usedBytes, found);
        found = map.end();
    }
    ++counters.misses;

    // ── 真实解析（与改造前的 resolveLocalImageSrc 同一套分支与返回口径）──
    std::error_code existsError;
    if (!fs::exists(path, existsError) || existsError) {
        return {};  // 不存在：空串，且不入表（零负缓存）
    }
    std::error_code canonicalError;
    const fs::path canonical = fs::weakly_canonical(path, canonicalError);
    if (canonicalError) {
        // 解析失败：按字面归一返回，但不入表 —— 下次照常重试，失败不会永久化。
        return path.lexically_normal().u8string();
    }
    const std::string resolved = canonical.u8string();

    if (stampUsable) {
        image_size_detail::ResolveEntry entry;
        entry.resolved = resolved;
        entry.mtime = mtime;
        entry.bytes = bytes;
        const auto inserted =
            map.emplace(key, entry).first;
        inserted->second.lru = order.insert(order.begin(), &inserted->first);
        usedBytes += image_size_detail::memoEntryBytes(key);
        image_size_detail::lruEvictToBudget(map, order, usedBytes, counters);
    }
    return resolved;
}

}  // namespace lp
}  // namespace neo
