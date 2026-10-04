// R2：图片粘贴落盘的模型层测试（需求 §9.3）。
//
// 覆盖（不碰真实剪贴板——capture 的系统路径靠人工验收）：
//   ① parseDib：24/32bpp × 上下行序、全零 alpha 视为不透明、BI_BITFIELDS 标准
//      掩码（40 头带掩码 DWORD / V4 头内掩码）、坏头/截断/超大像素/坏掩码拒绝；
//   ② png_encode：RGBA → PNG → stb 解码逐像素一致；
//   ③ attachment：shared/per-file 目标解析、时间戳命名冲突自增不覆盖、
//      Markdown 链接的 <> 包裹。

#include "model/attachment.h"
#include "model/png_encode.h"
#include "model/text_file.h"
#include "platform/clipboard_image.h"

#include "3rd/stb_image.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "[image_attach] FAIL: " << what << '\n';
        ++failures;
    }
}

const fs::path& workDir() {
    static const fs::path dir = fs::temp_directory_path() / "neo-image-attach-test";
    return dir;
}

std::string writeBytes(const std::string& name, const std::string& bytes) {
    const fs::path path = workDir() / name;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return neo::textfile::pathToUtf8(path);
}

// 小端写入器：拼 DIB 头与像素用。
class Bytes {
public:
    Bytes& u32(unsigned int value) {
        push(value & 0xFFu);
        push((value >> 8) & 0xFFu);
        push((value >> 16) & 0xFFu);
        push((value >> 24) & 0xFFu);
        return *this;
    }
    Bytes& s32(int value) {
        return u32(static_cast<unsigned int>(value));
    }
    Bytes& u16(unsigned int value) {
        push(value & 0xFFu);
        push((value >> 8) & 0xFFu);
        return *this;
    }
    Bytes& raw(const std::string& bytes) {
        for (const char character : bytes) {
            push(static_cast<unsigned char>(character));
        }
        return *this;
    }
    std::string str() const { return data_; }

private:
    void push(unsigned char byte) { data_.push_back(static_cast<char>(byte)); }
    std::string data_;
};

std::string dibHeader(int width, int height, unsigned short bitCount, unsigned int compression,
                      unsigned int headerSize = 40u, unsigned int clrUsed = 0u) {
    return Bytes()
        .u32(headerSize)
        .s32(width)
        .s32(height)
        .u16(1)                // biPlanes
        .u16(bitCount)
        .u32(compression)      // biCompression
        .u32(0)                // biSizeImage
        .s32(0)                // biXPelsPerMeter
        .s32(0)                // biYPelsPerMeter
        .u32(clrUsed)
        .u32(0)                // biClrImportant
        .str();
}

void testParseDib() {
    using neo::clipboardimage::ImageData;

    // 32bpp BI_RGB bottom-up：像素 BGRA，全零 alpha → 不透明；行序翻转。
    {
        const std::string pixels =
            std::string("\x01\x02\x03\x00", 4) +   // 底行第 0 列 BGR=01 02 03, alpha 0
            std::string("\x04\x05\x06\x00", 4) +
            std::string("\x07\x08\x09\x00", 4) +
            std::string("\x0A\x0B\x0C\x00", 4);
        const std::string dib = dibHeader(2, 2, 32, 0u) + pixels;
        ImageData image;
        std::string reason;
        check(neo::clipboardimage::parseDib(
                  reinterpret_cast<const unsigned char*>(dib.data()), dib.size(), image, reason),
              "32bpp BI_RGB bottom-up 应解析成功：" + reason);
        check(image.width == 2 && image.height == 2 && image.rgba.size() == 16u, "尺寸/字节数正确");
        // top-down 后：底行翻到最上；BGR→RGBA（B=07 G=08 R=09 → R 在前）。
        const unsigned char* row0 = image.rgba.data();
        check(row0[0] == 0x09 && row0[1] == 0x08 && row0[2] == 0x07 && row0[3] == 0xFF,
              "底行翻到最上、RGBA 顺序正确且全零 alpha 视为不透明");
        check(row0[7] == 0xFF, "第二像素同样不透明");
    }

    // 32bpp top-down（负高）带非零 alpha：原样保留。
    {
        const std::string pixels =
            std::string("\x10\x20\x30\x80", 4) + std::string("\x40\x50\x60\xC0", 4) +
            std::string("\x00\x00\x00\x01", 4) + std::string("\xFF\xFF\xFF\x7F", 4);
        const std::string dib = dibHeader(2, -2, 32, 0u) + pixels;
        ImageData image;
        std::string reason;
        check(neo::clipboardimage::parseDib(
                  reinterpret_cast<const unsigned char*>(dib.data()), dib.size(), image, reason),
              "32bpp top-down 应解析成功：" + reason);
        const unsigned char* row0 = image.rgba.data();
        check(row0[0] == 0x30 && row0[1] == 0x20 && row0[2] == 0x10 && row0[3] == 0x80,
              "首行不翻转、RGBA 顺序正确、alpha 保留");
    }

    // 24bpp bottom-up：BGR→RGBA、stride 4 字节对齐补齐、行序翻转。
    {
        const std::string pixels =
            std::string("\x01\x02\x03\x00", 4) +  // 底行：3 字节像素 + 1 字节补齐
            std::string("\x04\x05\x06\x00", 4);
        const std::string dib = dibHeader(1, 2, 24, 0u) + pixels;
        ImageData image;
        std::string reason;
        check(neo::clipboardimage::parseDib(
                  reinterpret_cast<const unsigned char*>(dib.data()), dib.size(), image, reason),
              "24bpp 应解析成功：" + reason);
        check(image.width == 1 && image.height == 2, "尺寸正确");
        check(image.rgba[0] == 0x06 && image.rgba[1] == 0x05 && image.rgba[2] == 0x04 &&
                  image.rgba[3] == 0xFF,
              "24bpp 翻转与 BGR→RGBA 正确、alpha 填充");
    }

    // 40 头 + BI_BITFIELDS：三掩码在颜色表位置，标准掩码应接受。
    {
        const std::string masks = Bytes().u32(0x00FF0000u).u32(0x0000FF00u).u32(0x000000FFu).str();
        const std::string pixels = std::string("\x10\x20\x30\xFF", 4);        const std::string dib = dibHeader(1, 1, 32, 3u) + masks + pixels;
        ImageData image;
        std::string reason;
        check(neo::clipboardimage::parseDib(
                  reinterpret_cast<const unsigned char*>(dib.data()), dib.size(), image, reason),
              "40 头 BI_BITFIELDS 标准掩码应接受：" + reason);
        check(image.rgba[0] == 0x30 && image.rgba[1] == 0x20 && image.rgba[2] == 0x10 &&
                  image.rgba[3] == 0xFF,
              "掩码像素解出正确（无 alpha 掩码 → 不透明）");
    }

    // 非标准掩码拒绝。
    {
        const std::string masks = Bytes().u32(0x000000FFu).u32(0x0000FF00u).u32(0x00FF0000u).str();
        const std::string dib = dibHeader(1, 1, 32, 3u) + masks + std::string(4, '\0');
        ImageData image;
        std::string reason;
        check(!neo::clipboardimage::parseDib(
                  reinterpret_cast<const unsigned char*>(dib.data()), dib.size(), image, reason) &&
                  !reason.empty(),
              "非标准掩码应拒绝");
    }

    // 16bpp / RLE 压缩 / 坏头 / 截断 / 像素超限。
    {
        ImageData image;
        std::string reason;
        const std::string dib16 = dibHeader(1, 1, 16, 0u) + std::string(4, '\0');
        check(!neo::clipboardimage::parseDib(reinterpret_cast<const unsigned char*>(dib16.data()),
                                             dib16.size(), image, reason),
              "16bpp 应拒绝");
        const std::string dibRle = dibHeader(1, 1, 8, 2u) + std::string(4, '\0');
        check(!neo::clipboardimage::parseDib(reinterpret_cast<const unsigned char*>(dibRle.data()),
                                             dibRle.size(), image, reason),
              "RLE 压缩应拒绝");
        const std::string dibCore = std::string("\x0C\x00\x00\x00", 4) + std::string(36, '\0');
        check(!neo::clipboardimage::parseDib(reinterpret_cast<const unsigned char*>(dibCore.data()),
                                             dibCore.size(), image, reason),
              "BITMAPCOREHEADER 应拒绝");
        const std::string truncated = dibHeader(64, 64, 32, 0u) + std::string(8, '\0');
        check(!neo::clipboardimage::parseDib(reinterpret_cast<const unsigned char*>(truncated.data()),
                                             truncated.size(), image, reason),
              "像素数据截断应拒绝");
        const std::string huge = dibHeader(0x7FFFFFFF, 0x7FFFFFFF, 32, 0u);
        check(!neo::clipboardimage::parseDib(reinterpret_cast<const unsigned char*>(huge.data()),
                                             huge.size(), image, reason) && !reason.empty(),
              "像素量超限应拒绝");
    }
}

void testPngEncode() {
    // 3x2 渐变（含 alpha 变化），编码后用 stb 解码逐像素比对。
    std::vector<unsigned char> rgba(3 * 2 * 4);
    for (int index = 0; index < 3 * 2; ++index) {
        rgba[index * 4 + 0] = static_cast<unsigned char>(index * 40);
        rgba[index * 4 + 1] = static_cast<unsigned char>(index * 20);
        rgba[index * 4 + 2] = static_cast<unsigned char>(200 - index * 10);
        rgba[index * 4 + 3] = static_cast<unsigned char>(index == 0 ? 0 : 255);
    }
    std::string png;
    std::string error;
    check(neo::pngencode::encodeRgba(3, 2, rgba.data(), png, error), "PNG 编码应成功：" + error);
    check(png.size() > 8 && png.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0, "输出应有 PNG magic");

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* decoded =
        stbi_load_from_memory(reinterpret_cast<const unsigned char*>(png.data()),
                              static_cast<int>(png.size()), &width, &height, &channels, 4);
    check(decoded != nullptr && width == 3 && height == 2, "PNG 应回码为 3x2");
    if (decoded != nullptr) {
        bool identical = true;
        for (std::size_t index = 0; index < rgba.size(); ++index) {
            if (decoded[index] != rgba[index]) {
                identical = false;
                break;
            }
        }
        check(identical, "解码像素应与输入 RGBA 逐字节一致");
        stbi_image_free(decoded);
    }

    std::string bad;
    check(!neo::pngencode::encodeRgba(0, 2, rgba.data(), bad, error), "非法尺寸应失败");
}

void testAttachment() {
    // shared / per-file 目标解析。
    neo::attachment::Target target;
    std::string error;
    check(neo::attachment::resolveTarget("D:/vault/笔记.md", 0, target, error), "shared 解析应成功");
    check(target.relativeDir == "_assets" && target.absoluteDir == "D:/vault/_assets",
          "shared 目录应为文档旁 _assets");
    check(neo::attachment::resolveTarget("D:/vault/子 目录/笔记.md", 1, target, error),
          "per-file 解析应成功");
    check(target.relativeDir == "笔记.assets" && target.absoluteDir == "D:/vault/子 目录/笔记.assets",
          "per-file 目录应为 <文档名>.assets");
    check(!neo::attachment::resolveTarget("", 0, target, error) && !error.empty(),
          "未落盘文档应拒绝解析");

    // Markdown 链接：目标包 <>，空格安全。
    check(neo::attachment::markdownLink("_assets", "image-1 2.png") == "![](<_assets/image-1 2.png>)",
          "链接应包 <> 以往返空格");

    // 命名冲突自增：第一次得到 image-<stamp>.png；把它建出来后再取应得 -2。
    const std::string dir = writeBytes(".keep", "x");  // 确保 workDir 存在
    (void)dir;
    const std::string dirUtf8 = neo::textfile::pathToUtf8(workDir());
    const std::string first = neo::attachment::uniqueFileName(dirUtf8, "image");
    check(first.rfind("image-", 0) == 0 && first.size() > strlen(".png") &&
              first.compare(first.size() - 4, 4, ".png") == 0,
          "文件名应为 image-<时间戳>.png 形态");
    check(!neo::attachment::fileExists(dirUtf8 + "/" + first), "首取名不应已存在");
    writeBytes(first, "occupied");
    check(neo::attachment::fileExists(dirUtf8 + "/" + first), "预置占用文件应存在");
    const std::string second = neo::attachment::uniqueFileName(dirUtf8, "image");
    check(second != first && second.find("-2.png") != std::string::npos,
          "撞名后应自增 -2 且不覆盖已有文件");
}

} // namespace

int main() {
    std::error_code cleanupError;
    fs::remove_all(workDir(), cleanupError);
    fs::create_directories(workDir(), cleanupError);

    testParseDib();
    testPngEncode();
    testAttachment();

    if (failures != 0) {
        std::cerr << failures << " image_attach test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "image_attach tests passed\n";
    return EXIT_SUCCESS;
}
