#pragma once

// Pulse's own open dialog: a modal window that replaces IFileOpenDialog for
// folders, pictures and files. It hosts the main window's renderer in embedded
// mode, so the address row, sidebar and file list are the same code as the
// main UI; only the title strip and the footer are the picker's own.
// Folders are read off the UI thread.

#include "folder_picker_model.h"

#include <d2d1.h>
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pulse::ui {

// A sidebar row: a known folder or a place the caller
// added (IFileDialog::AddPlace).
struct PickerPlace {
    std::wstring label;
    std::wstring path;
    std::wstring glyph;
};

struct FolderPickerSpec {
    PickerMode mode = PickerMode::Folder;
    std::wstring title;         // empty: "Choose a folder" / "Choose background image" / "Open" / "Save"
    std::wstring initial_path;  // empty: where the last pick of this mode was made
    std::wstring fallback_path;
    std::function<void(const std::wstring&)> on_directory_changed;
    std::vector<PickerFilter> filters;
    size_t filter_index = 0;
    std::wstring filename;
    std::wstring default_extension;
    std::function<void(HWND)> on_ready;
    bool allow_multiselect = false;
    bool show_hidden = false;
    // The main window's list style, so rows look the same in both places.
    bool selection_outline = false;
    float row_height_dip = 0.0f;          // 0: the renderer's default
    bool list_smart_date = true;
    bool list_zebra_rows = true;
    bool list_size_bar = false;
    bool thumbnail_badges = true;
    uint32_t details_columns = 0;         // 0: the renderer's default columns
    std::vector<PickerPlace> places;      // extra quick-access rows, shown first
};

struct FilePickerResult {
    std::vector<std::wstring> paths;
    size_t filter_index = 0;
};

bool ShowFilePicker(HWND owner, const FolderPickerSpec& spec, bool dark,
                    D2D1_COLOR_F accent, FilePickerResult& result);

// Returns true and sets `path` when the user picked something.
bool ShowFolderPicker(HWND owner, const FolderPickerSpec& spec, bool dark,
                      D2D1_COLOR_F accent, std::wstring& path);

} // namespace pulse::ui
