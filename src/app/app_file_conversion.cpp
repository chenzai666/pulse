#include "app_file_conversion.h"
#include "app_commands.h"
#include "app_internal.h"
#include "app_status_center.h"
#include "sidebar_navigation.h"
#include "window_factory.h"
#include "../common/edition.h"
#include "../common/localization.h"
#include "../ui/folder_picker_dialog.h"

#include <algorithm>

namespace pulse {
namespace {
constexpr int kTargetCommandBase = 31000;
constexpr size_t kMaxMenuTargets = 64;
constexpr UINT_PTR kChooseTimer = 0x50434645;

std::wstring ConversionTitle() {
    return l10n::Pick(L"格式转换", L"Format conversion");
}

void ShowFailure(AppState& state, const std::wstring& message) {
    state.notification_toast.ShowError(state.hwnd, ConversionTitle(), message);
}

void ShowProgress(AppState& state, double progress, bool force = false) {
    auto& ui_state = state.fileConversionUi;
    const auto now = GetTickCount64();
    const int percent = static_cast<int>(std::clamp(progress, 0.0, 1.0) * 100.0);
    if (!force && (now - ui_state.last_progress_tick < 350 || percent == ui_state.progress_percent)) return;
    ui_state.last_progress_tick = now;
    ui_state.progress_percent = percent;
    const auto text = std::wstring(l10n::Pick(L"正在转换为 ", L"Converting to ")) + ui_state.target_label;
    const auto detail = std::to_wstring(percent) + L"% · " + std::to_wstring(ui_state.input_count) +
        l10n::Pick(L" 个文件", L" files");
    state.notification_toast.ShowSnack(state.hwnd, ui::SnackIcon::Suggest, text, detail,
        {{l10n::Pick(L"取消", L"Cancel"), WM_FILE_CONVERSION_CANCEL}}, {}, 3600000);
}

bool PickOutputFolder(AppState& state, const std::vector<std::wstring>& inputs, std::wstring& output) {
    ui::FolderPickerSpec spec;
    spec.mode = ui::PickerMode::Folder;
    spec.title = l10n::Pick(L"选择转换后的文件保存位置", L"Choose where to save converted files");
    if (!inputs.empty()) spec.initial_path = fs::ParentPath(inputs.front());
    spec.selection_outline = state.appPrefs.list_selection_outline;
    spec.row_height_dip = static_cast<float>(app::EffectiveRowHeightDip(
        state.appPrefs.row_height, state.appPrefs.ui_font_scale));
    spec.list_smart_date = state.appPrefs.list_smart_date;
    spec.list_zebra_rows = state.appPrefs.list_zebra_rows;
    spec.list_size_bar = state.appPrefs.list_size_bar;
    spec.thumbnail_badges = state.appPrefs.list_thumbnail_badges;
    spec.details_columns = state.appPrefs.details_columns;
    spec.show_hidden = state.appPrefs.show_hidden_files;
    return ui::ShowFolderPicker(state.hwnd, spec, state.darkMode, state.accentColor, output);
}

void ChooseTarget(AppState& state, app::FileConversionEvent event) {
    const HWND owner = state.hwnd;
    WindowStateLease lease(state);
    auto& ui_state = state.fileConversionUi;
    if (event.generation != ui_state.capabilities_generation || !ui_state.choosing) return;
    struct SelectionScope {
        bool& flag;
        explicit SelectionScope(bool& value) : flag(value) { flag = true; }
        ~SelectionScope() { flag = false; }
    } selecting(ui_state.selecting);
    ui_state.choosing = false;
    state.notification_toast.DismissFor(state.hwnd, WM_FILE_CONVERSION_CANCEL);
    const auto* current = ActiveTab(state);
    if (state.fileConversion.busy() || !IsWindowEnabled(state.hwnd) ||
        GetAncestor(GetForegroundWindow(), GA_ROOT) != state.hwnd || !current ||
        SelectedFullPaths(*current) != event.inputs) {
        if (!state.fileConversion.busy()) state.fileConversion.Close();
        return;
    }
    if (event.targets.empty()) {
        ShowFailure(state, l10n::Pick(L"所选文件没有共同的可转换格式。请按文件类型分别选择。",
                                     L"These files have no common conversion format. Select one file type at a time."));
        state.fileConversion.Close();
        return;
    }
    ui::FluentMenu format_menu;
    if (!format_menu.Create(owner, &state.compositor, state.scale)) { state.fileConversion.Close(); return; }
    format_menu.SetTheme(state.darkMode, state.accentColor);
    format_menu.SetRowHeightDip(static_cast<float>(app::MenuRowHeightDip(
        app::EffectiveRowHeightDip(state.appPrefs.row_height, state.appPrefs.ui_font_scale))));
    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem heading;
    heading.text = std::to_wstring(event.inputs.size()) + l10n::Pick(L" 个文件 · 选择输出格式", L" files · Output format");
    heading.enabled = false;
    heading.separator_after = true;
    items.push_back(std::move(heading));
    const size_t count = (std::min)(event.targets.size(), kMaxMenuTargets);
    for (size_t index = 0; index < count; ++index) {
        ui::FluentMenuItem item;
        item.command = kTargetCommandBase + static_cast<int>(index);
        item.text = event.targets[index].label.empty() ? event.targets[index].id : event.targets[index].label;
        item.glyph = L"\xE8A5";
        if (event.targets[index].id == L"pdf")
            item.tooltip = l10n::Pick(L"文档转换支持常见内容；复杂公式、图表和版式可能发生变化。",
                                     L"Common document content is supported. Complex formulas, charts and layout may change.");
        items.push_back(std::move(item));
    }
    items.back().separator_after = true;
    ui::FluentMenuItem hint;
    hint.text = l10n::Pick(L"输出新文件，保留原文件", L"Creates new files and keeps originals");
    hint.enabled = false;
    items.push_back(std::move(hint));
    state.context_menu.Close();
    ui_state.active_menu = &format_menu;
    const int selected = format_menu.TrackPopup(ui_state.menu_point, std::move(items)) - kTargetCommandBase;
    ui_state.active_menu = nullptr;
    if (GetAppState(owner) != &state) return;
    if (event.generation != ui_state.capabilities_generation) return;
    if (selected < 0 || static_cast<size_t>(selected) >= count || state.fileConversion.busy()) {
        if (!state.fileConversion.busy()) state.fileConversion.Close();
        return;
    }
    std::wstring output;
    const bool picked = PickOutputFolder(state, event.inputs, output);
    if (GetAppState(owner) != &state) return;
    if (event.generation != ui_state.capabilities_generation || state.fileConversion.busy()) return;
    if (!picked) {
        state.fileConversion.Close();
        return;
    }
    const auto& target = event.targets[static_cast<size_t>(selected)];
    ui_state.target_label = target.label.empty() ? target.id : target.label;
    ui_state.input_count = event.inputs.size();
    ui_state.progress_percent = 0;
    ui_state.last_progress_tick = 0;
    ui_state.completed_outputs.clear();
    const auto conversion_inputs = event.inputs;
    ui_state.job_generation = state.fileConversion.Start(std::move(event.inputs), target.id, std::move(output));
    if (ui_state.job_generation) {
        RecordFileConversionQueued(state, ui_state.job_generation, conversion_inputs, target.id);
        ShowProgress(state, 0.0, true);
    }
    else ShowFailure(state, l10n::Pick(L"无法启动转换任务。", L"Unable to start conversion."));
}

void Complete(AppState& state, const app::FileConversionEvent& event) {
    size_t done = 0, cancelled = 0, failed = 0;
    auto& outputs = state.fileConversionUi.completed_outputs;
    outputs.clear();
    for (const auto& file : event.files) {
        outputs.insert(outputs.end(), file.outputs.begin(), file.outputs.end());
        if (file.state == L"done") {
            ++done;
        } else if (file.state == L"cancelled") ++cancelled;
        else ++failed;
    }
    state.notification_toast.DismissFor(state.hwnd, WM_FILE_CONVERSION_CANCEL);
    const auto text = std::wstring(l10n::Pick(L"转换完成：", L"Conversion complete: ")) + std::to_wstring(done) +
        l10n::Pick(L" 个成功", L" succeeded");
    std::wstring detail;
    if (failed) detail += std::to_wstring(failed) + l10n::Pick(L" 个失败", L" failed");
    if (cancelled) {
        if (!detail.empty()) detail += L" · ";
        detail += std::to_wstring(cancelled) + l10n::Pick(L" 个取消", L" cancelled");
    }
    if (!outputs.empty()) {
        state.notification_toast.ShowSnack(state.hwnd, ui::SnackIcon::Suggest, text, detail,
            {{l10n::Pick(L"查看输出", L"Show output"), WM_FILE_CONVERSION_OPEN_OUTPUT}}, {}, 15000);
    } else {
        const auto error = std::find_if(event.files.begin(), event.files.end(), [](const auto& file) {
            return !file.error.empty();
        });
        ShowFailure(state, error != event.files.end() ? error->error :
            l10n::Pick(L"没有生成输出文件，原文件已保留。", L"No output was generated. Originals are intact."));
    }
    state.fileConversionUi.job_generation = 0;
    state.fileConversion.Close();
}
}

void AppendFileConversionCommand(AppState& state, std::vector<ui::FluentMenuItem>& items) {
    if (!edition::kCustom || state.safeMode || state.isolatedTest) return;
    const auto* tab = ActiveTab(state);
    if (!tab || !tab->snapshot || IsRecycleTab(tab) || tab->SelectedCount() == 0) return;
    for (const int index : tab->SelectedIndices()) {
        if (index < 0 || index >= static_cast<int>(tab->EntryCount()) ||
            tab->EntryAt(static_cast<size_t>(index)).is_dir) return;
    }
    ui::FluentMenuItem item;
    item.command = state.fileConversion.busy() ? app::CmdFileConversionCancel : app::CmdFileConversion;
    item.text = state.fileConversion.busy() ? l10n::Pick(L"取消格式转换", L"Cancel conversion") :
                                            l10n::Pick(L"转换格式…", L"Convert format…");
    item.glyph = L"\xE8AB";
    items.insert(items.begin() + (std::min)(size_t{2}, items.size()), std::move(item));
}

bool HandleFileConversionCommand(AppState& state, int command,
                                 const std::vector<std::wstring>& inputs, POINT menu_point) {
    if (command == app::CmdFileConversionCancel) {
        state.fileConversionUi.choosing = false;
        state.fileConversionUi.capabilities_generation = 0;
        state.fileConversionUi.pending_choice.reset();
        KillTimer(state.hwnd, kChooseTimer);
        if (state.fileConversionUi.active_menu) state.fileConversionUi.active_menu->Dismiss();
        // A terminal event may already be queued even though busy() is false.
        if (state.fileConversionUi.job_generation) state.fileConversion.Cancel();
        else state.fileConversion.Close();
        return true;
    }
    if (command != app::CmdFileConversion) return false;
    if (inputs.empty() || state.fileConversion.busy() || state.fileConversionUi.choosing ||
        state.fileConversionUi.selecting || !IsWindowEnabled(state.hwnd)) return true;
    if (inputs.size() > 128) {
        ShowFailure(state, l10n::Pick(L"一次最多转换 128 个文件，请分批选择。",
                                     L"Convert up to 128 files at a time. Split this selection into batches."));
        return true;
    }
    if (!state.fileConversion.Initialize(state.hwnd, WM_FILE_CONVERSION_EVENT)) {
        ShowFailure(state, l10n::Pick(L"转换组件无法启动。", L"Unable to start conversion component."));
        return true;
    }
    auto& ui_state = state.fileConversionUi;
    ui_state.menu_point = menu_point;
    ui_state.choosing = true;
    ui_state.capabilities_generation = state.fileConversion.Capabilities(inputs);
    if (!ui_state.capabilities_generation) {
        ui_state.choosing = false;
        ShowFailure(state, l10n::Pick(L"无法读取所选文件的转换能力。", L"Unable to inspect conversion formats."));
    } else {
        state.notification_toast.Show(state.hwnd, ConversionTitle(),
            l10n::Pick(L"正在读取可用格式…", L"Loading available formats…"), true, WM_FILE_CONVERSION_CANCEL);
    }
    return true;
}

bool HandleFileConversionMessage(AppState& state, UINT message, WPARAM wparam, LPARAM token) {
    if (message == WM_TIMER && wparam == kChooseTimer) {
        PostMessageW(state.hwnd, WM_FILE_CONVERSION_CHOOSE, 0, 0);
        return true;
    }
    if (message == WM_FILE_CONVERSION_CANCEL && wparam != 0 && wparam != state.fileConversionUi.job_generation)
        return true;
    if (message == WM_FILE_CONVERSION_CANCEL)
        return HandleFileConversionCommand(state, app::CmdFileConversionCancel, {}, {});
    if (message == WM_FILE_CONVERSION_OPEN_OUTPUT) {
        QueueDroppedPathsInTabs(state, state.fileConversionUi.completed_outputs);
        return true;
    }
    if (message == WM_FILE_CONVERSION_CHOOSE) {
        auto& ui_state = state.fileConversionUi;
        if (!ui_state.pending_choice) return true;
        if ((state.menu && state.menu->IsOpen()) || ui_state.active_menu) {
            SetTimer(state.hwnd, kChooseTimer, 100, nullptr);
            return true;
        }
        KillTimer(state.hwnd, kChooseTimer);
        auto event = std::move(*ui_state.pending_choice);
        ui_state.pending_choice.reset();
        ChooseTarget(state, std::move(event));
        return true;
    }
    if (message != WM_FILE_CONVERSION_EVENT) return false;
    app::FileConversionEvent event;
    if (!state.fileConversion.TakeEvent(static_cast<UINT_PTR>(token), event)) return true;
    RecordFileConversionStatus(state, event);
    if (event.kind == app::FileConversionEventKind::Capabilities) {
        if (event.generation == state.fileConversionUi.capabilities_generation && state.fileConversionUi.choosing) {
            state.fileConversionUi.pending_choice = std::move(event);
            PostMessageW(state.hwnd, WM_FILE_CONVERSION_CHOOSE, 0, 0);
        }
    }
    else if (event.kind == app::FileConversionEventKind::Failed &&
             event.generation == state.fileConversionUi.capabilities_generation && state.fileConversionUi.choosing) {
        state.fileConversionUi.choosing = false;
        ShowFailure(state, event.error_text.empty() ?
            l10n::Pick(L"无法读取转换格式，请检查转换组件。", L"Unable to read conversion formats. Check the conversion component.") :
            event.error_text);
        state.fileConversion.Close();
    } else if (event.generation == state.fileConversionUi.job_generation) {
        switch (event.kind) {
        case app::FileConversionEventKind::Started: ShowProgress(state, 0.0, true); break;
        case app::FileConversionEventKind::Progress: ShowProgress(state, event.progress); break;
        case app::FileConversionEventKind::Completed: Complete(state, event); break;
        case app::FileConversionEventKind::Failed:
            ShowFailure(state, event.error_text.empty() ?
                l10n::Pick(L"转换失败，原文件已保留。", L"Conversion failed. Originals are intact.") : event.error_text);
            state.fileConversionUi.job_generation = 0;
            state.fileConversion.Close();
            break;
        case app::FileConversionEventKind::Stopped:
            state.notification_toast.ShowSnack(state.hwnd, ui::SnackIcon::Suggest,
                l10n::Pick(L"转换已取消", L"Conversion cancelled"),
                l10n::Pick(L"原文件和已完成的输出保留", L"Originals and completed output are kept"), {});
            state.fileConversionUi.job_generation = 0;
            state.fileConversion.Close();
            break;
        default: break;
        }
    }
    return true;
}

void ShutdownFileConversion(AppState& state) {
    state.fileConversionUi.choosing = false;
    state.fileConversionUi.capabilities_generation = 0;
    state.fileConversionUi.pending_choice.reset();
    KillTimer(state.hwnd, kChooseTimer);
    if (state.fileConversionUi.active_menu) state.fileConversionUi.active_menu->Dismiss();
    state.fileConversion.Shutdown();
}
}
