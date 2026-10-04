#pragma once

#include <string>

namespace neo::platform {

enum class DeleteStatus {
    Success,
    Cancelled,
    Error,
};

struct DeleteResult {
    DeleteStatus status = DeleteStatus::Error;
    std::string message;
};

// Performs the same containment checks without changing the filesystem.
DeleteResult validateVaultDeleteTarget(const std::string& vaultRoot,
                                       const std::string& relativePath);

// Deletes one vault entry. On Windows this asks the Shell to move it to the
// Recycle Bin; other platforms keep the existing permanent-delete behavior.
// `relativePath` must identify an entry below `vaultRoot`, never the root.
DeleteResult deleteVaultEntry(const std::string& vaultRoot,
                              const std::string& relativePath,
                              bool isDirectory);

} // namespace neo::platform
