#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse::fs {

struct ItemProperties {
    std::wstring name, location, type;
    uint64_t size_bytes = 0, allocated_bytes = 0, file_count = 0, folder_count = 0;
    FILETIME created{}, modified{}, accessed{};
    int readonly_state = 0, hidden_state = 0;
    bool has_times = false, size_complete = true;
    uint32_t unreadable_count = 0, skipped_links = 0;
    DWORD first_error = ERROR_SUCCESS;
};

// Runs on the caller's worker. Attribute states: 0 = clear, 1 = set, 2 = mixed/unknown.
// Type codes: file/folder/mixed; multiple selections have no individual name/times.
// Counts cover the selection union; folder_count excludes top-level selected folders.
// Reparse points are counted but never traversed; skipped content makes size incomplete.
ItemProperties ReadItemProperties(const std::vector<std::wstring>& paths,
    const std::atomic_bool& cancelled);

}
