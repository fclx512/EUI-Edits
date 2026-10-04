#pragma once

#include <string>

namespace neo::attachment {

// 依据设置与文档路径算出附件落点（需求 §9.3：shared/per-file 两种模式，
// 目录名只允许单层相对名）。失败时 error 给可读说明（文档未落盘、目录名非法）。
struct Target {
    std::string relativeDir;   // 相对文档目录的目录名（'/' 分隔），如 "_assets"
    std::string absoluteDir;   // 绝对目录（UTF-8，平台分隔符）
    std::string relativeLink;  // 插进 Markdown 的目录部分（'/' 分隔）
};

bool resolveTarget(const std::string& docPathUtf8, int attachmentMode, Target& out, std::string& error);

// 目标目录里下一个可用文件名：<stem>.png，冲突时 <stem>-2.png、<stem>-3.png…
// dirAbsolute 为空（尚未创建的目录）时只按 stem 命名。
std::string uniqueFileName(const std::string& dirAbsolute, const std::string& stem);

// 目录里 stem 是否已被占用（uniqueFileName 的存在性判定，测试可注入）。
bool fileExists(const std::string& absolutePathUtf8);

// 图片附件的 Markdown：目标一律包 <>（空格/括号可往返，MD4C 与 Obsidian 都认）。
std::string markdownLink(const std::string& relativeDir, const std::string& fileName);

} // namespace neo::attachment
