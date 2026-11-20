#pragma once
#include "../fs/fs_enum.h"
#include "../ui/ui_renderer.h"
#include <string>
#include <vector>

namespace pulse::app {

// True when a refresh changes an entry's active Size or Modified sort key.
bool EntrySortMetadataChanged(const fs::DirEntry& before, const fs::DirEntry& after,
                              ui::SortColumn col);

// True when `a` and `b` fall in the same group (always true when ungrouped).
bool SameEntryGroup(const fs::DirEntry& a, const fs::DirEntry& b,
                    ui::SortColumn col, ui::SortDirection dir);

// True when a's group is shown before b's (never when ungrouped).
bool GroupBefore(const fs::DirEntry& a, const fs::DirEntry& b,
                 ui::SortColumn col, ui::SortDirection dir);

// Inserts `entry` after the last row of its group, or before the first row of
// a later group; at the very end when ungrouped.
void InsertAtGroupEnd(std::vector<fs::DirEntry>& entries, fs::DirEntry entry,
                      ui::SortColumn col, ui::SortDirection dir);

// Puts `entry` in row `slot` when that row is in the same group, otherwise
// drops the row (if any) and inserts `entry` at the end of its group.
void PlaceEntryHeld(std::vector<fs::DirEntry>& entries, int slot, fs::DirEntry entry,
                    ui::SortColumn col, ui::SortDirection dir);

// A rename Pulse issued and whose refresh may arrive before the watcher event.
struct EntryRename {
    std::wstring old_name;
    std::wstring new_name;
};

// Merges a freshly enumerated listing (`fresh`, in sort order) into the order
// on screen (`shown`). Updates to active sort keys use the fresh sort order.
// Renames keep their row within their group. New items land at their group end.
std::vector<fs::DirEntry> KeepEntryOrder(const std::vector<fs::DirEntry>& shown,
                                         const std::vector<fs::DirEntry>& fresh,
                                         const std::vector<EntryRename>& renames,
                                         ui::SortColumn col, ui::SortDirection dir);

// Points selection names at their new names when `fresh` shows the rename
// landed (the old name is gone, the new one present), so a refresh that
// captured the selection before the rename keeps the renamed item selected.
void FollowHeldRenames(const std::vector<EntryRename>& renames,
                       const std::vector<fs::DirEntry>& fresh,
                       std::vector<std::wstring>& names, std::wstring& focus);

// Drops the renames `fresh` already reflects (or that failed and left the old
// name gone); the rest still wait for their refresh. Keeps the newest few.
void PruneHeldRenames(std::vector<EntryRename>& renames, const std::vector<fs::DirEntry>& fresh);

} // namespace pulse::app
