#pragma once

#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulse::app {

enum class FileConversionEventKind { Capabilities, Started, Progress, Completed, Failed, Stopped };

struct FileConversionRoute {
    std::wstring source;
    std::vector<std::wstring> steps;
};

struct FileConversionTarget {
    std::wstring id;
    std::wstring label;
    std::wstring extension;
    std::vector<FileConversionRoute> routes;
};

struct FileConversionFileResult {
    std::wstring source;
    std::wstring state;
    double progress = 0.0;
    std::vector<std::wstring> outputs;
    std::wstring error_key;
    std::wstring error;
};

struct FileConversionEvent {
    FileConversionEventKind kind = FileConversionEventKind::Stopped;
    uint64_t generation = 0;
    std::vector<FileConversionTarget> targets;
    std::vector<std::wstring> inputs;  // immutable request snapshot, never current UI selection
    std::wstring target;
    std::wstring state;
    uint32_t file_count = 0;
    double progress = 0.0;
    std::vector<FileConversionFileResult> files;
    std::vector<std::wstring> limitations;
    DWORD error = ERROR_SUCCESS;
    std::wstring error_key;
    std::wstring error_text;
};

// One session per Pulse window. UI methods enqueue requests or take events;
// validation, process startup, pipe I/O, JSON parsing and cleanup are background work.
class FileConversionIntegration {
public:
    FileConversionIntegration() = default;
    ~FileConversionIntegration();
    FileConversionIntegration(const FileConversionIntegration&) = delete;
    FileConversionIntegration& operator=(const FileConversionIntegration&) = delete;

    bool Initialize(HWND owner, UINT notify_message);
    uint64_t Capabilities(std::vector<std::wstring> inputs);
    uint64_t Start(std::vector<std::wstring> inputs, std::wstring target,
                   std::wstring output_directory, int quality = 90);
    void Cancel();   // abort this job and its complete native job tree
    void Close();    // cancel pending requests and close this window's host asynchronously
    void Shutdown(); // retire owner/tokens first, then asynchronously terminate all engines

    bool busy() const noexcept;
    bool running() const noexcept;
    // notify_message: wParam = 0, lParam = opaque token; never a heap pointer.
    // Capabilities have their own generation; querying a menu cannot retire a running job.
    bool TakeEvent(UINT_PTR token, FileConversionEvent& event);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace pulse::app
