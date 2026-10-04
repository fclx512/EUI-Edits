#include "model/i18n.h"
#include "model/attachment.h"

#include "model/text_file.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace neo::attachment {
namespace {

namespace fs = std::filesystem;

// 单层相对目录名：非空、不含路径分隔符/父级引用/绝对前缀/控制字符与换行。
bool isValidDirName(const std::string& name) {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    if (name == "." || name == "..") {
        return false;
    }
    for (const char character : name) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20u || character == '/' || character == '\\' || character == ':') {
            return false;
        }
    }
    // Windows 保留尾点与尾空格。
    const char last = name.back();
    if (last == '.' || last == ' ') {
        return false;
    }
    return true;
}

} // namespace

bool fileExists(const std::string& absolutePathUtf8) {
    std::error_code error;
    return fs::exists(textfile::pathFromUtf8(absolutePathUtf8), error) && !error;
}

bool resolveTarget(const std::string& docPathUtf8, int attachmentMode, Target& out, std::string& error) {
    out = Target{};
    if (docPathUtf8.empty()) {
        error = i18n::tr("attachment.unsaved");
        return false;
    }

    const std::string docDir = textfile::parentPath(docPathUtf8);
    std::string dirName;
    if (attachmentMode == 1) {
        // per-file：<文档名去扩展名>.assets/
        std::string stem = textfile::fileName(docPathUtf8);
        const std::size_t dot = stem.rfind('.');
        if (dot != std::string::npos && dot > 0) {
            stem.resize(dot);
        }
        dirName = stem + ".assets";
    } else {
        dirName = "_assets";
    }
    if (!isValidDirName(dirName)) {
        error = i18n::tr("attachment.invalid_directory");
        return false;
    }

    out.relativeDir = dirName;
    out.relativeLink = dirName;
    // Windows 也接受 '/' 分隔，统一走 pathFromUtf8 转换。
    out.absoluteDir = docDir.empty() ? dirName : docDir + '/' + dirName;
    return true;
}

std::string uniqueFileName(const std::string& dirAbsolute, const std::string& stem) {
    // 时间戳命名：image-YYYYMMDD-HHMMSS；同秒或撞名时 -2、-3 递增，绝不覆盖。
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d", local.tm_year + 1900, local.tm_mon + 1,
                  local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);

    const auto exists = [&](const std::string& name) {
        return dirAbsolute.empty() ? false : fileExists(dirAbsolute + "/" + name);
    };

    std::string candidate = stem + "-" + stamp + ".png";
    if (!exists(candidate)) {
        return candidate;
    }
    for (int suffix = 2; suffix < 1000; ++suffix) {
        candidate = stem + "-" + stamp + "-" + std::to_string(suffix) + ".png";
        if (!exists(candidate)) {
            return candidate;
        }
    }
    // 理论到不了这里：兜底用一个肯定不存在的名字。
    return stem + "-" + stamp + "-overflow.png";
}

std::string markdownLink(const std::string& relativeDir, const std::string& fileName) {
    return "![](<" + relativeDir + "/" + fileName + ">)";
}

} // namespace neo::attachment
