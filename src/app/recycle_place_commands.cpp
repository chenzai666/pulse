#include "recycle_place_commands.h"
#include "app_internal.h"
#include "context_menu.h"
#include "default_file_manager.h"
#include "pane_drop.h"
#include "sidebar_navigation.h"

namespace pulse {

std::vector<std::wstring> RecycleRestorePaths(const std::vector<fs::DirEntry>& entries) {
    std::vector<std::wstring> paths;
    paths.reserve(entries.size());
    for (const auto& entry : entries)
        if (!entry.recycle_path.empty()) paths.push_back(entry.recycle_path);
    return paths;
}

bool CanOpenRecycleInNewPane(const AppState& state) {
    const auto* layout = state.window_tabs.Active();
    return layout && layout->root && state.pane && !IsSettingsTab(state.pane->ActiveTab()) &&
        layout->VisiblePanes().size() < app::kMaxTabPanes;
}

bool HandleRecycleToolbarCommand(AppState& state, int action) {
    const auto* tab = ActiveTab(state);
    if (!IsRecycleTab(tab) || !tab->snapshot || tab->loading) return false;
    if (action == 2) {
        if (tab->SelectedCount() == 0) return false;
        RestoreSelected(state);
        return true;
    }
    if (tab->snapshot->empty()) return false;
    if (action == 0) return HandleRecyclePlaceCommand(state, app::CmdEmptyRecycle);
    if (action == 1) return HandleRecyclePlaceCommand(state, app::CmdRestoreAllRecycle);
    return false;
}

bool HandleRecyclePlaceCommand(AppState& state, int command) {
    if (HandleSidebarNavigationCommand(state, app::MakeRecyclePath(), command)) return true;
    switch (command) {
    case app::CmdRestoreAllRecycle:
        // 工作池在操作服务之前停止；任务只引用进程级服务，不引用窗口。
        state.worker.EnqueueSerialIo([operations = &state.core.ops] {
            std::vector<fs::DirEntry> entries;
            fs::EnumerateRecycleBin(entries);
            ops::OpRequest request;
            request.type = ops::OpType::RestoreRecycle;
            request.sources = RecycleRestorePaths(entries);
            if (!request.sources.empty()) operations->Submit(std::move(request));
        });
        return true;
    case app::CmdEmptyRecycle:
        EmptyRecycleBin(state);
        return true;
    case app::CmdUnpinQuickAccess:
        state.sidebarQuickAccessHiddenMask |= 1u << static_cast<unsigned>(app::BuiltinQuickAccess::RecycleBin);
        if (state.hwnd) InvalidateRect(state.hwnd, nullptr, FALSE);
        return true;
    case app::CmdProperties:
        state.ops.ShowProperties(std::wstring(app::kRecycleBinParsingName));
        return true;
    case app::CmdOpenInNewPaneVertical:
    case app::CmdOpenInNewPaneHorizontal: {
        if (!CanOpenRecycleInNewPane(state)) return true;
        auto* layout = state.window_tabs.Active();
        auto incoming = app::MakeSingleLayoutTab(app::MakeRecyclePath());
        app::PanePlacement placement;
        placement.layout = app::LayoutPreset::Custom;
        placement.pane = state.pane;
        placement.side = command == app::CmdOpenInNewPaneVertical ? app::ContentDrop::Right : app::ContentDrop::Bottom;
        if (!app::CanPlacePanes(*layout, *incoming, placement)) return true;
        if (state.addressEditing) HideAddressEditor(state, false);
        RememberLayoutFocus(state);
        if (!app::AddPaneFromTab(*layout, std::move(incoming), placement)) return true;
        BindCurrentLayout(state);
        if (auto* tab = ActiveTab(state)) StartLoadingPath(state, *tab, app::MakeRecyclePath());
        if (state.hwnd) InvalidateRect(state.hwnd, nullptr, FALSE);
        return true;
    }
    default:
        return false;
    }
}

}
