#pragma once

#include <windows.h>
#include <memory>
#include <string>

namespace pulse::app {

// Isolates foreground observation and external UIA calls in an owned process.
// The parent maintains its job and never waits indefinitely for the helper.
class NativeDialogFolderLink {
public:
    NativeDialogFolderLink();
    ~NativeDialogFolderLink();
    NativeDialogFolderLink(const NativeDialogFolderLink&) = delete;
    NativeDialogFolderLink& operator=(const NativeDialogFolderLink&) = delete;
    // A timeout retains the handle and stopped state; a new watcher must not
    // be created until a later call reports that the previous thread exited.
    bool Stop() noexcept;
    bool IsStopping() noexcept;

    struct Shared;
private:
    HANDLE job_ = nullptr;
    HANDLE process_ = nullptr;
    HANDLE stop_event_ = nullptr;
    HANDLE heartbeat_mapping_ = nullptr;
    HANDLE cleanup_event_ = nullptr;
    volatile LONG64* deadline_ = nullptr;
    void* recovery_view_ = nullptr;
    ULONGLONG stop_started_ = 0;
    std::wstring marker_;
};

int NativeDialogFolderLinkHelper(HANDLE parent, HANDLE stop_event, HANDLE heartbeat, const wchar_t* marker);

#ifdef PULSE_WITH_SELFTEST
// Only for a caller-owned native dialog on an isolated test desktop. The
// Call on a disposable worker thread with no windows or hooks; desktop is the
// test's private desktop. The returned value means navigation was issued;
// verify actual folder, filename, filter, focus and absence of acceptance.
bool NativeDialogFolderLinkNavigateForTest(HWND dialog, const std::wstring& folder,
                                            HDESK desktop, bool allow_nonforeground);
struct NativeDialogFolderLinkTestResult {
    bool first_issued = false;
    bool second_issued = false;
    LONG first_stage = -1;
    LONG second_stage = -1;
};
NativeDialogFolderLinkTestResult NativeDialogFolderLinkNavigateTwiceForTest(
    HWND dialog, const std::wstring& first, const std::wstring& second, HDESK desktop);
bool NativeDialogFolderLinkRecoverForTest(HWND dialog, HWND band, HWND toolbar, HWND edit,
    LONG stage, const std::wstring& previous, const std::wstring& attempted, HDESK desktop);
#endif

} // namespace pulse::app
