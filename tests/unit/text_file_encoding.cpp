// R4：文本文件编码严格往返（需求文档 §9.5）。
//
// 核心不变量：
//   ① load → save 的字节级往返：UTF-8（有/无 BOM）× LF/CRLF、UTF-16 LE/BE（BOM）
//      在统一 LF/CRLF 口径下逐字节还原；
//   ② 编码元数据在 Document 上保真（encoding / ansiCodePage / hadBom / lineEnding）；
//   ③ 严格性：UTF-16 奇数字节、孤立代理对、强制编码与 BOM 冲突、ANSI 严格解码
//      解释不了的字节，一律报错而不是静默替换；
//   ④ 保存前的编码全部在内存完成：ANSI 编不了字符（emoji 进 GBK）→ 整体失败、
//      原文件不动；原子替换失败 → 原文件逐字节完好。
// 已记录的非目标：混合换行（单独 CR）沿用归一化语义，不做字节级保真。

#include "model/atomic_write.h"
#include "model/text_file.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <string>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "[text_file_encoding] FAIL: " << what << '\n';
        ++failures;
    }
}

const fs::path& workDir() {
    static const fs::path dir = fs::temp_directory_path() / "neo-text-file-encoding-test";
    return dir;
}

std::string writeSample(const std::string& name, const std::string& bytes) {
    const fs::path path = workDir() / name;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return neo::textfile::pathToUtf8(path);
}

// 显式字节列表构造：char* 字面量里嵌 \x00 时转 std::string 会在第一个 NUL 处
// 被 strlen 截断（本文件前两个版本踩过这个坑），含 NUL 的样本一律走这里。
std::string bytes(std::initializer_list<unsigned> values) {
    std::string out;
    out.reserve(values.size());
    for (unsigned value : values) {
        out.push_back(static_cast<char>(value));
    }
    return out;
}

std::string readSample(const std::string& utf8Path) {
    std::ifstream input(neo::textfile::pathFromUtf8(utf8Path), std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// round-trip：load → save → 磁盘字节与最初完全一致，且 Document 字段如期望。
void roundTrip(const std::string& name,
               const std::string& bytes,
               neo::textfile::Encoding encoding,
               bool hadBom,
               neo::textfile::LineEnding lineEnding,
               unsigned int ansiCodePage = 0) {
    const std::string path = writeSample(name, bytes);
    neo::textfile::LoadResult loaded = neo::textfile::load(path);
    check(loaded.ok, name + ": 应能自动识别打开，error=" + loaded.error);
    if (!loaded.ok) {
        return;
    }
    check(loaded.document.encoding == encoding, name + ": 识别出的编码不符");
    check(loaded.document.hadBom == hadBom, name + ": BOM 标记不符");
    check(loaded.document.lineEnding == lineEnding, name + ": 换行风格不符");
    if (encoding == neo::textfile::Encoding::Ansi) {
        check(loaded.document.ansiCodePage == ansiCodePage, name + ": 固化的代码页不符");
    }

    std::string error;
    check(neo::textfile::save(path, loaded.document, error), name + ": 保存应成功（" + error + "）");
    const std::string after = readSample(path);
    if (after != bytes) {
        std::cerr << "  [" << name << "] expect " << bytes.size() << "B:";
        for (unsigned char ch : bytes) std::cerr << ' ' << std::hex << int(ch);
        std::cerr << "\n  [" << name << "] actual " << after.size() << "B:";
        for (unsigned char ch : after) std::cerr << ' ' << std::hex << int(ch);
        std::cerr << "\n  [" << name << "] decoded:";
        for (unsigned char ch : loaded.document.text) std::cerr << ' ' << std::hex << int(ch);
        std::cerr << std::dec << '\n';
    }
    check(after == bytes, name + ": load→save 应逐字节还原原文件");
}

void testUtf8RoundTrip() {
    roundTrip("utf8-lf.md", "第一行 中文\nsecond line\n",
              neo::textfile::Encoding::Utf8, false, neo::textfile::LineEnding::Lf);
    roundTrip("utf8-bom-crlf.md", "\xEF\xBB\xBF\xe7\xac\xac\xe4\xb8\x80\xe8\xa1\x8c\r\nsecond\r\n",
              neo::textfile::Encoding::Utf8, true, neo::textfile::LineEnding::CrLf);
    roundTrip("utf8-bom-only.md", "\xEF\xBB\xBF",
              neo::textfile::Encoding::Utf8, true, neo::textfile::LineEnding::Lf);
    roundTrip("utf8-empty.md", "",
              neo::textfile::Encoding::Utf8, false, neo::textfile::LineEnding::Lf);
}

void testUtf16RoundTrip() {
    // "中文 abc\n中文2\n" 的 UTF-16 LE / BE 字节（BOM 由文件自带）。
    const std::string le = bytes({
        0xFF, 0xFE,
        0x2D, 0x4E, 0x87, 0x65, 0x20, 0x00, 0x61, 0x00, 0x62, 0x00, 0x63, 0x00, 0x0A, 0x00,
        0x2D, 0x4E, 0x87, 0x65, 0x32, 0x00, 0x0A, 0x00});
    roundTrip("utf16le-bom.md", le,
              neo::textfile::Encoding::Utf16Le, true, neo::textfile::LineEnding::Lf);

    const std::string be = bytes({
        0xFE, 0xFF,
        0x4E, 0x2D, 0x65, 0x87, 0x00, 0x20, 0x00, 0x61, 0x00, 0x62, 0x00, 0x63, 0x00, 0x0A,
        0x4E, 0x2D, 0x65, 0x87, 0x00, 0x32, 0x00, 0x0A});
    roundTrip("utf16be-bom.md", be,
              neo::textfile::Encoding::Utf16Be, true, neo::textfile::LineEnding::Lf);

    // CRLF 同样要走 expandNewlines → utf8ToUtf16 的链路。
    const std::string leCrlf = bytes({0xFF, 0xFE, 0x2D, 0x4E, 0x87, 0x65, 0x0D, 0x00, 0x0A, 0x00});
    roundTrip("utf16le-bom-crlf.md", leCrlf,
              neo::textfile::Encoding::Utf16Le, true, neo::textfile::LineEnding::CrLf);
}

void testUtf16Strictness() {
    // 奇数字节（BOM 后 3 字节）：报错，不静默替换。
    const std::string odd = bytes({0xFF, 0xFE, 0x61, 0x00, 0x62});
    const std::string oddPath = writeSample("utf16-odd.md", odd);
    neo::textfile::LoadResult loaded = neo::textfile::load(oddPath);
    check(!loaded.ok && !loaded.error.empty(), "UTF-16 奇数字节应报错");

    // 孤立高位代理：D800 后跟 0041。
    const std::string lone = bytes({0xFF, 0xFE, 0x00, 0xD8, 0x41, 0x00});
    const std::string lonePath = writeSample("utf16-lone-surrogate.md", lone);
    loaded = neo::textfile::load(lonePath);
    check(!loaded.ok && !loaded.error.empty(), "孤立代理对应报错");

    // 孤立低位代理开头。
    const std::string loneLow = bytes({0xFF, 0xFE, 0x00, 0xDC});
    loaded = neo::textfile::load(writeSample("utf16-lone-low.md", loneLow));
    check(!loaded.ok && !loaded.error.empty(), "孤立低位代理应报错");

    // 合法的增补平面字符（代理对）必须正常解码：U+1F600 😀。
    const std::string emoji = bytes({0xFF, 0xFE, 0x3D, 0xD8, 0x00, 0xDE});
    loaded = neo::textfile::load(writeSample("utf16-emoji.md", emoji));
    if (!(loaded.ok && loaded.document.text == "\xF0\x9F\x98\x80")) {
        std::cerr << "  [emoji] ok=" << loaded.ok << " error=" << loaded.error << " text=";
        for (unsigned char ch : loaded.document.text) std::cerr << ' ' << std::hex << int(ch);
        std::cerr << std::dec << '\n';
    }
    check(loaded.ok && loaded.document.text == "\xF0\x9F\x98\x80", "合法代理对应解出 U+1F600");
}

void testForcedEncodingConflicts() {
    // 带 UTF-8 BOM 的文件强制 UTF-16 LE → 冲突报错。
    const std::string utf8Bom = writeSample("conflict-utf8.md", "\xEF\xBB\xBFhello\n");
    neo::textfile::LoadResult loaded = neo::textfile::loadWithEncoding(
        utf8Bom, {neo::textfile::Encoding::Utf16Le, 0});
    check(!loaded.ok && !loaded.error.empty(), "UTF-8 BOM × 强制 UTF-16 应报冲突");

    // 带 UTF-16 BOM 的文件强制 UTF-8 → 冲突报错。
    const std::string utf16Bom = writeSample("conflict-utf16.md", bytes({0xFF, 0xFE, 0x61, 0x00}));
    loaded = neo::textfile::loadWithEncoding(utf16Bom, {neo::textfile::Encoding::Utf8, 0});
    check(!loaded.ok && !loaded.error.empty(), "UTF-16 BOM × 强制 UTF-8 应报冲突");

    // 强制 UTF-8 遇非法字节 → 严格报错。
    const std::string badUtf8 = writeSample("bad-utf8.md", "ok\xC3\n");
    loaded = neo::textfile::loadWithEncoding(badUtf8, {neo::textfile::Encoding::Utf8, 0});
    check(!loaded.ok && !loaded.error.empty(), "强制 UTF-8 应拒绝非法字节");

    // 无 BOM 的 UTF-16 文件强制 UTF-16 LE：可读，hadBom=false，保存也无 BOM。
    const std::string noBomBytes = bytes({0x61, 0x00, 0x0A, 0x00});
    const std::string noBom = writeSample("utf16-nobom.md", noBomBytes);
    loaded = neo::textfile::loadWithEncoding(noBom, {neo::textfile::Encoding::Utf16Le, 0});
    if (!(loaded.ok && !loaded.document.hadBom && loaded.document.text == "a\n")) {
        std::cerr << "  [nobom] ok=" << loaded.ok << " error=" << loaded.error << " bom="
                  << loaded.document.hadBom << " text=";
        for (unsigned char ch : loaded.document.text) std::cerr << ' ' << std::hex << int(ch);
        std::cerr << std::dec << '\n';
    }
    check(loaded.ok && !loaded.document.hadBom && loaded.document.text == "a\n",
          "无 BOM 强制 UTF-16 LE 应可读且不记 BOM");
    std::string error;
    check(neo::textfile::save(noBom, loaded.document, error), "无 BOM 文档保存应成功");
    const std::string saved = readSample(noBom);
    if (saved != noBomBytes) {
        std::cerr << "  [nobom-save] expect:";
        for (unsigned char ch : noBomBytes) std::cerr << ' ' << std::hex << int(ch);
        std::cerr << " actual:";
        for (unsigned char ch : saved) std::cerr << ' ' << std::hex << int(ch);
        std::cerr << std::dec << '\n';
    }
    check(saved == noBomBytes, "无 BOM 保存应还原为无 BOM 字节");
}

#if defined(_WIN32)
void testAnsiRoundTripAndStrictness() {
    UINT acp = GetACP();
    if (acp != 936u) {
        std::cout << "[text_file_encoding] note: ACP is " << acp << ", skipping ACP-dependent cases\n";
    }

    // 自动检测：GBK 文本在 936 系统上按 ANSI 解码并固化页码。
    if (acp == 936u) {
        const std::string gbkBytes = std::string("\xD6\xD0\xCE\xC4\xC4\xDA\xC8\xDD\x0A");  // "中文内容\n"
        roundTrip("gbk-auto.md", gbkBytes, neo::textfile::Encoding::Ansi, false,
                  neo::textfile::LineEnding::Lf, 936u);
    }

    // 强制 936：与系统 ACP 无关，始终可测。
    const std::string gbkPath = writeSample("gbk-forced.md", "\xD6\xD0\xCE\xC4\x0D\x0A");  // "中文\r\n"
    neo::textfile::LoadResult loaded =
        neo::textfile::loadWithEncoding(gbkPath, {neo::textfile::Encoding::Ansi, 936u});
    check(loaded.ok && loaded.document.text == "\xE4\xB8\xAD\xE6\x96\x87\n",
          "强制 GBK 应解出「中文」+ LF");
    check(loaded.document.encoding == neo::textfile::Encoding::Ansi && loaded.document.ansiCodePage == 936u,
          "强制 GBK 后编码元数据应为 Ansi/936");
    std::string error;
    check(neo::textfile::save(gbkPath, loaded.document, error), "GBK 文档保存应成功");
    check(readSample(gbkPath) == "\xD6\xD0\xCE\xC4\x0D\x0A", "GBK 保存应逐字节还原（含 CRLF）");

    // 强制 932（Shift-JIS）："こんにちは" = 82 B1 82 F1 82 C9 82 BF 82 CD。
    const std::string sjisPath =
        writeSample("sjis-forced.md", "\x82\xB1\x82\xF1\x82\xC9\x82\xBF\x82\xCD\x0A");
    loaded = neo::textfile::loadWithEncoding(sjisPath, {neo::textfile::Encoding::Ansi, 932u});
    check(loaded.ok &&
              loaded.document.text == "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF\n",
          "强制 Shift-JIS 应解出「こんにちは」");
    error.clear();
    check(neo::textfile::save(sjisPath, loaded.document, error), "Shift-JIS 保存应成功");
    check(readSample(sjisPath) == "\x82\xB1\x82\xF1\x82\xC9\x82\xBF\x82\xCD\x0A",
          "Shift-JIS 保存应逐字节还原");

    // 严格解码：0xFF 在 936/950/932 下都解释不了 → 报错。
    for (unsigned int codePage : {936u, 950u, 932u}) {
        const std::string badPath = writeSample("ansi-bad-" + std::to_string(codePage) + ".md", "\xFF\n");
        loaded = neo::textfile::loadWithEncoding(badPath, {neo::textfile::Encoding::Ansi, codePage});
        check(!loaded.ok && !loaded.error.empty(),
              "0xFF 在代码页 " + std::to_string(codePage) + " 下应严格报错");
    }

    // 编不了就整体失败：GBK 文档里混入 emoji（U+1F600），保存必须失败且原文件不动。
    const std::string gbkPath2 = writeSample("gbk-emoji.md", "\xD6\xD0\xCE\xC4\x0A");
    loaded = neo::textfile::loadWithEncoding(gbkPath2, {neo::textfile::Encoding::Ansi, 936u});
    check(loaded.ok, "测试前提：GBK 文档应能打开");
    loaded.document.text += "\xF0\x9F\x98\x80\n";  // 😀
    const std::string before = readSample(gbkPath2);
    error.clear();
    check(!neo::textfile::save(gbkPath2, loaded.document, error) && !error.empty(),
          "GBK 编不了 emoji 时保存应失败并带错误说明");
    check(readSample(gbkPath2) == before, "保存失败后原文件必须逐字节完好");

    // 936 无映射的字符（U+FEFF 零宽不换行空格，GBK 里没有它）：无论走默认字符
    // 替换还是 best-fit 映射都必须整体失败，不能静默降级成 '?'。
    // （西里尔 А 在 GB2312 的 A7 区有精确映射，不能当反例用。）
    loaded = neo::textfile::loadWithEncoding(writeSample("gbk-quote.md", "\xD6\xD0\xCE\xC4\x0A"),
                                             {neo::textfile::Encoding::Ansi, 936u});
    check(loaded.ok, "测试前提：GBK 文档应能打开");
    loaded.document.text = "\xEF\xBB\xBF\n";  // U+FEFF
    check(!neo::textfile::save(gbkPath2, loaded.document, error) && !error.empty(),
          "936 编不了的字符应被拒绝而不是静默降级");
}
#endif

void testNormalizationAndSaveFailure() {
    // 已记录的非目标：单独 CR 归一成 LF，lineEnding 记为 LF。
    const std::string path = writeSample("mixed-eol.md", "a\rb\n");
    neo::textfile::LoadResult loaded = neo::textfile::load(path);
    check(loaded.ok && loaded.document.text == "a\nb\n" && loaded.document.lineEnding == neo::textfile::LineEnding::Lf,
          "单独 CR 应归一为 LF 且不标记 CRLF");

    // 原子替换失败：save 返回 false、旧文件逐字节完好、无临时文件残留。
    const std::string keep = writeSample("atomic.md", "keep me\n");
    neo::textfile::LoadResult doc = neo::textfile::load(keep);
    check(doc.ok, "测试前提：atomic.md 应能打开");
    doc.document.text = "changed\n";
    const std::string before = readSample(keep);
    neo::atomicwrite::testing::failBeforeReplace(true);
    std::string error;
    check(!neo::textfile::save(keep, doc.document, error), "注入替换失败时 save 应返回 false");
    neo::atomicwrite::testing::failBeforeReplace(false);
    check(readSample(keep) == before, "替换失败后旧文件必须逐字节完好");

    std::size_t leftovers = 0;
    for (const fs::path& entry : fs::directory_iterator(workDir())) {
        if (entry.filename().string().find(".neo-tmp") != std::string::npos ||
            entry.filename().string().find(".tmp") != std::string::npos) {
            ++leftovers;
        }
    }
    check(leftovers == 0, "失败路径不应留下临时文件");
}

void testEncodingLabels() {
    neo::textfile::Document doc;
    check(neo::textfile::encodingLabel(doc) == "UTF-8", "默认编码显示应为 UTF-8");
    doc.encoding = neo::textfile::Encoding::Utf16Le;
    check(neo::textfile::encodingLabel(doc) == "UTF-16 LE", "UTF-16 LE 显示名");
    doc.encoding = neo::textfile::Encoding::Utf16Be;
    check(neo::textfile::encodingLabel(doc) == "UTF-16 BE", "UTF-16 BE 显示名");
    doc.encoding = neo::textfile::Encoding::Ansi;
    doc.ansiCodePage = 936u;
    check(neo::textfile::encodingLabel(doc) == "GBK", "936 应显示 GBK");
    doc.ansiCodePage = 950u;
    check(neo::textfile::encodingLabel(doc) == "Big5", "950 应显示 Big5");
    doc.ansiCodePage = 932u;
    check(neo::textfile::encodingLabel(doc) == "Shift-JIS", "932 应显示 Shift-JIS");
    doc.ansiCodePage = 1252u;
    check(neo::textfile::encodingLabel(doc) == "ANSI (1252)", "未知页码不冒认，显示 ANSI (N)");
}

// 路径类环境变量（APPDATA / LOCALAPPDATA / WINDIR）必须走宽字符 API 读。
// 2026-09-29 实测：std::getenv 在这些变量上返回 ANSI（本机 GBK）字节，把这种字节当
// UTF-8 交给 pathFromUtf8（std::filesystem::u8path）会抛异常；settings::current() 在
// 静态初始化里调它，异常未捕获 = 启动即崩（0xC0000409）。中文用户名
// （C:\Users\张三\AppData\Roaming）必现。
void testEnvironmentPathUtf8() {
#if defined(_WIN32)
    const std::wstring chinesePath = L"D:\\probe\\测试用户\\AppData\\Roaming";
    SetEnvironmentVariableW(L"NEO_TEST_PATH_ENV", chinesePath.c_str());
    const std::string utf8 = neo::textfile::environmentPathUtf8("NEO_TEST_PATH_ENV");
    check(!utf8.empty(), "中文环境变量应能读出（宽字符 API）");
    check(utf8 == neo::textfile::pathToUtf8(std::filesystem::path(chinesePath)),
          "读出的必须是该路径的 UTF-8 表示");
    check(!neo::textfile::pathFromUtf8(utf8).empty(), "读出的 UTF-8 必须能安全转回路径");

    // 反例（只在非 UTF-8 代码页的机器上可判）：旧写法把 getenv 的 ANSI 字节当 UTF-8，
    // 中文路径必然解不出来。这条断言固定住"为什么不能再用 std::getenv"。
    const char* raw = std::getenv("NEO_TEST_PATH_ENV");
    if (GetACP() != 65001u && raw != nullptr) {
        bool threw = false;
        try {
            (void)std::filesystem::u8path(std::string(raw));
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "getenv 的 ANSI 字节当 UTF-8 用应判为非法（启动崩溃的机制）");
    }

    check(neo::textfile::environmentPathUtf8("NEO_TEST_PATH_ENV_DEFINITELY_MISSING").empty(),
          "未设置的环境变量应返回空串而不是抛异常");
    SetEnvironmentVariableW(L"NEO_TEST_PATH_ENV", nullptr);
#endif
}

} // namespace

int main() {
    std::error_code cleanupError;
    fs::remove_all(workDir(), cleanupError);
    fs::create_directories(workDir(), cleanupError);

    testUtf8RoundTrip();
    testUtf16RoundTrip();
    testUtf16Strictness();
    testForcedEncodingConflicts();
#if defined(_WIN32)
    testAnsiRoundTripAndStrictness();
#endif
    testNormalizationAndSaveFailure();
    testEncodingLabels();
    testEnvironmentPathUtf8();

    if (failures != 0) {
        std::cerr << failures << " text_file_encoding test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "text_file_encoding tests passed\n";
    return EXIT_SUCCESS;
}
