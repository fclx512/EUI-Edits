#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace neo::clipboardimage {

// 剪贴板位图，统一为 top-down RGBA8（width*height*4 字节，无行间隙）。
// 仅 Windows 有真实实现；其他平台 available()/capture() 恒为 false。
struct ImageData {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;
};

// Windows 剪贴板可能提供真实位图（DIB/PNG），也可能只提供 Explorer 文件列表。
// 文件载荷保留原路径，以便应用把原文件字节复制到附件目录而不转码。
struct ClipboardPayload {
    enum class Kind { None, Bitmap, File } kind = Kind::None;
    ImageData image;
    std::string filePathUtf8;
};

// 像素量上限（需求 §9.3：首版 16M 像素，超限明确报错，不静默缩小）。
inline constexpr long long kMaxPixels = 16ll * 1024ll * 1024ll;

// 纯解析：从一份 DIB（BITMAPINFOHEADER 起，含调色板/掩码，到位图数据末尾）
// 解出 RGBA。data/size 是真实可读上界（如 GlobalLock/GlobalSize 的结果）。
// 只接受 24/32bpp 的 BI_RGB 与标准掩码的 BI_BITFIELDS；头损坏、尺寸溢出、
// 数据不完整、像素量超限一律失败并给可读 reason。
bool parseDib(const unsigned char* data, std::size_t size, ImageData& out, std::string& reason);
// 解析剪贴板注册 PNG 格式，统一输出 top-down RGBA，并应用像素量上限。
bool parsePng(const unsigned char* data, std::size_t size, ImageData& out, std::string& reason);

// 剪贴板里是否有可尝试的单文件/位图载荷（CF_HDROP、PNG 注册格式、DIBV5、DIB）。
bool available();

// 捕获一张位图或单个支持的本地图片文件。多个文件/不支持的文件会明确失败。
bool capture(ClipboardPayload& out, std::string& reason);

// 兼容纯位图调用方；剪贴板是文件列表时返回 false。
bool capture(ImageData& out, std::string& reason);

} // namespace neo::clipboardimage
