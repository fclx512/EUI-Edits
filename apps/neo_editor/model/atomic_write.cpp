#include "model/atomic_write.h"

#include <atomic>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace neo::atomicwrite {
namespace {

namespace fs = std::filesystem;

std::atomic<bool> g_failBeforeReplace{false};

unsigned long long processTag() {
#if defined(_WIN32)
    return static_cast<unsigned long long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long long>(::getpid());
#endif
}

// 临时名 = 目标名 + ".neo-tmp-<pid>-<计数>"。
// 固定 ".neo-tmp" 在"两个编辑器实例共享同一配置目录"时会让两路写入共用一个
// 临时文件、互相截断对方的半成品；pid+计数保证同机并发写同一目标各写各的，
// 谁后替换谁生效，内容永远不会串成两份的混合。
// 崩溃留下的旧临时文件是惰性的：pid 复用时会被直接 trunc 复用，否则只是旁边
// 一个残留，不影响下一次写入（tests/unit/settings_atomic.cpp 覆盖这两种情况）。
fs::path temporaryPathFor(const fs::path& target) {
    static const unsigned long long tag = processTag();
    static std::atomic<unsigned long long> counter{0};
    fs::path temporary = target;
    temporary += ".neo-tmp-" + std::to_string(tag) + "-" +
                 std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    return temporary;
}

void removeQuietly(const fs::path& path) {
    std::error_code error;
    fs::remove(path, error);
}

bool replaceTarget(const fs::path& temporary, const fs::path& target) {
#if defined(_WIN32)
    // Windows 上"目标已存在"时 std::filesystem::rename 的行为依实现而异：
    // MSVC STL 走 MoveFileExW 的替换语义，MinGW libstdc++ 则落到 CRT rename，
    // 目标存在时直接失败（text_file.cpp 里那个兼容兜底就是被它逼出来的）。
    // 这里统一用 MoveFileExW：REPLACE_EXISTING 原子替换已存在目标，
    // WRITE_THROUGH 保证返回前替换已经落盘。两个参数都是 textfile::pathFromUtf8
    // 产出的原生宽字符路径，c_str() 直接可用，不引入任何编码转换。
    return ::MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    // POSIX rename：目标存在时原子替换。
    std::error_code error;
    fs::rename(temporary, target, error);
    return !error;
#endif
}

} // namespace

bool writeFile(const fs::path& target, std::string_view content, const std::function<bool()>& beforeReplace) {
    const fs::path temporary = temporaryPathFor(target);
    const auto abandon = [&temporary]() {
        removeQuietly(temporary);
        return false;
    };

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return abandon();
        }
        if (!content.empty()) {
            output.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        output.flush();
        if (!output.good()) {
            output.close();
            return abandon();
        }
        // 显式 close 并检查状态：把缓冲交出去时的错误不能像析构那样被吞掉。
        output.close();
        if (output.fail()) {
            return abandon();
        }
    }

    if (g_failBeforeReplace.load(std::memory_order_relaxed)) {
        return abandon();
    }
    if (beforeReplace && !beforeReplace()) return abandon();
    if (!replaceTarget(temporary, target)) {
        return abandon();
    }
    return true;
}

void testing::failBeforeReplace(bool enabled) {
    g_failBeforeReplace.store(enabled, std::memory_order_relaxed);
}

} // namespace neo::atomicwrite
