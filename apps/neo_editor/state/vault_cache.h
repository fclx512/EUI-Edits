#pragma once

// 按根共享的文档库扫描缓存（执行计划阶段 B）。
//
// 目标：切页不再同步整库扫描；同根的多个标签共用一份只读扫描快照；目录事件到来时
// 后台重扫，完成后原子发布。每页仍各自保存 expanded/filter/selection/scroll，rows 是
// 每页的视图投影，绝不把一页的筛选结果共享给另一页。
//
// 生命周期：worker 只拿到拥有根字符串的不可变输入，产出 ScanResult；完成回调在**主线程**
// （core::async::dispatchReady）里发布，只有根身份仍匹配时才替换快照。快照是
// shared_ptr<const ScanResult>，被页/投影捕获时按引用计数记账。

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

// 同步扫描并发布（首次无快照时用于避免空白，或测试用）。返回代次。
std::uint64_t requestScanSync(const std::string& root);

// 丢弃某根的缓存（换根、根被删除）。
void invalidate(const std::string& root);
void clear();

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
