#include "snapshot_patch.h"
#include "entry_order_hold.h"
#include "entry_sort.h"
#include "../fs/enumeration_schedule.h"
#include "../fs/fs_enum.h"
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <unordered_set>

namespace pulse::app {

namespace {

std::wstring ChildPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L'\\' + name;
}

int FindName(const std::vector<fs::DirEntry>& entries, const std::wstring& name) {
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        if (_wcsicmp(entries[static_cast<size_t>(i)].name.c_str(), name.c_str()) == 0)
            return i;
    }
    return -1;
}

bool NeedsLinkResolution(const fs::DirEntry& entry) {
    if (fs::ClassifyLink(entry.attrs, entry.reparse_tag) != fs::LinkKind::None) return true;
    if (entry.is_dir || entry.name.size() < 4) return false;
    const wchar_t* suffix = entry.name.c_str() + entry.name.size() - 4;
    return _wcsicmp(suffix, L".lnk") == 0 || _wcsicmp(suffix, L".url") == 0;
}

} // namespace

bool FillDirEntry(const std::wstring& dir, const std::wstring& name, fs::DirEntry& out) {
    if (name.empty() || name.find_first_of(L"\\/") != std::wstring::npos) return false;
    const std::wstring full = fs::NormalizePath(ChildPath(dir, name));
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileExW(full.c_str(), FindExInfoBasic, &data,
        FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return false;
    FindClose(find);
    out = fs::DirEntry{};
    out.name = name;
    out.attrs = data.dwFileAttributes;
    out.is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out.is_reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    out.reparse_tag = out.is_reparse ? data.dwReserved0 : 0;
    out.cloud_recall = (data.dwFileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
    out.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    out.mtime = data.ftLastWriteTime;
    out.ctime = data.ftCreationTime;
    out.atime = data.ftLastAccessTime;
    return true;
}

NotifyPatch ApplyDirNotify(std::vector<fs::DirEntry>& entries, const std::wstring& folder,
                           const fs::DirNotifyEvent& event,
                           ui::SortColumn col, ui::SortDirection sort_dir) {
    return ApplyDirNotifyBatch(entries, folder, {event}, col, sort_dir);
}

namespace {

// Case-insensitive name key, folded per character like _wcsicmp/FindName.
struct FoldedNameHash {
    size_t operator()(const std::wstring& name) const noexcept {
        uint64_t hash = 1469598103934665603ull;
        for (wchar_t c : name) {
            hash ^= static_cast<uint64_t>(std::towlower(c));
            hash *= 1099511628211ull;
        }
        return static_cast<size_t>(hash);
    }
};

struct FoldedNameEqual {
    bool operator()(const std::wstring& a, const std::wstring& b) const noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::towlower(a[i]) != std::towlower(b[i])) return false;
        return true;
    }
};

using UpdatedNames = std::unordered_set<std::wstring, FoldedNameHash, FoldedNameEqual>;

bool IsRename(const fs::DirNotifyEvent& event) {
    return event.action == FILE_ACTION_RENAMED_NEW_NAME && !event.old_name.empty();
}

bool ValidNotify(const fs::DirNotifyEvent& event) {
    return !event.name.empty() && event.name.find_first_of(L"\\/") == std::wstring::npos &&
        (event.action == FILE_ACTION_REMOVED || event.action == FILE_ACTION_RENAMED_NEW_NAME ||
         event.action == FILE_ACTION_ADDED || event.action == FILE_ACTION_MODIFIED);
}

// Replay in event order when a sort key changes. A later rename or addition
// may depend on the position that an earlier relocation left behind.
void ReplayWithSortUpdates(std::vector<fs::DirEntry>& entries, const PreparedDirNotifyBatch& prepared,
                           const UpdatedNames& updated, ui::SortColumn col,
                           ui::SortDirection sort_dir) {
    for (const auto& event : prepared.events) {
        const bool rename = IsRename(event);
        const int at = FindName(entries, rename ? event.old_name : event.name);
        if (event.action == FILE_ACTION_REMOVED) {
            if (at >= 0) entries.erase(entries.begin() + at);
            continue;
        }
        const auto& entry = prepared.metadata.at(event.name);
        if (!entry) {
            if (at >= 0) entries.erase(entries.begin() + at);
            continue;
        }
        int slot = at;
        const int target = rename ? FindName(entries, event.name) : at;
        const bool replaces = updated.contains(event.name) &&
            (!rename || !FoldedNameEqual{}(event.old_name, event.name));
        const bool move = replaces && (target < 0 ||
            EntrySortMetadataChanged(entries[static_cast<size_t>(target)], *entry, col));
        if (target >= 0 && target != at) {
            if (at >= 0) {
                entries.erase(entries.begin() + target);
                if (target < at) --slot;
            } else {
                slot = target;
            }
        }
        if (move) {
            if (slot >= 0) entries.erase(entries.begin() + slot);
            const auto before = std::find_if(entries.begin(), entries.end(), [&](const fs::DirEntry& row) {
                return EntryLess(*entry, row, col, sort_dir);
            });
            entries.insert(before, *entry);
        } else {
            PlaceEntryHeld(entries, slot, *entry, col, sort_dir);
        }
    }
}

} // namespace

PreparedDirNotifyBatch PrepareDirNotifyBatch(const std::wstring& folder,
    const std::vector<fs::DirNotifyEvent>& events) {
    PreparedDirNotifyBatch prepared;
    prepared.events = events;
    const bool metadata_needed = std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.action != FILE_ACTION_REMOVED;
    });
    if (metadata_needed && fs::NeedsBoundedEnumeration(folder)) {
        prepared.need_full_enum = true;
        return prepared;
    }
    for (const auto& event : events) {
        if (!ValidNotify(event)) {
            prepared.need_full_enum = true;
            return prepared;
        }
        if (event.action == FILE_ACTION_REMOVED || prepared.metadata.contains(event.name)) continue;
        fs::DirEntry entry;
        std::optional<fs::DirEntry> value;
        if (FillDirEntry(folder, event.name, entry)) {
            if (NeedsLinkResolution(entry)) {
                prepared.need_full_enum = true;
                return prepared;
            }
            value = std::move(entry);
        }
        prepared.metadata.emplace(event.name, std::move(value));
    }
    return prepared;
}

NotifyPatch ApplyDirNotifyBatch(std::vector<fs::DirEntry>& entries, const std::wstring& folder,
    const std::vector<fs::DirNotifyEvent>& events, ui::SortColumn col, ui::SortDirection sort_dir) {
    return ApplyPreparedDirNotifyBatch(entries, PrepareDirNotifyBatch(folder, events), col, sort_dir);
}

NotifyPatch ApplyPreparedDirNotifyBatch(std::vector<fs::DirEntry>& entries,
    const PreparedDirNotifyBatch& prepared, ui::SortColumn col, ui::SortDirection sort_dir) {
    if (prepared.need_full_enum) return NotifyPatch::NeedFullEnum;
    const auto& events = prepared.events;
    if (events.empty()) return NotifyPatch::Applied;
    if (std::any_of(events.begin(), events.end(), [](const auto& event) { return !ValidNotify(event); }))
        return NotifyPatch::NeedFullEnum;
    constexpr size_t kNone = static_cast<size_t>(-1);
    std::unordered_map<std::wstring, size_t, FoldedNameHash, FoldedNameEqual> where;
    where.reserve(entries.size() + events.size());
    for (size_t i = 0; i < entries.size(); ++i) where.emplace(entries[i].name, i);
    const auto find = [&](const std::wstring& name) {
        const auto it = where.find(name);
        return it == where.end() ? kNone : it->second;
    };
    // Preflight before moving entries so a refresh request leaves the snapshot intact.
    UpdatedNames updated;
    for (const auto& event : events) {
        if (event.action == FILE_ACTION_REMOVED) continue;
        const bool rename = IsRename(event);
        const auto& old_name = rename ? event.old_name : event.name;
        const size_t at = find(old_name);
        if (at != kNone && NeedsLinkResolution(entries[at])) return NotifyPatch::NeedFullEnum;
        const auto metadata = prepared.metadata.find(event.name);
        if (metadata == prepared.metadata.end()) return NotifyPatch::NeedFullEnum;
        const auto& entry = metadata->second;
        if (!entry) continue;
        if (NeedsLinkResolution(*entry)) return NotifyPatch::NeedFullEnum;
        const size_t target = rename ? find(event.name) : at;
        if (target != kNone && (!rename || !FoldedNameEqual{}(old_name, event.name)) &&
            EntrySortMetadataChanged(entries[target], *entry, col))
            updated.insert(event.name);
    }
    if (!updated.empty()) {
        // Limit repeated scans/moves on the UI thread. A full Size sort also
        // accounts for cached folder totals when files and folders are mixed.
        constexpr size_t kMaxRowsVisited = 131072;
        if (events.size() > 1 && entries.size() > kMaxRowsVisited / events.size())
            return NotifyPatch::NeedFullEnum;
        if (col == ui::SortColumn::Size && CurrentFolderSortMode() == FolderSortMode::Mixed &&
            std::any_of(entries.begin(), entries.end(), [](const auto& row) { return row.is_dir; }))
            return NotifyPatch::NeedFullEnum;
        ReplayWithSortUpdates(entries, prepared, updated, col, sort_dir);
        return NotifyPatch::Applied;
    }

    // Replays the per-event rules on slots instead of a vector: rows keep
    // their slot, and a row that has to move (new, or its group changed) gets
    // a new slot after every original one. Appended slots are merged into
    // their group's end at the finish, exactly where InsertAtGroupEnd would
    // have put them one at a time.
    struct Slot {
        fs::DirEntry entry;
        bool live = true;
    };
    std::vector<Slot> slots;
    slots.reserve(entries.size() + events.size());
    for (auto& entry : entries) slots.push_back({std::move(entry), true});
    const size_t original = slots.size();
    const auto drop = [&](size_t slot) {
        if (slot == kNone) return;
        where.erase(slots[slot].entry.name);
        slots[slot].live = false;
    };
    const auto place = [&](size_t slot, fs::DirEntry entry) {
        if (slot != kNone && SameEntryGroup(slots[slot].entry, entry, col, sort_dir)) {
            where.erase(slots[slot].entry.name);
            slots[slot].entry = std::move(entry);
            where[slots[slot].entry.name] = slot;
            return;
        }
        drop(slot);
        slots.push_back({std::move(entry), true});
        where[slots.back().entry.name] = slots.size() - 1;
    };
    for (const auto& event : events) {
        if (event.action == FILE_ACTION_REMOVED) {
            drop(find(event.name));
            continue;
        }
        if (event.action == FILE_ACTION_RENAMED_NEW_NAME) {
            const size_t at = find(event.old_name.empty() ? event.name : event.old_name);
            const auto& entry = prepared.metadata.at(event.name);
            if (!entry) {
                drop(at);
                continue;
            }
            size_t slot = at;
            const size_t dup = find(event.name);
            if (dup != kNone && dup != at) {
                if (at != kNone) drop(dup);
                else slot = dup;
            }
            place(slot, *entry);
            continue;
        }
        const auto& entry = prepared.metadata.at(event.name);
        if (!entry) drop(find(event.name));
        else place(find(event.name), *entry);
    }

    std::vector<size_t> appended;
    for (size_t i = original; i < slots.size(); ++i)
        if (slots[i].live) appended.push_back(i);
    std::stable_sort(appended.begin(), appended.end(), [&](size_t a, size_t b) {
        return GroupBefore(slots[a].entry, slots[b].entry, col, sort_dir);
    });
    std::vector<fs::DirEntry> merged;
    merged.reserve(slots.size());
    size_t next = 0;
    for (size_t i = 0; i < original; ++i) {
        if (!slots[i].live) continue;
        while (next < appended.size() &&
               GroupBefore(slots[appended[next]].entry, slots[i].entry, col, sort_dir))
            merged.push_back(std::move(slots[appended[next++]].entry));
        merged.push_back(std::move(slots[i].entry));
    }
    for (; next < appended.size(); ++next) merged.push_back(std::move(slots[appended[next]].entry));
    entries = std::move(merged);
    return NotifyPatch::Applied;
}

} // namespace pulse::app
