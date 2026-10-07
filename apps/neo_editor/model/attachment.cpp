#include "model/i18n.h"
#include "model/attachment.h"

#include "model/text_file.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <algorithm>
#include <cctype>

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
    return uniqueFileName(dirAbsolute, stem, "png");
}

std::string imageExtension(const std::string& pathUtf8) {
    const std::string name = fs::u8path(pathUtf8).filename().u8string();
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size()) return {};
    std::string extension = name.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return extension;
}

bool isSupportedImagePath(const std::string& pathUtf8) {
    const std::string extension = imageExtension(pathUtf8);
    const auto& extensions = supportedImageExtensions();
    return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

const std::vector<std::string>& supportedImageExtensions() {
    static const std::vector<std::string> extensions = {
        "png", "jpg", "jpeg", "bmp", "gif", "svg", "tga", "hdr",
        "ppm", "pgm", "pnm", "psd", "pic"};
    return extensions;
}

std::string copyImageFile(const std::string& sourcePathUtf8, const std::string& targetDirUtf8,
                          const std::string& stem, std::string& error) {
    error.clear();
    if (!isSupportedImagePath(sourcePathUtf8) || targetDirUtf8.empty()) {
        error = "unsupported image source or empty target directory";
        return {};
    }
    const fs::path source = textfile::pathFromUtf8(sourcePathUtf8);
    const fs::path targetDir = textfile::pathFromUtf8(targetDirUtf8);
    std::error_code ec;
    if (!fs::is_regular_file(source, ec) || ec) {
        error = ec ? ec.message() : "image source is not a regular file";
        return {};
    }
    const bool madeDirectory = fs::create_directories(targetDir, ec);
    if (ec) {
        error = ec.message();
        return {};
    }
    const std::string extension = imageExtension(sourcePathUtf8);
    for (int attempt = 0; attempt < 32; ++attempt) {
        const std::string name = uniqueFileName(targetDirUtf8, stem, extension);
        const fs::path destination = targetDir / textfile::pathFromUtf8(name);
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path temporary = targetDir / textfile::pathFromUtf8(
            name + ".tmp-" + std::to_string(nonce) + "-" + std::to_string(attempt));
        if (fs::exists(destination, ec) || ec || fs::exists(temporary, ec) || ec) {
            ec.clear();
            continue;
        }
        if (!fs::copy_file(source, temporary, fs::copy_options::none, ec)) {
            error = ec ? ec.message() : "could not copy image";
            std::error_code cleanupError;
            fs::remove(temporary, cleanupError);
            if (madeDirectory) { std::error_code directoryError; fs::remove(targetDir, directoryError); }
            return {};
        }
        fs::rename(temporary, destination, ec);
        if (ec) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            if (fs::exists(destination, ignored)) {
                ec.clear();
                continue;
            }
            error = ec.message();
            if (madeDirectory) { std::error_code ignored; fs::remove(targetDir, ignored); }
            return {};
        }
        return name;
    }
    error = "could not choose a unique image filename";
    return {};
}

std::string uniqueFileName(const std::string& dirAbsolute, const std::string& stem,
                           const std::string& extension) {
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

    const std::string extensionSuffix = extension.empty() ? std::string{} : "." + extension;
    std::string candidate = stem + "-" + stamp + extensionSuffix;
    if (!exists(candidate)) {
        return candidate;
    }
    for (int collisionIndex = 2; collisionIndex < 1000; ++collisionIndex) {
        candidate = stem + "-" + stamp + "-" + std::to_string(collisionIndex) + extensionSuffix;
        if (!exists(candidate)) {
            return candidate;
        }
    }
    // 理论到不了这里：兜底用一个肯定不存在的名字。
    return stem + "-" + stamp + "-overflow" + extensionSuffix;
}

std::string markdownLink(const std::string& relativeDir, const std::string& fileName) {
    return "![](<" + relativeDir + "/" + fileName + ">)";
}

std::string markdownAbsoluteLink(std::string pathUtf8) {
    std::replace(pathUtf8.begin(), pathUtf8.end(), '\\', '/');
    return "![](<" + pathUtf8 + ">)";
}

} // namespace neo::attachment
