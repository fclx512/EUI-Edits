#if defined(_WIN32)
// 只是读环境变量，getenv 的"不安全"提示在这个用法下没有意义（同 model/settings.cpp）。
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "model/font_catalog.h"
#include "model/text_file.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace neo::fonts {
namespace {

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

void appendUtf8(std::string& out, uint32_t code) {
    if (code < 0x80u) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (code >> 6)));
        out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    } else if (code < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (code >> 12)));
        out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (code >> 18)));
        out.push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    }
}

// name 表的字符串编码：Windows/Unicode 平台是 UTF-16BE，Mac 平台是 Mac Roman。
// 拉丁字体名在 Mac Roman 里就是 ASCII，够用；非 ASCII 的 Mac 记录会被 Windows 记录压过去。
std::string decodeName(const uint8_t* data, std::size_t size, uint16_t platformId) {
    std::string out;
    out.reserve(size);
    if (platformId == 0 || platformId == 3) {
        for (std::size_t i = 0; i + 1 < size; i += 2) {
            appendUtf8(out, (static_cast<uint32_t>(data[i]) << 8) | data[i + 1]);
        }
    } else {
        for (std::size_t i = 0; i < size; ++i) {
            const uint8_t c = data[i];
            appendUtf8(out, c < 0x80u ? c : 0xFFFDu);
        }
    }
    return out;
}

// 只读文件头部区域，绝不整文件读入：字体动辄 10~30MB。
// 构造必须收 fs::path（宽字符）：窄串 ifstream 会走 ANSI 代码页，
// 中文路径打不开、非法序列还会直接抛 system_error。
class FileSlice {
public:
    explicit FileSlice(const std::filesystem::path& path) : in_(path, std::ios::binary) {
        if (!in_) {
            return;
        }
        in_.seekg(0, std::ios::end);
        const std::streamoff end = in_.tellg();
        if (end < 0) {
            return;
        }
        size_ = static_cast<std::uint64_t>(end);
        in_.clear();
        ok_ = true;
    }

    bool ok() const { return ok_; }
    std::uint64_t size() const { return size_; }

    bool read(std::uint64_t offset, std::size_t count, std::vector<uint8_t>& out) {
        if (!ok_ || offset > size_ || count > size_ - offset ||
            offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
            count > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
            return false;
        }
        out.assign(count, 0);
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        in_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(count));
        return static_cast<std::size_t>(in_.gcount()) == count;
    }

private:
    std::ifstream in_;
    bool ok_ = false;
    std::uint64_t size_ = 0;
};

struct NameRecord {
    std::string text;
    int score = -1;
};

void consider(NameRecord& target, int score, std::string text) {
    if (score > target.score && !text.empty()) {
        target.score = score;
        target.text = std::move(text);
    }
}

struct TableRange {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

struct FaceMetadata {
    std::string family;
    bool monospaceHint = false;
    int weight = 400;
    bool italic = false;
};

// 解析 face 0 的 name、OS/2 与 post 表。表目录和表体均只做 FileSlice 有界读取，
// 字体枚举阶段不把任何字体交给 FreeType。
FaceMetadata faceMetadata(const std::filesystem::path& path) {
    FaceMetadata metadata;
    FileSlice file(path);
    std::vector<uint8_t> head;
    if (!file.read(0, 12, head)) {
        return metadata;
    }

    std::uint64_t base = 0;
    if (head[0] == 't' && head[1] == 't' && head[2] == 'c' && head[3] == 'f') {
        // TTC：框架只取 face 0，这里也只解析 face 0，保持一致。
        std::vector<uint8_t> countBytes;
        if (!file.read(8, 4, countBytes)) {
            return metadata;
        }
        std::vector<uint8_t> firstOffset;
        if (be32(countBytes.data()) == 0 || !file.read(12, 4, firstOffset)) {
            return metadata;
        }
        base = be32(firstOffset.data());
        if (!file.read(base, 12, head)) {
            return metadata;
        }
    }

    const uint16_t tableCount = be16(&head[4]);
    if (tableCount == 0 || tableCount > 512) {
        return metadata;
    }
    std::vector<uint8_t> directory;
    if (!file.read(base + 12, static_cast<std::size_t>(tableCount) * 16u, directory)) {
        return metadata;
    }

    std::map<std::string, TableRange> tables;
    for (uint16_t i = 0; i < tableCount; ++i) {
        const uint8_t* record = &directory[static_cast<std::size_t>(i) * 16u];
        const std::string tag(reinterpret_cast<const char*>(record), 4u);
        if (tag == "name" || tag == "OS/2" || tag == "post") {
            const std::uint64_t offset = be32(record + 8);
            const std::uint64_t length = be32(record + 12);
            if (offset <= file.size() && length <= file.size() - offset) {
                tables[tag] = {offset, length};
            }
        }
    }

    if (const auto found = tables.find("OS/2"); found != tables.end() && found->second.length >= 6u) {
        std::vector<uint8_t> os2Weight;
        if (file.read(found->second.offset + 4u, 2u, os2Weight)) {
            const unsigned weight = be16(os2Weight.data());
            if (weight >= 1u && weight <= 1000u) {
                metadata.weight = static_cast<int>(weight);
            }
        }
    }
    if (const auto found = tables.find("post"); found != tables.end() && found->second.length >= 16u) {
        std::vector<uint8_t> post;
        if (file.read(found->second.offset, 16u, post)) {
            metadata.italic = be32(post.data() + 4u) != 0u;
            metadata.monospaceHint = be32(post.data() + 12u) != 0u;
        }
    }

    const auto nameTable = tables.find("name");
    if (nameTable == tables.end() || nameTable->second.length < 6u ||
        nameTable->second.length > (1u << 20)) {
        return metadata;
    }

    std::vector<uint8_t> name;
    if (!file.read(nameTable->second.offset, static_cast<std::size_t>(nameTable->second.length), name)) {
        return metadata;
    }

    const uint16_t recordCount = be16(&name[2]);
    const uint16_t stringOffset = be16(&name[4]);
    if (recordCount == 0 || name.size() < 6u + static_cast<std::size_t>(recordCount) * 12u) {
        return metadata;
    }

    const uint8_t* strings = name.data() + std::min<std::size_t>(name.size(), stringOffset);
    const std::size_t stringsAvailable = name.size() > stringOffset ? name.size() - stringOffset : 0;

    NameRecord family;
    NameRecord typographic;
    for (uint16_t i = 0; i < recordCount; ++i) {
        const uint8_t* record = &name[6u + static_cast<std::size_t>(i) * 12u];
        const uint16_t platformId = be16(record + 0);
        const uint16_t encodingId = be16(record + 2);
        const uint16_t languageId = be16(record + 4);
        const uint16_t nameId = be16(record + 6);
        const uint16_t length = be16(record + 8);
        const uint16_t offset = be16(record + 10);
        if (nameId != 1 && nameId != 16) {
            continue;
        }
        if (static_cast<std::size_t>(offset) + length > stringsAvailable) {
            continue;
        }
        // 评分只用来在多个平台/语言记录里挑一个最好的：Windows 英文 > Unicode > Mac。
        int score = 0;
        if (platformId == 3) {
            score = 3;
        } else if (platformId == 0) {
            score = 2;
        } else {
            score = 1;
        }
        if (languageId == 0x0409) {
            score += 1;
        }
        if (platformId == 3 && encodingId != 1 && encodingId != 10) {
            score -= 1;
        }
        NameRecord& target = (nameId == 16) ? typographic : family;
        consider(target, score, decodeName(strings + offset, length, platformId));
    }

    metadata.family = !typographic.text.empty() ? typographic.text : family.text;
    return metadata;
}

std::string fileStem(const std::string& path) {
    return textfile::pathToUtf8(textfile::pathFromUtf8(path).stem());
}

// 常用字体置顶。命中就排到列表最前面，省得在 200 多个字体里翻。
bool isCommonFont(const std::string& stemLower, const std::string& nameLower) {
    static const std::set<std::string> kCommonStems = {
        "msyh", "simsun", "simhei", "simfang", "simkai", "deng", "consola",
        "cascadiacode", "cascadiamono", "segoeui",
    };
    if (kCommonStems.count(stemLower) != 0) {
        return true;
    }
    // 英文族名用**相等**判断：早先写成子串匹配，于是 "Segoe UI Emoji/Historic/
    // Symbol/Variable" 全被当成"Segoe UI"挤进了置顶区。中文名仍用子串，
    // 因为同一族的中文名在不同字体里写法不一（"微软雅黑" / "Microsoft YaHei UI"）。
    static const std::set<std::string> kCommonNames = {"microsoft yahei", "segoe ui", "consolas"};
    if (kCommonNames.count(nameLower) != 0) {
        return true;
    }
    static const char* kCommonSubstrings[] = {"微软雅黑", "等线", "楷体", "仿宋", "宋体", "黑体"};
    for (const char* needle : kCommonSubstrings) {
        if (nameLower.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> fontRoots() {
    std::vector<std::string> roots;
    // 路径类环境变量统一走 environmentPathUtf8：std::getenv 给的是 ANSI/GBK 字节，
    // 当 UTF-8 用会在后面的 pathFromUtf8 处抛异常 —— 而本函数在静态初始化里跑。
    const std::string windir = textfile::environmentPathUtf8("WINDIR");
    roots.push_back(windir.empty() ? std::string("C:\\Windows\\Fonts") : windir + "\\Fonts");
    const std::string local = textfile::environmentPathUtf8("LOCALAPPDATA");
    if (!local.empty()) {
        // 用户自己装的字体不在这里就会漏掉一半。
        roots.push_back(local + "\\Microsoft\\Windows\\Fonts");
    }
    return roots;
}

} // namespace

const std::vector<Entry>& catalog() {
    static const std::vector<Entry> entries = [] {
        std::vector<Entry> result;
        std::error_code rootError;
        for (const std::string& root : fontRoots()) {
            // root 是 environmentPathUtf8 给的 UTF-8 串。非法路径只丢这一条根，
            // 绝不让异常把静态初始化炸掉（那等于启动即崩）。
            std::filesystem::path rootPath;
            try {
                rootPath = textfile::pathFromUtf8(root);
            } catch (...) {
                continue;
            }
            std::filesystem::directory_iterator iterator(rootPath, rootError);
            if (rootError) {
                rootError.clear();
                continue;
            }
            for (const std::filesystem::directory_entry& item : iterator) {
                std::error_code itemError;
                if (!item.is_regular_file(itemError) || itemError) {
                    continue;
                }
                const std::string extension = toLower(textfile::pathToUtf8(item.path().extension()));
                if (extension != ".ttf" && extension != ".otf" && extension != ".ttc") {
                    continue;
                }
                // entry.path 统一存 UTF-8：.string() 会按 ANSI 代码页转码，遇到
                // 代码页表示不了的字体名直接抛 system_error（静态初始化，必崩）。
                Entry entry;
                entry.path = textfile::pathToUtf8(item.path());
                const FaceMetadata metadata = faceMetadata(textfile::pathFromUtf8(entry.path));
                entry.displayName = metadata.family;
                if (entry.displayName.empty()) {
                    entry.displayName = fileStem(entry.path);
                }
                entry.rank = isCommonFont(toLower(fileStem(entry.path)), toLower(entry.displayName)) ? 0 : 3;
                entry.monospaceHint = metadata.monospaceHint;
                entry.weight = metadata.weight;
                entry.italic = metadata.italic;
                result.push_back(std::move(entry));
            }
        }

        // 同一个字体族只留一个文件：常规 400 字重优先，其次取最接近 400 的字重，
        // 再优先非斜体，避免路径字典序让 Bold/Italic 抢在 Regular 前面。
        std::sort(result.begin(), result.end(), [](const Entry& left, const Entry& right) {
            const std::string leftName = toLower(left.displayName);
            const std::string rightName = toLower(right.displayName);
            if (leftName != rightName) {
                return leftName < rightName;
            }
            const bool leftRegular = left.weight == 400 && !left.italic;
            const bool rightRegular = right.weight == 400 && !right.italic;
            if (leftRegular != rightRegular) {
                return leftRegular;
            }
            const int leftDistance = std::abs(left.weight - 400);
            const int rightDistance = std::abs(right.weight - 400);
            if (leftDistance != rightDistance) {
                return leftDistance < rightDistance;
            }
            if (left.italic != right.italic) {
                return !left.italic;
            }
            return left.path < right.path;
        });
        std::vector<Entry> deduped;
        std::set<std::string> seenNames;
        for (Entry& entry : result) {
            const std::string key = toLower(entry.displayName);
            if (!seenNames.insert(key).second) {
                continue;
            }
            deduped.push_back(std::move(entry));
        }

        std::sort(deduped.begin(), deduped.end(), [](const Entry& left, const Entry& right) {
            if (left.rank != right.rank) {
                return left.rank < right.rank;
            }
            return left.displayName < right.displayName;
        });
        return deduped;
    }();
    return entries;
}

std::string displayNameFor(const std::string& path) {
    if (path.empty()) {
        return {};
    }
    const std::string target = toLower(path);
    for (const Entry& entry : catalog()) {
        if (toLower(entry.path) == target) {
            return entry.displayName;
        }
    }
    return fileStem(path);
}

} // namespace neo::fonts
