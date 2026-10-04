#pragma once

#include <string>

namespace neo::platform {
enum class RenameStatus { Success, Conflict, Error };
struct RenameResult {
    RenameStatus status = RenameStatus::Error;
    std::string message;
    std::string suggestedName;
};
// Windows-compatible names on every platform. No trimming or silent correction.
std::string validateRenameName(const std::string& name);
int renameSelectionEnd(const std::string& name, bool isDirectory);
bool sameVaultRelativePath(const std::string& a, const std::string& b);
// Canonical parent containment plus native no-replace rename. A conflict only
// proposes a numbered name; the caller must obtain consent before trying it.
RenameResult renameVaultEntry(const std::string& root, const std::string& relative,
                             const std::string& newName);
} // namespace neo::platform
