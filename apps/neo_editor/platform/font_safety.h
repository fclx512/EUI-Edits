#pragma once

#include <string>
#include <string_view>

namespace neo::settings {
struct Data;
} // namespace neo::settings

namespace neo::fontsafety {

struct ProbeResult {
    bool ok = false;
    bool monospace = false;
    std::string error;
};

// An empty path means the application preset and is always safe.
// On Windows, parsing happens in a short-lived, memory-limited helper process.
// Other platforms use a bounded in-process check and report that limitation in
// ProbeResult::error, including when validation succeeds.
ProbeResult validate(const std::string& utf8Path, unsigned timeoutMs = 4000);

// Call before the single-instance gate. Returns -1 for a normal invocation or
// a helper process exit code when --neo-font-probe was requested.
int probeCommandLineIfRequested();

// Consume the previous run's marker. Clears only font paths that still match
// the values recorded by that run. `data` should be settings::current().
// Returns true when a marker was found and successfully handled.
bool recoverSession(settings::Data& data, std::string& message);

// Arm startup recovery for the actual custom fonts used by this run. An empty
// set removes the marker. Failure means callers must not activate custom fonts.
bool armSession(const settings::Data& data, std::string& error);

// Remove the marker during an orderly shutdown, unless recovery could not save.
void cleanSession();

namespace testing {

struct SessionFonts {
    std::string editor;
    std::string ui;
    std::string code;
};

// Pure marker helpers for unit tests. Paths are stored length-prefixed so
// unusual UTF-8 path bytes cannot alter the record structure.
std::string encodeSessionMarker(const SessionFonts& fonts);
bool decodeSessionMarker(std::string_view marker, SessionFonts& fonts);

// Windows command-line quoting is platform-independent and can be tested
// without creating a process.
std::wstring quoteWindowsArgument(std::wstring_view argument);

} // namespace testing
} // namespace neo::fontsafety
