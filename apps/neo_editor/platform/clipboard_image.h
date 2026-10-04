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

// 像素量上限（需求 §9.3：首版 16M 像素，超限明确报错，不静默缩小）。
inline constexpr long long kMaxPixels = 16ll * 1024ll * 1024ll;

// 纯解析：从一份 DIB（BITMAPINFOHEADER 起，含调色板/掩码，到位图数据末尾）
// 解出 RGBA。data/size 是真实可读上界（如 GlobalLock/GlobalSize 的结果）。
// 只接受 24/32bpp 的 BI_RGB 与标准掩码的 BI_BITFIELDS；头损坏、尺寸溢出、
// 数据不完整、像素量超限一律失败并给可读 reason。
bool parseDib(const unsigned char* data, std::size_t size, ImageData& out, std::string& reason);

// 剪贴板里是否有可尝试的位图（CF_DIBV5 优先于 CF_DIB）。
bool available();

// 取出当前剪贴板位图；失败时 reason 给可读说明（没有位图 / 打不开 / 格式不支持）。
bool capture(ImageData& out, std::string& reason);

} // namespace neo::clipboardimage
