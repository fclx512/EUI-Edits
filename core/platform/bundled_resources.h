#pragma once

#include <cstddef>
#include <filesystem>

namespace core::platform {

enum class BundledResourceId : unsigned int {
    EuiEditsLicenses = 502
};

struct BundledResourceView {
    const unsigned char* data = nullptr;
    std::size_t size = 0;

    explicit operator bool() const { return data != nullptr && size != 0; }
};

// Returns bytes stored in the current executable's RCDATA resources, if present.
// The view remains valid for the lifetime of the process.
BundledResourceView bundledResource(BundledResourceId id) noexcept;

bool exportEuiEditsLicenses(const std::filesystem::path& outputPath) noexcept;

// Returns -1 when this is not a license-export invocation; otherwise the process
// can return the provided exit code without entering the GUI application.
int handleEuiEditsLicenseCommandLine() noexcept;

// Opens a Save As dialog, exports the embedded license bundle, then opens the
// saved text file with the user's registered text viewer.
bool showEuiEditsLicenseExportDialog(void* ownerWindow = nullptr) noexcept;


} // namespace core::platform
