#pragma once

#include <cstddef>
#include <string>

namespace neo::cleanai {

struct Result {
    std::string text;
    std::size_t selectionBegin = 0;
    std::size_t selectionEnd = 0;
    bool changed = false;
};

// Clean the selected byte range while using the entire UTF-8 document to identify
// Markdown regions that must be preserved. The range must end on UTF-8 boundaries.
Result clean(const std::string& document, std::size_t selectionBegin, std::size_t selectionEnd);

} // namespace neo::cleanai
