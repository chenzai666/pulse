#include "entry_order_hold.h"
#include "entry_group.h"
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <unordered_map>

namespace pulse::app {

namespace {

// <0 when a's group is shown before b's; 0 in the same group or ungrouped.
int GroupOrder(const fs::DirEntry& a, const fs::DirEntry& b,
               ui::SortColumn col, ui::SortDirection dir) {
    const EntryGrouping* grouping = CurrentEntryGrouping();
    if (!grouping || grouping->by == GroupBy::None) return 0;
    return GroupCompare(a, b, grouping->by, grouping->clock, col, dir);
}

bool Grouped() noexcept {
    const EntryGrouping* grouping = CurrentEntryGrouping();
    return grouping && grouping->by != GroupBy::None;
}

// Case-insensitive name key, folded per character like _wcsicmp.
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

} // namespace

bool EntrySortMetadataChanged(const fs::DirEntry& before, const fs::DirEntry& after,
                              ui::SortColumn col) {
    if (col == ui::SortColumn::Size) return before.size != after.size;
    if (col == ui::SortColumn::Mtime) return CompareFileTime(&before.mtime, &after.mtime) != 0;
    return false;
}

bool SameEntryGroup(const fs::DirEntry& a, const fs::DirEntry& b,
                    ui::SortColumn col, ui::SortDirection dir) {
    return GroupOrder(a, b, col, dir) == 0;
}

bool GroupBefore(const fs::DirEntry& a, const fs::DirEntry& b,
                 ui::SortColumn col, ui::SortDirection dir) {
    return GroupOrder(a, b, col, dir) < 0;
}

void InsertAtGroupEnd(std::vector<fs::DirEntry>& entries, fs::DirEntry entry,
                      ui::SortColumn col, ui::SortDirection dir) {
    if (!Grouped()) {
        entries.push_back(std::move(entry));
        return;
    }
    const auto it = std::upper_bound(entries.begin(), entries.end(), entry,
        [col, dir](const fs::DirEntry& value, const fs::DirEntry& row) {
            return GroupOrder(value, row, col, dir) < 0;
        });
    entries.insert(it, std::move(entry));
}

void PlaceEntryHeld(std::vector<fs::DirEntry>& entries, int slot, fs::DirEntry entry,
                    ui::SortColumn col, ui::SortDirection dir) {
    if (slot >= 0 && slot < static_cast<int>(entries.size())) {
        auto& row = entries[static_cast<size_t>(slot)];
        if (SameEntryGroup(row, entry, col, dir)) {
            row = std::move(entry);
            return;
        }
        entries.erase(entries.begin() + slot);
    }
    InsertAtGroupEnd(entries, std::move(entry), col, dir);
}

std::vector<fs::DirEntry> KeepEntryOrder(const std::vector<fs::DirEntry>& shown,
                                         const std::vector<fs::DirEntry>& fresh,
                                         const std::vector<EntryRename>& renames,
                                         ui::SortColumn col, ui::SortDirection dir) {
    std::unordered_map<std::wstring, size_t, FoldedNameHash, FoldedNameEqual> fresh_at;
    fresh_at.reserve(fresh.size());
    for (size_t i = 0; i < fresh.size(); ++i) fresh_at.emplace(fresh[i].name, i);
    std::unordered_map<std::wstring, std::wstring, FoldedNameHash, FoldedNameEqual> renamed_to;
    for (const auto& rename : renames) renamed_to[rename.old_name] = rename.new_name;

    // Rows that keep their place, in the order shown.
    enum class Placement { New, Held, Updated };
    std::vector<Placement> placement(fresh.size(), Placement::New);
    std::vector<size_t> kept;
    kept.reserve(std::min(shown.size(), fresh.size()));
    std::vector<size_t> updated;
    for (const auto& row : shown) {
        bool renamed = false;
        auto it = fresh_at.find(row.name);
        if (it == fresh_at.end()) {
            const auto rename = renamed_to.find(row.name);
            if (rename == renamed_to.end()) continue;
            renamed = true;
            it = fresh_at.find(rename->second);
            if (it == fresh_at.end()) continue;
        }
        const size_t at = it->second;
        if (!renamed && row.name != fresh[at].name) {
            const auto rename = renamed_to.find(row.name);
            renamed = rename != renamed_to.end() && rename->second == fresh[at].name;
        }
        if (!renamed && EntrySortMetadataChanged(row, fresh[at], col)) {
            if (placement[at] != Placement::Updated) updated.push_back(at);
            placement[at] = Placement::Updated;
        } else if (placement[at] == Placement::New && SameEntryGroup(row, fresh[at], col, dir)) {
            placement[at] = Placement::Held;
            kept.push_back(at);
        }
    }

    if (!updated.empty()) {
        // Indices in fresh include the worker's order by cached folder sizes.
        std::sort(updated.begin(), updated.end());
        std::vector<size_t> merged;
        merged.reserve(kept.size() + updated.size());
        auto next_update = updated.begin();
        for (const size_t at : kept) {
            // A rename onto an existing name may have first claimed its destination.
            if (placement[at] == Placement::Updated) continue;
            while (next_update != updated.end() && *next_update < at)
                merged.push_back(*next_update++);
            merged.push_back(at);
        }
        merged.insert(merged.end(), next_update, updated.end());
        kept.swap(merged);
    }

    // Fresh is in sort order, so each group's newcomers follow its kept rows.
    std::vector<fs::DirEntry> out;
    out.reserve(fresh.size());
    size_t next = 0;
    const auto skip_used = [&] {
        while (next < fresh.size() && placement[next] != Placement::New) ++next;
    };
    skip_used();
    for (const size_t at : kept) {
        while (next < fresh.size() && GroupOrder(fresh[next], fresh[at], col, dir) < 0) {
            out.push_back(fresh[next++]);
            skip_used();
        }
        out.push_back(fresh[at]);
    }
    for (; next < fresh.size(); ++next)
        if (placement[next] == Placement::New) out.push_back(fresh[next]);
    return out;
}

void FollowHeldRenames(const std::vector<EntryRename>& renames,
                       const std::vector<fs::DirEntry>& fresh,
                       std::vector<std::wstring>& names, std::wstring& focus) {
    if (renames.empty()) return;
    std::unordered_map<std::wstring, char, FoldedNameHash, FoldedNameEqual> present;
    present.reserve(fresh.size());
    for (const auto& entry : fresh) present.emplace(entry.name, char{});
    const auto follow = [&](std::wstring& name) {
        if (name.empty() || present.find(name) != present.end()) return;
        // Newest rename first: a row renamed twice follows the latest name.
        for (auto it = renames.rbegin(); it != renames.rend(); ++it) {
            if (!FoldedNameEqual{}(it->old_name, name)) continue;
            if (present.find(it->new_name) != present.end()) name = it->new_name;
            return;
        }
    };
    for (auto& name : names) follow(name);
    follow(focus);
}

void PruneHeldRenames(std::vector<EntryRename>& renames, const std::vector<fs::DirEntry>& fresh) {
    if (renames.empty()) return;
    std::unordered_map<std::wstring, char, FoldedNameHash, FoldedNameEqual> present;
    present.reserve(fresh.size());
    for (const auto& entry : fresh) present.emplace(entry.name, char{});
    // A rename still pending leaves its old name on disk.
    renames.erase(std::remove_if(renames.begin(), renames.end(), [&](const EntryRename& rename) {
        return present.find(rename.old_name) == present.end() ||
               FoldedNameEqual{}(rename.old_name, rename.new_name);
    }), renames.end());
    constexpr size_t kMaxHeldRenames = 32;
    if (renames.size() > kMaxHeldRenames)
        renames.erase(renames.begin(), renames.end() - kMaxHeldRenames);
}

} // namespace pulse::app
