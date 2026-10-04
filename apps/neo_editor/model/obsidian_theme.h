#pragma once

#include "model/style_schema.h"

#include <string>

namespace neo::obsidiantheme {

// Accepts root and .theme-light/.theme-dark custom properties. Unsupported CSS
// values are omitted from the palette and therefore fall back to built-in colors.
bool parse(const std::string& css, const std::string& name,
           ThemeFileData& out, std::string& error);

} // namespace neo::obsidiantheme
