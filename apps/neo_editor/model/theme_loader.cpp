#include "model/i18n.h"
#include "model/theme_loader.h"
#include "model/obsidian_theme.h"

#include "model/text_file.h"  // pathFromUtf8（UTF-8 → 原生路径的唯一入口）

#include "eui/json.h"  // 仓库自带的 JSON 解析（yyjson 封装，不引第三方依赖）

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>

namespace neo::themeloader {
namespace {

namespace fs = std::filesystem;

// 数值字段的合法区间（越界当错误报，不当"截断"处理：主题文件是人写的，
// 静默截断会让作者以为改生效了）。
struct NumberField {
    const char* name;
    std::optional<float> ThemeTypography::* member;
    float minimum;
    float maximum;
};

const NumberField kNumberFields[] = {
    {"bodyLineHeight", &ThemeTypography::bodyLineHeight, 0.5f, 4.0f},
    {"blockGap", &ThemeTypography::blockGap, 0.0f, 4.0f},
    {"listIndent", &ThemeTypography::listIndent, 0.0f, 8.0f},
    {"radius", &ThemeTypography::radius, 0.0f, 2.0f},
    {"codeSizeFactor", &ThemeTypography::codeSizeFactor, 0.5f, 2.0f},
};

const NumberField kV2NumberFields[] = {
    {"h1SizeFactor", &ThemeTypography::h1SizeFactor, 0.5f, 4.0f},
    {"h2SizeFactor", &ThemeTypography::h2SizeFactor, 0.5f, 4.0f},
    {"h3SizeFactor", &ThemeTypography::h3SizeFactor, 0.5f, 4.0f},
    {"h1LineHeightFactor", &ThemeTypography::h1LineHeightFactor, 1.0f, 4.0f},
    {"h2LineHeightFactor", &ThemeTypography::h2LineHeightFactor, 1.0f, 4.0f},
    {"h3LineHeightFactor", &ThemeTypography::h3LineHeightFactor, 1.0f, 4.0f},
    {"headingSpaceBefore", &ThemeTypography::headingSpaceBefore, 0.0f, 4.0f},
    {"tableSpaceBefore", &ThemeTypography::tableSpaceBefore, 0.0f, 4.0f},
};

struct FontField {
    const char* name;
    std::optional<std::string> ThemeTypography::* member;
};

const FontField kFontFields[] = {
    {"bodyFontFamily", &ThemeTypography::bodyFontFamily},
    {"codeFontFamily", &ThemeTypography::codeFontFamily},
};

int hexValue(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

float channel(int high, int low) {
    return static_cast<float>(high * 16 + low) / 255.0f;
}

// "#RRGGBB" / "#RRGGBBAA"（大小写不敏感，必须带 #）。
bool parseHexColor(const std::string& text, eui::Color& out) {
    if ((text.size() != 7 && text.size() != 9) || text[0] != '#') {
        return false;
    }
    const int digits = static_cast<int>(text.size()) - 1;
    int values[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int index = 0; index < digits; ++index) {
        values[index] = hexValue(text[static_cast<std::size_t>(index + 1)]);
        if (values[index] < 0) {
            return false;
        }
    }
    out = eui::Color{channel(values[0], values[1]), channel(values[2], values[3]),
                     channel(values[4], values[5]),
                     digits == 8 ? channel(values[6], values[7]) : 1.0f};
    return true;
}

// 单个颜色取值：字符串 hex，或 0..1 浮点的 [r,g,b] / [r,g,b,a] 数组。
bool parseColorValue(const eui::json::Value& value, eui::Color& out, std::string& error) {
    std::string text;
    if (value.string(text)) {
        if (parseHexColor(text, out)) {
            return true;
        }
        error = i18n::tr("theme.color_hex");
        return false;
    }
    if (value.type() == eui::json::Type::Array && (value.size() == 3 || value.size() == 4)) {
        double channels[4] = {0.0, 0.0, 0.0, 1.0};
        const std::size_t count = value.size();
        for (std::size_t index = 0; index < count; ++index) {
            if (!value.at(index).number(channels[index]) || !std::isfinite(channels[index]) ||
                channels[index] < 0.0 || channels[index] > 1.0) {
                error = i18n::tr("theme.color_component");
                return false;
            }
        }
        out = eui::Color{static_cast<float>(channels[0]), static_cast<float>(channels[1]),
                         static_cast<float>(channels[2]), static_cast<float>(channels[3])};
        return true;
    }
    error = i18n::tr("theme.color_value");
    return false;
}

bool parseTypography(const eui::json::Value& object, ThemeTypography& out, std::string& error,
                     bool v2 = false) {
    int present = 0;
    const auto parseNumbers = [&](const NumberField* fields, std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            const NumberField& field = fields[index];
            const eui::json::Value value = object.get(field.name);
            if (!value.valid()) continue;
            ++present;
            double parsed = 0.0;
            if (!value.number(parsed) || !std::isfinite(parsed) ||
                parsed < static_cast<double>(field.minimum) ||
                parsed > static_cast<double>(field.maximum)) {
                error = i18n::format("theme.number_range", {{"field", field.name},
                    {"min", std::to_string(field.minimum)}, {"max", std::to_string(field.maximum)}});
                return false;
            }
            out.*(field.member) = static_cast<float>(parsed);
        }
        return true;
    };
    if (!parseNumbers(kNumberFields, std::size(kNumberFields)) ||
        (v2 && !parseNumbers(kV2NumberFields, std::size(kV2NumberFields)))) {
        return false;
    }
    for (const FontField& field : kFontFields) {
        const eui::json::Value value = object.get(field.name);
        if (!value.valid()) {
            continue;
        }
        ++present;
        std::string text;
        if (!value.string(text)) {
            error = i18n::format("theme.font_string", {{"field", field.name}});
            return false;
        }
        // 空串 = 不覆盖（与导出侧"没有建议字体就写空串"对齐，来回读写不漂）。
        if (!text.empty()) {
            out.*(field.member) = text;
        }
    }
    const int known = static_cast<int>(std::size(kNumberFields)) +
        static_cast<int>(std::size(kFontFields)) +
        (v2 ? static_cast<int>(std::size(kV2NumberFields)) : 0);
    // 成员数 != 认识的字段数 ⇒ 对象里混着写错的字段名。v1 全集只有 `known` 个，
    // 认不出就报错而不是静默忽略 —— 静默会让作者以为改生效了。
    if (static_cast<int>(object.size()) != present) {
        error = i18n::format("theme.typography_unknown", {{"count", std::to_string(known)}});
        return false;
    }
    return true;
}

bool parseColors(const eui::json::Value& object, std::map<std::string, eui::Color>& out,
                 std::string& error) {
    if (object.type() != eui::json::Type::Object) {
        error = i18n::tr("theme.colors_object");
        return false;
    }
    int present = 0;
    for (const ThemeColorField& field : kThemeColorFields) {
        const eui::json::Value value = object.get(field.name);
        if (!value.valid()) continue;
        ++present;
        eui::Color color;
        std::string colorError;
        if (!parseColorValue(value, color, colorError)) {
            error = std::string("colors.") + field.name + " " + colorError;
            return false;
        }
        out[field.name] = color;
    }
    if (static_cast<int>(object.size()) != present) {
        error = i18n::tr("theme.colors_unknown");
        return false;
    }
    return true;
}

bool parseVariant(const eui::json::Value& object, ThemeVariant& out, std::string& error) {
    if (object.type() != eui::json::Type::Object) {
        error = i18n::tr("theme.sides_object");
        return false;
    }
    int present = 0;
    const eui::json::Value colors = object.get("colors");
    if (colors.valid()) {
        ++present;
        if (!parseColors(colors, out.colors, error)) return false;
    }
    const eui::json::Value typography = object.get("typography");
    if (typography.valid()) {
        ++present;
        if (typography.type() != eui::json::Type::Object ||
            !parseTypography(typography, out.typography, error, true)) {
            if (error.empty()) error = i18n::tr("theme.typography_object");
            return false;
        }
    }
    if (static_cast<int>(object.size()) != present) {
        error = i18n::tr("theme.sides_unknown");
        return false;
    }
    return true;
}

int byteOf(float value) {
    const float clamped = std::min(1.0f, std::max(0.0f, value));
    return static_cast<int>(std::lround(clamped * 255.0f));
}

std::string twoDigits(int value) {
    std::ostringstream out;
    out << std::hex << std::nouppercase << std::setfill('0') << std::setw(2) << value;
    return out.str();
}

// 导出用的规范形态：一律 #rrggbbaa（8 位）。
std::string colorToHex(const eui::Color& color) {
    return "#" + twoDigits(byteOf(color.r)) + twoDigits(byteOf(color.g)) +
           twoDigits(byteOf(color.b)) + twoDigits(byteOf(color.a));
}

// JSON 字符串字面量：转义 " \\ 与控制字符；UTF-8 原样透传（JSON 允许裸 UTF-8）。
std::string jsonString(const std::string& text) {
    std::string out = "\"";
    for (const char character : text) {
        switch (character) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(character) < 0x20) {
                    std::ostringstream control;
                    control << "\\u" << std::hex << std::setfill('0') << std::setw(4)
                            << static_cast<int>(static_cast<unsigned char>(character));
                    out += control.str();
                } else {
                    out.push_back(character);
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

// 足够精度的数字字面量（float 的 max_digits10 = 9，保证导出→解析逐位回到原值）。
std::string jsonNumber(float value) {
    std::ostringstream out;
    out << std::setprecision(9) << value;
    return out.str();
}

void writeVariant(std::ostringstream& out, const EditorColors& colors,
                  const ThemeTypography& typography, const char* indent) {
    const std::string pad(indent);
    out << "{\n" << pad << "  \"colors\": {\n";
    for (std::size_t index = 0; index < std::size(kThemeColorFields); ++index) {
        const ThemeColorField& field = kThemeColorFields[index];
        out << pad << "    \"" << field.name << "\": \""
            << colorToHex(colors.*(field.member)) << "\""
            << (index + 1 == std::size(kThemeColorFields) ? "\n" : ",\n");
    }
    out << pad << "  },\n" << pad << "  \"typography\": {\n";
    const auto number = [&](const char* key, const std::optional<float>& value, float fallback,
                            bool last = false) {
        out << pad << "    \"" << key << "\": " << jsonNumber(value.value_or(fallback))
            << (last ? "\n" : ",\n");
    };
    number("bodyLineHeight", typography.bodyLineHeight, kMarkdownBodyLineHeight);
    number("blockGap", typography.blockGap, kMarkdownBlockGap);
    number("listIndent", typography.listIndent, kMarkdownListIndent);
    number("radius", typography.radius, kMarkdownRadius);
    number("codeSizeFactor", typography.codeSizeFactor, kMarkdownCodeSizeFactor);
    number("h1SizeFactor", typography.h1SizeFactor, 1.618f);
    number("h2SizeFactor", typography.h2SizeFactor, 1.462f);
    number("h3SizeFactor", typography.h3SizeFactor, 1.318f);
    number("h1LineHeightFactor", typography.h1LineHeightFactor, 1.2f);
    number("h2LineHeightFactor", typography.h2LineHeightFactor, 1.2f);
    number("h3LineHeightFactor", typography.h3LineHeightFactor, 1.3f);
    number("headingSpaceBefore", typography.headingSpaceBefore, 1.0f);
    number("tableSpaceBefore", typography.tableSpaceBefore, 1.0f);
    out << pad << "    \"bodyFontFamily\": "
        << jsonString(typography.bodyFontFamily.value_or(std::string{})) << ",\n";
    out << pad << "    \"codeFontFamily\": "
        << jsonString(typography.codeFontFamily.value_or(std::string{})) << "\n";
    out << pad << "  }\n" << pad << "}";
}

} // namespace

bool parse(const std::string& json, ThemeFileData& out, std::string& error) {
    out = ThemeFileData{};
    error.clear();

    eui::json::Document document;
    if (!document.parse(json)) {
        const eui::json::ParseError& parseError = document.error();
        error = i18n::format("theme.json_error", {{"offset", std::to_string(parseError.offset)}, {"error", parseError.message}});
        return false;
    }
    const eui::json::Value root = document.root();
    if (!root.valid() || root.type() != eui::json::Type::Object) {
        error = i18n::tr("theme.root_object");
        return false;
    }

    int recognized = 0;

    const eui::json::Value schema = root.get("schema");
    if (schema.valid()) {
        ++recognized;
        std::string text;
        if (!schema.string(text) || text != kSchemaId) {
            error = i18n::format("theme.schema", {{"schema", kSchemaId}});
            return false;
        }
    }

    const eui::json::Value version = root.get("version");
    if (!version.valid()) {
        error = i18n::tr("theme.version_missing");
        return false;
    }
    ++recognized;
    double versionValue = 0.0;
    if (!version.number(versionValue) ||
        (versionValue != kSchemaVersion && versionValue != kSchemaVersion2)) {
        error = i18n::tr("theme.version_invalid");
        return false;
    }
    out.version = static_cast<int>(versionValue);

    const eui::json::Value name = root.get("name");
    if (name.valid()) {
        ++recognized;
        if (!name.string(out.name)) {
            error = i18n::tr("theme.name_string");
            return false;
        }
    }

    if (out.version == kSchemaVersion2) {
        int sides = 0;
        const eui::json::Value light = root.get("light");
        const eui::json::Value dark = root.get("dark");
        if (light.valid()) {
            ++recognized;
            ++sides;
            if (!parseVariant(light, out.light, error)) return false;
        }
        if (dark.valid()) {
            ++recognized;
            ++sides;
            if (!parseVariant(dark, out.dark, error)) return false;
        }
        if (sides == 0) {
            error = i18n::tr("theme.sides_missing");
            return false;
        }
        if (static_cast<int>(root.size()) != recognized) {
            error = i18n::tr("theme.top_v2");
            return false;
        }
        out.baseLight = light.valid() && !dark.valid();
        return true;
    }

    const eui::json::Value base = root.get("base");
    if (base.valid()) {
        ++recognized;
        std::string text;
        if (!base.string(text) || (text != "dark" && text != "light")) {
            error = i18n::tr("theme.base");
            return false;
        }
        out.baseLight = text == "light";
    }

    const eui::json::Value colors = root.get("colors");
    if (colors.valid()) {
        ++recognized;
        if (!parseColors(colors, out.colors, error)) return false;
    }

    const eui::json::Value typography = root.get("typography");
    if (typography.valid()) {
        ++recognized;
        if (typography.type() != eui::json::Type::Object) {
            error = i18n::tr("theme.typography_object");
            return false;
        }
        if (!parseTypography(typography, out.typography, error)) {
            return false;
        }
    }

    if (static_cast<int>(root.size()) != recognized) {
        error = i18n::tr("theme.top_v1");
        return false;
    }
    return true;
}

bool load(const std::string& utf8Path, std::string& error) {
    error.clear();
    if (utf8Path.empty()) {
        error = i18n::tr("theme.empty_path");
        return false;
    }
    // UTF-8 → 原生路径必须走 pathFromUtf8（GBK 代码页下 fs::path 直接吃窄串会抛）。
    const fs::path path = textfile::pathFromUtf8(utf8Path);
    std::error_code systemError;
    if (!fs::is_regular_file(path, systemError)) {
        error = systemError ? i18n::format("theme.read_error", {{"path", utf8Path}, {"error", systemError.message()}})
                            : i18n::format("theme.not_found", {{"path", utf8Path}});
        return false;
    }
    constexpr std::uintmax_t kMaxThemeBytes = 8ULL * 1024ULL * 1024ULL;
    const std::uintmax_t sourceBytes = fs::file_size(path, systemError);
    if (systemError || sourceBytes > kMaxThemeBytes) {
        error = systemError ? i18n::format("theme.read_error", {{"path", utf8Path}, {"error", systemError.message()}})
                            : i18n::format("theme.too_large", {{"path", utf8Path}});
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = i18n::format("theme.open_error", {{"path", utf8Path}});
        return false;
    }
    const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    ThemeFileData data;
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    bool importedCss = extension == ".css";
    fs::path cssPath = path;
    std::string themeName = textfile::pathToUtf8(path.stem());
    if (extension == ".json") {
        eui::json::Document document;
        if (document.parse(contents)) {
            const eui::json::Value root = document.root();
            std::string manifestName;
            const eui::json::Value name = root.get("name");
            const eui::json::Value minVersion = root.get("minAppVersion");
            if (!root.get("schema").valid() && name.string(manifestName) &&
                minVersion.valid()) {
                importedCss = true;
                themeName = manifestName;
                cssPath = path.parent_path() / "theme.css";
            }
        }
    }
    if (importedCss) {
        if (extension == ".css") {
            const fs::path manifestPath = path.parent_path() / "manifest.json";
            std::ifstream manifestInput(manifestPath, std::ios::binary);
            std::error_code manifestError;
            if (manifestInput && fs::file_size(manifestPath, manifestError) <= kMaxThemeBytes &&
                !manifestError) {
                const std::string manifest{std::istreambuf_iterator<char>(manifestInput),
                                           std::istreambuf_iterator<char>()};
                eui::json::Document document;
                if (document.parse(manifest)) {
                    std::string name;
                    if (document.root().get("name").string(name) && !name.empty())
                        themeName = name;
                }
            }
        }
        std::string css;
        if (extension == ".css") {
            css = contents;
        } else {
            if (!fs::is_regular_file(cssPath, systemError)) {
                error = i18n::format("theme.css_missing", {{"path", utf8Path}});
                return false;
            }
            const std::uintmax_t cssBytes = fs::file_size(cssPath, systemError);
            if (systemError || cssBytes > kMaxThemeBytes) {
                error = systemError ? i18n::format("theme.css_read_error", {{"path", utf8Path}, {"error", systemError.message()}})
                                    : i18n::format("theme.css_too_large", {{"path", utf8Path}});
                return false;
            }
            std::ifstream cssInput(cssPath, std::ios::binary);
            if (!cssInput) {
                error = i18n::format("theme.css_open_error", {{"path", utf8Path}});
                return false;
            }
            css.assign(std::istreambuf_iterator<char>(cssInput), std::istreambuf_iterator<char>());
        }
        if (!obsidiantheme::parse(css, themeName, data, error)) {
            error = i18n::format("theme.css_error", {{"path", utf8Path}, {"error", error}});
            return false;
        }
    } else if (!parse(contents, data, error)) {
        error = i18n::format("theme.file_error", {{"path", utf8Path}, {"error", error}});
        return false;
    }
    activate(data, utf8Path);
    return true;
}

void activate(const ThemeFileData& data, const std::string& utf8Path) {
    ThemeFileData& target = activeTheme();
    // revision 单调递增：data 是调用方的草稿（revision 通常是 0），不能直接抄，
    // 否则"加载 → 加载"会撞上同一个版本号，缓存不重建。
    const unsigned long long revision = target.revision + 1;
    target = data;
    target.path = utf8Path;
    target.revision = revision;
}

void reset() {
    ThemeFileData& target = activeTheme();
    const unsigned long long revision = target.revision + 1;
    target = ThemeFileData{};
    target.revision = revision;
}

std::string serialize(const std::string& name, bool baseLight,
                      const EditorColors& colors, const ThemeTypography& typography) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"" << kSchemaId << "\",\n";
    out << "  \"version\": " << kSchemaVersion2 << ",\n";
    out << "  \"name\": " << jsonString(name) << ",\n";
    out << "  \"" << (baseLight ? "light" : "dark") << "\": ";
    writeVariant(out, colors, typography, "  ");
    out << "\n";
    out << "}\n";
    return out.str();
}

std::string serializeDual(const std::string& name,
                          const EditorColors& light, const ThemeTypography& lightTypography,
                          const EditorColors& dark, const ThemeTypography& darkTypography) {
    std::ostringstream out;
    out << "{\n  \"schema\": \"" << kSchemaId << "\",\n"
        << "  \"version\": " << kSchemaVersion2 << ",\n"
        << "  \"name\": " << jsonString(name) << ",\n"
        << "  \"light\": ";
    writeVariant(out, light, lightTypography, "  ");
    out << ",\n  \"dark\": ";
    writeVariant(out, dark, darkTypography, "  ");
    out << "\n}\n";
    return out.str();
}

} // namespace neo::themeloader
