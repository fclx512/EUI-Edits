#include "model/i18n.h"
#include "platform/clipboard_image.h"

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cstring>

namespace neo::clipboardimage {
namespace {

unsigned int readU32(const unsigned char* p) {
    return static_cast<unsigned int>(p[0]) | (static_cast<unsigned int>(p[1]) << 8) |
           (static_cast<unsigned int>(p[2]) << 16) | (static_cast<unsigned int>(p[3]) << 24);
}

int readS32(const unsigned char* p) {
    return static_cast<int>(readU32(p));
}

unsigned short readU16(const unsigned char* p) {
    return static_cast<unsigned short>(static_cast<unsigned short>(p[0]) |
                                       (static_cast<unsigned short>(p[1]) << 8));
}

constexpr unsigned int kMaskRed = 0x00FF0000u;
constexpr unsigned int kMaskGreen = 0x0000FF00u;
constexpr unsigned int kMaskBlue = 0x000000FFu;
constexpr unsigned int kMaskAlpha = 0xFF000000u;

} // namespace

bool parseDib(const unsigned char* data, std::size_t size, ImageData& out, std::string& reason) {
    out = ImageData{};
    if (size < 40) {
        reason = i18n::tr("clipboard.header_partial");
        return false;
    }

    const unsigned int headerSize = readU32(data);
    // 40 = BITMAPINFOHEADER；52/56 = V3 变体（带掩码）；108/124 = V4/V5。
    if (headerSize != 40u && headerSize != 52u && headerSize != 56u && headerSize != 108u && headerSize != 124u) {
        reason = i18n::tr("clipboard.header_unsupported");
        return false;
    }
    if (headerSize > size) {
        reason = i18n::tr("clipboard.header_partial");
        return false;
    }

    const int width = readS32(data + 4);
    const int rawHeight = readS32(data + 8);
    const unsigned short bitCount = readU16(data + 14);
    const unsigned int compression = readU32(data + 16);
    const unsigned int clrUsed = readU32(data + 32);

    if (width <= 0 || rawHeight == 0) {
        reason = i18n::tr("clipboard.dimensions");
        return false;
    }
    const int height = rawHeight > 0 ? rawHeight : -rawHeight;
    if (static_cast<long long>(width) * static_cast<long long>(height) > kMaxPixels) {
        reason = i18n::tr("clipboard.pixel_limit");
        return false;
    }
    if (bitCount != 24 && bitCount != 32) {
        reason = i18n::tr("clipboard.depth");
        return false;
    }

    // BI_RGB(0)；BI_BITFIELDS(3) 只在 32bpp 且掩码是标准布局时接受。
    unsigned int maskRed = kMaskRed;
    unsigned int maskGreen = kMaskGreen;
    unsigned int maskBlue = kMaskBlue;
    unsigned int maskAlpha = 0u;
    std::size_t maskBytes = 0;
    if (compression == 0u) {
        // BI_RGB：掩码按默认字节序布局。
    } else if (compression == 3u && bitCount == 32) {
        const unsigned char* masks = nullptr;
        if (headerSize >= 56u) {
            // V4/V5：掩码在头内（bV4RedMask 等在 offset 40 起）。
            masks = data + 40;
        } else if (headerSize == 40u) {
            // 旧头：三个掩码 DWORD 紧跟在头后（颜色表位置）。
            masks = data + headerSize;
            maskBytes = 3u * 4u;
        } else {
            reason = i18n::tr("clipboard.header_unsupported");
            return false;
        }
        maskRed = readU32(masks);
        maskGreen = readU32(masks + 4);
        maskBlue = readU32(masks + 8);
        maskAlpha = headerSize >= 56u ? readU32(masks + 12) : 0u;
        const bool standard = maskRed == kMaskRed && maskGreen == kMaskGreen && maskBlue == kMaskBlue &&
                              (maskAlpha == 0u || maskAlpha == kMaskAlpha);
        if (!standard) {
            reason = i18n::tr("clipboard.mask");
            return false;
        }
    } else {
        reason = i18n::tr("clipboard.compression");
        return false;
    }

    // 调色板仅在 clrUsed 显式给出时计入手 stride（24/32bpp 通常为 0）。
    std::size_t paletteBytes = 0;
    if (clrUsed > 0u) {
        if (clrUsed > 256u) {
            reason = i18n::tr("clipboard.palette");
            return false;
        }
        paletteBytes = static_cast<std::size_t>(clrUsed) * 4u;
    }

    const std::size_t offset = headerSize + maskBytes + paletteBytes;
    const std::size_t stride = (static_cast<std::size_t>(width) * bitCount / 8u + 3u) & ~std::size_t{3u};
    const std::size_t required = offset + stride * static_cast<std::size_t>(height);
    if (required > size) {
        reason = i18n::tr("clipboard.data_partial");
        return false;
    }

    out.width = width;
    out.height = height;
    out.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0u);
    const bool bottomUp = rawHeight > 0;

    // 先扫 alpha：老式 32 位 BI_RGB 常把 alpha 字节全置 0，视作不透明。
    bool anyAlpha = false;
    if (bitCount == 32) {
        for (int row = 0; row < height && !anyAlpha; ++row) {
            const unsigned char* src = data + offset + static_cast<std::size_t>(row) * stride;
            for (int column = 0; column < width; ++column) {
                if (src[column * 4u + 3u] != 0u) {
                    anyAlpha = true;
                    break;
                }
            }
        }
    }

    for (int row = 0; row < height; ++row) {
        const int sourceRow = bottomUp ? height - 1 - row : row;
        const unsigned char* src = data + offset + static_cast<std::size_t>(sourceRow) * stride;
        unsigned char* dst = out.rgba.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 4u;
        for (int column = 0; column < width; ++column) {
            if (bitCount == 24) {
                dst[column * 4u + 0u] = src[column * 3u + 2u];
                dst[column * 4u + 1u] = src[column * 3u + 1u];
                dst[column * 4u + 2u] = src[column * 3u + 0u];
                dst[column * 4u + 3u] = 0xFFu;
            } else if (compression == 3u) {
                const unsigned int pixel = readU32(src + column * 4u);
                dst[column * 4u + 0u] =
                    static_cast<unsigned char>((pixel & maskRed) >> 16);   // 掩码已验证为标准布局
                dst[column * 4u + 1u] = static_cast<unsigned char>((pixel & maskGreen) >> 8);
                dst[column * 4u + 2u] = static_cast<unsigned char>(pixel & maskBlue);
                dst[column * 4u + 3u] = maskAlpha != 0u ? static_cast<unsigned char>((pixel & maskAlpha) >> 24)
                                                        : 0xFFu;
            } else {
                dst[column * 4u + 0u] = src[column * 4u + 2u];
                dst[column * 4u + 1u] = src[column * 4u + 1u];
                dst[column * 4u + 2u] = src[column * 4u + 0u];
                dst[column * 4u + 3u] = anyAlpha ? src[column * 4u + 3u] : 0xFFu;
            }
        }
    }
    return true;
}

#if defined(_WIN32)
namespace {

// GlobalLock/Unlock 与 OpenClipboard/CloseClipboard 的 RAII 配对（需求 §9.3）。
class ClipboardSession {
public:
    ClipboardSession() {
        opened_ = OpenClipboard(nullptr) != FALSE;
    }
    ~ClipboardSession() {
        if (opened_) {
            CloseClipboard();
        }
    }
    ClipboardSession(const ClipboardSession&) = delete;
    ClipboardSession& operator=(const ClipboardSession&) = delete;
    bool opened() const { return opened_; }

private:
    bool opened_ = false;
};

class GlobalSegment {
public:
    GlobalSegment(HGLOBAL handle) : handle_(handle) {
        data_ = handle_ != nullptr ? static_cast<const unsigned char*>(GlobalLock(handle_)) : nullptr;
        if (data_ != nullptr) {
            size_ = GlobalSize(handle_);
        }
    }
    ~GlobalSegment() {
        if (data_ != nullptr) {
            GlobalUnlock(handle_);
        }
    }
    GlobalSegment(const GlobalSegment&) = delete;
    GlobalSegment& operator=(const GlobalSegment&) = delete;
    const unsigned char* data() const { return data_; }
    std::size_t size() const { return size_; }

private:
    HGLOBAL handle_ = nullptr;
    const unsigned char* data_ = nullptr;
    std::size_t size_ = 0;
};

UINT preferredFormat() {
    // CF_DIBV5 是预定义常量（带 alpha 语义），优先于 CF_DIB。
    if (IsClipboardFormatAvailable(CF_DIBV5)) {
        return CF_DIBV5;
    }
    if (IsClipboardFormatAvailable(CF_DIB)) {
        return CF_DIB;
    }
    return 0u;
}

} // namespace

bool available() {
    return preferredFormat() != 0u;
}

bool capture(ImageData& out, std::string& reason) {
    out = ImageData{};
    reason.clear();

    const UINT format = preferredFormat();
    if (format == 0u) {
        reason = i18n::tr("clipboard.no_bitmap");
        return false;
    }

    ClipboardSession session;
    if (!session.opened()) {
        reason = i18n::tr("clipboard.open");
        return false;
    }

    HANDLE handle = GetClipboardData(format);
    if (handle == nullptr) {
        reason = i18n::tr("clipboard.read");
        return false;
    }

    GlobalSegment segment(static_cast<HGLOBAL>(handle));
    if (segment.data() == nullptr || segment.size() == 0) {
        reason = i18n::tr("clipboard.read");
        return false;
    }

    return parseDib(segment.data(), segment.size(), out, reason);
}

#else

bool available() {
    return false;
}

bool capture(ImageData& out, std::string& reason) {
    out = ImageData{};
    reason = i18n::tr("clipboard.platform");
    return false;
}

#endif

} // namespace neo::clipboardimage
