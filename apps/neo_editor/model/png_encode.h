#pragma once

#include <cstddef>
#include <string>

namespace neo::pngencode {

// 把 top-down RGBA8 像素（连续 width*height*4 字节，无行间隙）编码为 PNG。
// 用 libpng 简化写 API，全部在内存完成；失败时 error 给可读说明，out 内容无效。
bool encodeRgba(int width, int height, const unsigned char* rgba, std::string& out, std::string& error);

} // namespace neo::pngencode
