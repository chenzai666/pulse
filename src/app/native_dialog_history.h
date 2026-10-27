#pragma once
#include "../common/utf8_file.h"
#include "../common/runtime_log.h"
#include "../ui/folder_picker_model.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace pulse::app {
inline std::wstring DialogHistoryFile(const std::wstring& directory, ui::PickerMode mode) {
    if (directory.empty()) return {};
    return directory + L"\\picker-last-" + std::to_wstring(static_cast<int>(mode)) + L".txt";
}
inline std::wstring SharedDialogHistoryFile(const std::wstring& directory) {
    return directory.empty() ? std::wstring() : directory + L"\\picker-last.txt";
}
inline bool DialogHistoryPathValid(const std::wstring& value) {
    if (value.empty() || value.size() > 32767 || value.find_first_of(L"\r\n\"") != std::wstring::npos ||
        value.find(L'\0') != std::wstring::npos) return false;
    return (value.size() >= 3 && ((value[0] >= L'A' && value[0] <= L'Z') ||
        (value[0] >= L'a' && value[0] <= L'z')) && value[1] == L':' && value[2] == L'\\') ||
        (value.starts_with(L"\\\\") && !value.starts_with(L"\\\\.\\"));
}
inline std::wstring ReadDialogHistoryFile(const std::wstring& file, FILETIME* modified = nullptr) {
    std::wstring value;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (file.empty() || !GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || info.nFileSizeHigh || info.nFileSizeLow > 131072 ||
        !ReadUtf8File(file, value) || !DialogHistoryPathValid(value)) return {};
    if (modified) *modified = info.ftLastWriteTime;
    return value;
}
inline std::wstring ReadDialogHistory(const std::wstring& directory, ui::PickerMode mode) {
    auto value = ReadDialogHistoryFile(SharedDialogHistoryFile(directory));
    if (!value.empty()) return value;
    // 兼容旧版分模式记录，首次迁移使用最近写入的一项。
    FILETIME newest{};
    for (const auto candidate : {mode, ui::PickerMode::Folder, ui::PickerMode::Image,
                                ui::PickerMode::File, ui::PickerMode::Save}) {
        FILETIME modified{};
        auto previous = ReadDialogHistoryFile(DialogHistoryFile(directory, candidate), &modified);
        if (!previous.empty() && (value.empty() || CompareFileTime(&modified, &newest) > 0)) {
            value = std::move(previous);
            newest = modified;
        }
    }
    // 目录存在性由异步 loader 检查，避免 UNC 路径阻塞窗口启动。
    return value;
}
inline bool WriteDialogHistory(const std::wstring& directory, ui::PickerMode mode,
                               const std::wstring& value, DWORD* error = nullptr) {
    (void)mode;
    const auto fail = [error](DWORD result) {
        if (error) *error = result;
        SetLastError(result);
        return false;
    };
    if (directory.empty() || !DialogHistoryPathValid(value)) return fail(ERROR_INVALID_NAME);
    std::vector<uint8_t> bytes;
    if (!EncodeUtf8Bytes(value, bytes)) return fail(GetLastError() ? GetLastError() : ERROR_NO_UNICODE_TRANSLATION);
    const auto file = SharedDialogHistoryFile(directory);
    const auto temporary = file + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetCurrentThreadId()) + L".tmp";
    HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (output == INVALID_HANDLE_VALUE) return fail(GetLastError());
    DWORD written = 0;
    DWORD result = ERROR_SUCCESS;
    if (!WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) result = GetLastError();
    else if (written != bytes.size()) result = ERROR_WRITE_FAULT;
    else if (!FlushFileBuffers(output)) result = GetLastError();
    if (!CloseHandle(output) && result == ERROR_SUCCESS) result = GetLastError();
    if (result == ERROR_SUCCESS && !MoveFileExW(temporary.c_str(), file.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) result = GetLastError();
    if (result != ERROR_SUCCESS) {
        DeleteFileW(temporary.c_str());
        return fail(result);
    }
    if (error) *error = ERROR_SUCCESS;
    return true;
}

// 成功访问即交给本地写入线程，关闭前排空；磁盘刷新不阻塞浏览操作。
class DialogHistoryWriter {
public:
    DialogHistoryWriter(std::wstring directory, ui::PickerMode mode)
        : directory_(std::move(directory)), mode_(mode), worker_([this] { Run(); }) {}
    ~DialogHistoryWriter() {
        Flush();
        { std::lock_guard lock(mutex_); stopping_ = true; }
        changed_.notify_one();
        worker_.join();
    }
    DialogHistoryWriter(const DialogHistoryWriter&) = delete;
    DialogHistoryWriter& operator=(const DialogHistoryWriter&) = delete;

    void Visit(const std::wstring& path, uint64_t phase = 0) {
        { std::lock_guard lock(mutex_); pending_ = path; phase_ = phase; ++queued_; }
        changed_.notify_one();
    }
    bool Flush(DWORD* error = nullptr) {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] { return completed_ == queued_; });
        if (error) *error = error_;
        return error_ == ERROR_SUCCESS;
    }

private:
    void Run() noexcept {
        for (;;) {
            std::wstring path;
            uint64_t generation = 0, phase = 0;
            DWORD error = ERROR_SUCCESS;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [this] { return stopping_ || queued_ != completed_; });
                if (stopping_) return;
                generation = queued_; phase = phase_;
                try { path = pending_; } catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
            }
            bool saved = false;
            if (!error) {
                try { saved = WriteDialogHistory(directory_, mode_, path, &error); }
                catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
            }
            diagnostics::runtime::Event("native_dialog_history_write", {{"mode", static_cast<uint64_t>(mode_)},
                {"phase", phase}, {"saved", saved ? 1u : 0u}, {"error", error}},
                saved ? diagnostics::runtime::Level::Info : diagnostics::runtime::Level::Warning);
            {
                std::lock_guard lock(mutex_);
                completed_ = generation;
                error_ = error;
            }
            changed_.notify_all();
        }
    }
    const std::wstring directory_;
    const ui::PickerMode mode_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::wstring pending_;
    uint64_t queued_ = 0, completed_ = 0, phase_ = 0;
    DWORD error_ = ERROR_SUCCESS;
    bool stopping_ = false;
    std::thread worker_;
};
}
