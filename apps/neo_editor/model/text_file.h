#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

namespace neo::textfile {

enum class LineEnding { Lf, CrLf };

// 原文件的编码。内存中的文档永远是 UTF-8 + LF，这里记住编码特征，保存时严格还原。
enum class Encoding {
    Utf8,
    Utf16Le,
    Utf16Be,
    // Windows ANSI 代码页（具体页码在加载时固化为数字，保存时不再查询系统，
    // 避免"加载后用户改了系统区域设置再保存"这种漂移）。仅 Windows 支持。
    Ansi,
};

struct Document {
    std::string text;
    bool hadBom = false;
    LineEnding lineEnding = LineEnding::Lf;
    Encoding encoding = Encoding::Utf8;
    unsigned int ansiCodePage = 0;  // encoding == Ansi 时有效
};

// "以指定编码重新打开"用的强制编码说明。encoding == Ansi 时 codePage 必填
// （936 = GBK、950 = Big5、932 = Shift-JIS）。
struct ForcedEncoding {
    Encoding encoding = Encoding::Utf8;
    unsigned int codePage = 0;
};

struct LoadResult {
    bool ok = false;
    std::string error;
    Document document;
};

struct Stats {
    int lines = 1;
    int characters = 0;
};

// 读取文本文件：识别 UTF-8/UTF-16 BOM，Windows 上把非 UTF-8 内容按本地 ANSI
// 代码页（加载时固化页码）解码，统一转换为 UTF-8 + LF。
// 严格性：UTF-16 奇数字节 / 孤立代理对直接报错，不静默替换。
// 超过 maxBytes 或疑似二进制时失败。
LoadResult load(const std::string& path, std::size_t maxBytes = 64u * 1024u * 1024u);

// 按指定编码读取（"以编码重新打开"）。与 BOM 冲突（如对带 UTF-16 BOM 的文件
// 强制 UTF-8）时报错；无 BOM 强制 UTF-16 可读，保存时也按无 BOM 还原。
// 强制 Ansi 使用严格解码（MB_ERR_INVALID_CHARS），解码不出即报错。
LoadResult loadWithEncoding(const std::string& path, const ForcedEncoding& forced,
                            std::size_t maxBytes = 64u * 1024u * 1024u);

// 写入文本文件：按 document.encoding 严格还原字节（UTF-8/UTF-16 BOM、ANSI 代码页），
// Ansi 编码遇到无法表示的字符（如 GBK 里的 emoji）时整体失败、旧文件不动，
// 不做默认字符替换。先在内存完成全部编码，再走原子替换。
bool save(const std::string& path, const Document& document, std::string& error,
          const std::function<bool()>& beforeReplace = {});

// 状态栏/菜单共用的编码显示名。Ansi 只把已知页码叫 GBK/Big5/Shift-JIS，
// 其余页码显示 "ANSI (N)"，不冒认。
std::string encodingLabel(const Document& document);

bool isMarkdown(const std::string& path);
std::string fileName(const std::string& path);
std::string parentPath(const std::string& path);
std::string extensionLower(const std::string& path);

Stats measure(const std::string& text);

// 路径统一按 UTF-8 std::string 传递；在 Windows 上经由宽字符转换，中文路径才不会乱码。
std::string pathToUtf8(const std::filesystem::path& value);
std::filesystem::path pathFromUtf8(const std::string& value);

// 读取"值本身是路径"的环境变量（APPDATA / LOCALAPPDATA / WINDIR / HOME…）并返回 UTF-8。
//
// 不能直接用 std::getenv：它返回的是 **ANSI 代码页**（本机 GBK）字节，把这种字节当
// UTF-8 交给 pathFromUtf8（= std::filesystem::u8path）会因非法序列抛异常 ——
// 中文用户名的机器（C:\Users\张三\AppData\Roaming）会在启动读配置时直接崩掉。
// Windows 上这里走 GetEnvironmentVariableW，从宽字符转 UTF-8；POSIX 上 getenv 本来
// 就是 UTF-8，行为不变。
std::string environmentPathUtf8(const char* name);

} // namespace neo::textfile
