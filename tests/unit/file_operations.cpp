// R3 path/result checks. No valid deletion is issued here: Windows Shell may
// display real recycle-bin confirmation or fallback prompts by design.

#include "model/text_file.h"
#include "platform/file_operations.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct TempTree {
    fs::path root;
    fs::path outside;

    TempTree() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path base = fs::temp_directory_path() /
                              ("neo-file-operations-" + std::to_string(stamp));
        root = base / "CaseVault";
        outside = base / "outside";
        fs::create_directories(root / "child");
        fs::create_directories(outside);
    }

    ~TempTree() {
        std::error_code ignored;
        const fs::path base = root.parent_path();
        const fs::path temp = fs::temp_directory_path(ignored);
        const fs::path relative = base.lexically_relative(temp);
        const std::string leaf = base.filename().string();
        if (!ignored && !relative.empty() && relative != "." && *relative.begin() != ".." &&
            leaf.rfind("neo-file-operations-", 0) == 0) {
            fs::remove_all(base, ignored);
        }
    }
};

neo::platform::DeleteResult validate(const fs::path& root, const std::string& relative) {
    return neo::platform::validateVaultDeleteTarget(neo::textfile::pathToUtf8(root), relative);
}

} // namespace

int main() {
    TempTree tree;

    check(validate(tree.root, "child/missing.md").status == neo::platform::DeleteStatus::Success,
          "valid in-vault relative target is accepted without deleting it");
    check(validate(tree.root, "..").status == neo::platform::DeleteStatus::Error,
          "parent traversal is rejected");
    check(validate(tree.root, "child/../outside.txt").status == neo::platform::DeleteStatus::Error,
          "normalized parent traversal is rejected");
    check(validate(tree.root, ".").status == neo::platform::DeleteStatus::Error,
          "vault root itself is rejected");
    check(validate(tree.root, neo::textfile::pathToUtf8(tree.outside / "secret.md")).status ==
              neo::platform::DeleteStatus::Error,
          "absolute target is rejected");
    check(validate(tree.root, std::string("child/") + '\0' + "hidden.md").status ==
              neo::platform::DeleteStatus::Error,
          "embedded NUL is rejected");
    check(validate(tree.root, "child/*.md").status == neo::platform::DeleteStatus::Error,
          "Shell wildcard characters are rejected");

#if defined(_WIN32)
    check(validate(tree.root, ". ").status == neo::platform::DeleteStatus::Error,
          "Windows trailing-dot/space aliases are rejected");
    check(validate(tree.root, ".. ").status == neo::platform::DeleteStatus::Error,
          "Windows parent traversal with a trailing-space alias is rejected");
    check(validate(tree.root, "child/.. ").status == neo::platform::DeleteStatus::Error,
          "Windows nested parent traversal with a trailing-space alias is rejected");

    std::wstring rootVariant = tree.root.wstring();
    std::transform(rootVariant.begin(), rootVariant.end(), rootVariant.begin(), [](wchar_t ch) {
        return ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch - L'A' + L'a') : ch;
    });
    check(neo::platform::validateVaultDeleteTarget(neo::textfile::pathToUtf8(fs::path(rootVariant)),
                                                   "child/missing.md")
                  .status == neo::platform::DeleteStatus::Success,
          "Windows vault containment accepts a differently cased root path");
#endif

    std::error_code linkError;
    fs::create_directory_symlink(tree.outside, tree.root / "final-link", linkError);
    if (linkError) {
        std::cout << "SKIP: platform did not permit creation of an isolated directory symlink: "
                  << linkError.message() << '\n';
    } else {
        check(validate(tree.root, "final-link").status == neo::platform::DeleteStatus::Success,
              "a final symlink is treated as the entry to delete");
        check(validate(tree.root, "final-link/secret.md").status ==
                  neo::platform::DeleteStatus::Error,
              "a symlink/junction parent cannot redirect deletion outside the vault");
    }

    std::cout << (failures == 0 ? "PASS" : "FAIL") << ": file_operations path checks ("
              << failures << " failures)\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
