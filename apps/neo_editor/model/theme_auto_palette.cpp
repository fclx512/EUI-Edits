#include "model/theme_auto_palette.h"
#include "cpp/cam/hct.h"
#include "cpp/palettes/tones.h"
#include "cpp/scheme/scheme_neutral.h"
#include "cpp/scheme/scheme_tonal_spot.h"
#include "cpp/scheme/scheme_vibrant.h"
#include <cmath>
#include <cctype>

namespace neo::themeauto {
namespace {
namespace mcu = material_color_utilities;
eui::Color color(mcu::Argb argb, double alpha = 1.0) {
    return {((argb >> 16) & 255) / 255.0f, ((argb >> 8) & 255) / 255.0f,
            (argb & 255) / 255.0f, static_cast<float>(std::round(alpha * 255.0) / 255.0)};
}
}
bool validRecipe(const ThemeAutoRecipe& recipe) {
    if (recipe.seed.size() != 7 || recipe.seed[0] != '#' || recipe.round < 0 || recipe.round > 1000000) return false;
    for (size_t i = 1; i < 7; ++i) if (!std::isxdigit(static_cast<unsigned char>(recipe.seed[i]))) return false;
    return recipe.variant == "neutral" || recipe.variant == "soft" || recipe.variant == "vivid";
}

bool generate(const ThemeAutoRecipe& recipe, ThemeFileData& result, std::string& error) {
    error.clear();
    if (!validRecipe(recipe)) { error = "请选择有效的主色和配色方案。"; return false; }
    const auto seed = static_cast<mcu::Argb>(std::stoul(recipe.seed.substr(1), nullptr, 16)) | 0xff000000u;
    const mcu::Hct source(seed);
    constexpr int rotations[]{0, 10, -10, 20, -20, 5, -5};
    const double hue = std::fmod(source.get_hue() + rotations[recipe.round % 7] + 360.0, 360.0);
    const mcu::Hct hct(hue, source.get_chroma(), source.get_tone());
    const bool neutral = recipe.variant == "neutral", vivid = recipe.variant == "vivid";
    const mcu::TonalPalette base(hue, neutral ? 1 : vivid ? 11 : 6);
    const mcu::TonalPalette secondary(hue + 25, neutral ? 10 : vivid ? 36 : 20);
    ThemeFileData generated;
    generated.version = 2;
    generated.autoRecipe = recipe;
    for (bool dark : {false, true}) {
        const mcu::DynamicScheme scheme = neutral ? mcu::DynamicScheme(mcu::SchemeNeutral(hct, dark, 0)) :
            vivid ? mcu::DynamicScheme(mcu::SchemeVibrant(hct, dark, 0)) :
                    mcu::DynamicScheme(mcu::SchemeTonalSpot(hct, dark, 0));
        auto& p = (dark ? generated.dark : generated.light).colors;
        const auto surface = [&](double tone) { return color(base.get(tone)); };
        p["window"] = surface(neutral ? (dark ? 8 : 94) : (dark ? 9 : 93));
        p["panel"] = surface((dark ? 12 : 96) + (vivid ? (dark ? 1 : -1) : 0));
        p["toolbar"] = surface((dark ? 15 : 94) + (vivid ? (dark ? 2 : -1) : 0));
        p["editor"] = surface(neutral ? (dark ? 15 : 100) : (dark ? 17 : 99));
        p["statusBar"] = surface(dark ? 11 : 92);
        p["text"] = surface(dark ? 92 : 16);
        p["textMuted"] = surface(dark ? 76 : 36);
        p["border"] = surface(dark ? 36 : 74);
        p["accent"] = color(scheme.GetPrimary());
        p["selectionBackground"] = color(scheme.GetPrimary(), dark ? .26 : .22);
        p["rowHover"] = surface(dark ? 23 : 87);
        p["rowActive"] = color(scheme.GetPrimary(), dark ? .18 : .14);
        p["pressed"] = surface(dark ? 29 : 80);
        p["heading"] = color((neutral ? base : secondary).get(dark ? 94 : 17));
        p["markdownAccent"] = color(scheme.tertiary_palette.get(dark ? 82 : 35));
        p["quoteBackground"] = color(scheme.GetPrimary(), dark ? .09 : .07);
        p["divider"] = surface(dark ? 40 : 72);
        p["codeBackground"] = surface(dark ? 12 : 96);
        p["codeText"] = surface(dark ? 90 : 19);
        const std::pair<const char*, double> anchors[]{
            {"tokenKeyword",325},{"tokenString",145},{"tokenNumber",285},
            {"tokenOperator",25},{"tokenFunction",75},{"tokenProperty",210}};
        for (const auto& [name, anchor] : anchors) {
            const double delta = std::fmod(hue - anchor + 540, 360) - 180;
            const mcu::TonalPalette token(std::fmod(anchor + delta * .12 + 360, 360), neutral ? 22 : vivid ? 52 : 36);
            p[name] = color(token.get(dark ? 79 : 34));
        }
        p["tokenComment"] = surface(dark ? 72 : 39);
    }
    result = std::move(generated);
    return true;
}
} // namespace neo::themeauto
