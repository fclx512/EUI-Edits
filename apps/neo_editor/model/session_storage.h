#pragma once

#include "model/text_file.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neo::sessionstorage {

using TabId = std::uint64_t;

struct WriteRecord {
    TabId id = 0;
    std::string path;
    std::string vaultRoot;
    std::string language;
    int wrapOverride = -1;
    bool dirty = false;
    const textfile::Document* document = nullptr;
};

struct ReadRecord {
    TabId id = 0;
    std::string path;
    std::string vaultRoot;
    std::string language;
    int wrapOverride = -1;
    bool dirty = false;
    textfile::Document document;
};

// Persists one complete tab snapshot. The manifest is the commit point: failure
// leaves the previous generation usable. Dirty documents are UTF-8/LF in memory;
// encoding metadata is stored beside each body so a later save can restore it.
bool write(const std::vector<WriteRecord>& records, TabId activeId);

// A missing manifest is a successful empty session. Any malformed manifest or
// missing/corrupt dirty body fails the entire read and returns empty outputs.
bool read(std::vector<ReadRecord>& records, TabId& activeId);

// Removes this module's session manifest and owned body files only.
// Returns false if the manifest or any owned body could not be removed.
bool clear();

// Normal application exit also removes the legacy single-document recovery
// file, but still leaves unrelated files in the config/session directory alone.
bool clearForNormalExit();

} // namespace neo::sessionstorage
