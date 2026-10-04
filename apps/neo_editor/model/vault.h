#pragma once

#include <set>
#include <string>
#include <vector>

namespace neo::vault {

// 目录树节点。relative 是相对库根的路径，统一使用 '/' 分隔。
struct Entry {
    std::string name;
    std::string relative;
    bool isDir = false;
    std::vector<Entry> children;
};

struct ScanResult {
    bool ok = false;
    std::string error;
    std::string warning;
    std::vector<Entry> roots;
    int fileCount = 0;
    int directoryCount = 0;
};

// 扁平化之后的可见行，供虚拟列表按索引渲染。
struct Row {
    std::string name;
    std::string relative;
    int depth = 0;
    bool isDir = false;
    bool expanded = false;
};

// 始终列出目录下全部常规文件；不再有“常见文本”过滤模式。
ScanResult scan(const std::string& root);

// 按展开状态展开目录，得到当前可见行。
void flatten(const ScanResult& result, const std::set<std::string>& expanded, std::vector<Row>& rows);

// 过滤时改为扁平的匹配列表，匹配对象是相对路径。
void flattenFiltered(const ScanResult& result, const std::string& filter, std::vector<Row>& rows);

} // namespace neo::vault
