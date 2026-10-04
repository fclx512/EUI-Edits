// T16 图片尺寸记忆化（apps/neo_editor/model/image_size.h）的有界性单测。
//
// 只从公开入口 readImageExtent 打进去，账本经 image_size_detail 的只读 stats /
// reset 钩子观测（钩子在 detail 命名空间，不占 neo::lp 公开 API）。断言点：
//   1) 双预算：1000 个**存在**的路径（500 有效 + 500 已存在但解析失败）扫过之后，
//      条目数 ≤ kMaxMemoEntries、估算字节 ≤ kMaxMemoBytes，且确实按 LRU 淘汰了
//      「1000 - 上限」条；最新一条仍在表内（命中），最旧一条已被淘汰（重新读盘）；
//   2) 不存在的路径不入表、不计命中/未命中（无负缓存），扫表计数不被它们干扰；
//      missing → appears 后立即读到；
//   3) 失败项策略：已存在但解析失败的文件以 ok=false 入表 —— 二次调用直接命中、
//      不再读盘，与成功项一样计预算、参与淘汰（不存在的失败项则根本不入表）；
//   4) (mtime, size) 失效：只改 mtime、只改 size 都会自动重读刷新，原地更新不加条目；
//   5) UTF-8 路径（中文目录 + 中文文件名）读取与记忆化都正常；
//   6) 单条目字节账本可精确对账（键字节 + 定长开销估算）；resetMemo 清表清计数。
//
// T7 路径解析记忆化（resolveImagePathMemo）另有一组断言：
//   7) 键 = UTF-8 src + '\x1f' + UTF-8 docDir：同名文件放两个目录各解析各的，
//      目录进了键才不会串台；键里真的有中文/分隔符字节；
//   8) 零负缓存：不存在的路径不入表、不计命中/未命中，missing→appears 当场解析；
//      已缓存的目标被删掉 → 旧条目当场丢弃（返回空且不留"永久失败"的条目）；
//   9) (mtime, size) stamp 失效：源路径目标变了就丢条目重解析，原地不加重复条目；
//  10) 有界：600 条不同键扫过后条目 ≤ kMaxMemoEntries、字节 ≤ kMaxMemoBytes，
//      按 LRU 淘汰；且与头解析 memo **互不计数、互不拷键**（两张表各自独立）。
//
// 运行：ctest --test-dir build-win32 -C Release -R image_size_memo --output-on-failure

#include "apps/neo_editor/model/image_size.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using neo::lp::ImageExtent;
using MemoStats = neo::lp::image_size_detail::MemoStats;

constexpr std::size_t kMaxEntries = neo::lp::image_size_detail::kMaxMemoEntries;
constexpr std::size_t kMaxBytes = neo::lp::image_size_detail::kMaxMemoBytes;
constexpr std::size_t kEntryOverhead = neo::lp::image_size_detail::kMemoEntryOverheadBytes;

int gFailures = 0;
int gChecks = 0;
fs::path gTempDir;

void check(bool ok, const std::string& what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::printf("  !! %s\n", what.c_str());
    }
}

MemoStats stats() {
    return neo::lp::image_size_detail::memoStats();
}

void resetMemo() {
    neo::lp::image_size_detail::resetMemo();
}

// 键必须是 UTF-8：Windows 上 path::string() 走 ANSI 代码页，用户目录一旦含中文
// 就会拿到乱码键，u8string() 才与 readImageExtent 内部的 u8path 互为逆运算。
std::string keyOf(const fs::path& file) {
    return file.u8string();
}

void writeFile(const fs::path& file, const std::string& bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// 32 字节的最小 PNG：签名(8) + IHDR 段长(4) + "IHDR"(4) + 宽(4) + 高(4) + 余量(8)。
// 解析器只看前 24 字节，不校验像素 / CRC。
std::string pngBytes(int width, int height) {
    const auto be32 = [](std::uint32_t value) {
        return std::string{static_cast<char>(value >> 24), static_cast<char>(value >> 16),
                           static_cast<char>(value >> 8), static_cast<char>(value)};
    };
    std::string bytes("\x89PNG\r\n\x1a\n", 8);
    bytes += std::string("\0\0\0\rIHDR", 8);
    bytes += be32(static_cast<std::uint32_t>(width));
    bytes += be32(static_cast<std::uint32_t>(height));
    bytes += std::string(8, '\0');
    return bytes;
}

// ① 单条目字节账本精确对账 + 命中 / 未命中计数。
void testSingleEntryAccounting() {
    resetMemo();
    check(stats().entries == 0 && stats().bytes == 0, "reset 后账本应为空");

    const fs::path file = gTempDir / "single.png";
    writeFile(file, pngBytes(64, 48));
    const std::string key = keyOf(file);

    const auto first = neo::lp::readImageExtent(key);
    check(first.has_value() && first->width == 64 && first->height == 48, "首读应解析出 64x48");
    MemoStats after = stats();
    check(after.entries == 1, "首读后应入表 1 条");
    check(after.bytes == key.size() + kEntryOverhead, "单条目字节 = 键字节 + 定长开销估算");
    check(after.misses == 1 && after.hits == 0, "首读计一次 miss");

    const auto second = neo::lp::readImageExtent(key);
    check(second.has_value() && second->width == 64 && second->height == 48, "命中应给同样结果");
    after = stats();
    check(after.hits == 1 && after.misses == 1, "第二次读取计一次 hit");
    check(after.entries == 1 && after.bytes == key.size() + kEntryOverhead,
          "命中不改条目数与字节账本");
}

// ② 已存在但解析失败 → ok=false 入表并可命中；不存在 → 不入表；missing→appears。
void testFailureAndMissingPolicy() {
    resetMemo();

    // 存在但坏：入表（失败项也占条目、也计预算），二次调用直接命中不重读。
    const fs::path broken = gTempDir / "broken.png";
    writeFile(broken, "not an image at all");
    const std::string brokenKey = keyOf(broken);
    check(!neo::lp::readImageExtent(brokenKey).has_value(), "坏文件应解析失败");
    MemoStats after = stats();
    check(after.entries == 1 && after.misses == 1, "已存在但解析失败也要入表(ok=false)");
    check(after.bytes == brokenKey.size() + kEntryOverhead, "失败项同样计入字节预算");

    check(!neo::lp::readImageExtent(brokenKey).has_value(), "坏文件第二次仍是失败");
    after = stats();
    check(after.hits == 1 && after.misses == 1, "失败项走记忆化命中、不重读");
    check(after.entries == 1, "命中不新增条目");

    // 不存在：连表都不进（负缓存会让下面的 missing→appears 永远读不到）。
    const fs::path absent = gTempDir / "appears.png";
    for (int i = 0; i < 5; ++i) {
        check(!neo::lp::readImageExtent(keyOf(absent)).has_value(), "缺失文件应返回空");
    }
    after = stats();
    check(after.entries == 1 && after.bytes == brokenKey.size() + kEntryOverhead,
          "缺失路径不得入表（除已有那条坏文件外条目不增）");
    check(after.hits == 1 && after.misses == 1 && after.evictions == 0,
          "缺失路径不计命中 / 未命中 / 淘汰");

    // missing → appears：文件一出现立刻读到，不需要任何 reset。
    writeFile(absent, pngBytes(120, 90));
    const auto appeared = neo::lp::readImageExtent(keyOf(absent));
    check(appeared.has_value() && appeared->width == 120 && appeared->height == 90,
          "文件出现后应立即读到 120x90");
    check(stats().entries == 2, "出现后正常入表");
}

// ③ 1000 个存在（一半有效一半解析失败）的路径扫表：双预算都守住，且按 LRU 淘汰。
void testBoundedSweep() {
    resetMemo();

    std::vector<std::string> keys;
    keys.reserve(1000);
    for (int i = 0; i < 500; ++i) {
        const fs::path file = gTempDir / ("ok_" + std::to_string(i) + ".png");
        writeFile(file, pngBytes(16, 16));
        keys.push_back(keyOf(file));
    }
    for (int i = 0; i < 500; ++i) {
        const fs::path file = gTempDir / ("bad_" + std::to_string(i) + ".png");
        writeFile(file, "garbage bytes");  // 存在，但头部解析失败
        keys.push_back(keyOf(file));
    }
    std::vector<std::string> missing;
    missing.reserve(200);
    for (int i = 0; i < 200; ++i) {
        missing.push_back(keyOf(gTempDir / ("gone_" + std::to_string(i) + ".png")));
    }

    int wrong = 0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto extent = neo::lp::readImageExtent(keys[i]);
        const bool valid = i < 500;  // 前 500 条有效，后 500 条损坏
        if (extent.has_value() != valid) {
            ++wrong;
        }
        if (valid && extent.has_value() && extent->width != 16) {
            ++wrong;
        }
    }
    check(wrong == 0, "1000 个存在的文件应给出正确解析结果");

    MemoStats after = stats();
    check(after.entries == kMaxEntries, "1000 条入表应压到条目上限 512");
    check(after.bytes <= kMaxBytes, "估算字节不得超过 256KB 上限");
    check(after.evictions == 1000 - kMaxEntries, "应恰好按 LRU 淘汰「1000 - 上限」条");
    check(after.misses == 1000 && after.hits == 0, "1000 个新路径各计一次 miss、0 命中");

    // 不存在的 200 条：既不进表也不计数，扫表结论不受它们影响。
    for (const std::string& key : missing) {
        check(!neo::lp::readImageExtent(key).has_value(), "缺失路径应返回空");
    }
    const MemoStats afterMissing = stats();
    check(afterMissing.entries == after.entries && afterMissing.bytes == after.bytes &&
              afterMissing.misses == after.misses && afterMissing.evictions == after.evictions,
          "缺失路径不改条目 / 字节 / 计数（不入表）");

    // LRU 序：最新一条（bad_499）仍在表内 → 命中。
    check(!neo::lp::readImageExtent(keys.back()).has_value(), "最新一条应仍在表内");
    check(stats().hits == 1, "最新一条命中（未被淘汰）");

    // 最旧一条（ok_0）已被淘汰 → 重新读盘，结果仍正确，条目仍压在上限内。
    const auto reloaded = neo::lp::readImageExtent(keys.front());
    check(reloaded.has_value() && reloaded->width == 16 && reloaded->height == 16,
          "被淘汰的最旧一条应能重新读到 16x16");
    const MemoStats afterReload = stats();
    check(afterReload.misses == 1001, "最旧一条已不在表内（重新读盘）");
    check(afterReload.entries == kMaxEntries && afterReload.bytes <= kMaxBytes,
          "重新入表后仍压在双预算内");
    std::printf("  sweep 实测驻留: %zu 条 / %zu 字节（上限 %zu 条 / %zu 字节）\n",
                afterReload.entries, afterReload.bytes, kMaxEntries, kMaxBytes);
}

// ④ (mtime, size) 失效：任一变化都要自动重读刷新（原地更新，不加条目）。
void testStampInvalidation() {
    resetMemo();
    const fs::path file = gTempDir / "stamp.png";
    const std::string key = keyOf(file);
    std::error_code error;

    writeFile(file, pngBytes(100, 50));
    const auto first = neo::lp::readImageExtent(key);
    check(first.has_value() && first->width == 100 && first->height == 50, "初始应为 100x50");
    check(stats().entries == 1 && stats().misses == 1, "首读入表计一次 miss");

    // 同样字节数换内容 + 手工把 mtime 拨快 1 小时：不依赖文件系统的时间精度，
    // 单独验证「只有 mtime 变化」也会失效。
    writeFile(file, pngBytes(50, 100));
    error.clear();
    const fs::file_time_type bumped = fs::last_write_time(file, error) + std::chrono::hours(1);
    check(!error, "读取 mtime 应成功");
    fs::last_write_time(file, bumped, error);
    check(!error, "回写 mtime 应成功");
    const auto afterMtime = neo::lp::readImageExtent(key);
    check(afterMtime.has_value() && afterMtime->width == 50 && afterMtime->height == 100,
          "仅 mtime 变化也应重读刷新为 50x100");
    check(stats().entries == 1 && stats().misses == 2, "mtime 失效是原地刷新、不加条目");

    // 把 mtime 拨回表内 stamp，只让文件大小变化：验证 size 单独也能失效。
    error.clear();
    const fs::file_time_type stamp = fs::last_write_time(file, error);
    check(!error, "再次读取 mtime 应成功");
    std::string bigger = pngBytes(100, 50);
    bigger += std::string(64, '\0');
    writeFile(file, bigger);
    fs::last_write_time(file, stamp, error);  // 压掉 mtime 变化，只留 size 变化
    check(!error, "把 mtime 拨回原值应成功");
    const auto afterSize = neo::lp::readImageExtent(key);
    check(afterSize.has_value() && afterSize->width == 100 && afterSize->height == 50,
          "仅 size 变化也应重读刷新为 100x50");
    check(stats().entries == 1, "size 失效同样是原地刷新");

    // stamp 完全没动 → 命中，不重读。
    const auto unchanged = neo::lp::readImageExtent(key);
    check(unchanged.has_value() && unchanged->width == 100 && unchanged->height == 50,
          "stamp 未变应命中并给同结果");
    const MemoStats after = stats();
    check(after.hits == 1 && after.misses == 3 && after.entries == 1,
          "无变化的重复读取计 hit、不重读、不加条目");
}

// ⑤ UTF-8 路径：中文目录 + 中文文件名的读取与记忆化。
void testUtf8Path() {
    resetMemo();
    std::error_code error;
    const fs::path dir = gTempDir / u8"图片目录";
    fs::create_directories(dir, error);
    check(!error, "创建中文目录应成功");
    const fs::path file = dir / u8"测试 图片.png";
    writeFile(file, pngBytes(320, 240));
    const std::string key = keyOf(file);
    check(key.find(static_cast<char>(0xE5)) != std::string::npos,
          "键里应真的含非 ASCII 字节（中文目录名）");

    const auto first = neo::lp::readImageExtent(key);
    check(first.has_value() && first->width == 320 && first->height == 240,
          "中文路径应解析出 320x240");
    const auto second = neo::lp::readImageExtent(key);
    check(second.has_value() && second->width == 320, "中文路径记忆化命中");
    const MemoStats after = stats();
    check(after.entries == 1 && after.misses == 1 && after.hits == 1,
          "中文键只入一条、命中一次");
}

// ⑥ reset 钩子：清表、清字节账本、清全部计数。
void testReset() {
    // 先保证表非空 —— 即使前面用例有失败，本用例也自洽。
    const fs::path file = gTempDir / "reset.png";
    writeFile(file, pngBytes(8, 8));
    (void)neo::lp::readImageExtent(keyOf(file));
    check(stats().entries > 0, "reset 前表应非空");

    resetMemo();
    const MemoStats fresh = stats();
    check(fresh.entries == 0 && fresh.bytes == 0 && fresh.hits == 0 && fresh.misses == 0 &&
              fresh.evictions == 0,
          "resetMemo 应清空表、字节账本与全部计数");
}

// ── T7：本地图片路径解析记忆化（resolveImagePathMemo）───────────────────────

MemoStats resolveStats() {
    return neo::lp::image_size_detail::resolveMemoStats();
}

void resetResolveMemo() {
    neo::lp::image_size_detail::resetResolveMemo();
}

std::string resolve(const std::string& raw, const std::string& docDir) {
    return neo::lp::resolveImagePathMemo(raw, docDir);
}

// ⑦ 键 = UTF-8 src + docDir（目录必须进键）；命中给同一条；不碰头解析那张表。
void testResolveKeyAndHit() {
    resetMemo();
    resetResolveMemo();
    const fs::path dir = gTempDir / "resolve_doc";
    std::error_code error;
    fs::create_directories(dir, error);
    check(!error, "创建解析用目录应成功");
    writeFile(dir / "pic.png", pngBytes(20, 10));
    const std::string docDir = dir.u8string();

    const std::string first = resolve("pic.png", docDir);
    check(!first.empty(), "存在的相对图片应解析出绝对路径");
    check(fs::u8path(first).is_absolute(), "解析结果应是绝对路径");
    MemoStats after = resolveStats();
    check(after.entries == 1 && after.misses == 1 && after.hits == 0, "首解入表计一次 miss");

    const std::string second = resolve("pic.png", docDir);
    check(second == first, "同 (src, docDir) 二次解析应命中同一条");
    after = resolveStats();
    check(after.hits == 1 && after.entries == 1, "第二次计 hit、不加条目");

    // 目录进键：同名文件换个 docDir 是另一条键（解析基准不同，结果也不同）。
    const fs::path other = gTempDir / "resolve_other";
    fs::create_directories(other, error);
    check(!error, "创建第二个目录应成功");
    writeFile(other / "pic.png", pngBytes(30, 40));
    const std::string otherResolved = resolve("pic.png", other.u8string());
    check(!otherResolved.empty() && otherResolved != first,
          "同名文件在不同 docDir 下必须解析出不同路径（docDir 进键）");
    after = resolveStats();
    check(after.entries == 2 && after.misses == 2 && after.hits == 1,
          "另一个 docDir 是第二条键，各计各的 miss/hit");
    check(stats().entries == 0 && stats().hits == 0 && stats().misses == 0,
          "路径解析不得动头解析 memo 的表与计数（两张表互不计数）");
}

// ⑧ 零负缓存：不存在的不入表不计数；missing→appears 当场解析；删文件丢旧条目。
void testResolveNoNegativeCache() {
    resetMemo();
    resetResolveMemo();
    const fs::path dir = gTempDir / "resolve_neg";
    std::error_code error;
    fs::create_directories(dir, error);
    check(!error, "创建目录应成功");
    const std::string docDir = dir.u8string();

    for (int i = 0; i < 3; ++i) {
        check(resolve("ghost.png", docDir).empty(), "不存在的图片应解析为空");
    }
    MemoStats after = resolveStats();
    check(after.entries == 0 && after.hits == 0 && after.misses == 0,
          "不存在的路径不入表、不计命中/未命中（零负缓存）");

    writeFile(dir / "ghost.png", pngBytes(64, 64));
    const std::string appeared = resolve("ghost.png", docDir);
    check(!appeared.empty(), "missing→appears 应当场解析出来（没有负缓存挡路）");
    check(resolve("ghost.png", docDir) == appeared, "出现后第二次应命中同一条");
    after = resolveStats();
    check(after.entries == 1 && after.misses == 1 && after.hits == 1,
          "出现后入表一次 miss，第二次计 hit");

    // 目标被删：stamp 读不到 → 丢掉旧条目、返回空，绝不能留一条会命中旧路径的缓存。
    fs::remove(dir / "ghost.png", error);
    check(!error, "删除文件应成功");
    check(resolve("ghost.png", docDir).empty(), "目标删除后应返回空");
    after = resolveStats();
    check(after.entries == 0, "目标删除必须当场丢掉旧条目");
    check(after.hits == 1 && after.misses == 1,
          "删除那次既不计 hit 也不计 miss（stamp 读不到直接返回）");

    // 再次出现：仍然能解析（失败不会永久化）。
    writeFile(dir / "ghost.png", pngBytes(80, 90));
    check(!resolve("ghost.png", docDir).empty(), "重新出现后应再次解析");
    check(resolveStats().entries == 1, "重新出现后回到 1 条");
}

// ⑨ (mtime, size) stamp 失效：源路径目标变了就丢条目重解析，原地不加重复条目。
void testResolveStampInvalidation() {
    resetMemo();
    resetResolveMemo();
    const fs::path dir = gTempDir / "resolve_stamp";
    std::error_code error;
    fs::create_directories(dir, error);
    check(!error, "创建目录应成功");
    const fs::path file = dir / "s.png";
    const std::string docDir = dir.u8string();

    writeFile(file, pngBytes(10, 10));
    check(!resolve("s.png", docDir).empty(), "初始应解析成功");
    check(resolveStats().misses == 1 && resolveStats().entries == 1, "首解入表");

    // 只改 mtime：丢条目重解析（结果路径不变，靠计数观测），原地仍是 1 条。
    const fs::file_time_type bumped =
        fs::last_write_time(file, error) + std::chrono::hours(1);
    check(!error, "读取 mtime 应成功");
    fs::last_write_time(file, bumped, error);
    check(!error, "回写 mtime 应成功");
    check(!resolve("s.png", docDir).empty(), "mtime 变化后仍应解析出路径");
    MemoStats after = resolveStats();
    check(after.misses == 2 && after.entries == 1,
          "仅 mtime 变化也要丢条目重解析（原地刷新、不加条目）");

    const std::string refreshed = resolve("s.png", docDir);
    check(!refreshed.empty(), "stamp 稳定后应命中并给同样的路径");
    after = resolveStats();
    check(after.hits == 1 && after.misses == 2, "stamp 稳定后的调用计 hit、不重解析");

    // 压掉 mtime、只让 size 变化：size 单独也要失效。
    const fs::file_time_type stamp = fs::last_write_time(file, error);
    check(!error, "再次读取 mtime 应成功");
    std::string bigger = pngBytes(10, 10);
    bigger += std::string(64, '\0');
    writeFile(file, bigger);
    fs::last_write_time(file, stamp, error);
    check(!error, "把 mtime 拨回原值应成功");
    check(!resolve("s.png", docDir).empty(), "size 变化后仍应解析出路径");
    after = resolveStats();
    check(after.misses == 3 && after.entries == 1, "仅 size 变化也要丢条目重解析");
}

// ⑩ 有界（600 条压到双预算）+ 与头解析 memo 完全隔离 + reset 钩子。
void testResolveBoundedSweep() {
    resetMemo();
    resetResolveMemo();
    const fs::path dir = gTempDir / "resolve_sweep";
    std::error_code error;
    fs::create_directories(dir, error);
    check(!error, "创建目录应成功");
    const std::string docDir = dir.u8string();

    std::vector<std::string> names;
    names.reserve(600);
    for (int i = 0; i < 600; ++i) {
        const std::string name = "r_" + std::to_string(i) + ".png";
        writeFile(dir / name, pngBytes(4, 4));
        names.push_back(name);
    }
    int wrong = 0;
    for (const std::string& name : names) {
        const std::string resolved = resolve(name, docDir);
        if (resolved.empty() || fs::u8path(resolved).filename().u8string() != name) {
            ++wrong;
        }
    }
    check(wrong == 0, "600 个存在的图片都应解析出正确路径");

    MemoStats after = resolveStats();
    check(after.entries == kMaxEntries, "600 条入表应压到条目上限 512");
    check(after.bytes <= kMaxBytes, "估算字节不得超过 256KB 上限");
    check(after.evictions == 600 - kMaxEntries, "应恰好按 LRU 淘汰「600 - 上限」条");
    check(after.misses == 600 && after.hits == 0, "600 个新键各计一次 miss");
    check(stats().entries == 0 && stats().bytes == 0 && stats().hits == 0 &&
              stats().misses == 0 && stats().evictions == 0,
          "600 次路径解析后头解析 memo 仍是空表（两表互不拷键、互不计数）");

    // 最旧一条已被淘汰 → 重新解析仍正确，账本仍在预算内。
    check(!resolve(names.front(), docDir).empty(), "被淘汰的最旧一条应能重新解析");
    after = resolveStats();
    check(after.entries == kMaxEntries && after.bytes <= kMaxBytes,
          "重新入表后仍压在双预算内");
    std::printf("  resolve 实测驻留: %zu 条 / %zu 字节（上限 %zu 条 / %zu 字节）\n",
                after.entries, after.bytes, kMaxEntries, kMaxBytes);

    // reset 只清自己那张表：头解析的账本不动（反过来也一样，由 ①~⑥ 保证）。
    resetResolveMemo();
    const MemoStats fresh = resolveStats();
    check(fresh.entries == 0 && fresh.bytes == 0 && fresh.hits == 0 && fresh.misses == 0 &&
              fresh.evictions == 0,
          "resetResolveMemo 应清空解析表、账本与计数");
    check(stats().entries == 0 && stats().bytes == 0 && stats().hits == 0 &&
              stats().misses == 0 && stats().evictions == 0,
          "resetResolveMemo 不牵连头解析 memo");
}

// ⑦-b UTF-8：中文目录 + 中文文件名的解析与记忆化（键与结果都按 UTF-8 往返）。
void testResolveUtf8Path() {
    resetMemo();
    resetResolveMemo();
    std::error_code error;
    // 路径一律用 fs::u8path 构造：resolveImagePathMemo 内部就是 u8path(docDir)/u8path(src)，
    // 这样"测试建的文件"与"解析器找的文件"才是同一个（fs::path 从 const char* 走的是
    // ANSI 代码页，直接 / u8"..." 会得到另一套名字）。
    const fs::path dir = gTempDir / fs::u8path(u8"图片目录2");
    fs::create_directories(dir, error);
    check(!error, "创建中文目录应成功");
    const fs::path file = dir / fs::u8path(u8"照片.png");
    writeFile(file, pngBytes(12, 34));
    const std::string docDir = dir.u8string();
    check(docDir.find(static_cast<char>(0xE5)) != std::string::npos,
          "docDir 键里应真的含非 ASCII 字节");

    const std::string first = resolve(u8"照片.png", docDir);
    check(!first.empty(), "中文路径应能解析");
    check(first.find(u8"照片.png") != std::string::npos, "解析结果应保留中文文件名（UTF-8 往返）");
    check(resolve(u8"照片.png", docDir) == first, "中文键二次解析应命中");
    const MemoStats after = resolveStats();
    check(after.entries == 1 && after.misses == 1 && after.hits == 1,
          "中文键只入一条、命中一次");
}

}  // namespace

int main() {
    std::error_code error;
    gTempDir = fs::temp_directory_path(error) / "eui_neo_image_size_memo_test";
    if (error) {
        std::printf("[image_size_memo] 无法定位临时目录: %s\n", error.message().c_str());
        return 1;
    }
    fs::remove_all(gTempDir, error);
    error.clear();
    fs::create_directories(gTempDir, error);
    if (error) {
        std::printf("[image_size_memo] 无法创建临时目录: %s\n", error.message().c_str());
        return 1;
    }

    testSingleEntryAccounting();
    testFailureAndMissingPolicy();
    testBoundedSweep();
    testStampInvalidation();
    testUtf8Path();
    testReset();
    // T7 路径解析记忆化（与上面 ①~⑥ 各自独立的表，见文件头 7)~10)）。
    testResolveKeyAndHit();
    testResolveNoNegativeCache();
    testResolveStampInvalidation();
    testResolveBoundedSweep();
    testResolveUtf8Path();

    fs::remove_all(gTempDir, error);

    if (gFailures > 0) {
        std::printf("[image_size_memo] %d/%d 处失败\n", gFailures, gChecks);
        return 1;
    }
    std::printf("image size memo: ALL PASS (%d checks)\n", gChecks);
    return 0;
}
