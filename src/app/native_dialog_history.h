#pragma once
#include "../common/utf8_file.h"
#include "../ui/folder_picker_model.h"

namespace pulse::app {
// Each mode owns a file; closing an Open dialog cannot overwrite Save history.
inline std::wstring DialogHistoryFile(const std::wstring& directory, ui::PickerMode mode) {
    if (directory.empty()) return {};
    return directory + L"\\picker-last-" + std::to_wstring(static_cast<int>(mode)) + L".txt";
}
inline bool DialogHistoryPathValid(const std::wstring& value) {
    if (value.empty() || value.size() > 32767 || value.find_first_of(L"\r\n\"") != std::wstring::npos ||
        value.find(L'\0') != std::wstring::npos) return false;
    return (value.size() >= 3 && ((value[0] >= L'A' && value[0] <= L'Z') ||
        (value[0] >= L'a' && value[0] <= L'z')) && value[1] == L':' && value[2] == L'\\') ||
        (value.starts_with(L"\\\\") && !value.starts_with(L"\\\\.\\"));
}
inline std::wstring ReadDialogHistory(const std::wstring& directory, ui::PickerMode mode) {
    std::wstring value;
    const auto file = DialogHistoryFile(directory, mode);
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (file.empty() || !GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &info) ||
        info.nFileSizeHigh || info.nFileSizeLow > 131072 || !ReadUtf8File(file, value) ||
        !DialogHistoryPathValid(value)) return {};
    // Existence is checked by the asynchronous picker loader, including UNC paths.
    return value;
}
inline bool WriteDialogHistory(const std::wstring& directory, ui::PickerMode mode,
                               const std::wstring& value) {
    if (directory.empty() || !DialogHistoryPathValid(value)) return false;
    const auto file = DialogHistoryFile(directory, mode);
    const auto temporary = file + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetCurrentThreadId());
    if (!WriteUtf8FileAtomic(temporary, value)) return false;
    const bool saved = MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!saved) DeleteFileW(temporary.c_str());
    return saved;
}
}
