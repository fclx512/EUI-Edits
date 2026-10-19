#pragma once

#include <set>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace neo::vault {
struct PageCursor;

// 目录树节点。relative 是相对库根的路径，统一使用 '/' 分隔。
struct Entry {
    std::string name;
    std::string relative;
    bool isDir = false;
    std::vector<Entry> children;
    bool childrenLoaded = false;
    bool linkLeaf = false;
    bool hasMore = false;
    bool cacheLimitReached = false;
    std::string pageError;
    std::shared_ptr<PageCursor> cursor;
};

struct ScanResult {
    bool ok = false;
    std::string error;
    std::string warning;
    std::vector<Entry> roots;
    int fileCount = 0;       // 此层已加载的直属文件数，不含子目录内部
    int directoryCount = 0;  // 此层已加载的直属目录数
    bool hasMore = false;
    bool cancelled = false;
    bool cacheLimitReached = false;
    std::string pageError;
    std::shared_ptr<PageCursor> cursor;
};

// 扁平化之后的可见行，供虚拟列表按索引渲染。
struct Row {
    std::string name;
    std::string relative;
    int depth = 0;
    bool isDir = false;
    bool expanded = false;
    bool loadMore = false;
    bool cacheLimitReached = false;
    std::string pageError;
};

constexpr std::size_t kPageSize = 500;

// 只读取根的第一批直属条目，不进入子目录。
ScanResult scan(const std::string& root);

// 从保留的枚举游标读取下一批，最多查看 500 个直属条目；批内排序。
// cursor 为空表示从头打开该层。调用者同一时刻只可在一个 worker 使用游标。
ScanResult scanPage(const std::string& root, const std::string& relative,
                    std::shared_ptr<PageCursor> cursor = {},
                    const std::function<bool()>& cancelled = {});
// 释放快照持有的目录枚举句柄；旧不可变快照可继续展示已加载行。
void closeCursors(const ScanResult& result);
void closeCursors(const Entry& entry);
void closeCursor(const std::shared_ptr<PageCursor>& cursor);

// 按展开状态展开目录，得到当前可见行。
void flatten(const ScanResult& result, const std::set<std::string>& expanded, std::vector<Row>& rows);

// 过滤时改为扁平的匹配列表，匹配对象是相对路径。
void flattenFiltered(const ScanResult& result, const std::string& filter, std::vector<Row>& rows);

} // namespace neo::vault
