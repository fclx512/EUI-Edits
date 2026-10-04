#pragma once

// 串行、后台的会话恢复写协调器（执行计划阶段 E）。
//
// 问题：旧实现每次切页在 UI 线程同步 sessionstorage::write，把所有脏页正文整篇写盘
// （32MiB 脏稿约 23ms），切页因此卡顿；而且多个请求并行时旧快照可能覆盖新快照。
//
// 设计：
//   · 只有一个 writer 真正执行磁盘事务；队列上限 = 一个 in-flight + 一个"最新待提交"。
//   · 协调器把提交转成**拥有的不可变快照**（dirty 正文用 shared_ptr<const Document>），
//     worker 只读这份快照，绝不捕获 AppState / DocumentSession / 可变 string。
//   · 正文复用：某页 (TabId, revision) 与最近一次已拥有快照相同 → 直接复用 shared_ptr，
//     不重复复制整篇正文。
//   · 提交顺序由"同一时刻只有一个在飞 + 完成后才起下一个"保证，单调 requestSeq/committedSeq；
//     仅 commit 成功才推进 committedSeq。superseded 的待提交请求被更新的请求替换（只保留最新）。
//   · 入队成功 ≠ 持久化成功：submit 返回请求号/是否入队，持久化以 committedSeq 为准。

#include "model/session_storage.h"
#include "model/text_file.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace neo::sessionwriter {

struct Record {
    sessionstorage::TabId id = 0;
    std::string path;
    std::string vaultRoot;
    std::string language;
    int wrapOverride = -1;
    bool dirty = false;
    unsigned long long revision = 0;                 // 正文代次：判"内容变没变"
    const textfile::Document* document = nullptr;    // dirty 时非空
};

struct Submission {
    std::vector<Record> records;
    sessionstorage::TabId activeId = 0;
};

// 提交一次会话写请求（非阻塞）。返回请求序号（0 = 拒绝）。最新的待提交请求会替换
// 尚未开始的旧请求；在飞的请求不受影响。
std::uint64_t submit(Submission submission);

// 最近一次**已提交**（manifest commit 成功）的请求序号。
std::uint64_t committedSeq();
bool pending();
std::uint64_t lastSeq();

// 页关闭 / ID 释放：丢弃该页的正文快照缓存，避免 Id 复用后误复用旧正文。
void forgetTab(sessionstorage::TabId id);

// 退出屏障：提交并等待到 committedSeq >= 该请求，或超时。只在退出/关闭路径调用
// （帧循环已停时它自己 pump dispatchReady）。返回是否在超时前确认提交。
bool checkpointBlocking(Submission submission, int timeoutMs = 5000);

// Begin normal-close finalization without blocking the UI thread. The writer
// rejects new submissions and drops its pending snapshot immediately. Once an
// in-flight write has finished, it clears the persisted session on the async
// worker and calls back on the main thread. On clear failure, submissions are
// accepted again so the caller can restore the current recovery snapshot.
using DiscardCallback = std::function<void(bool ok)>;
bool beginDiscardOnClose(DiscardCallback callback);

// commit 回调（主线程调用）：ok = 该次事务是否提交成功。
using CommitCallback = std::function<void(bool ok, std::uint64_t seq)>;
void setCommitCallback(CommitCallback callback);

struct Stats {
    std::uint64_t submitCount = 0;
    std::uint64_t writeCount = 0;
    std::uint64_t committedCount = 0;
    std::uint64_t failedCount = 0;
    std::uint64_t supersededCount = 0;
    std::uint64_t bodyCopies = 0;     // 实际深拷贝正文的次数（复用则不计）
    std::uint64_t bodyReuses = 0;
    std::uint64_t retainedBodyBytes = 0; // 当前 lastDocs 去重缓存估算；不含在飞/待提交快照
};
Stats stats();

// ── 测试接线 ────────────────────────────────────────────────────────────────
// 替换底层 writer（默认 sessionstorage::write）。测试用它统计写次数/字节。
using Writer = std::function<bool(const std::vector<sessionstorage::WriteRecord>&, sessionstorage::TabId)>;
void setWriterForTest(Writer writer);
// 内联执行（不派发 worker）：让旧同步测试保持确定性，同时走同一套快照/顺序逻辑。
void setInlineForTest(bool inlineMode);
void resetForTest();

} // namespace neo::sessionwriter
