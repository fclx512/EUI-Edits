// T13 第一阶段：主题 JSON（model/theme_loader + style_schema 的版本号配色缓存）。
//
// 覆盖：
//   ① 内置配色导出 → 重载 → 与 makeColors 现值**逐字段 golden**（28 个语义色按
//      #rrggbbaa 比对：主题文件里的颜色就是 hex，往返必须逐位回到原值）；
//   ② 缺字段回退：只写 window，其余颜色与 typography 全部回到内置值
//      （markdownStyle 逐字段对照，标题间距一点都不能变）；
//   ③ typography 覆盖：bodyLineHeight/codeSizeFactor 生效，没自选字体时主题字体
//      才采纳，标题字号/行盒仍不许被改；
//   ④ 坏 JSON / 文件不存在 / 版本不认识 / 字段名写错 → 报错且**不改**当前配色；
//   ⑤ 路径含 Unicode（中文 + 空格）能读入 —— pathFromUtf8 那条链；
//   ⑥ 切换主题文件时静态缓存**真的**失效：返回地址不变（就地重建、不是每帧新建
//      颜色对象）而值跟着变；reset 后回内置；
//   ⑦ settings 的 last_theme_file：解析 → 写出 → **换一个进程重读**（写进去的读得回来）。
//
// 不依赖 参考/ 下的素材；临时文件都写在系统临时目录里。

#include "model/i18n.h"
#include "model/settings.h"
#include "model/style_schema.h"
#include "model/text_file.h"
#include "model/theme_loader.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

// 与 theme_loader.cpp 导出侧**独立**实现的 hex 转换：golden 比对不能复用被测代码，
// 否则两边一起写错也会"通过"。
std::string hexByte(float value) {
    float clamped = value;
    if (clamped < 0.0f) {
        clamped = 0.0f;
    }
    if (clamped > 1.0f) {
        clamped = 1.0f;
    }
    const int byte = static_cast<int>(clamped * 255.0f + 0.5f);
    static const char* const kDigits = "0123456789abcdef";
    std::string out;
    out.push_back(kDigits[(byte >> 4) & 0xF]);
    out.push_back(kDigits[byte & 0xF]);
    return out;
}

std::string hexOf(const eui::Color& color) {
    return "#" + hexByte(color.r) + hexByte(color.g) + hexByte(color.b) + hexByte(color.a);
}

// 6 位 RGB（不透明色的简写形态），让期望值写起来短一点。
std::string hexRgb(const eui::Color& color) {
    return hexOf(color).substr(0, 7);
}

const eui::Color& colorAt(const neo::EditorColors& colors, std::size_t index) {
    return colors.*(neo::kThemeColorFields[index].member);
}

std::string readBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool writeBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    return output.good();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// 把配置目录重定向到临时根目录：configDirectory() 每次都重新读环境变量，
// 设置后立刻生效，测试因此不会碰用户真实的 APPDATA / XDG_CONFIG_HOME。
bool redirectConfigTo(const fs::path& root) {
    const std::string utf8 = neo::textfile::pathToUtf8(root);
#if defined(_WIN32)
    return _putenv_s("APPDATA", utf8.c_str()) == 0;
#else
    return setenv("XDG_CONFIG_HOME", utf8.c_str(), 1) == 0;
#endif
}

// 往目录里写一份主题 JSON（文件名走 pathFromUtf8，覆盖中文 + 空格）。
fs::path writeThemeFile(const fs::path& dir, const std::string& utf8Name,
                        const std::string& json) {
    std::error_code error;
    fs::create_directories(dir, error);
    const fs::path path = dir / neo::textfile::pathFromUtf8(utf8Name);
    if (!writeBytes(path, json)) {
        check(false, "预置主题文件失败：" + utf8Name);
    }
    return path;
}

bool parseOk(const std::string& json, neo::ThemeFileData& out) {
    std::string error;
    if (neo::themeloader::parse(json, out, error)) {
        return true;
    }
    check(false, "本应解析成功：" + error);
    return false;
}

// ── ① 内置配色导出 → 重载 → 逐字段 golden ─────────────────────────────────
void testGoldenRoundTrip() {
    const neo::EditorColors builtinDark = neo::makeColors(false);
    const std::string json = neo::themeloader::serialize("内置暗色", false, builtinDark,
                                                         neo::ThemeTypography{});
    neo::ThemeFileData parsed;
    if (!parseOk(json, parsed)) {
        return;
    }
    check(parsed.version == neo::themeloader::kSchemaVersion2, "导出后的 version 应是 2");
    check(!parsed.baseLight, "导出暗色配色时 base 应是 dark");
    check(parsed.name == "内置暗色", "导出后的 name 应回到原值");
    check(parsed.dark.colors.size() == std::size(neo::kThemeColorFields),
          "导出应写全每一个可主题化颜色字段");

    neo::themeloader::activate(parsed, "memory:builtin-dark");
    const neo::EditorColors& reloaded = neo::editorColors(neo::ThemeMode::Dark);
    for (std::size_t index = 0; index < std::size(neo::kThemeColorFields); ++index) {
        const std::string field = neo::kThemeColorFields[index].name;
        check(hexOf(colorAt(reloaded, index)) == hexOf(colorAt(builtinDark, index)),
              "golden 字段 " + field + "：重载 " + hexOf(colorAt(reloaded, index)) +
                  " != 内置 " + hexOf(colorAt(builtinDark, index)));
    }

    // base = dark ⇒ 亮色那一侧必须原封不动（主题只覆盖 base 对应的一侧）。
    const neo::EditorColors builtinLight = neo::makeColors(true);
    for (std::size_t index = 0; index < std::size(neo::kThemeColorFields); ++index) {
        if (hexOf(colorAt(neo::editorColors(neo::ThemeMode::Light), index)) !=
            hexOf(colorAt(builtinLight, index))) {
            check(false, std::string("base=dark 的主题不应改动亮色配色：") +
                             neo::kThemeColorFields[index].name);
            break;
        }
    }
    neo::themeloader::reset();
    check(neo::activeTheme().path.empty(), "reset 后应记为内置配色");
}

// ── ② 缺字段回退 ───────────────────────────────────────────────────────────
void testFallback() {
    neo::themeloader::reset();
    const neo::EditorColors builtinDark = neo::makeColors(false);
    const components::MarkdownStyle baseline =
        neo::markdownStyle(16.0f, neo::kUiFontFamily, "monospace", builtinDark);

    neo::ThemeFileData data;
    if (!parseOk(R"({"version":1,"base":"dark","colors":{"window":"#ff0000"}})", data)) {
        return;
    }
    neo::themeloader::activate(data, "memory:minimal");

    const neo::EditorColors& themed = neo::editorColors(neo::ThemeMode::Dark);
    check(hexRgb(themed.window) == "#ff0000",
          "显式写出的 window 应生效，实际 " + hexOf(themed.window));
    for (std::size_t index = 0; index < std::size(neo::kThemeColorFields); ++index) {
        const std::string field = neo::kThemeColorFields[index].name;
        if (field == "window") {
            continue;
        }
        check(hexOf(colorAt(themed, index)) == hexOf(colorAt(builtinDark, index)),
              "缺字段应回退内置配色：" + field);
    }

    // typography 整段没写 ⇒ markdownStyle 逐字段等于基线，标题间距一点不变。
    const components::MarkdownStyle after =
        neo::markdownStyle(16.0f, neo::kUiFontFamily, "monospace", themed);
    check(after.bodySize == baseline.bodySize && after.bodyLineHeight == baseline.bodyLineHeight,
          "没写 typography 时正文行高应回退内置");
    check(after.h1Size == baseline.h1Size && after.h2Size == baseline.h2Size &&
              after.h3Size == baseline.h3Size,
          "没写 typography 时标题字号应回退内置");
    check(after.h1LineHeight == baseline.h1LineHeight &&
              after.h2LineHeight == baseline.h2LineHeight &&
              after.h3LineHeight == baseline.h3LineHeight,
          "标题行盒（含 1em 上方留白）不许被主题改动");
    check(after.codeSize == baseline.codeSize && after.codeLineHeight == baseline.codeLineHeight,
          "没写 typography 时代码字号应回退内置");
    check(after.blockGap == baseline.blockGap && after.listIndent == baseline.listIndent &&
              after.radius == baseline.radius,
          "没写 typography 时段距/缩进/圆角应回退内置");
    check(after.fontFamily == baseline.fontFamily &&
              after.codeFontFamily == baseline.codeFontFamily,
          "没写 typography 时字体应回退调用方给的值");

    neo::themeloader::reset();
    check(hexOf(neo::editorColors(neo::ThemeMode::Dark).window) == hexOf(builtinDark.window),
          "reset 后应回内置配色（缓存必须跟着失效）");
    const components::MarkdownStyle afterReset =
        neo::markdownStyle(16.0f, neo::kUiFontFamily, "monospace",
                           neo::editorColors(neo::ThemeMode::Dark));
    check(afterReset.bodyLineHeight == baseline.bodyLineHeight,
          "reset 后 markdownStyle应回基线");
}

// ── ③ typography 覆盖 ──────────────────────────────────────────────────────
void testTypographyOverride() {
    neo::themeloader::reset();
    const neo::EditorColors colors = neo::makeColors(false);
    const components::MarkdownStyle baseline =
        neo::markdownStyle(16.0f, neo::kUiFontFamily, "monospace", colors);

    neo::ThemeFileData data;
    if (!parseOk(R"({"version":1,"typography":{"bodyLineHeight":2.0,"codeSizeFactor":1.0,)"
                 R"("bodyFontFamily":"Georgia","codeFontFamily":"Consolas"}})",
                 data)) {
        return;
    }
    neo::themeloader::activate(data, "memory:typography");

    const components::MarkdownStyle themed =
        neo::markdownStyle(16.0f, neo::kUiFontFamily, "monospace", colors);
    check(themed.bodyLineHeight == 32.0f, "bodyLineHeight=2.0 应生效（16 × 2 = 32）");
    check(themed.codeSize == 16.0f, "codeSizeFactor=1.0 应生效（16 × 1 = 16）");
    check(themed.fontFamily == "Georgia", "没自选字体时主题的正文字体应生效");
    check(themed.codeFontFamily == "Consolas", "没自选字体时主题的代码字体应生效");
    check(themed.h1LineHeight == baseline.h1LineHeight &&
              themed.h2LineHeight == baseline.h2LineHeight &&
              themed.h3LineHeight == baseline.h3LineHeight &&
              themed.h1Size == baseline.h1Size,
          "标题字号与行盒不在主题可覆盖范围内（v1）");
    check(themed.blockGap == baseline.blockGap, "blockGap 没写时应回退内置");

    // 设置里自选过字体（传入的不是预设名）⇒ 主题字体让位。
    const components::MarkdownStyle customFont =
        neo::markdownStyle(16.0f, "C:/fonts/自选字体.ttf", "自选等宽.ttf", colors);
    check(customFont.fontFamily == "C:/fonts/自选字体.ttf" &&
              customFont.codeFontFamily == "自选等宽.ttf",
          "设置里自选的字体必须优先于主题建议字体");

    neo::themeloader::reset();
    const components::MarkdownStyle afterReset =
        neo::markdownStyle(16.0f, neo::kUiFontFamily, "monospace", colors);
    check(afterReset.bodyLineHeight == baseline.bodyLineHeight &&
              afterReset.fontFamily == baseline.fontFamily,
          "reset 后 typography 覆盖应全部撤掉");
}

// ── ④ 错误路径：报错且不动当前配色 ─────────────────────────────────────────
void testErrorPaths(const fs::path& root) {
    neo::themeloader::reset();
    neo::ThemeFileData out;
    std::string error;

    check(!neo::themeloader::parse("{\"version\":1,", out, error) && !error.empty(),
          "坏 JSON 应返回错误说明");
    check(!neo::themeloader::parse(R"({"version":3,"colors":{}})", out, error) &&
              contains(error, "version"),
          "不认识的版本应报错而不是猜着解析");
    check(!neo::themeloader::parse(R"({"colors":{}})", out, error) && !error.empty(),
          "缺 version 应报错");
    check(!neo::themeloader::parse(R"({"version":1,"colors":{"widnow":"#112233"}})", out, error) &&
              contains(error, "colors"),
          "写错的字段名应报错（静默忽略会让作者以为改生效了）");
    check(!neo::themeloader::parse(R"({"version":1,"typografy":{"radius":1}})", out, error) &&
              !error.empty(),
          "写错的顶层字段应报错");
    check(!neo::themeloader::parse(R"({"version":1,"typography":{"radius":99}})", out, error) &&
              contains(error, "radius"),
          "越界的排版数值应报错");
    check(!neo::themeloader::parse(R"({"version":1,"base":"blue","colors":{}})", out, error) &&
              !error.empty(),
          "非法 base 应报错");
    check(!neo::themeloader::parse(R"({"version":1,"colors":{"window":5}})", out, error) &&
              contains(error, "window"),
          "非法颜色取值应报错");
    check(!neo::themeloader::parse("[1,2,3]", out, error) && !error.empty(),
          "根不是对象应报错");
    check(out.colors.empty(), "解析失败时输出必须是空的");

    // 失败的 load 不得改动当前生效的配色。
    const unsigned long long revision = neo::themeRevision();
    const std::string windowHex = hexOf(neo::editorColors(neo::ThemeMode::Dark).window);
    const std::string missing = neo::textfile::pathToUtf8(root / "根本不存在的目录/主题.json");
    error.clear();
    check(!neo::themeloader::load(missing, error) && !error.empty() &&
              (contains(error, "不存在") || contains(error, "无法读取")),
          "文件不存在应报错并说明：" + error);
    check(neo::themeRevision() == revision, "加载失败不得推进主题版本");
    check(hexOf(neo::editorColors(neo::ThemeMode::Dark).window) == windowHex,
          "加载失败不得改动当前配色");
}

// ── ⑤⑥ Unicode 路径 + 静态缓存真实失效 ────────────────────────────────────
void testUnicodePathAndCache(const fs::path& root) {
    neo::themeloader::reset();
    const fs::path themeDir = root / neo::textfile::pathFromUtf8("主题 目录");
    const fs::path first = writeThemeFile(
        themeDir, "第一个 主题.json",
        R"({"schema":"euiedits-theme","version":1,"name":"第一版","base":"dark",)"
        R"("colors":{"window":"#112233","text":"#445566"}})");
    const std::string firstUtf8 = neo::textfile::pathToUtf8(first);
    check(contains(firstUtf8, "主题 目录") && contains(firstUtf8, "第一个 主题.json"),
          "临时主题文件名应含中文与空格（Unicode 路径前提）");

    const neo::EditorColors* addressBefore = &neo::editorColors(neo::ThemeMode::Dark);
    const std::string builtinWindow = hexRgb(neo::makeColors(false).window);
    check(builtinWindow != "#112233" && builtinWindow != "#445566",
          "前提：内置暗色 window 不能与测试主题撞色（实际 " + builtinWindow + "）");

    std::string error;
    if (!neo::themeloader::load(firstUtf8, error)) {
        check(false, "Unicode 路径应能读入主题：" + error);
        return;
    }
    check(neo::activeTheme().path == firstUtf8, "生效主题应记下 UTF-8 路径");
    check(neo::activeTheme().name == "第一版", "主题名应随文件载入");

    const neo::EditorColors* addressAfter = &neo::editorColors(neo::ThemeMode::Dark);
    check(addressAfter == addressBefore,
          "缓存应就地重建（地址不变），而不是每次新建颜色对象");
    check(hexRgb(addressAfter->window) == "#112233", "主题文件的 window 应立即生效");
    check(hexRgb(addressAfter->text) == "#445566", "主题文件的 text 应立即生效");
    check(hexOf(neo::editorColors(neo::ThemeMode::Light).window) ==
              hexOf(neo::makeColors(true).window),
          "base=dark 的主题不改亮色那一侧");

    // 第二个主题文件：静态缓存必须再次失效（旧版 static const 会锁死首次颜色）。
    const fs::path second = writeThemeFile(
        themeDir, "第二个主题.json",
        R"({"version":1,"base":"dark","colors":{"window":"#445566"}})");
    if (!neo::themeloader::load(neo::textfile::pathToUtf8(second), error)) {
        check(false, "切换主题文件应成功：" + error);
        return;
    }
    check(&neo::editorColors(neo::ThemeMode::Dark) == addressBefore,
          "第二次切换同样应就地重建");
    check(hexRgb(neo::editorColors(neo::ThemeMode::Dark).window) == "#445566",
          "切换主题文件后缓存必须真实失效");

    // 损坏文件：报错、当前主题保持第二个文件。
    const fs::path broken = writeThemeFile(themeDir, "坏主题.json", "{\"version\":1,");
    error.clear();
    check(!neo::themeloader::load(neo::textfile::pathToUtf8(broken), error) &&
              !error.empty() && contains(error, "坏主题.json"),
          "坏 JSON 应报错且错误里带上文件名");
    check(hexRgb(neo::editorColors(neo::ThemeMode::Dark).window) == "#445566",
          "坏文件不得改动当前生效的配色");

    neo::themeloader::reset();
    check(hexOf(neo::editorColors(neo::ThemeMode::Dark).window) ==
              hexOf(neo::makeColors(false).window),
          "reset 后应回内置暗色");
    check(neo::activeTheme().revision > 0, "reset 必须推进主题版本（否则缓存不重建）");
}

// ── ⑦ settings：解析 → 写出 → 换进程重读 ───────────────────────────────────
void testSettingsRoundTrip(const fs::path& configRoot, const char* selfPath) {
    if (!redirectConfigTo(configRoot)) {
        check(false, "无法重定向配置目录环境变量");
        return;
    }
    const fs::path configDir = neo::textfile::pathFromUtf8(neo::settings::configDirectory());
    const fs::path settingsIni = configDir / "settings.ini";

    // 解析：手写一份带 last_theme_file 的 settings.ini（值含中文与空格）。
    const std::string savedPath = "D:/主题目录/暗色 主题.json";
    check(writeBytes(settingsIni, "theme=1\nlast_theme_file=" + savedPath + "\n"),
          "预置 settings.ini 失败");
    check(neo::settings::current().lastThemeFile == savedPath,
          "settings 应解析 last_theme_file（含 Unicode）");

    // 写出：已有键不能丢，新键要带上。
    const std::string other = "E:/主题/亮色.json";
    neo::settings::current().lastThemeFile = other;
    check(neo::settings::flush(), "flush 应成功");
    const std::string written = readBytes(settingsIni);
    check(contains(written, "last_theme_file=" + other + "\n"), "flush 应写出 last_theme_file");
    check(contains(written, "theme=1\n"), "flush 不得丢掉已有键");

    // 换一个进程重读：current() 是进程内的 static，只有新进程能证明"写进去的读得回来"。
    const std::string command = "\"" + std::string(selfPath) + "\" --dump-settings";
    const int status = std::system(command.c_str());
    check(status == 0, "子进程 --dump-settings 应退出成功");
    const std::string readBack = readBytes(configDir / "readback.txt");
    check(readBack == other, "换进程读回的 last_theme_file 应等于写入值，实际：" + readBack);

    // 空值不写出该行（settings.ini 的空值形态保持原样，
    // 与 tests/unit/settings_atomic.cpp 的逐字节黄金样例一致）。
    neo::settings::current().lastThemeFile.clear();
    check(neo::settings::flush(), "清空后 flush 应成功");
    check(!contains(readBytes(settingsIni), "last_theme_file="),
          "空值不应写出 last_theme_file 行");
}

void testDualAndObsidianImport(const fs::path& root) {
    const std::string exported = neo::themeloader::serializeDual(
        "two palettes", neo::makeColors(true), neo::ThemeTypography{},
        neo::makeColors(false), neo::ThemeTypography{});
    neo::ThemeFileData roundTrip;
    if (parseOk(exported, roundTrip)) {
        check(roundTrip.version == 2 &&
              roundTrip.light.colors.size() == std::size(neo::kThemeColorFields) &&
              roundTrip.dark.colors.size() == std::size(neo::kThemeColorFields),
              "v2 export should round-trip both complete palettes");
    }
    neo::ThemeFileData data;
    if (!parseOk(R"({"schema":"euiedits-theme","version":2,"name":"dual",)"
                 R"("light":{"colors":{"text":"#112233"},"typography":{"h1SizeFactor":2.0}},)"
                 R"("dark":{"colors":{"text":"#445566"}}})", data)) return;
    neo::themeloader::activate(data, "memory:dual");
    check(hexRgb(neo::editorColors(neo::ThemeMode::Light).text) == "#112233",
          "v2 light override should apply only to the light side");
    check(hexRgb(neo::editorColors(neo::ThemeMode::Dark).text) == "#445566",
          "v2 dark override should apply only to the dark side");
    const auto lightStyle = neo::markdownStyle(16.0f, neo::kUiFontFamily, "monospace",
        neo::editorColors(neo::ThemeMode::Light), neo::ThemeMode::Light);
    check(lightStyle.h1Size == 32.0f, "v2 light heading size should apply");
    const auto darkStyle = neo::markdownStyle(16.0f, neo::kUiFontFamily, "monospace",
        neo::editorColors(neo::ThemeMode::Dark), neo::ThemeMode::Dark);
    check(darkStyle.h1Size != 32.0f, "v2 dark heading should retain its own value");
    neo::themeloader::reset();

    const fs::path sampleDir = root / "Obsidian 示例";
    fs::create_directories(sampleDir);
    const fs::path css = sampleDir / "theme.css";
    const fs::path manifest = sampleDir / "manifest.json";
    const std::string synthetic =
        ":root { --background-primary: #123; --a: var(--b);"
        " --b: var(--a); }"
        ".theme-light { --text-normal: hsl(0, 100%, 50%); }"
        ".theme-dark { --text-normal: rgb(0, 128, 255);"
        " --code-normal: var(--a); --hr-color: calc(1 + 2); }";
    check(writeBytes(css, synthetic) &&
          writeBytes(manifest, R"({"name":"示例","minAppVersion":"1.0.0"})"),
          "synthetic Obsidian files should be created");
    std::string error;
    check(neo::themeloader::load(neo::textfile::pathToUtf8(manifest), error),
          "manifest should locate sibling theme.css: " + error);
    check(neo::activeTheme().name == "示例", "manifest name should be used");
    check(hexRgb(neo::editorColors(neo::ThemeMode::Light).text) == "#ff0000",
          "light hsl should resolve");
    check(hexRgb(neo::editorColors(neo::ThemeMode::Dark).text) == "#0080ff",
          "dark rgb should resolve");
    check(neo::activeTheme().dark.colors.count("divider") == 0,
          "unsupported calc should leave the divider on built-in fallback");
    check(neo::activeTheme().dark.colors.count("codeText") == 0,
          "cyclic var should leave code text on built-in fallback");
    check(neo::themeloader::load(neo::textfile::pathToUtf8(css), error),
          "direct theme.css should also load: " + error);
    neo::themeloader::reset();

    check(writeBytes(css, ".theme-light { --text-normal: #abcdef; }"),
          "single-side CSS fixture should be created");
    check(neo::themeloader::load(neo::textfile::pathToUtf8(css), error),
          "single-side CSS should load: " + error);
    check(neo::activeTheme().dark.colors.empty(),
          "missing dark side should stay on built-in fallback");
    neo::themeloader::reset();

    // Optional local Obsidian reference: compare against the independent bridge output
    // when both ignored reference files are available on this development machine.
    fs::path repo = fs::current_path();
    const fs::path relativeCss = neo::textfile::pathFromUtf8("参考/obsidian-style/app.css");
    while (!fs::exists(repo / relativeCss) && repo.has_parent_path() &&
           repo.parent_path() != repo) repo = repo.parent_path();
    const fs::path appCss = repo / relativeCss;
    if (!fs::exists(appCss)) return;
    const fs::path lightGolden = repo / neo::textfile::pathFromUtf8(
        "参考/tools/theme_bridge/out/theme_light.json");
    const fs::path darkGolden = repo / neo::textfile::pathFromUtf8(
        "参考/tools/theme_bridge/out/theme_dark.json");
    if (!fs::exists(lightGolden) || !fs::exists(darkGolden)) return;
    const std::string appCssUtf8 = neo::textfile::pathToUtf8(appCss);
    check(neo::themeloader::load(appCssUtf8, error),
          "app.css should load: " + error);
    const neo::ThemeFileData imported = neo::activeTheme();
    for (const bool light : {true, false}) {
        const fs::path goldenPath = light ? lightGolden : darkGolden;
        neo::ThemeFileData golden;
        if (!parseOk(readBytes(goldenPath), golden)) continue;
        const auto& actual = light ? imported.light.colors : imported.dark.colors;
        check(actual.size() == golden.colors.size(),
              std::string(light ? "light" : "dark") + " app.css coverage should match bridge");
        for (const auto& entry : golden.colors) {
            const auto found = actual.find(entry.first);
            check(found != actual.end() && hexOf(found->second) == hexOf(entry.second),
                  std::string(light ? "light " : "dark ") + entry.first +
                  " should match bridge color");
        }
    }
    neo::themeloader::reset();
}

} // namespace

int main(int argc, char** argv) {
    // 断言里匹配的是中文文案（"不存在"/"无法读取"）：不钉语言就会跟着 CI 的
    // 英文系统走，必然假失败。field 名等 ASCII 断言（version/colors/radius）
    // 在 zh 文案里同样成立，不受影响。
    neo::i18n::initialize("zh-CN");
    if (argc >= 3 && std::string(argv[1]) == "--probe-file") {
        std::string error;
        if (!neo::themeloader::load(argv[2], error)) {
            std::cerr << error << '\n';
            return 1;
        }
        const auto& theme = neo::activeTheme();
        std::cout << theme.name << ": light " << theme.light.colors.size()
                  << "/28, dark " << theme.dark.colors.size() << "/28\n";
        return 0;
    }
    // 子进程模式：把 current().lastThemeFile 原样写进配置目录，供父进程比对。
    // （父进程通过环境变量继承被重定向的配置目录，命令行上不带任何 Unicode。）
    if (argc >= 2 && std::string(argv[1]) == "--dump-settings") {
        const fs::path out = fs::path(neo::settings::configDirectory()) / "readback.txt";
        if (!writeBytes(out, neo::settings::current().lastThemeFile)) {
            std::cerr << "无法写出 readback.txt\n";
            return 1;
        }
        return 0;
    }

    std::error_code error;
    const fs::path root = fs::temp_directory_path(error) / "eui_neo_theme_loader_test";
    if (error) {
        std::cerr << "无法定位临时目录: " << error.message() << "\n";
        return 1;
    }
    fs::remove_all(root, error);
    error.clear();
    fs::create_directories(root, error);
    if (error) {
        std::cerr << "无法创建测试目录: " << error.message() << "\n";
        return 1;
    }

    testGoldenRoundTrip();
    testFallback();
    testTypographyOverride();
    testErrorPaths(root);
    testUnicodePathAndCache(root);
    testSettingsRoundTrip(root / "config", argv[0]);
    testDualAndObsidianImport(root);

    fs::remove_all(root, error);
    if (failures != 0) {
        std::cerr << failures << " 项检查失败\n";
        return 1;
    }
    std::cout << "theme_loader: golden round trip, missing-field fallback, typography, "
                 "error paths, unicode path, cache invalidation and settings round trip "
                 "ALL PASS\n";
    return 0;
}
