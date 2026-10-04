#include "model/atomic_write.h"
#include "model/session_storage.h"
#include "model/settings.h"
#include "model/text_file.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

std::string readBytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool writeBytes(const fs::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return output.good();
}

bool redirectConfig(const fs::path& root) {
    const std::string utf8 = neo::textfile::pathToUtf8(root);
#if defined(_WIN32)
    return _putenv_s("APPDATA", utf8.c_str()) == 0;
#else
    return setenv("XDG_CONFIG_HOME", utf8.c_str(), 1) == 0;
#endif
}

bool sameDocument(const neo::textfile::Document& left, const neo::textfile::Document& right) {
    return left.text == right.text && left.hadBom == right.hadBom &&
           left.lineEnding == right.lineEnding && left.encoding == right.encoding &&
           left.ansiCodePage == right.ansiCodePage;
}

std::vector<fs::path> bodyFiles(const fs::path& directory) {
    std::vector<fs::path> files;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        const std::string name = neo::textfile::pathToUtf8(it->path().filename());
        if (name.rfind("body-", 0) == 0 && it->path().extension() == ".utf8") files.push_back(it->path());
    }
    return files;
}

} // namespace

int main() {
    std::error_code error;
    const fs::path root = fs::temp_directory_path(error) / "eui_neo_session_storage_test";
    if (error) return 1;
    fs::remove_all(root, error);
    error.clear();
    fs::create_directories(root, error);
    if (error || !redirectConfig(root)) return 1;

    const fs::path config = neo::textfile::pathFromUtf8(neo::settings::configDirectory());
    const fs::path directory = config / "session";
    const fs::path manifest = directory / "manifest.json";
    const fs::path legacyRecovery = config / "recovery.txt";
    check(writeBytes(legacyRecovery, "legacy recovery must survive\n"), "write legacy recovery fixture");

    neo::textfile::Document utf16;
    utf16.text = "第一篇\n内容 🙂\n";
    utf16.encoding = neo::textfile::Encoding::Utf16Be;
    utf16.hadBom = true;
    utf16.lineEnding = neo::textfile::LineEnding::CrLf;

    neo::textfile::Document emptyAnsi;
    emptyAnsi.encoding = neo::textfile::Encoding::Ansi;
    emptyAnsi.ansiCodePage = 936;
    emptyAnsi.lineEnding = neo::textfile::LineEnding::Lf;

    std::vector<neo::sessionstorage::WriteRecord> records = {
        {22, "D:/文档/第二篇.md", "D:/文档", "markdown", 1, true, &emptyAnsi},
        {31, "D:/文档/clean.txt", "D:/文档", "text", -1, false, nullptr},
        {11, "D:/文档/第一篇.md", "D:/文档", "markdown", 0, true, &utf16},
    };
    check(neo::sessionstorage::write(records, 31), "write multiple dirty and clean tabs");

    std::vector<neo::sessionstorage::ReadRecord> restored;
    neo::sessionstorage::TabId active = 0;
    check(neo::sessionstorage::read(restored, active), "read complete session");
    check(active == 31 && restored.size() == 3, "preserve active ID and tab order");
    if (restored.size() == 3) {
        check(restored[0].id == 22 && restored[0].path == "D:/文档/第二篇.md" && restored[0].dirty &&
              sameDocument(restored[0].document, emptyAnsi), "preserve empty dirty page and ANSI metadata");
        check(restored[1].id == 31 && !restored[1].dirty && restored[1].document.text.empty(),
              "clean page stores metadata without recovery body");
        check(restored[2].id == 11 && restored[2].language == "markdown" && restored[2].wrapOverride == 0 &&
              sameDocument(restored[2].document, utf16), "preserve Unicode body and UTF-16/BOM/CRLF metadata");
    }
    check(readBytes(legacyRecovery) == "legacy recovery must survive\n", "session write does not touch legacy recovery");
    check(bodyFiles(directory).size() == 2, "write exactly the dirty-page bodies");

    // Removing one tab from the new snapshot must not erase the remaining page.
    std::vector<neo::sessionstorage::WriteRecord> oneTab = {{22, records[0].path, records[0].vaultRoot,
        records[0].language, records[0].wrapOverride, true, &emptyAnsi}};
    check(neo::sessionstorage::write(oneTab, 22), "write session after closing other tabs");
    restored.clear(); active = 0;
    check(neo::sessionstorage::read(restored, active) && restored.size() == 1 && restored[0].id == 22 &&
          restored[0].dirty && sameDocument(restored[0].document, emptyAnsi),
          "closing one tab preserves the other dirty tab");
    check(bodyFiles(directory).size() == 1, "successful manifest commit cleans previous generation bodies");

    // A manifest replacement failure after all metadata is prepared retains the old generation.
    const std::string oldManifest = readBytes(manifest);
    const fs::path oldBody = bodyFiles(directory).front();
    const std::string oldBodyBytes = readBytes(oldBody);
    const std::vector<neo::sessionstorage::WriteRecord> cleanOnly = {
        {42, "D:/另一个.md", "D:/", "markdown", -1, false, nullptr},
    };
    neo::atomicwrite::testing::failBeforeReplace(true);
    const bool replacement = neo::sessionstorage::write(cleanOnly, 42);
    neo::atomicwrite::testing::failBeforeReplace(false);
    check(!replacement, "manifest commit failure is reported");
    check(readBytes(manifest) == oldManifest && readBytes(oldBody) == oldBodyBytes,
          "manifest failure preserves previous manifest and body");
    restored.clear(); active = 0;
    check(neo::sessionstorage::read(restored, active) && restored.size() == 1 && restored[0].id == 22 && active == 22,
          "previous session remains readable after failed replacement");

    neo::textfile::Document candidateBody;
    candidateBody.text = "new candidate";
    const std::vector<neo::sessionstorage::WriteRecord> dirtyCandidate = {
        {43, "candidate.md", "", "markdown", -1, true, &candidateBody},
    };
    neo::atomicwrite::testing::failBeforeReplace(true);
    const bool bodyWrite = neo::sessionstorage::write(dirtyCandidate, 43);
    neo::atomicwrite::testing::failBeforeReplace(false);
    check(!bodyWrite, "body staging failure is reported");
    check(readBytes(manifest) == oldManifest && readBytes(oldBody) == oldBodyBytes,
          "body staging failure keeps the previous manifest and body");
    check(bodyFiles(directory).size() == 1,
          "body staging failure removes its partial candidate generation");

    // Malformed manifests and missing dirty payloads must not turn into empty or clean sessions.
    const std::string validManifest = readBytes(manifest);
    check(writeBytes(manifest, "{broken"), "write corrupt manifest fixture");
    restored.push_back({}); active = 999;
    check(!neo::sessionstorage::read(restored, active) && restored.empty() && active == 0,
          "corrupt manifest fails closed and clears output");
    check(writeBytes(manifest, validManifest), "restore valid manifest fixture");
    fs::remove(oldBody, error);
    restored.push_back({}); active = 999;
    check(!neo::sessionstorage::read(restored, active) && restored.empty() && active == 0,
          "missing dirty body fails the complete read");

    check(!neo::sessionstorage::write({{0, "", "", "", -1, false, nullptr}}, 0),
          "reject zero tab ID");
    check(!neo::sessionstorage::write({{5, "", "", "", -1, true, nullptr}}, 5),
          "dirty page requires a document object");
    check(!neo::sessionstorage::write({{5, "", "", "", -1, false, nullptr},
                                       {5, "", "", "", -1, false, nullptr}}, 5),
          "reject duplicate tab IDs");
    check(!neo::sessionstorage::write({{std::numeric_limits<neo::sessionstorage::TabId>::max(),
                                        "", "", "", -1, false, nullptr}},
                                      std::numeric_limits<neo::sessionstorage::TabId>::max()),
          "reject maximum tab ID so the caller can safely allocate the next ID");

    // v2: the body field is a validated opaque content id; it cannot redirect body
    // reads outside the private session directory.
    const std::vector<neo::sessionstorage::WriteRecord> traversalSource = {
        {61, "safe.md", "", "markdown", -1, true, &candidateBody},
    };
    check(neo::sessionstorage::write(traversalSource, 61), "write traversal fixture session");
    const std::string traversalManifestSource = readBytes(manifest);
    const std::string bodyKey = "\"body\":\"";
    const std::size_t bodyStart = traversalManifestSource.find(bodyKey);
    check(bodyStart != std::string::npos, "find body field in manifest");
    if (bodyStart != std::string::npos) {
        const std::size_t valueStart = bodyStart + bodyKey.size();
        const std::size_t valueEnd = traversalManifestSource.find('"', valueStart);
        check(valueEnd != std::string::npos, "body value is terminated");
        if (valueEnd != std::string::npos) {
            std::string tampered = traversalManifestSource;
            tampered.replace(valueStart, valueEnd - valueStart, "../../outside");
            check(writeBytes(manifest, tampered), "write traversal body fixture");
            restored.push_back({}); active = 999;
            check(!neo::sessionstorage::read(restored, active) && restored.empty() && active == 0,
                  "reject path traversal in content-addressed body name");
        }
    }

    // v1 manifests (legacy generation + body-<generation>-<id>.utf8) stay readable.
    {
        const std::string legacyId(32, 'a');
        const std::string legacyBodyName = "body-" + legacyId + "-7.utf8";
        check(writeBytes(directory / legacyBodyName, "legacy v1 body\n"), "write v1 legacy body fixture");
        const std::string v1 =
            "{\"version\":1,\"generation\":\"" + legacyId +
            "\",\"active\":7,\"records\":[{\"id\":7,\"path\":\"legacy.md\",\"vaultRoot\":\"\","
            "\"language\":\"markdown\",\"wrap\":-1,\"dirty\":true,\"encoding\":\"utf8\","
            "\"codepage\":0,\"bom\":false,\"crlf\":false}]}\n";
        check(writeBytes(manifest, v1), "write v1 legacy manifest fixture");
        restored.clear(); active = 0;
        check(neo::sessionstorage::read(restored, active) && active == 7 && restored.size() == 1 &&
                  restored[0].dirty && restored[0].document.text == "legacy v1 body\n",
              "v1 manifest remains readable and migrates on next write");
    }

    // Duplicate IDs in persisted data are rejected even when each record is clean.
    check(writeBytes(manifest, validManifest), "restore valid manifest for duplicate-ID validation");
    const std::vector<neo::sessionstorage::WriteRecord> cleanPair = {
        {51, "one.md", "", "markdown", -1, false, nullptr},
        {52, "two.md", "", "markdown", -1, false, nullptr},
    };
    check(neo::sessionstorage::write(cleanPair, 52), "write clean pair for duplicate-ID validation");
    const std::string cleanManifest = readBytes(manifest);
    const std::string secondId = "\"id\":52";
    const std::size_t secondIdAt = cleanManifest.find(secondId);
    check(secondIdAt != std::string::npos, "find second ID in clean manifest");
    if (secondIdAt != std::string::npos) {
        std::string duplicateManifest = cleanManifest;
        duplicateManifest.replace(secondIdAt, secondId.size(), "\"id\":51");
        check(writeBytes(manifest, duplicateManifest), "write duplicate-ID manifest fixture");
        restored.push_back({}); active = 999;
        check(!neo::sessionstorage::read(restored, active) && restored.empty() && active == 0,
              "reject duplicate tab IDs in persisted manifest");
    }

    // v1 只读兼容 + 迁移：读一次成功；下一次 v2 写入后清单升 v2、旧 generation body 被清理。
    {
        const std::string legacyId(32, 'c');
        const std::string legacyBodyName = "body-" + legacyId + "-7.utf8";
        check(writeBytes(directory / legacyBodyName, "legacy migrate body\n"), "write v1 migration body fixture");
        const std::string v1 =
            "{\"version\":1,\"generation\":\"" + legacyId +
            "\",\"active\":7,\"records\":[{\"id\":7,\"path\":\"legacy.md\",\"vaultRoot\":\"\","
            "\"language\":\"markdown\",\"wrap\":-1,\"dirty\":true,\"encoding\":\"utf8\","
            "\"codepage\":0,\"bom\":false,\"crlf\":false}]}\n";
        check(writeBytes(manifest, v1), "write v1 migration manifest fixture");
        restored.clear(); active = 0;
        check(neo::sessionstorage::read(restored, active) && restored.size() == 1 && active == 7 &&
                  restored[0].document.text == "legacy migrate body\n",
              "v1 manifest is readable before migration");

        neo::textfile::Document migrated;
        migrated.text = "legacy migrate body\n";
        const std::vector<neo::sessionstorage::WriteRecord> upgraded = {
            {7, "legacy.md", "", "markdown", -1, true, &migrated},
            {8, "second.md", "", "text", -1, false, nullptr},
        };
        check(neo::sessionstorage::write(upgraded, 8), "migration write to v2 succeeds");
        check(readBytes(manifest).find("\"version\":2") != std::string::npos,
              "migration promotes the manifest to v2");
        check(!fs::exists(directory / legacyBodyName),
              "migration cleans the unreferenced v1 generation body");
        restored.clear(); active = 0;
        check(neo::sessionstorage::read(restored, active) && restored.size() == 2 && active == 8 &&
                  restored[0].document.text == "legacy migrate body\n",
              "migrated v2 session restores with identical body bytes");
    }

    // v2 读取侧故障：body 尺寸与清单 bytes 不符 / 同尺寸篡改 / 非法 UTF-8。
    {
        neo::textfile::Document authentic;
        authentic.text = "authentic body text";
        const std::vector<neo::sessionstorage::WriteRecord> source = {
            {91, "auth.md", "", "markdown", -1, true, &authentic},
        };
        check(neo::sessionstorage::write(source, 91), "write authenticity fixture session");
        const std::string authenticManifest = readBytes(manifest);
        const std::string bodyKey = "\"body\":\"";
        const std::size_t at = authenticManifest.find(bodyKey);
        std::string bodyName;
        if (at != std::string::npos) {
            const std::size_t valueStart = at + bodyKey.size();
            const std::size_t valueEnd = authenticManifest.find('"', valueStart);
            if (valueEnd != std::string::npos) bodyName = authenticManifest.substr(valueStart, valueEnd - valueStart);
        }
        check(!bodyName.empty(), "locate v2 content-addressed body name");
        const fs::path bodyPath = directory / bodyName;

        // (a) 截断成 0 字节：清单 bytes 说 N，body 为空 → 拒绝，不能恢复成空草稿。
        check(writeBytes(bodyPath, ""), "truncate body fixture");
        restored.clear(); active = 999;
        check(!neo::sessionstorage::read(restored, active) && restored.empty() && active == 0,
              "body truncated below the declared size fails closed instead of yielding an empty draft");

        // (b) 同尺寸但内容不同：当前实现不重算内容哈希，原样返回文件字节（缺口，见节点报告）。
        const std::string sameSize(authentic.text.size(), 'x');
        check(writeBytes(bodyPath, sameSize), "rewrite body with same-size different content");
        restored.clear(); active = 0;
        const bool sameSizeRead = neo::sessionstorage::read(restored, active);
        check(sameSizeRead && restored.size() == 1 && active == 91 &&
                  restored[0].document.text.size() == authentic.text.size(),
              "same-size body tamper does not crash and keeps the declared byte count");
        if (sameSizeRead && restored.size() == 1) {
            check(restored[0].document.text == sameSize,
                  "same-size body content is returned verbatim (content hash is not re-verified; documented gap)");
        }

        // (c) 同尺寸但非法 UTF-8：严格 UTF-8 校验拒绝。
        std::string invalid(authentic.text.size(), 'a');
        invalid[0] = '\xc3';
        invalid[1] = '\x28';
        check(writeBytes(bodyPath, invalid), "rewrite body with invalid UTF-8");
        restored.clear(); active = 999;
        check(!neo::sessionstorage::read(restored, active) && restored.empty() && active == 0,
              "invalid UTF-8 body fails closed");
    }

    // 新保存替换旧快照：旧 body 被清理；把旧快照再写一次得到自洽旧会话，绝不与新状态混合。
    {
        neo::textfile::Document oldSnapshot;
        oldSnapshot.text = "old snapshot body";
        neo::textfile::Document newSnapshot;
        newSnapshot.text = "new snapshot body";
        const std::vector<neo::sessionstorage::WriteRecord> older = {
            {71, "shared.md", "", "markdown", -1, true, &oldSnapshot}};
        const std::vector<neo::sessionstorage::WriteRecord> newer = {
            {71, "shared.md", "", "markdown", -1, true, &newSnapshot}};
        check(neo::sessionstorage::write(older, 71), "write older snapshot");
        check(neo::sessionstorage::write(newer, 71), "write newer snapshot over the older one");
        restored.clear(); active = 0;
        check(neo::sessionstorage::read(restored, active) && restored.size() == 1 &&
                  restored[0].document.text == "new snapshot body",
              "newer save fully replaces the older snapshot with no blended state");
        check(bodyFiles(directory).size() == 1, "newer save cleans the unreferenced older body");
        // 迟到的旧 writer（storage 层无序号护栏，顺序由 session_write_scheduler 的单 in-flight 保证）
        // 再次落盘旧快照：只能得到一个自洽的旧会话，不会把两次提交混在一起。
        check(neo::sessionstorage::write(older, 71), "a late older writer still writes a consistent snapshot");
        restored.clear(); active = 0;
        check(neo::sessionstorage::read(restored, active) && restored.size() == 1 &&
                  restored[0].document.text == "old snapshot body",
              "a late older snapshot is self-consistent (last-write-wins; ordering is scheduler-enforced)");
    }

    // A blocked manifest removal is observable and must not delete the bodies
    // that the still-present manifest may reference.
    const std::vector<fs::path> bodiesBeforeClear = bodyFiles(directory);
    const std::string validManifestBeforeClear = readBytes(manifest);
    check(!validManifestBeforeClear.empty(), "capture the committed manifest before clear failure fixture");
    error.clear();
    check(fs::remove(manifest, error) && !error, "remove committed manifest before obstruction fixture");
    error.clear();
    check(fs::create_directory(manifest, error) && !error,
          "create a non-empty manifest-path obstruction for clear failure");
    const fs::path obstruction = manifest / "keep.txt";
    check(writeBytes(obstruction, "preserve session on clear failure\n"),
          "write manifest-path obstruction marker");
    check(!neo::sessionstorage::clear(), "clear reports an obstructed manifest removal");
    check(bodyFiles(directory).size() == bodiesBeforeClear.size(),
          "failed clear preserves owned bodies while manifest removal fails");
    check(fs::exists(obstruction), "failed clear leaves the obstruction untouched");
    fs::remove_all(manifest, error);
    check(!error, "remove clear obstruction fixture");
    error.clear();
    check(writeBytes(manifest, validManifestBeforeClear), "restore the committed manifest after failed clear fixture");

    check(neo::sessionstorage::clear(), "clear succeeds after the obstruction is removed");
    check(!fs::exists(manifest), "clear removes session manifest");
    check(bodyFiles(directory).empty(), "clear removes only owned session body files");
    restored.clear(); active = 99;
    check(neo::sessionstorage::read(restored, active) && restored.empty() && active == 0,
          "after clear, the next restore is an empty session");
    check(readBytes(legacyRecovery) == "legacy recovery must survive\n", "clear leaves legacy recovery untouched");

    const fs::path foreignConfigFile = config / "unrelated.txt";
    const fs::path foreignSessionFile = directory / "unrelated.bin";
    check(writeBytes(foreignConfigFile, "keep config file\n") &&
              writeBytes(foreignSessionFile, "keep session file\n"),
          "create unrelated files around normal-exit cleanup");
    error.clear();
    check(fs::remove(legacyRecovery, error) && !error,
          "remove the legacy recovery file before blocking-removal fixture");
    error.clear();
    check(fs::create_directory(legacyRecovery, error) && !error,
          "create a recovery-path obstruction for normal-exit cleanup");
    const fs::path recoveryObstruction = legacyRecovery / "keep.txt";
    check(writeBytes(recoveryObstruction, "preserve obstruction\n"),
          "write legacy recovery obstruction marker");
    check(!neo::sessionstorage::clearForNormalExit(),
          "normal-exit cleanup reports a blocked legacy recovery removal");
    check(fs::exists(recoveryObstruction), "blocked legacy recovery remains untouched");
    check(fs::exists(foreignConfigFile) && fs::exists(foreignSessionFile),
          "failed normal-exit cleanup preserves unrelated files");
    fs::remove_all(legacyRecovery, error);
    check(!error, "remove legacy recovery obstruction fixture");
    error.clear();
    check(writeBytes(legacyRecovery, "legacy recovery to clear\n"),
          "write legacy recovery fixture for successful normal exit");
    check(neo::sessionstorage::clearForNormalExit(), "normal-exit cleanup removes session and legacy recovery");
    check(!fs::exists(legacyRecovery), "successful normal exit removes legacy recovery.txt");
    check(fs::exists(foreignConfigFile) && fs::exists(foreignSessionFile),
          "successful normal-exit cleanup leaves unrelated files alone");
    fs::remove_all(root, error);
    return failures == 0 ? 0 : 1;
}
