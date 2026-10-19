#pragma once
#include "model/style_schema.h"

namespace neo::themeauto {
// Offline HCT generation. No global theme state or draft history is changed.
bool validRecipe(const ThemeAutoRecipe& recipe);
bool generate(const ThemeAutoRecipe& recipe, ThemeFileData& result, std::string& error);
} // namespace neo::themeauto
