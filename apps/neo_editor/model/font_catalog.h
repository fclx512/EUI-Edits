#pragma once

#include <string>
#include <vector>

// 系统字体目录的只读索引。
//
// 存在的理由：框架的字体参数（`fontFamily`）对"含 `.` 的字符串"直接按文件路径加载
// （`core/render/text.cpp:1850-1852` 实测），所以应用层不用碰 core 就能让用户自选字体。
// 但直接拿文件名当选项太丑（`msyhbd`），所以这里解析字体自己的 `name` 表取字体族名。
//
// TTC 当前只展示其 face 0；post/OS/2 的等宽、字重和斜体字段是目录提示，
// 实际代码字体仍由受隔离的 FreeType 预检确认等宽。
namespace neo::fonts {

struct Entry {
    std::string displayName;  // 字体族名（name 表 nameID=1），解析失败时退回文件名
    std::string path;         // 完整文件路径，直接交给 fontFamily
    int rank = 3;             // 常用字体置顶用：0 最靠前，3 默认
    bool monospaceHint = false; // post.isFixedPitch 元数据提示，不替代运行时等宽验证
    int weight = 400;          // OS/2.usWeightClass，缺失或异常时按常规字重处理
    bool italic = false;       // post.italicAngle 非零
};

// 扫描系统字体目录（`%WINDIR%\Fonts` 与 `%LOCALAPPDATA%\Microsoft\Windows\Fonts`），
// 按 (rank, 字体族名) 排序。首次调用扫描并缓存，之后返回同一份。
const std::vector<Entry>& catalog();

// 按文件路径查字体族名：命中缓存返回族名，否则退回文件名（去扩展名）。
// 用于设置面板显示"当前选的是哪个字体"。
std::string displayNameFor(const std::string& path);

} // namespace neo::fonts
