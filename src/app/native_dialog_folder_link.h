#pragma once

#include <windows.h>
#include <memory>
#include <string>

namespace pulse::app {

// Observes foreground transitions on a dedicated thread. It neither replaces
// native dialogs nor commits them. Stop must be called before app shutdown.
class NativeDialogFolderLink {
public:
    NativeDialogFolderLink();
    ~NativeDialogFolderLink();
    NativeDialogFolderLink(const NativeDialogFolderLink&) = delete;
    NativeDialogFolderLink& operator=(const NativeDialogFolderLink&) = delete;
    // A timeout retains the handle and stopped state; a new watcher must not
    // be created until a later call reports that the previous thread exited.
    bool Stop() noexcept;
    bool IsStopping() const noexcept;

    struct Shared;
private:
    std::shared_ptr<Shared> shared_;
    HANDLE thread_ = nullptr;
};

#ifdef PULSE_WITH_SELFTEST
// Only for a caller-owned native dialog on an isolated test desktop. The
// Call on a disposable worker thread with no windows or hooks; desktop is the
// test's private desktop. The returned value means navigation was issued;
// verify actual folder, filename, filter, focus and absence of acceptance.
bool NativeDialogFolderLinkNavigateForTest(HWND dialog, const std::wstring& folder,
                                            HDESK desktop, bool allow_nonforeground);
#endif

} // namespace pulse::app
