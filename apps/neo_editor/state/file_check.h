#pragma once

// 干净页的后台文件核验（执行计划阶段 D）。
//
// 切到干净页时不再在主线程跑 filesafety::load（inspect→load→inspect 三遍全文读取）。
// 改为：立即显示已缓存正文 + 发一个后台核验请求。worker 先做一次 inspect（内容双哈希），
// 指纹与请求时记录的基准一致就不解码；不一致才走可靠 load。结果回 UI 后由调用方校验
// 页面身份/代次/干净状态，再决定是否替换正文。
//
// 元数据（大小/时间戳）只作为快路径，不作为永久跳过核验的依据 —— inspect 本身就哈希
// 全文，同尺寸同时间戳的外部修改同样会被发现。核验结果绝不是保存授权。

#include "model/file_safety.h"
#include "model/text_file.h"

#include <cstdint>
#include <functional>
#include <string>

namespace neo::filecheck {

struct Request {
    std::uint64_t requestId = 0;            // 调用方给的请求号（诊断关联用）
    std::string path;                       // worker 拥有的路径副本
    filesafety::Fingerprint baseline;       // 请求时页面记录的磁盘指纹
};

enum class Status {
    Unchanged,   // 指纹与基准一致，未解码
    Changed,     // 内容变了（或基准不可比），loaded 里是可靠加载的新内容
    Missing,     // 文件不存在
    Failed,      // 读取/解码失败（loaded.error / error 说明）
    Canceled     // 被更新的请求取代
};

struct Result {
    std::uint64_t requestId = 0;
    Status status = Status::Canceled;
    filesafety::Fingerprint observed;
    textfile::LoadResult loaded;
    std::string error;
};

using Completion = std::function<void(Result)>;

// 调度一次后台核验。key 建议按页（"neo.file.check.<tabId>"）——同页只保留最新
// 一次 in-flight，旧请求被 restart 取代后其回调不再触发（结果丢弃）。
void request(const std::string& key, Request request, Completion completion);

// 取消某 key 的在飞请求（页关闭时）。
void cancel(const std::string& key);

// 测试注入：替换核验实现，统计调用次数。设为空则恢复默认（真实 inspect/load）。
using Verifier = std::function<Result(const Request&)>;
void setVerifierForTest(Verifier verifier);
void resetForTest();

} // namespace neo::filecheck
