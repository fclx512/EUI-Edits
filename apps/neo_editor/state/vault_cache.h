#pragma once

// 按根共享的文档库分层分页缓存。
//
// 同根的多个标签共用已加载快照；后台每次至多读取一个目录的 500 个直属条目。
// 每页仍各自保存 expanded/filter/selection/scroll，rows 是
// 每页的视图投影，绝不把一页的筛选结果共享给另一页。
//
// 生命周期：worker 读取目录迭代游标，完成回调在主线程发布新快照；
// 取消/切根时关闭游标并丢弃过期结果。

#include "model/vault.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace neo::vaultcache {

struct ScanView {
    std::shared_ptr<const vault::ScanResult> scan;  // 当前已发布快照（可能为空）
    std::uint64_t generation = 0;                   // 快照代次；0 = 从无快照
    bool scanning = false;                          // 同根是否有扫描在飞
    bool rescanPending = false;                     // 在飞期间又来过变化
    bool loadingPages = false;                      // 子目录或续页读取在飞
};

// 查询某根当前快照，不触发扫描。
ScanView query(const std::string& root);

// 请求扫描。同根已有扫描在飞时只置"需要重扫"标记并返回 false；否则调度一次后台扫描
// 并返回 true。返回 epoch（自增请求号，供诊断/发布校验用）。
struct RequestResult {
    bool scheduled = false;
    std::uint64_t epoch = 0;
};
RequestResult requestScan(const std::string& root);

// 异步读取目录的首批或下一批。relative 为空表示根；refresh=true 丢弃该层
// 既有页并从头读取（文件操作后使用）。同层在飞请求去重。
RequestResult requestPage(const std::string& root, const std::string& relative,
                          bool refresh = false);

// 仅测试/兼容调用：同步读取根的首批直属条目并发布。产品路径不调用。
std::uint64_t requestScanSync(const std::string& root);

// 丢弃某根的缓存（换根、根被删除）。
void invalidate(const std::string& root);
void clear();
// 激活浏览根：取消其他根在飞扫描，防止快切时旧任务继续遍历。
void activateRoot(const std::string& root);
void setExpanded(const std::string& root, const std::set<std::string>& expanded);

// LRU 修剪：保留活动根，从最久未用的非活动根开始淘汰，直到根数/估算字节回到预算内。
void prune(const std::string& activeRoot);

// 预算（可在运行时由 F 阶段调整）。
std::size_t rootBudgetBytes();
void setRootBudgetBytes(std::size_t bytes);
std::size_t rootCountLimit();
void setRootCountLimit(std::size_t count);

struct Stats {
    std::size_t cachedRoots = 0;
    std::size_t estimatedBytes = 0;
    std::uint64_t scansCompleted = 0;
    std::uint64_t scanRequests = 0;
    std::uint64_t scansDropped = 0;   // 完成时根身份已不匹配而被丢弃
};
Stats stats();

// 规范化根身份：Windows 下大小写折叠 + 词法规范，保证同一物理根只占一条缓存。
std::string rootKey(const std::string& root);

// 估算一份扫描快照的驻留字节（含条目字符串与容器开销），用于预算计账。
std::size_t estimateBytes(const vault::ScanResult& result);

// ── 测试接线 ────────────────────────────────────────────────────────────────
// 注入扫描实现（默认调用 vault::scan）。测试用它统计 scanCount 并制造确定的顺序。
using Scanner = std::function<vault::ScanResult(const std::string& root)>;
void setScannerForTest(Scanner scanner);
void resetScannerForTest();

// 清空所有在飞任务状态（测试用）。
void resetForTest();

} // namespace neo::vaultcache
