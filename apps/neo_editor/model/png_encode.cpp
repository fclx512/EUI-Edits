#include "model/i18n.h"
#include "model/png_encode.h"

#include "png.h"

#include <cstring>

namespace neo::pngencode {

// 用 libpng 简化写 API（png_image）：错误经 image.message 传回，无需自挂回调。
bool encodeRgba(int width, int height, const unsigned char* rgba, std::string& out, std::string& error) {
    out.clear();
    if (width <= 0 || height <= 0 || rgba == nullptr) {
        error = i18n::tr("png.invalid_pixels");
        return false;
    }

    png_image image;
    std::memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    image.width = static_cast<png_uint_32>(width);
    image.height = static_cast<png_uint_32>(height);
    image.format = PNG_FORMAT_RGBA;

    // 第一遍量出所需字节数，第二遍真正写出（简化 API 的标准两步用法）。
    png_alloc_size_t size = 0;
    if (png_image_write_to_memory(&image, nullptr, &size, 0, rgba, 0, nullptr) == 0) {
        error = i18n::format("png.encode_error", {{"error", image.message[0] != '\0' ? image.message : i18n::tr("error.unknown")}});
        png_image_free(&image);
        return false;
    }

    out.resize(static_cast<std::size_t>(size));
    if (png_image_write_to_memory(&image, out.data(), &size, 0, rgba, 0, nullptr) == 0) {
        error = i18n::format("png.encode_error", {{"error", image.message[0] != '\0' ? image.message : i18n::tr("error.unknown")}});
        png_image_free(&image);
        out.clear();
        return false;
    }
    out.resize(static_cast<std::size_t>(size));

    png_image_free(&image);
    return true;
}

} // namespace neo::pngencode
