#include "listary_search_entry.h"
#include "global_search_controller.h"
#include "app_worker.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <objbase.h>
#include <cstdint>
#include <cwchar>
#include <string_view>
#include <atomic>
#include <memory>
#include <utility>

namespace pulse {
namespace {

struct Request {
    HWND window = nullptr;
    uintptr_t token = 0;
    std::atomic_bool cancelled{false};
    std::atomic_int result{0}; // 0 pending, 1 opened, 2 failed
#ifdef PULSE_WITH_SELFTEST
    std::function<bool()> test_launcher;
#endif
};

// UI-thread-owned. Workers retain only their immutable request identity and
// atomic result/cancellation state, never AppState or its window state.
std::shared_ptr<Request> pending;
uintptr_t next_token = 0;
#ifdef PULSE_WITH_SELFTEST
std::function<bool()> test_launcher;
#endif

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};

bool OpenListary(const Request& request) {
    if (request.cancelled) return false;
    ComApartment com;
    if (FAILED(com.result)) return false;
    // Checking the protocol association first avoids an app-selection dialog
    // on machines without Listary. Do not search PATH or start a bundled copy.
    wchar_t executable[32768]{};
    DWORD chars = ARRAYSIZE(executable);
    if (AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_EXECUTABLE,
            L"listary", L"open", executable, &chars) != S_OK) return false;
    const std::wstring_view path(executable);
    const auto slash = path.find_last_of(L"\\/");
    if (path.size() < 4 || !((path[0] >= L'A' && path[0] <= L'Z') ||
            (path[0] >= L'a' && path[0] <= L'z')) || path[1] != L':' ||
        (path[2] != L'\\' && path[2] != L'/') || slash == std::wstring_view::npos ||
        _wcsicmp(executable + slash + 1, L"Listary.exe") != 0) return false;

    // Official Listary 6.3+ URI; Listary owns the UI, index and hotkeys.
    // https://help.listary.com/changelog (6.3.0.78)
    if (request.cancelled) return false;
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    // The standalone launcher needs no parent; an owner may close while this
    // worker is inside ShellExecuteEx, so never pass its borrowed HWND.
    launch.lpVerb = L"open";
    launch.lpFile = L"listary://show-launcher";
    launch.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&launch) != FALSE;
}

} // namespace

bool QueueListaryGlobalSearch(app::WorkerPool& worker, HWND owner) {
    if (!owner) return false;
    if (pending && !pending->cancelled) return true;
    try {
        auto request = std::make_shared<Request>();
        request->window = owner;
        if (++next_token == 0) ++next_token;
        request->token = next_token;
#ifdef PULSE_WITH_SELFTEST
        request->test_launcher = test_launcher;
#endif
        pending = request;
        worker.EnqueueIo([request] {
            if (request->cancelled) return;
            bool opened = false;
            try {
#ifdef PULSE_WITH_SELFTEST
                opened = request->test_launcher ? request->test_launcher() : OpenListary(*request);
#else
                opened = OpenListary(*request);
#endif
            } catch (...) {}
            request->result.store(opened ? 1 : 2, std::memory_order_release);
            if (!request->cancelled)
                PostMessageW(request->window, kListarySearchResultMessage, request->token, 0);
        });
        return true;
    } catch (...) {
        if (pending) pending->cancelled = true;
        pending.reset();
        return false;
    }
}

bool TakeListaryGlobalSearchResult(HWND owner, WPARAM token, bool& opened) {
    if (!pending || pending->window != owner || pending->token != token || pending->cancelled)
        return false;
    const int result = pending->result.load(std::memory_order_acquire);
    if (!result) return false;
    opened = result == 1;
    pending.reset();
    return true;
}

void CancelListaryGlobalSearch(HWND owner) {
    if (!pending || pending->window != owner) return;
    pending->cancelled = true;
    pending.reset();
}

#ifdef PULSE_WITH_SELFTEST
void SetListarySearchEntryTestLauncher(std::function<bool()> launcher) {
    test_launcher = std::move(launcher);
}
#endif

} // namespace pulse
