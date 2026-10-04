#pragma once

#include <filesystem>
#include <functional>
#include <string_view>

namespace neo::atomicwrite {

// 原子写文件（T9：settings.ini / recovery.txt 共用）。
//
// 流程：内容完整写进"目标同目录的唯一临时文件" → 检查 stream 状态与 close →
// 原子替换目标。任何一步失败都会删掉临时文件并原样保留旧目标。
//
// 刻意不做"替换失败就直接 trunc 覆写目标"的兜底：那种做法会把半截内容写进
// 旧文件，正是本 helper 要防的事故。替换失败时宁可返回 false（本次写入丢失，
// 旧文件完好），也不冒截断旧文件的险。
//
// 路径一律沿用工程的 UTF-8 约定（调用方用 textfile::pathFromUtf8 构造），
// 这里不做任何本地编码转换。
bool writeFile(const std::filesystem::path& target, std::string_view content,
               const std::function<bool()>& beforeReplace = {});

namespace testing {
// 故障注入：置 true 后，writeFile 会在"临时文件已写完、替换之前"失败，
// 用于验证替换失败时旧文件逐字节完好、临时文件被清理。仅测试代码调用。
void failBeforeReplace(bool enabled);
} // namespace testing

} // namespace neo::atomicwrite
