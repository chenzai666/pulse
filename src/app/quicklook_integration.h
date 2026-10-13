#pragma once

#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>

namespace pulse::app {

enum class QuickLookEventKind { Ready, Visibility, Failed, Navigate, Stopped };

struct QuickLookEvent {
    QuickLookEventKind kind = QuickLookEventKind::Stopped;
    uint64_t generation = 0;
    std::wstring path;
    DWORD error = ERROR_SUCCESS;
    bool preview_visible = false;
    int window_count = 0;
    int direction = 0;
};

// One controller per Pulse window. Methods enqueue work; process startup,
// filesystem validation, pipe I/O and process cleanup run off the UI thread.
class QuickLookIntegration {
public:
    QuickLookIntegration() = default;
    ~QuickLookIntegration();
    QuickLookIntegration(const QuickLookIntegration&) = delete;
    QuickLookIntegration& operator=(const QuickLookIntegration&) = delete;

    bool Initialize(HWND owner, UINT notify_message);
    bool Toggle(const std::wstring& path);
    bool Switch(const std::wstring& path);
    // Closes the current preview, preserving explicitly pinned windows.
    void Close();
    // Cancels the session and closes all of this owner's windows asynchronously.
    void Shutdown();

    // Active includes a requested preview whose startup/command is pending.
    bool active() const noexcept;
    bool pending() const noexcept;
    bool running() const noexcept;
    std::wstring path() const;
    uint64_t generation() const noexcept;

    // notify_message uses wParam = 0 and lParam = token. Tokens own no pointers.
    // Obsolete requests and notifications from a retired owner are discarded.
    bool TakeEvent(UINT_PTR token, QuickLookEvent& event);

private:
    struct State;
    std::shared_ptr<State> state_;
    bool Submit(const std::wstring& path, bool toggle);
};

} // namespace pulse::app
