#pragma once

// 标签/切页诊断 trace。显式启用、默认关闭：只有环境变量 NEO_TABS_TRACE 指向一个
// 可写文件时才记录。记录的是 QPC 微秒时间戳 + 线程号 + 阶段名 + 少量键值，用于把
// 一次切页拆成"扫描/核验/恢复写/计划/装饰/排版/呈现"各段并归因。
//
// 边界（与执行计划 §4 一致）：
//   · 正文与完整用户路径不入 trace，只记阶段、耗时、字节数和身份号；
//   · 内存里只保留有界待写队列，不逐条同步 flush 磁盘，避免诊断本身制造新内存问题；
//   · 只提供**应用进程内**的分段计时。跨进程输入关联（SendInput dwExtraInfo /
//     GetMessageExtraInfo）未接线：可见时延由 Python 探针按截图轮询另测，二者分开报告。

#include <cstdint>
#include <string>

namespace neo::tracelog {

// NEO_TABS_TRACE 未设置或文件打不开 → false，全部记录调用变成廉价空操作。
bool enabled();

// 当前进程内自增的请求号。同一次切页/打开/核验动作共用一个 requestId。
std::uint64_t nextRequestId();

// QPC 微秒（不可跨进程直接比较；探针侧另记自己的 QPC 频率与值）。
std::uint64_t nowMicros();

// 自增的帧序号（每次 compose 取一个），用于把同一帧的分段归到一起。
std::uint64_t nextFrameSequence();

// 切页请求的应用侧打点：document_tabs.cpp 受理 Ctrl+Tab / 点击标签时调用；app.cpp 的
// compose 用 switchRequestMicros() 折算"请求受理 → 下一帧开始"的排队间隔，读后调用
// clearSwitchRequest() 消费掉，保证只有切页后的第一帧带 gap。未打点时为 0。
void markSwitchRequest();
std::uint64_t switchRequestMicros();
void clearSwitchRequest();

// 追加一条记录：qpc_us,thread,requestId,stage,detail
void event(const char* stage, std::uint64_t requestId = 0, const std::string& detail = {});

// 作用域计时：析构时记一条 <stage> 事件，detail 里带 elapsed_us。
class Span {
public:
    explicit Span(const char* stage, std::uint64_t requestId = 0);
    ~Span();
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;
    void note(const std::string& detail) { detail_ = detail; }
    std::uint64_t elapsedMicros() const;

private:
    const char* stage_;
    std::uint64_t requestId_;
    std::uint64_t begin_;
    std::string detail_;
};

// 把内存里待写的记录落盘（进程退出前、或探针要求对齐时调用）。有界队列满时会自动落盘。
void flush();

} // namespace neo::tracelog
