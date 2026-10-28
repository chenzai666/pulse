#pragma once

#include "../ui/folder_picker_model.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace pulse::app {

enum class NativeDialogKind { Open, Save, Folder };

struct NativeFileDialog {
    HWND window = nullptr;
    DWORD process_id = 0;
    NativeDialogKind kind = NativeDialogKind::Open;
    std::wstring title;
    std::wstring initial_path;
    std::wstring filename;
    std::vector<ui::PickerFilter> filters;
    size_t filter_index = 0;
    bool allow_multiselect = false;
    DWORD window_thread_id = 0;
    FILETIME process_created{};
    HWND filename_window = nullptr;
    HWND confirm_window = nullptr;
    bool legacy_folder = false;
};

// 专用工作线程须在初始化 COM 前切换到目标桌面；原生窗口须保持存在。
// 仅预检结构与身份，允许顶层尚未显示；模式、文件名和过滤器仍须完整 Inspect。
bool CanConcealNativeFileDialog(HWND window, NativeFileDialog& dialog, std::wstring& reason);
bool InspectNativeFileDialog(HWND window, NativeFileDialog& dialog, std::wstring& reason);
bool NativeDialogStillMatches(const NativeFileDialog& dialog);

// true 仅表示已向原生对话框发送确认请求；宿主仍负责路径和覆盖校验。
bool CommitNativeDialogSelection(const NativeFileDialog& dialog,
    const std::vector<std::wstring>& paths, size_t filter_index, std::wstring& reason,
    const std::function<bool()>& cancelled = {}, bool* confirmation_attempted = nullptr);
bool CancelNativeDialog(const NativeFileDialog& dialog, std::wstring& reason,
    const std::function<bool()>& cancelled = {});

} // namespace pulse::app
