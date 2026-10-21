#define PULSE_EXPLORER_TAKEOVER_CONNECTION_TEST 1
#include "../app/explorer_window_takeover.cpp"

#include <olectl.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

using Microsoft::WRL::ComPtr;

int failures = 0;

void Check(bool passed, const char* label) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    if (!passed) ++failures;
}

struct ConnectionStats {
    unsigned count_calls = 0;
    unsigned item_calls = 0;
    unsigned advises = 0;
    unsigned unadvises = 0;
    unsigned invalid_unadvises = 0;
};

class FakeShellWindows final : public IShellWindows, public IConnectionPointContainer, public IConnectionPoint {
public:
    explicit FakeShellWindows(ConnectionStats& stats) : stats_(stats) {}
    HRESULT count_result = S_OK;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDispatch || iid == IID_IShellWindows)
            *object = static_cast<IShellWindows*>(this);
        else if (iid == IID_IConnectionPointContainer)
            *object = static_cast<IConnectionPointContainer*>(this);
        else if (iid == IID_IConnectionPoint)
            *object = static_cast<IConnectionPoint*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE get_Count(long* count) override {
        ++stats_.count_calls;
        if (!count) return E_POINTER;
        *count = 0;
        return count_result;
    }
    HRESULT STDMETHODCALLTYPE Item(VARIANT, IDispatch**) override { ++stats_.item_calls; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE _NewEnum(IUnknown**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Register(IDispatch*, long, int, long*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RegisterPending(long, VARIANT*, VARIANT*, int, long*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Revoke(long) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnNavigate(long, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnActivated(long, VARIANT_BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FindWindowSW(VARIANT*, VARIANT*, int, long*, int, IDispatch**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnCreated(long, IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ProcessAttachDetach(VARIANT_BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumConnectionPoints(IEnumConnectionPoints**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FindConnectionPoint(REFIID iid, IConnectionPoint** point) override {
        if (!point) return E_POINTER;
        *point = nullptr;
        if (iid != DIID_DShellWindowsEvents) return CONNECT_E_NOCONNECTION;
        *point = static_cast<IConnectionPoint*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetConnectionInterface(IID* iid) override {
        if (!iid) return E_POINTER;
        *iid = DIID_DShellWindowsEvents;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetConnectionPointContainer(IConnectionPointContainer** container) override {
        if (!container) return E_POINTER;
        *container = static_cast<IConnectionPointContainer*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Advise(IUnknown* sink, DWORD* cookie) override {
        if (!sink || !cookie) return E_POINTER;
        *cookie = 0;
        if (sink_) return CONNECT_E_ADVISELIMIT;
        const HRESULT result = sink->QueryInterface(DIID_DShellWindowsEvents,
            reinterpret_cast<void**>(sink_.GetAddressOf()));
        if (FAILED(result)) return result;
        ++stats_.advises;
        *cookie = kCookie;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Unadvise(DWORD cookie) override {
        if (cookie != kCookie || !sink_) { ++stats_.invalid_unadvises; return CONNECT_E_NOCONNECTION; }
        ++stats_.unadvises;
        sink_.Reset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumConnections(IEnumConnections**) override { return E_NOTIMPL; }
    bool HasSubscriber() const { return sink_ != nullptr; }

private:
    static constexpr DWORD kCookie = 7;
    ULONG references_ = 1;
    ConnectionStats& stats_;
    ComPtr<IDispatch> sink_;
};

struct EventRecord {
    const char* name = nullptr;
    uint64_t hresult = 0;
    uint64_t advised = 0;
};

std::array<EventRecord, 16> events{};
size_t event_count = 0;
FakeShellWindows* first_connection = nullptr;
FakeShellWindows* replacement_connection = nullptr;
unsigned factory_calls = 0;

unsigned CountEvents(const char* name, uint64_t result, uint64_t advised = 0) {
    unsigned count = 0;
    for (size_t index = 0; index < event_count; ++index) {
        const auto& event = events[index];
        if (std::strcmp(event.name, name) == 0 && event.hresult == result && event.advised == advised) ++count;
    }
    return count;
}

void ReconnectAfterExplorerRestarts() {
    ConnectionStats first_stats, replacement_stats;
    ComPtr<FakeShellWindows> first, replacement;
    first.Attach(new FakeShellWindows(first_stats));
    replacement.Attach(new FakeShellWindows(replacement_stats));
    first_connection = first.Get();
    replacement_connection = replacement.Get();
    {
        pulse::app::ExplorerWindowTakeover::Shared shared;
        pulse::app::Watcher watcher(shared);
        Check(watcher.Start() && factory_calls == 1 && first_stats.advises == 1 && first->HasSubscriber(),
            "watcher starts by subscribing to the original ShellWindows connection");

        first->count_result = RPC_E_DISCONNECTED;
        const unsigned previous_count_calls = first_stats.count_calls;
        watcher.Scan();
        Check(first_stats.count_calls == previous_count_calls + 1 && factory_calls == 1 &&
            CountEvents("explorer_takeover_disconnected", static_cast<uint32_t>(RPC_E_DISCONNECTED)) == 1,
            "scan detects the dead Explorer enumerator and records its RPC disconnection");

        watcher.Scan();
        Check(factory_calls == 2 && first_stats.unadvises == 1 && !first->HasSubscriber() &&
            replacement_stats.advises == 1 && replacement_stats.count_calls == 1 && replacement->HasSubscriber(),
            "next scan retires the dead subscription and enumerates a freshly advised ShellWindows connection");
        Check(CountEvents("explorer_takeover_reconnect", static_cast<uint32_t>(S_OK), 1) == 2,
            "startup and replacement each report a successfully advised connection");

        const unsigned old_count_calls = first_stats.count_calls;
        watcher.Scan();
        Check(factory_calls == 2 && replacement_stats.advises == 1 && replacement_stats.count_calls == 2 &&
            first_stats.count_calls == old_count_calls && first_stats.unadvises == 1,
            "healthy scans reuse the replacement without recreating or resubscribing the COM connection");
        Check(first_stats.item_calls == 0 && replacement_stats.item_calls == 0,
            "empty mocked enumerators expose no shell views or real Explorer windows to the watcher");
        Check(CountEvents("explorer_takeover_hidden", 0) == 0 && CountEvents("explorer_takeover_candidate", 0) == 0,
            "connection recovery never hides or takes over a window in this empty-view fixture");
    }
    Check(first_stats.unadvises == 1 && replacement_stats.unadvises == 1 &&
        !first->HasSubscriber() && !replacement->HasSubscriber() &&
        first_stats.invalid_unadvises == 0 && replacement_stats.invalid_unadvises == 0,
        "watcher destruction unadvises the active connection exactly once with no duplicate cleanup");
    first_connection = nullptr;
    replacement_connection = nullptr;
}

} // namespace

namespace pulse::app {

HRESULT CreateExplorerShellWindowsForTest(IShellWindows** windows) {
    if (!windows) return E_POINTER;
    *windows = nullptr;
    ++factory_calls;
    FakeShellWindows* selected = factory_calls == 1 ? first_connection : replacement_connection;
    if (!selected) return E_UNEXPECTED;
    *windows = static_cast<IShellWindows*>(selected);
    selected->AddRef();
    return S_OK;
}

void TraceShellWindows(const wchar_t*, ...) {}

} // namespace pulse::app

namespace pulse::diagnostics::runtime {

void Event(const char* name, std::initializer_list<Field> fields, Level) noexcept {
    if (event_count >= events.size()) return;
    auto& event = events[event_count++];
    event.name = name;
    for (const auto& field : fields) {
        if (std::strcmp(field.name, "hresult") == 0) event.hresult = field.value;
        else if (std::strcmp(field.name, "advised") == 0) event.advised = field.value;
    }
}

} // namespace pulse::diagnostics::runtime

int main() {
    const std::wstring desktop_name = L"PulseExplorerConnectionTest_" + std::to_wstring(GetCurrentProcessId()) +
        L"_" + std::to_wstring(GetTickCount64());
    constexpr DWORD access = DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_CREATEWINDOW |
        DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL;
    const HDESK desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, access, nullptr);
    Check(desktop != nullptr, "create an isolated desktop without switching the user's visible desktop");
    if (!desktop) return 1;
    try {
        std::thread worker([&] {
            if (!SetThreadDesktop(desktop)) { Check(false, "attach private desktop before COM and hooks"); return; }
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            Check(SUCCEEDED(initialized), "initialize the isolated watcher STA");
            if (FAILED(initialized)) return;
            struct ComScope { ~ComScope() { CoUninitialize(); } } com;
            try { ReconnectAfterExplorerRestarts(); }
            catch (...) { Check(false, "connection test completes without an unexpected exception"); }
        });
        worker.join();
    } catch (...) { Check(false, "isolated worker starts and joins successfully"); }
    Check(CloseDesktop(desktop) != FALSE, "private desktop closes after watcher hooks and COM are released");
    std::printf("SUMMARY failures=%d\n", failures);
    return failures ? 1 : 0;
}
