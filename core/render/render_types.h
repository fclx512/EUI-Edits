#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace core {

enum class ImageFit {
    Cover,
    Contain,
    Stretch
};

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Color {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    /** @brief 默认不透明白色，保留原有 RGBA 数值初始化语义。 */
    constexpr Color() = default;
    constexpr Color(float red, float green = 1.0f, float blue = 1.0f, float alpha = 1.0f)
        : r(red), g(green), b(blue), a(alpha) {}

    /** @brief 接受 #RGB、#RGBA、#RRGGBB、#RRGGBBAA；非法输入回退为透明黑。 */
    Color(std::string_view hex) : Color(fromHex(hex)) {}
    // 限定字符指针，避免 Color{0} 同时匹配 RGBA 数值和空指针。
    template<typename Char, std::enable_if_t<std::is_same_v<Char, char>, int> = 0>
    Color(const Char* hex) : Color(hex ? std::string_view(hex) : std::string_view{}) {}
    Color(const std::string& hex) : Color(std::string_view(hex)) {}

    /** @brief 数值形式固定解释为 0xRRGGBB，alpha 默认为 1。 */
    static constexpr Color fromHex(std::uint32_t rgb) {
        return {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f,
                (rgb & 255) / 255.0f, 1.0f};
    }

    /** @brief 数值形式固定解释为 0xRRGGBBAA，包含透明度。 */
    static constexpr Color fromHexRgba(std::uint32_t rgba) {
        return {((rgba >> 24) & 255) / 255.0f, ((rgba >> 16) & 255) / 255.0f,
                ((rgba >> 8) & 255) / 255.0f, (rgba & 255) / 255.0f};
    }

    /** @brief 严格解析 HEX；失败返回 false 且不修改 output。不分配内存、不抛异常。 */
    static bool tryFromHex(std::string_view hex, Color& output) {
        if (hex.empty() || hex.front() != '#') return false;
        hex.remove_prefix(1);
        const bool shortForm = hex.size() == 3 || hex.size() == 4;
        if (!shortForm && hex.size() != 6 && hex.size() != 8) return false;
        std::uint32_t value = 0;
        for (char c : hex) {
            const int digit = c >= '0' && c <= '9' ? c - '0' :
                              c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                              c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) return false;
            value = shortForm ? (value << 8) | (digit * 17) : (value << 4) | digit;
        }
        output = hex.size() == 3 || hex.size() == 6 ? fromHex(value) : fromHexRgba(value);
        return true;
    }

    /** @brief 解析 HEX 字符串；失败时返回透明黑，需要错误反馈时使用 tryFromHex。 */
    static Color fromHex(std::string_view hex) {
        Color result{0.0f, 0.0f, 0.0f, 0.0f};
        tryFromHex(hex, result);
        return result;
    }
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    bool contains(double pointX, double pointY) const {
        return pointX >= x && pointX <= x + width &&
               pointY >= y && pointY <= y + height;
    }
};

enum class GradientDirection {
    Horizontal = 0,
    Vertical = 1
};

struct Gradient {
    bool enabled = false;
    Color start = {1.0f, 1.0f, 1.0f, 1.0f};
    Color end = {1.0f, 1.0f, 1.0f, 1.0f};
    GradientDirection direction = GradientDirection::Vertical;
};

struct Border {
    float width = 0.0f;
    Color color = {1.0f, 1.0f, 1.0f, 1.0f};
};

struct Shadow {
    bool enabled = false;
    Vec2 offset = {0.0f, 4.0f};
    float blur = 8.0f;
    float spread = 0.0f;
    Color color = {0.0f, 0.0f, 0.0f, 0.28f};
    bool inset = false;
};

struct Transform {
    Vec2 translate = {0.0f, 0.0f};
    float translateZ = 0.0f;
    Vec2 scale = {1.0f, 1.0f};
    float rotate = 0.0f;
    float rotateX = 0.0f;
    float rotateY = 0.0f;
    Vec2 origin = {0.5f, 0.5f};
    float perspective = 0.0f;
};

struct TransformMatrix {
    float m00 = 1.0f;
    float m01 = 0.0f;
    float tx = 0.0f;
    float m10 = 0.0f;
    float m11 = 1.0f;
    float ty = 0.0f;
    float px = 0.0f;
    float py = 0.0f;
    float pw = 1.0f;
};

inline Vec2 transformPoint(const TransformMatrix& matrix, float x, float y) {
    const float w = matrix.px * x + matrix.py * y + matrix.pw;
    const float invW = std::fabs(w) > 0.0001f ? 1.0f / w : 1.0f;
    return {
        (matrix.m00 * x + matrix.m01 * y + matrix.tx) * invW,
        (matrix.m10 * x + matrix.m11 * y + matrix.ty) * invW
    };
}

inline Vec3 transformPointWithW(const TransformMatrix& matrix, float x, float y) {
    float w = matrix.px * x + matrix.py * y + matrix.pw;
    if (std::fabs(w) <= 0.0001f) {
        w = w < 0.0f ? -0.0001f : 0.0001f;
    }
    const float invW = 1.0f / w;
    return {
        (matrix.m00 * x + matrix.m01 * y + matrix.tx) * invW,
        (matrix.m10 * x + matrix.m11 * y + matrix.ty) * invW,
        w
    };
}

inline Color mixColor(const Color& from, const Color& to, float amount) {
    const float clampedAmount = std::clamp(amount, 0.0f, 1.0f);
    // 预乘空间插值：跨 alpha 的过渡（例如从悬停高亮淡向低透明黑的按压态）合成到
    // 背景后恒在两端之间，不会经过比两端更暗/更亮的中间灰——那是直 alpha 插值
    // 配直 alpha 合成时"按压/松手瞬间闪一下"的根源。两端 alpha 相同（尤其都
    // 不透明）时，结果与直空间插值逐通道一致，静态取色调用不受影响。
    const float fromR = from.r * from.a;
    const float fromG = from.g * from.a;
    const float fromB = from.b * from.a;
    const float toR = to.r * to.a;
    const float toG = to.g * to.a;
    const float toB = to.b * to.a;
    const float a = from.a + (to.a - from.a) * clampedAmount;
    if (a <= 0.0005f) {
        // 全透明没有可分离的 rgb：保留直插值作为色相记忆，反向动画起点才不丢色相。
        return {
            from.r + (to.r - from.r) * clampedAmount,
            from.g + (to.g - from.g) * clampedAmount,
            from.b + (to.b - from.b) * clampedAmount,
            0.0f
        };
    }
    return {
        (fromR + (toR - fromR) * clampedAmount) / a,
        (fromG + (toG - fromG) * clampedAmount) / a,
        (fromB + (toB - fromB) * clampedAmount) / a,
        a
    };
}

} // namespace core
