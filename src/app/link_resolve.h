// link_resolve.h — Read saved link destinations without Shell tracking or UI.
#pragma once
#include "../fs/fs_enum.h"
#include <functional>
#include <string>
#include <vector>

namespace pulse::app {

// Fill e.link_target* when lnk_path is a readable shortcut whose target
// currently exists. Clears prior target metadata before reading, returns false
// otherwise. link_destination records a readable
// shortcut's saved path even when the target is missing; it is presentation only.
bool ResolveLink(const std::wstring& lnk_path, fs::DirEntry& e);

// Populate immediate destinations for .lnk, .url, symlinks, and junctions.
// Only shortcuts on a known local target drive acquire target metadata.
// Optional metadata has a 100 ms budget and independent provider quota.
// Entries with a full path use it in place of the enumerated directory.
void ResolveLinksInPlace(const std::wstring& parent_path,
                         std::vector<fs::DirEntry>& entries,
                         const std::function<bool()>& cancel);

} // namespace pulse::app
