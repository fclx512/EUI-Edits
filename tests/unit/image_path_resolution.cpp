#include "core/render/image_source.h"
#include "core/platform/platform.h"

#include <filesystem>
#include <iostream>

int main() {
    std::error_code error;
    const std::filesystem::path original = std::filesystem::current_path(error);
    if (error) {
        std::cerr << "failed to capture current path: " << error.message() << "\n";
        return 1;
    }

    const std::filesystem::path tempDir =
        std::filesystem::temp_directory_path(error) / "eui_neo_deleted_cwd_image_path_test";
    if (error) {
        std::cerr << "failed to locate temp directory: " << error.message() << "\n";
        return 1;
    }

    std::filesystem::remove_all(tempDir, error);
    error.clear();
    std::filesystem::create_directories(tempDir, error);
    if (error) {
        std::cerr << "failed to create temp directory: " << error.message() << "\n";
        return 1;
    }

    std::filesystem::current_path(tempDir, error);
    if (error) {
        std::cerr << "failed to enter temp directory: " << error.message() << "\n";
        return 1;
    }

    std::filesystem::remove_all(tempDir, error);
    error.clear();

    if (!core::platform::repairCurrentWorkingDirectory()) {
        std::cerr << "failed to repair deleted current directory\n";
        return 1;
    }

    bool pending = true;
    const std::string resolved = core::render::image::resolveImagePath("missing-image.png", &pending);

    std::filesystem::current_path(original, error);
    if (error) {
        std::cerr << "failed to restore current path: " << error.message() << "\n";
        return 1;
    }

    if (!resolved.empty()) {
        std::cerr << "missing image unexpectedly resolved to " << resolved << "\n";
        return 1;
    }
    if (pending) {
        std::cerr << "local missing image should not be pending\n";
        return 1;
    }
    return 0;
}
