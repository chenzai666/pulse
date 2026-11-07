#include "../common/windows_compat.h"
#include "../ui/folder_picker_listary_search.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
std::atomic<bool> enabled{true};
std::atomic<int> calls{0}, active{0}, maximum_active{0}, cancellations{0};
std::atomic<bool> release_slow{false};
std::mutex gate;
std::condition_variable released;
int failures = 0;

void Check(bool passed, const char* name) {
    std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    if (!passed) ++failures;
}

bool Wait(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    return condition();
}

void Release() {
    release_slow.store(true);
    released.notify_all();
}
} // namespace

// These are a deterministic endpoint substitute, not a second implementation
// of the search mailbox. The test links the real production UI-state worker.
namespace pulse::integrations {
ListarySearchConfig ReadListarySearchConfig() { return {enabled.load(), 38431}; }

ListarySearchResult SearchListary(const ListarySearchConfig&, const std::wstring& query,
                                  bool folders_only, const std::atomic<bool>& cancelled) {
    ++calls;
    const int running = ++active;
    int previous = maximum_active.load();
    while (running > previous && !maximum_active.compare_exchange_weak(previous, running)) {}
    bool saw_cancellation = false;
    if (query.starts_with(L"slow")) {
        std::unique_lock lock(gate);
        while (!release_slow.load()) {
            if (cancelled.load() && !saw_cancellation) {
                ++cancellations;
                saw_cancellation = true;
            }
            released.wait_for(lock, std::chrono::milliseconds(2));
        }
    }
    ListarySearchResult result;
    // Deliberately return a stale result after cancellation, as an endpoint
    // can complete just as cancellation arrives. The mailbox must reject it.
    for (int i = 0; i < 12; ++i) {
        const auto name = query + std::to_wstring(i);
        result.items.push_back({L"C:\\results\\" + name, name, folders_only});
    }
    --active;
    return result;
}
} // namespace pulse::integrations

int main() {
    using pulse::ui::PickerListarySearch;
    {
        PickerListarySearch search(false);
        search.Layout(640, 316, 112, 1);
        search.SetFocused(true);
        Check(Wait([&] { return search.Tick(); }), "initial configuration read is asynchronous");
        Check(calls.load() == 0, "empty query never calls the search endpoint");
        search.SetQuery(L"debounce");
        search.Tick();
        Check(calls.load() == 0, "input is not sent before the debounce expires");
        Check(Wait([&] { search.Tick(); return search.Selected().has_value(); }),
              "debounced request returns to the real UI mailbox");
        Check(calls.load() == 1, "debounce sends one request");
        search.SelectRow(7);
        Check(search.Selected() && search.Selected()->name == L"debounce7", "eighth result remains selectable");
        search.SelectRow(8);
        Check(search.Selected() && search.Selected()->name == L"debounce7", "result list is capped at eight entries");

        for (const float scale : {1.0f, 1.5f, 2.0f}) {
            for (const float width : {320.0f, 640.0f, 1000.0f}) {
                const float footer_top = (420.0f - 104.0f) * scale;
                const float content_top = 112.0f * scale;
                search.Layout(width * scale, footer_top, content_top, scale);
                const auto input = search.InputBounds();
                Check(input.left >= 0 && input.right <= width * scale && input.right > input.left,
                      "narrow width keeps the search field inside the client");
                Check(input.bottom < footer_top && input.top >= footer_top - PickerListarySearch::FooterDip() * scale,
                      "search band never covers the existing filename or button footer");
                Check(!search.Contains(input.left + 1, content_top - 1) &&
                      !search.Contains(input.left + 1, footer_top + 1),
                      "DPI-scaled result panel stays below toolbar and above filename footer");
                Check(search.HitRow(input.left + 1, footer_top - PickerListarySearch::FooterDip() * scale - 1) == 7,
                      "short popup scrolls the selected result into its last visible row");
            }
        }
        Check(search.Dismiss() && !search.Selected(), "Escape dismissal prevents invisible result activation");
        search.SetFocused(false);
        search.MoveSelection(1);
        Check(!search.PopupVisible(), "blurred search never displays a result panel");
        search.SetFocused(true);
        Check(search.PopupVisible(), "focus can restore the result panel");

        release_slow.store(false);
        search.SetQuery(L"slow-first");
        search.SearchNow();
        Check(Wait([] { return active.load() == 1; }), "slow endpoint request has actually started");
        const int before = calls.load();
        for (int i = 0; i < 20; ++i) {
            search.SetQuery(L"new" + std::to_wstring(i));
            search.SearchNow();
        }
        Check(calls.load() == before && maximum_active.load() == 1,
              "rapid input keeps one worker and one pending latest request");
        Check(Wait([] { return cancellations.load() > 0; }), "superseding query cancels the in-flight request");
        Release();
        Check(Wait([&] { search.Tick(); return search.Selected() && search.Selected()->name == L"new190"; }),
              "cancelled stale response cannot replace the newest generation");
        Check(calls.load() == before + 1, "obsolete queued searches never reach the endpoint");

        enabled.store(false);
        search.SetQuery(L"disabled");
        search.SearchNow();
        Check(Wait([&] { return search.Tick(); }) && !search.Selected() && !search.StatusText().empty(),
              "disabled interface returns status and clears selectable results");
        Check(calls.load() == before + 1, "disabled configuration never starts HTTP");
        enabled.store(true);
    }

    release_slow.store(false);
    const int cancellations_before = cancellations.load();
    auto closing = std::make_unique<PickerListarySearch>(true);
    closing->SetQuery(L"slow-close");
    closing->SearchNow();
    Check(Wait([] { return active.load() == 1; }), "close test owns an active endpoint request");
    const auto start = std::chrono::steady_clock::now();
    closing.reset();
    Check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100),
          "closing does not join or wait on a blocked network request");
    Check(Wait([&] { return cancellations.load() > cancellations_before; }),
          "closing signals the worker-owned cancellation flag safely");
    Release();
    Check(Wait([] { return active.load() == 0; }), "cancelled request exits after picker destruction without UI access");
    return failures ? 1 : 0;
}
