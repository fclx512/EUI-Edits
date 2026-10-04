// T9：settings.ini / recovery.txt 原子写。
//
// 核心不变量：目标已存在时，任何一次写失败都绝不能动旧文件一个字节
// （半写截断是这条任务要消灭的事故）。覆盖：
//   ① 目标不存在时首次创建成功；
//   ② 目标已存在时正常替换，落盘内容与旧直写路径的序列化逐字节一致；
//   ③ 写完临时文件、替换之前注入失败 → 旧文件逐字节完好、临时文件被清理、
//      注入解除后下一次写入可恢复；
//   ④ 上次崩溃留下的旧临时文件不挡住下一次写入。
//
// 不测任何不存在的特性（比如 settings.ini 并没有"头部注释"）。

#include "model/atomic_write.h"
#include "model/settings.h"
#include "model/i18n.h"
#include "model/text_file.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

std::string preview(const std::string& bytes) {
    std::string out;
    for (const char character : bytes) {
        if (out.size() >= 160) {
            out += "...";
            break;
        }
        if (character == '\n') {
            out += "\\n";
        } else if (character == '\r') {
            out += "\\r";
        } else {
            out.push_back(character);
        }
    }
    return out;
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

void expectFileEquals(const fs::path& path, const std::string& expected, const char* what) {
    const std::string actual = readBytes(path);
    if (actual == expected) {
        return;
    }
    std::cerr << "FAIL: " << what << "\n"
              << "  path:     " << neo::textfile::pathToUtf8(path) << "\n"
              << "  expected: " << expected.size() << " bytes: " << preview(expected) << "\n"
              << "  actual:   " << actual.size() << " bytes: " << preview(actual) << "\n";
    ++failures;
}

// 配置目录里还剩多少 .neo-tmp 残留：正常/失败的写入都不该留下自己这一轮的临时文件。
// 读取失败返回 -1（断言侧会因 != 0 判失败）。
int temporaryLeftovers(const fs::path& directory) {
    std::error_code error;
    fs::directory_iterator iterator(directory, error);
    if (error) {
        return -1;
    }
    int count = 0;
    const fs::directory_iterator end;
    while (iterator != end) {
        const std::string name = neo::textfile::pathToUtf8(iterator->path().filename());
        if (name.find(".neo-tmp") != std::string::npos) {
            ++count;
        }
        iterator.increment(error);
        if (error) {
            return -1;
        }
    }
    return count;
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

// 与 settings.cpp flush() 同一份序列化（格式是被测的"旧直写输出"，一字不能差）。
std::string serializeSettings(const neo::settings::Data& data) {
    std::ostringstream out;
    out << "vault=" << data.vault << '\n';
    out << "last_file=" << data.lastFile << '\n';
    for (const std::string& path : data.recentFiles) {
        if (!path.empty()) {
            out << "recent_file=" << path << '\n';
        }
    }
    out << "mode=" << data.mode << '\n';
    out << "ui_language=" << neo::i18n::normalizePreference(data.uiLanguage) << '\n';
    out << "show_status_bar=" << (data.showStatusBar ? 1 : 0) << '\n';
    out << "line_numbers=" << (data.lineNumbers ? 1 : 0) << '\n';
    out << "readable_width=" << (data.readableWidth ? 1 : 0) << '\n';
    out << "animations=" << (data.animations ? 1 : 0) << '\n';
    out << "attachment_mode=" << data.attachmentMode << '\n';
    out << "theme=" << data.theme << '\n';
    std::ostringstream scale;
    scale << data.uiScale;
    out << "ui_scale=" << scale.str() << '\n';
    std::ostringstream fontSize;
    fontSize << data.editorFontSize;
    out << "editor_font_size=" << fontSize.str() << '\n';
    std::ostringstream width;
    width << data.vaultWidth;
    out << "vault_width=" << width.str() << '\n';
    std::ostringstream uiFont;
    uiFont << data.uiFontSize;
    out << "ui_font_size=" << uiFont.str() << '\n';
    out << "editor_font_file=" << data.editorFontFile << '\n';
    out << "ui_font_file=" << data.uiFontFile << '\n';
    out << "code_font_file=" << data.codeFontFile << '\n';
    return out.str();
}

// 默认 Data 的固定期望值：独立于上面的镜像函数，钉死首次创建的字节形态。
const char* const kDefaultSettings =
    "vault=\n"
    "last_file=\n"
    "mode=0\n"
    "ui_language=system\n"
    "show_status_bar=1\n"
    "line_numbers=1\n"
    "readable_width=1\n"
    "animations=1\n"
    "attachment_mode=0\n"
    "theme=0\n"
    "ui_scale=1\n"
    "editor_font_size=16\n"
    "vault_width=264\n"
    "ui_font_size=14\n"
    "editor_font_file=\n"
    "ui_font_file=\n"
    "code_font_file=\n";

} // namespace

int main(int argc, char** argv) {
    std::error_code error;
    const std::string languageCase = argc > 1 ? argv[1] : "";
    const fs::path root = fs::temp_directory_path(error) / ("eui_neo_settings_atomic_test" + languageCase);
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
    if (!redirectConfigTo(root)) {
        std::cerr << "无法重定向配置目录环境变量\n";
        return 1;
    }

    const fs::path configDir = neo::textfile::pathFromUtf8(neo::settings::configDirectory());
    const fs::path settingsIni = configDir / "settings.ini";
    const fs::path recoveryTxt = configDir / "recovery.txt";

    if (!languageCase.empty()) {
        const std::string value = languageCase == "--zh" ? "zh-CN" : languageCase == "--en" ? "en" : "bad-value";
        const std::string expected = languageCase == "--legacy" ? "system" : neo::i18n::normalizePreference(value);
        check(writeBytes(settingsIni, languageCase == "--legacy" ? "mode=1\n" : "ui_language=" + value + "\n"),
              "write language migration fixture");
        check(neo::settings::current().uiLanguage == expected, "read language preference or safe default");
        if (languageCase == "--invalid") neo::settings::current().uiLanguage = "untrusted-value";
        check(neo::settings::flush(), "persist normalized language preference");
        check(readBytes(settingsIni).find("ui_language=" + expected + "\n") != std::string::npos,
              "language preference must round-trip");
        fs::remove_all(root, error);
        return failures == 0 ? 0 : 1;
    }

    // ── ① 不存在目标：首次创建成功 ───────────────────────────────────────────
    check(!fs::exists(settingsIni), "测试前提：settings.ini 尚不存在");
    check(neo::settings::flush(), "不存在目标时首次创建 settings.ini 应成功");
    expectFileEquals(settingsIni, kDefaultSettings, "首次创建的 settings.ini 应等于旧直写序列化");
    check(temporaryLeftovers(configDir) == 0, "首次创建后不应留下临时文件");

    check(!fs::exists(recoveryTxt), "测试前提：recovery.txt 尚不存在");
    const std::string firstDraft = "first draft\n第二行 unsaved 内容";
    const std::string firstOrigin = "C:/vault/笔记.md";
    check(neo::settings::writeRecovery(firstDraft, firstOrigin),
          "不存在目标时首次创建 recovery.txt 应成功");
    expectFileEquals(recoveryTxt, "#neo-recovery " + firstOrigin + "\n" + firstDraft,
                     "首次创建的 recovery.txt 应等于头部行+原文");
    check(temporaryLeftovers(configDir) == 0, "recovery 首次创建后不应留下临时文件");

    // ── ② 已存在目标：正常替换，内容与旧直写一致 ─────────────────────────────
    neo::settings::Data& data = neo::settings::current();
    data.vault = "D:/vault/new";
    data.lastFile = "D:/vault/new/笔记.md";
    data.recentFiles = {"D:/vault/new/笔记.md", "D:/vault/参考 文档.md"};
    data.mode = 1;
    data.uiLanguage = "en";
    data.showStatusBar = false;
    data.readableWidth = false;
    data.theme = 1;
    data.uiScale = 1.25f;
    data.editorFontSize = 18.0f;
    data.vaultWidth = 300.0f;
    data.uiFontSize = 15.0f;
    data.editorFontFile = "C:/fonts/编辑体.ttf";
    check(neo::settings::flush(), "覆盖已存在的 settings.ini 应成功");
    expectFileEquals(settingsIni, serializeSettings(data), "替换后的 settings.ini 应等于序列化输出");
    check(readBytes(settingsIni).find("readable_width=0\n") != std::string::npos,
          "非默认的限制行宽设置必须写入 settings.ini");
    check(readBytes(settingsIni).find("attachment_mode=0\n") != std::string::npos,
          "附件模式默认值必须写入 settings.ini");
    check(readBytes(settingsIni).find("ui_language=en\n") != std::string::npos,
          "manual UI language preference must persist");
    check(readBytes(settingsIni).find("recent_file=D:/vault/new/笔记.md\n") != std::string::npos &&
              readBytes(settingsIni).find("recent_file=D:/vault/参考 文档.md\n") != std::string::npos,
          "最近文件应按 UTF-8 路径逐条写入 settings.ini");
    check(temporaryLeftovers(configDir) == 0, "覆盖写入后不应留下临时文件");

    check(neo::settings::writeRecovery("second draft", "origin2"), "覆盖已存在的 recovery.txt 应成功");
    expectFileEquals(recoveryTxt, "#neo-recovery origin2\nsecond draft",
                     "替换后的 recovery.txt 应等于头部行+原文");
    std::string text;
    std::string origin;
    check(neo::settings::readRecovery(text, origin) && text == "second draft" && origin == "origin2",
          "替换后 readRecovery 应读到新内容");
    check(temporaryLeftovers(configDir) == 0, "recovery 覆盖写入后不应留下临时文件");

    // 大载荷：一次性完整写入 + 替换，字节数不能少（stream 必须真的写完）。
    std::string bigDraft;
    bigDraft.reserve(512u * 1024u);
    for (int index = 0; index < 16000; ++index) {
        bigDraft += "第 " + std::to_string(index) + " 行，很长很长的一行内容 mixed ASCII 1234567890\n";
    }
    check(neo::settings::writeRecovery(bigDraft, "big-origin"), "大载荷 recovery 写入应成功");
    expectFileEquals(recoveryTxt, "#neo-recovery big-origin\n" + bigDraft,
                     "大载荷 recovery 落盘内容应与写入内容逐字节一致");
    check(temporaryLeftovers(configDir) == 0, "大载荷写入后不应留下临时文件");

    // ── ③ 写临时成功、替换之前失败：旧文件必须逐字节完好 ─────────────────────
    std::string oldSettings = readBytes(settingsIni);
    check(!oldSettings.empty(), "测试前提：settings.ini 可读且非空");
    data.vault = "injected-failure"; // 新内容与旧内容必须不同，否则断言失去意义
    neo::atomicwrite::testing::failBeforeReplace(true);
    check(!neo::settings::flush(), "替换前注入失败时 flush 应返回 false");
    neo::atomicwrite::testing::failBeforeReplace(false);
    expectFileEquals(settingsIni, oldSettings, "注入失败后 settings.ini 必须逐字节完好");
    check(temporaryLeftovers(configDir) == 0, "注入失败时临时文件必须被清理");
    check(neo::settings::flush(), "解除注入后 flush 应恢复成功");
    expectFileEquals(settingsIni, serializeSettings(data), "恢复后 settings.ini 应为新内容");
    check(temporaryLeftovers(configDir) == 0, "恢复写入后不应留下临时文件");

    std::string oldRecovery = readBytes(recoveryTxt);
    check(!oldRecovery.empty(), "测试前提：recovery.txt 可读且非空");
    neo::atomicwrite::testing::failBeforeReplace(true);
    check(!neo::settings::writeRecovery("never-landed", "never-origin"),
          "替换前注入失败时 writeRecovery 应返回 false");
    neo::atomicwrite::testing::failBeforeReplace(false);
    expectFileEquals(recoveryTxt, oldRecovery, "注入失败后 recovery.txt 必须逐字节完好");
    check(temporaryLeftovers(configDir) == 0, "recovery 注入失败时临时文件必须被清理");
    check(neo::settings::readRecovery(text, origin) && origin == "big-origin" && text == bigDraft,
          "注入失败后 readRecovery 仍读到失败前的旧副本");
    check(neo::settings::writeRecovery("landed-after-recovery", "origin3"),
          "解除注入后 writeRecovery 应恢复成功");
    expectFileEquals(recoveryTxt, "#neo-recovery origin3\nlanded-after-recovery",
                     "恢复后 recovery.txt 应为新内容");

    // ── ④ 崩溃遗留的旧临时文件：不挡住下一次写入 ─────────────────────────────
    fs::path stale = settingsIni;
    stale += ".neo-tmp-999999-0";
    check(writeBytes(stale, "上一次进程崩溃留下的半成品"), "预置崩溃遗留临时文件失败");
    check(neo::settings::flush(), "存在遗留临时文件时写入仍应成功");
    expectFileEquals(settingsIni, serializeSettings(data),
                     "遗留临时文件不应污染 settings.ini 的内容");
    // 本轮自己写的临时文件都已落位/清理，剩下的只有预置的那一个。
    check(temporaryLeftovers(configDir) == 1, "只应剩预置的遗留临时文件");

    // ── ⑤ recovery v2：带编码元数据的应急副本（R4）────────────────────────────
    {
        neo::textfile::Document meta;
        meta.encoding = neo::textfile::Encoding::Ansi;
        meta.ansiCodePage = 936u;
        meta.hadBom = false;
        meta.lineEnding = neo::textfile::LineEnding::CrLf;
        const std::string gbkDraft = "GBK 文稿正文\nsecond line";
        const std::string gbkOrigin = "D:\\vault\\中文 目录\\笔记.md";
        check(neo::settings::writeRecovery(gbkDraft, gbkOrigin, &meta),
              "带 Document 的 recovery 写入应成功");
        check(readBytes(recoveryTxt).rfind("#neo-recovery-v2 {", 0) == 0,
              "带 Document 的 recovery 应写 v2 头");
        neo::settings::RecoverySnapshot snapshot;
        check(neo::settings::readRecovery(snapshot) && snapshot.hasMeta, "v2 副本应能读回且带元数据");
        check(snapshot.text == gbkDraft, "v2 正文应逐字节一致");
        check(snapshot.originPath == gbkOrigin, "v2 来源路径应原样读回（含反斜杠与中文）");
        check(snapshot.doc.encoding == neo::textfile::Encoding::Ansi && snapshot.doc.ansiCodePage == 936u,
              "v2 编码/页码元数据应读回");
        check(!snapshot.doc.hadBom && snapshot.doc.lineEnding == neo::textfile::LineEnding::CrLf,
              "v2 BOM/换行元数据应读回");

        check(neo::settings::writeRecovery("unsaved Markdown", "", &meta, "markdown"), "unsaved language writes into optional metadata");
        snapshot = neo::settings::RecoverySnapshot{};
        check(neo::settings::readRecovery(snapshot) && snapshot.language=="markdown" && snapshot.originPath.empty(), "unsaved Markdown presentation survives recovery without inventing a file path");

        // 旧格式兼容：2 参写出的副本仍可读（无元数据），新读法也能读旧副本。
        check(neo::settings::writeRecovery("legacy draft", "legacy-origin"), "旧格式写入应成功");
        snapshot = neo::settings::RecoverySnapshot{};
        check(neo::settings::readRecovery(snapshot) && !snapshot.hasMeta &&
                  snapshot.text == "legacy draft" && snapshot.originPath == "legacy-origin",
              "旧格式副本应读回且 hasMeta=false");
        text.clear();
        origin.clear();
        check(neo::settings::readRecovery(text, origin) && text == "legacy draft",
              "兼容 readRecovery 应继续读旧格式");

        // 手工破坏的 v2 头：拒绝恢复，不冒认。
        const char* broken[] = {
            "#neo-recovery-v2 {\"origin\":\"a.md\"}\nbody",                          // 缺 encoding
            "#neo-recovery-v2 {\"origin\":\"a.md\",\"encoding\":\"shift_jis\"}\nx",  // 未知枚举
            "#neo-recovery-v2 {\"origin\":\"a.md\",\"encoding\":\"ansi\"}\nx",       // ansi 缺页码
            "#neo-recovery-v2 {\"origin\":\"a.md\",\"encoding\":\"utf8\",\"crlf\":\"yes\"}\nx",  // 类型错
            "#neo-recovery-v2 {origin broken\nx",                                    // 坏 JSON
        };
        for (const char* sample : broken) {
            check(writeBytes(recoveryTxt, sample), "预置损坏 v2 副本失败");
            snapshot = neo::settings::RecoverySnapshot{};
            check(!neo::settings::readRecovery(snapshot), "损坏的 v2 头应拒绝恢复");
        }
        // 超长元数据行同样拒绝。
        std::string oversized = "#neo-recovery-v2 {\"origin\":\"";
        oversized.append(8192, 'x');
        oversized.append("\"}\nbody");
        check(writeBytes(recoveryTxt, oversized), "预置超长元数据失败");
        snapshot = neo::settings::RecoverySnapshot{};
        check(!neo::settings::readRecovery(snapshot), "超长元数据应拒绝恢复");
        // 清回已知状态，避免影响上面对 recovery.txt 的其他断言顺序。
        neo::settings::clearRecovery();
    }

    fs::remove_all(root, error);
    if (failures != 0) {
        std::cerr << failures << " 项检查失败\n";
        return 1;
    }
    std::cout << "settings/recovery atomic write: first create, replace, injected "
                 "replace-failure and stale temp recovery ALL PASS\n";
    return 0;
}
