#pragma once
#include "../fs/fs_enum.h"
#include "../fs/fs_watch.h"
#include "../ui/ui_renderer.h"
#include <optional>
#include <unordered_map>
#include <vector>

namespace pulse::app {

enum class NotifyPatch { Applied, NeedFullEnum };

struct PreparedDirNotifyBatch {
    std::vector<fs::DirNotifyEvent> events;
    std::unordered_map<std::wstring, std::optional<fs::DirEntry>> metadata;
    bool need_full_enum = false;
};

// Probes changed files on the I/O worker. Playback only updates memory.
PreparedDirNotifyBatch PrepareDirNotifyBatch(const std::wstring& folder,
    const std::vector<fs::DirNotifyEvent>& events);
NotifyPatch ApplyPreparedDirNotifyBatch(std::vector<fs::DirEntry>& entries,
    const PreparedDirNotifyBatch& prepared, ui::SortColumn col, ui::SortDirection sort_dir);

bool FillDirEntry(const std::wstring& dir, const std::wstring& name, fs::DirEntry& out);

// Ordinary renames keep their rows. Changes to active Size or Modified keys
// move the existing rows to their sort positions.
NotifyPatch ApplyDirNotify(std::vector<fs::DirEntry>& entries, const std::wstring& folder,
                           const fs::DirNotifyEvent& event,
                           ui::SortColumn col, ui::SortDirection sort_dir);

// Fast slot replay for batches that don't change existing sort keys. A batch
// requiring too much incremental sort work requests a full enumeration.
NotifyPatch ApplyDirNotifyBatch(std::vector<fs::DirEntry>& entries, const std::wstring& folder,
                                const std::vector<fs::DirNotifyEvent>& events,
                                ui::SortColumn col, ui::SortDirection sort_dir);

} // namespace pulse::app
