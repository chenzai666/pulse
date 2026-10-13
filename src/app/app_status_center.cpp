#include "app_status_center.h"
#include "app_internal.h"
#include "app_ops_ui.h"
#include "app_commands.h"
#include "app_messages.h"
#include "context_menu.h"
#include "../ui/status_center_window.h"

namespace pulse {
namespace {

void RefreshStatusCenter(AppState& state) {
    if (state.core.statusCenterWindow) state.core.statusCenterWindow->Update(state.core.statusCenter.records());
    app::ForEachWindowSharingCore(state, [](AppState& window) {
        RECT title{0, 0, static_cast<LONG>(window.compositor.Width()), static_cast<LONG>(window.renderer.TitleBarHeight())};
        InvalidateRect(window.hwnd, &title, FALSE);
    });
}

} // namespace

void RecordOperationStatus(AppState& state, const ops::OpStatus& status) {
    if (state.core.statusCenter.RecordOperation(status)) RefreshStatusCenter(state);
}

void RecordFinishedOperation(AppState& state, const ops::FinishedTask& task) {
    if (state.core.statusCenter.RecordFinishedOperation(task)) RefreshStatusCenter(state);
}

void RecordFileConversionStatus(AppState& state, const app::FileConversionEvent& event) {
    if (event.kind == app::FileConversionEventKind::Capabilities || !event.generation ||
        event.generation != state.fileConversionUi.job_generation) return;
    if (state.core.statusCenter.RecordConversion(state.hwnd, event)) RefreshStatusCenter(state);
}

void RecordFileConversionQueued(AppState& state, uint64_t generation, const std::vector<std::wstring>& inputs,
                                const std::wstring& target) {
    if (generation && generation == state.fileConversionUi.job_generation &&
        state.core.statusCenter.RecordConversionQueued(state.hwnd, generation, inputs, target)) RefreshStatusCenter(state);
}

void RetireStatusCenterOwner(AppState& state) {
    if (state.core.statusCenterWindow && state.core.statusCenterWindow->owner() == state.hwnd)
        state.core.statusCenterWindow->Destroy();
    if (state.core.statusCenter.RetireConversionOwner(state.hwnd)) RefreshStatusCenter(state);
}

void ShowStatusCenter(AppState& state) {
    auto& core = state.core;
    if (!core.statusCenterWindow) core.statusCenterWindow = std::make_unique<ui::StatusCenterWindow>();
    ui::StatusCenterCallbacks callbacks;
    callbacks.cancel = [&core](app::StatusTaskKey key) {
        const auto* record = core.statusCenter.Find(key);
        if (!record || !record->cancellable) return;
        if (key.source == app::StatusTaskSource::Operation) core.ops.CancelCurrent(key.id);
        else if (!core.windows.Lookup(key.owner) ||
                 !PostMessageW(key.owner, WM_FILE_CONVERSION_CANCEL, static_cast<WPARAM>(key.id), 0)) return;
        if (core.statusCenter.MarkCancelling(key) && core.statusCenterWindow)
            core.statusCenterWindow->Update(core.statusCenter.records());
    };
    callbacks.operation_details = [&core](uint64_t task_id) {
        const auto status = core.ops.Status();
        if (!status.active || status.task_id != task_id) return;
        if (auto* current = core.windows.MostRecent()) PinAndShowOperationWindow(*current);
    };
    callbacks.clear_completed = [&core] {
        if (!core.statusCenter.ClearCompleted()) return;
        if (core.statusCenterWindow) core.statusCenterWindow->Update(core.statusCenter.records());
        core.windows.ForEach([](HWND hwnd, AppState&) { InvalidateRect(hwnd, nullptr, FALSE); });
    };
    if (!core.statusCenterWindow->Create(state.hwnd, std::move(callbacks))) return;
    core.statusCenterWindow->SetTheme(state.darkMode, state.accentColor);
    core.statusCenterWindow->Update(core.statusCenter.records());
    POINT anchor{static_cast<LONG>(state.compositor.Width() - 148 * state.scale), static_cast<LONG>(state.renderer.TitleBarHeight())};
    ClientToScreen(state.hwnd, &anchor);
    core.statusCenterWindow->Show(anchor);
}

void ShowToolbarProperties(AppState& state) {
    const auto* tab = ActiveTab(state);
    if (!tab) return;
    if (tab->SelectedCount() > 0) DispatchMenuCommand(state, app::CmdProperties);
    else if (!tab->current_path.empty() && !fs::IsVirtualPath(tab->current_path))
        DispatchMenuCommand(state, app::CmdFolderProperties);
}

} // namespace pulse
