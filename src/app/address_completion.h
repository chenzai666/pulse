#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace pulse {

struct AddressCompletionQuery {
    std::wstring parent;
    std::wstring prefix;
};

struct AddressCompletionEntry {
    std::wstring name;
    std::wstring path;
    bool directory = false;
};

// Resolve relative and root-relative paths using the focused folder.
// An empty result leaves virtual locations, shortcuts and malformed paths to
// the existing navigation path.
std::wstring ResolveAddressFilesystemPath(std::wstring text, const std::wstring& base);
bool ParseAddressCompletion(std::wstring text, const std::wstring& base,
                            AddressCompletionQuery& query);
std::vector<AddressCompletionEntry> EnumerateAddressCompletion(
    const AddressCompletionQuery& query, const std::atomic_bool& cancelled);

} // namespace pulse
