#ifdef PULSE_WITH_SELFTEST
#include "listary_search_entry.h"
#include "global_search_controller.h"
#include "app_worker.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace pulse {
namespace {
constexpr DWORD kTimeoutMs = 5000;

struct Launcher {
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic_uint calls{0};
    std::atomic<DWORD> thread{0};
    std::atomic_bool timed_out{false};
    bool result = false;

    ~Launcher() {
        if (entered) CloseHandle(entered);
        if (release) CloseHandle(release);
    }
    bool Invoke() {
        ++calls;
        thread = GetCurrentThreadId();
        SetEvent(entered);
        if (WaitForSingleObject(release, kTimeoutMs) != WAIT_OBJECT_0) {
            timed_out = true;
            return false;
        }
        return result;
    }
};

struct Fixture {
    app::WorkerPool worker;
    HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, nullptr, nullptr);
    HWND other = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, nullptr, nullptr);
    std::vector<std::shared_ptr<Launcher>> launchers;

    ~Fixture() {
        CancelListaryGlobalSearch(window);
        CancelListaryGlobalSearch(other);
        for (const auto& launcher : launchers) if (launcher->release) SetEvent(launcher->release);
        worker.Stop();
        SetListarySearchEntryTestLauncher({});
        if (other) DestroyWindow(other);
        if (window) DestroyWindow(window);
    }
    std::shared_ptr<Launcher> Prepare(bool result, bool blocked = false) {
        auto launcher = std::make_shared<Launcher>();
        launchers.push_back(launcher);
        launcher->result = result;
        if (!blocked && launcher->release) SetEvent(launcher->release);
        SetListarySearchEntryTestLauncher([launcher] { return launcher->Invoke(); });
        return launcher;
    }
};

bool WaitForResult(HWND window, MSG& message) {
    const auto deadline = GetTickCount64() + kTimeoutMs;
    do {
        if (PeekMessageW(&message, window, kListarySearchResultMessage,
                kListarySearchResultMessage, PM_REMOVE)) return true;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    return false;
}
}

bool RunListarySearchEntryTest() {
    const auto output = std::filesystem::absolute(L"bench_data/listary-search-entry");
    std::filesystem::create_directories(output);
    std::ofstream log(output / L"results.log");
    if (!log) return false;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] Listary queue: %s\n", ok ? "PASS" : "FAIL", label);
        log << (ok ? "[PASS] " : "[FAIL] ") << label << std::endl;
        failures += !ok;
        return ok;
    };
    try {
        Fixture fixture;
        if (!check(fixture.window && fixture.other, "isolated message windows created")) return false;
        const DWORD ui_thread = GetCurrentThreadId();
        fixture.worker.Start({});
        auto success = fixture.Prepare(true, true);
        if (!check(success->entered && success->release, "bounded launcher gate created")) return false;
        if (!check(QueueListaryGlobalSearch(fixture.worker, fixture.window), "success request admitted")) return false;
        if (!check(WaitForSingleObject(success->entered, kTimeoutMs) == WAIT_OBJECT_0,
                "real WorkerPool runs the launcher")) return false;
        check(success->thread.load() != 0 && success->thread.load() != ui_thread,
            "launcher runs outside the UI thread");
        bool coalesced = true;
        for (int i = 0; i < 128; ++i)
            coalesced = QueueListaryGlobalSearch(fixture.worker, fixture.window) && coalesced;
        check(coalesced && success->calls.load() == 1, "rapid hotkeys coalesce while launch is blocked");
        MSG message{};
        check(!PeekMessageW(&message, fixture.window, kListarySearchResultMessage,
            kListarySearchResultMessage, PM_REMOVE), "blocked launcher has no premature UI completion");
        SetEvent(success->release);
        if (!check(WaitForResult(fixture.window, message), "success posts a bounded UI completion")) return false;
        const WPARAM successful_token = message.wParam;
        bool opened = false;
        check(!TakeListaryGlobalSearchResult(fixture.other, successful_token, opened),
            "another window cannot consume the result");
        check(!TakeListaryGlobalSearchResult(fixture.window, successful_token + 1, opened),
            "wrong token cannot consume the result");
        check(TakeListaryGlobalSearchResult(fixture.window, successful_token, opened) && opened,
            "success is accepted once by the owning UI window");
        check(!TakeListaryGlobalSearchResult(fixture.window, successful_token, opened),
            "consumed completion cannot be replayed");
        fixture.worker.Stop();
        check(success->calls.load() == 1 && !success->timed_out,
            "coalesced hotkeys create one launcher call");

        fixture.worker.Start({});
        auto failure = fixture.Prepare(false);
        if (!check(failure->entered && failure->release &&
                QueueListaryGlobalSearch(fixture.worker, fixture.window), "failure request admitted")) return false;
        if (!check(WaitForResult(fixture.window, message), "failure posts a bounded UI completion")) return false;
        const WPARAM failed_token = message.wParam;
        opened = true;
        check(!TakeListaryGlobalSearchResult(fixture.window, successful_token, opened),
            "late previous token leaves the current request intact");
        check(TakeListaryGlobalSearchResult(fixture.window, failed_token, opened) && !opened,
            "false launcher result requests native fallback");
        fixture.worker.Stop();

        fixture.worker.Start({});
        auto cancelled = fixture.Prepare(true, true);
        if (!check(cancelled->entered && cancelled->release &&
                QueueListaryGlobalSearch(fixture.worker, fixture.window), "cancel fixture admitted")) return false;
        if (!check(WaitForSingleObject(cancelled->entered, kTimeoutMs) == WAIT_OBJECT_0,
                "cancel fixture is in flight")) return false;
        CancelListaryGlobalSearch(fixture.window);
        SetEvent(cancelled->release);
        fixture.worker.Stop();
        check(!PeekMessageW(&message, fixture.window, kListarySearchResultMessage,
            kListarySearchResultMessage, PM_REMOVE) && !cancelled->timed_out,
            "cancelled in-flight launcher cannot post a late completion");

        fixture.worker.Start({});
        auto held = fixture.Prepare(true);
        if (!check(held->entered && held->release &&
                QueueListaryGlobalSearch(fixture.worker, fixture.window), "held completion admitted")) return false;
        if (!check(WaitForResult(fixture.window, message), "held completion reaches the UI queue")) return false;
        const WPARAM cancelled_token = message.wParam;
        CancelListaryGlobalSearch(fixture.window);
        check(!TakeListaryGlobalSearchResult(fixture.window, cancelled_token, opened),
            "cancellation retires an already posted result");
        auto replacement = fixture.Prepare(false);
        if (!check(replacement->entered && replacement->release &&
                QueueListaryGlobalSearch(fixture.worker, fixture.window), "replacement request admitted")) return false;
        check(!TakeListaryGlobalSearchResult(fixture.window, cancelled_token, opened),
            "cancelled token cannot consume its replacement");
        if (!check(WaitForResult(fixture.window, message), "replacement completes within five seconds")) return false;
        opened = true;
        check(message.wParam != cancelled_token &&
            TakeListaryGlobalSearchResult(fixture.window, message.wParam, opened) && !opened,
            "replacement retains its own fallback result after a late token");
        fixture.worker.Stop();
        for (const auto& launcher : fixture.launchers)
            check(!launcher->timed_out && launcher->thread.load() != ui_thread,
                "each launcher completes on a worker within its wait bound");
    } catch (...) {
        check(false, "fixture exception");
    }
    return failures == 0;
}
} // namespace pulse
#endif
