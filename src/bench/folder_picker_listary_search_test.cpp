#include "../common/windows_compat.h"
#include "../ui/folder_picker_listary_search.h"

#include <atomic>
#include <algorithm>
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
std::atomic<int> page_failures{0};
std::atomic<uint64_t> last_offset{0};
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
                                  bool folders_only, const std::atomic<bool>& cancelled, uint64_t offset) {
    ++calls;
    last_offset = offset;
    const int running = ++active;
    int previous = maximum_active.load();
    while (running > previous && !maximum_active.compare_exchange_weak(previous, running)) {}
    bool saw_cancellation = false;
    if (query.starts_with(L"slow") || (query == L"pages-slow" && offset)) {
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
    result.offset = offset;
    const bool paged = query.starts_with(L"pages");
    result.total = query == L"pages-duplicate" ? 143 : paged ? 93 : 12;
    if (query == L"pages-retry" && offset && page_failures.fetch_add(1) == 0) {
        result.error = ListarySearchError::Timeout;
        --active;
        return result;
    }
    if (query == L"pages-offset" && offset) result.offset = 0;
    // Deliberately return a stale result after cancellation, as an endpoint
    // can complete just as cancellation arrives. The mailbox must reject it.
    const uint64_t end = (std::min)(offset + (paged ? uint64_t{50} : uint64_t{12}), result.total);
    for (uint64_t i = offset; i < end; ++i) {
        const uint64_t identity = query == L"pages-duplicate" && i == 50 ? 0 : i;
        const auto name = query + std::to_wstring(identity);
        result.items.push_back({L"C:\\results\\" + name, name, folders_only});
    }
    result.has_more = end < result.total;
    --active;
    return result;
}
} // namespace pulse::integrations

int main() {
    using pulse::ui::PickerListarySearch;
    using pulse::ui::ListaryNameMatches;
    using pulse::ui::ListaryResultName;
    using pulse::ui::ListaryResultParent;
    {
        const auto chinese = ListaryNameMatches(L"音乐播放器.exe", L"音乐", L"音乐播放器.exe");
        Check(chinese.size() == 1 && chinese[0].start == 0 && chinese[0].length == 2,
              "Chinese query highlights the filename UTF-16 span");
        const auto casing = ListaryNameMatches(L"Firefox.exe", L"FIRE", L"Firefox.exe");
        Check(casing.size() == 1 && casing[0].start == 0 && casing[0].length == 4,
              "filename highlight uses ordinal case-insensitive matching");
        const auto repeated = ListaryNameMatches(L"foo-FOO.exe", L"foo", L"foo-FOO.exe");
        Check(repeated.size() == 2 && repeated[1].start == 4 && repeated[1].length == 3,
              "all repeated filename matches retain their original indexes");
        const auto clipped = ListaryNameMatches(L"Firefox.exe", L"firefox", L"Fire\u2026");
        Check(clipped.size() == 1 && clipped[0].length == 4,
              "highlight stops before the generated ellipsis");
        Check(ListaryNameMatches(L"Firefox.exe", L"exe", L"Fire\u2026").empty(),
              "hidden filename suffix does not create a visible highlight");
        const std::wstring emoji = L"\xD83D\xDD0D";
        const auto pair = ListaryNameMatches(emoji + L"Search.exe", emoji, emoji + L"Search.exe");
        Check(pair.size() == 1 && pair[0].start == 0 && pair[0].length == 2,
              "surrogate-pair query keeps both UTF-16 units in one highlight");
        Check(ListaryNameMatches(emoji + L"Search.exe", emoji,
              std::wstring(1, emoji[0]) + L"\u2026").empty(),
              "truncation cannot leave half a surrogate pair highlighted");
        Check(ListaryNameMatches(emoji + L"Search.exe", std::wstring(1, emoji[0]),
              emoji + L"Search.exe").empty(), "malformed UTF-16 query does not produce a highlight");
        const auto terms = ListaryNameMatches(L"Firefox Portable.exe", L"fire portable ext:exe -fox",
                                               L"Firefox Portable.exe");
        Check(terms.size() == 2 && terms[0].length == 4 && terms[1].start == 8 && terms[1].length == 8,
              "filename terms highlight separately while filters and exclusions are ignored");
        const auto model = ListaryNameMatches(L"LA 516.pdf", L"LA516", L"LA 516.pdf");
        Check(model.size() == 2 && model[0].start == 0 && model[0].length == 2 &&
              model[1].start == 3 && model[1].length == 3,
              "compact model highlights letters and numbers across the filename space");
        const auto suffix_model = ListaryNameMatches(L"OX 1228D 682009.pdf", L"OX1228D", L"OX 1228D 682009.pdf");
        Check(suffix_model.size() == 2 && suffix_model[0].length == 2 &&
              suffix_model[1].start == 3 && suffix_model[1].length == 5,
              "model suffix highlights its actual characters without the following order number");
        const auto separated_model = ListaryNameMatches(L"ox_1228-d.pdf", L"OX1228D", L"ox_1228-d.pdf");
        Check(separated_model.size() == 3 && separated_model[1].start == 3 &&
              separated_model[1].length == 4 && separated_model[2].start == 8 && separated_model[2].length == 1,
              "compact model matching ignores case and crosses underscore and hyphen separators");
        const auto clipped_model = ListaryNameMatches(emoji + L" OX 1228D 682.pdf", L"OX1228D",
                                                       emoji + L" OX 12\u2026");
        Check(clipped_model.size() == 2 && clipped_model[0].start == 3 && clipped_model[0].length == 2 &&
              clipped_model[1].start == 6 && clipped_model[1].length == 2,
              "model highlighting preserves preceding surrogate indexes and the visible ellipsis boundary");
        Check(ListaryNameMatches(L"OX 1 D.txt", L"OX1D", L"OX 1 D.txt").empty() &&
              ListaryNameMatches(L"A 1228.txt", L"A1228", L"A 1228.txt").empty(),
              "separator matching is restricted to models with two prefix letters and two consecutive digits");
        Check(ListaryResultName({L"C:\\Apps\\XTerminal.exe", L"", false}) == L"XTerminal.exe",
              "missing display name uses the leaf instead of repeating the whole path");
        Check(ListaryResultParent(L"C:\\Apps\\XTerminal.exe") == L"C:\\Apps",
              "second line displays the containing folder only");
        Check(ListaryResultParent(L"C:\\Apps\\Tools\\") == L"C:\\Apps",
              "folder results display their parent without the trailing leaf");
        Check(ListaryResultParent(L"C:\\app.exe") == L"C:\\" && ListaryResultParent(L"C:\\") == L"C:\\",
              "drive-root parents remain valid absolute roots");
        Check(ListaryResultParent(L"\\\\server\\share\\app.exe") == L"\\\\server\\share" &&
              ListaryResultParent(L"\\\\server\\share\\") == L"\\\\server\\share",
              "UNC result and share-root parents retain the valid share path");
    }
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
        Check(search.Selected() && search.Selected()->name == L"debounce8", "ninth loaded result remains reachable beyond the eight-row viewport");
        search.SelectRow(7);

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

    {
        PickerListarySearch search(false);
        search.Layout(1000, 700, 112, 1);
        search.SetFocused(true);
        auto first_page = [&](const wchar_t* query) {
            search.SetQuery(query);
            search.SearchNow();
            return Wait([&] { search.Tick(); return search.Selected().has_value(); });
        };
        Check(first_page(L"pages"), "paged search loads its actual first fifty results");
        Check(search.StatusText().find(L"50") != std::wstring::npos &&
              search.StatusText().find(L"93") != std::wstring::npos,
              "status exposes loaded and total counts");
        search.SelectRow(8);
        Check(search.Selected() && search.Selected()->name == L"pages8", "ninth result can be selected in a paged response");
        search.SelectRow(49);
        const int before = calls.load();
        search.MoveSelection(1);
        for (int i = 0; i < 20; ++i) search.MoveSelection(1);
        Check(Wait([&] { return search.Tick(); }) && last_offset.load() == 50 && calls.load() == before + 1,
              "Down at page boundary requests one next page at the raw offset");
        Check(search.Selected() && search.Selected()->name == L"pages49", "appended page preserves the current selection");
        search.MoveSelection(1);
        Check(search.Selected() && search.Selected()->name == L"pages50", "next Down reaches the first appended result");
        search.SelectRow(92);
        Check(search.Selected() && search.Selected()->name == L"pages92", "all ninety-three results are reachable");
        const int completed_calls = calls.load();
        search.MoveSelection(8);
        Check(calls.load() == completed_calls && search.Selected()->name == L"pages92",
              "last page does not wrap or request an endless next page");
        Check(search.HitRow(110, 700 - PickerListarySearch::FooterDip() - 1) == 92 &&
              !search.Contains(110, 700 - PickerListarySearch::FooterDip() - 8 * 44 - 1),
              "large result sets still draw only eight rows with consistent hit testing");

        page_failures = 0;
        Check(first_page(L"pages-retry"), "retry scenario loads the first page");
        search.SelectRow(49);
        search.MoveSelection(1);
        Check(Wait([&] { return search.Tick(); }) && search.Selected() &&
              search.Selected()->name == L"pages-retry49" && search.StatusText().find(L"50") != std::wstring::npos,
              "failed next page preserves existing results and selection");
        const auto input = search.InputBounds();
        Check(search.HitLoadMore(input.left + 1, input.bottom + 1), "footer provides a mouse load-more retry target");
        search.LoadMore();
        Check(Wait([&] { return search.Tick(); }), "failed next-page offset can be retried explicitly");
        search.MoveSelection(1);
        Check(search.Selected() && search.Selected()->name == L"pages-retry50", "retry appends the missing page without losing the first");

        Check(first_page(L"pages-offset"), "invalid-offset scenario loads first page");
        search.SelectRow(49);
        search.MoveSelection(1);
        Check(Wait([&] { return search.Tick(); }) && search.Selected()->name == L"pages-offset49",
              "wrong response offset is rejected without replacing the first page");
        search.SelectRow(50);
        Check(search.Selected()->name == L"pages-offset49", "out-of-order page never enters the selectable list");

        Check(first_page(L"pages-duplicate"), "duplicate scenario loads first page");
        search.SelectRow(49);
        search.MoveSelection(1);
        Check(Wait([&] { return search.Tick(); }), "duplicate-path page is accepted with valid raw metadata");
        search.SelectRow(91);
        Check(search.Selected() && search.Selected()->name == L"pages-duplicate92" && last_offset.load() == 50,
              "duplicate paths are omitted without dropping other page results");
        search.SelectRow(98);
        search.MoveSelection(1);
        Check(Wait([&] { return search.Tick(); }) && last_offset.load() == 100,
              "next offset advances by the original page count before deduplication");
        search.SelectRow(141);
        Check(search.Selected() && search.Selected()->name == L"pages-duplicate142",
              "deduplicated multi-page results retain the final record");

        release_slow = false;
        Check(first_page(L"pages-slow"), "stale-page scenario loads first page without blocking");
        search.SelectRow(49);
        search.MoveSelection(1);
        Check(Wait([] { return active.load() == 1; }), "old-query pagination is actually in flight");
        search.SetQuery(L"replacement");
        search.SearchNow();
        Release();
        Check(Wait([&] { search.Tick(); return search.Selected() && search.Selected()->name == L"replacement0"; }),
              "query change cancels old pagination and never mixes its results");
        search.SelectRow(12);
        Check(search.Selected()->name == L"replacement0", "old page cannot remain beyond the new query result count");
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
