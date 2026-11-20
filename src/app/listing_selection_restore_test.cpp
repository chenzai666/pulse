#ifdef PULSE_WITH_SELFTEST
#include "listing_selection_restore.h"
#include "entry_order_hold.h"
#include <cstdio>
#include <filesystem>
#include <set>

bool RunListingSelectionRestoreTest() {
    using namespace pulse;
    int failures = 0;
    std::filesystem::create_directories(L"bench_data");
    FILE* log = nullptr;
    _wfopen_s(&log, L"bench_data/m10003_selection_restore_test.log", L"w");
    auto check = [&](bool ok, const char* label) {
        if (log) { fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label); fflush(log); }
        failures += !ok;
    };
    auto snapshot = [](std::initializer_list<const wchar_t*> names) {
        auto entries = std::make_shared<std::vector<fs::DirEntry>>();
        for (auto name : names) {
            fs::DirEntry entry;
            entry.name = name;
            entries->push_back(std::move(entry));
        }
        return entries;
    };
    auto initialize = [&](app::Tab& tab) {
        tab.current_path = L"C:\\listing-selection-fixture";
        tab.SetSnapshot(snapshot({L"a", L"b", L"c", L"d"}));
        tab.SelectOnly(0);
        app::CapturePendingListingSelection(tab);
    };
    auto names = [](const app::Tab& tab) {
        std::set<std::wstring> result;
        for (int i : tab.SelectedIndices()) result.insert(tab.EntryAt(static_cast<size_t>(i)).name);
        return result;
    };
    auto publish = [&](app::Tab& tab, std::initializer_list<const wchar_t*> entries) {
        auto restore = app::TakeListingSelection(tab, tab.current_path);
        tab.SetSnapshot(snapshot(entries));
        app::RestoreListingSelection(tab, restore);
        return restore.user_changed;
    };
    check(log != nullptr, "regression log opened");
    {
        app::Tab tab; initialize(tab);
        check(!publish(tab, {L"d", L"c", L"b", L"a"}) && names(tab) == std::set<std::wstring>{L"a"},
            "unchanged selection retains captured identity across reorder");
        check(tab.pending_selection_revision == UINT64_MAX, "consumed capture resets revision marker");
    }
    {
        app::Tab tab; initialize(tab); tab.SelectOnly(2);
        check(publish(tab, {L"c", L"d", L"a", L"b"}) && names(tab) == std::set<std::wstring>{L"c"},
            "new single selection supersedes refresh capture");
    }
    {
        app::Tab tab; initialize(tab); tab.ToggleSelect(2);
        check(publish(tab, {L"d", L"c", L"b", L"a"}) &&
            names(tab) == std::set<std::wstring>{L"a", L"c"}, "latest multiple selection survives reorder");
        check(tab.EntryAt(static_cast<size_t>(tab.selected_index)).name == L"c", "latest focused identity survives reorder");
    }
    {
        app::Tab tab; initialize(tab);
        auto shown = snapshot({L"a", L"b", L"c"});
        (*shown)[0].size = 10;
        (*shown)[1].size = 20;
        (*shown)[2].size = 30;
        tab.SetSnapshot(shown);
        tab.SelectOnly(0);
        tab.ToggleSelect(2);
        tab.selected_index = 0;
        app::CapturePendingListingSelection(tab);
        auto restore = app::TakeListingSelection(tab, tab.current_path);
        auto fresh = snapshot({L"b", L"a", L"c"});
        (*fresh)[0].size = 20;
        (*fresh)[1].size = 25;
        (*fresh)[2].size = 30;
        tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(app::KeepEntryOrder(
            *shown, *fresh, {}, ui::SortColumn::Size, ui::SortDirection::Asc)));
        app::RestoreListingSelection(tab, restore);
        check(names(tab) == std::set<std::wstring>{L"a", L"c"} && tab.selected_index == 1 &&
              tab.EntryAt(1).name == L"a", "overwrite moves the focused row and keeps all selected identities");
    }
    {
        app::Tab tab; initialize(tab); tab.SelectRange(1, 3);
        check(publish(tab, {L"d", L"c", L"b", L"a"}) &&
            names(tab) == std::set<std::wstring>{L"b", L"c", L"d"}, "latest range maps by identity");
    }
    {
        app::Tab tab; initialize(tab); tab.ClearSelection();
        check(publish(tab, {L"d", L"c", L"b", L"a"}) && tab.SelectedCount() == 0 &&
            tab.selected_index == -1, "Escape empty selection does not fall back to first item");
    }
    {
        app::Tab tab; initialize(tab); tab.SelectOnly(2);
        check(publish(tab, {L"a", L"b"}) && tab.SelectedCount() == 0,
            "disappeared latest selection does not select unrelated first row");
    }
    {
        app::Tab tab; initialize(tab); tab.SelectIndices({1, 2});
        check(publish(tab, {L"a", L"c"}) && names(tab) == std::set<std::wstring>{L"c"},
            "partly removed latest selection retains surviving identity");
    }
    {
        app::Tab tab; initialize(tab);
        tab.pending_selected_names = {L"target"}; tab.pending_selected_name = L"target";
        tab.current_path = L"C:\\different-fixture"; tab.ClearSelection();
        check(!publish(tab, {L"other", L"target"}) && names(tab) == std::set<std::wstring>{L"target"},
            "different directory keeps explicit navigation target");
    }
    {
        app::Tab tab; initialize(tab);
        tab.pending_selected_names = {L"b"}; tab.pending_selected_name = L"b";
        tab.SelectOnly(1); tab.pending_selection_revision = tab.selection_revision;
        check(!publish(tab, {L"c", L"b"}) && names(tab) == std::set<std::wstring>{L"b"},
            "explicit same-directory selection establishes its own baseline");
        app::CapturePendingListingSelection(tab); tab.ClearSelection();
        check(publish(tab, {L"b", L"c"}) && tab.SelectedCount() == 0,
            "later Escape overrides explicit selection baseline");
    }
    {
        app::Tab tab; initialize(tab); tab.SelectOnly(2);
        auto restore = app::TakeListingSelection(tab, tab.current_path);
        auto fresh = snapshot({L"a", L"renamed"});
        app::FollowHeldRenames({{L"c", L"renamed"}}, *fresh, restore.names, restore.focus);
        tab.SetSnapshot(fresh);
        app::RestoreListingSelection(tab, restore);
        check(names(tab) == std::set<std::wstring>{L"renamed"},
            "latest selection follows production held-rename mapping");
    }
    {
        app::Tab tab; initialize(tab);
        tab.pending_selection_revision = UINT64_MAX;
        tab.pending_selected_names = {L"b"}; tab.pending_selected_name = L"b"; tab.ClearSelection();
        check(!publish(tab, {L"a", L"b"}) && names(tab) == std::set<std::wstring>{L"b"},
            "uncaptured legacy explicit selection keeps existing behavior");
    }
    {
        app::Tab tab; initialize(tab); tab.SelectOnly(2);
        app::CapturePendingListingSelection(tab);
        publish(tab, {L"a", L"b", L"d"});
        check(names(tab) == std::set<std::wstring>{L"d"}, "deleted middle item selects its surviving successor");
    }
    {
        app::Tab tab; initialize(tab); tab.SelectOnly(3);
        app::CapturePendingListingSelection(tab);
        publish(tab, {L"a", L"b", L"c"});
        check(names(tab) == std::set<std::wstring>{L"c"}, "deleted last item selects its surviving predecessor");
    }
    {
        app::Tab tab; initialize(tab); tab.SelectIndices({0, 2}); tab.selected_index = 2;
        app::CapturePendingListingSelection(tab);
        publish(tab, {L"b", L"d"});
        check(names(tab) == std::set<std::wstring>{L"d"}, "noncontiguous deletion follows identity rather than shifted index");
    }
    {
        app::Tab tab; initialize(tab); tab.SelectIndices({1, 2}); tab.selected_index = 2;
        app::CapturePendingListingSelection(tab);
        publish(tab, {L"a", L"b", L"d"});
        check(names(tab) == std::set<std::wstring>{L"b"}, "partial deletion preserves surviving selected items");
    }
    {
        app::Tab tab; initialize(tab);
        tab.SetSnapshot(snapshot({L"d", L"c", L"b", L"a"})); tab.SelectOnly(1);
        app::CapturePendingListingSelection(tab);
        publish(tab, {L"d", L"b", L"a"});
        check(names(tab) == std::set<std::wstring>{L"b"}, "descending deletion follows displayed successor");
    }
    {
        app::Tab tab; initialize(tab); tab.SelectAll();
        app::CapturePendingListingSelection(tab); publish(tab, {});
        check(tab.SelectedCount() == 0 && tab.selected_index == -1, "deleting all entries leaves no phantom selection");
    }
    {
        app::Pane pane;
        auto& tab = pane.view; initialize(tab);
        tab.SetSnapshot(snapshot({L"a.keep", L"b.keep", L"c.skip", L"d.keep"}));
        tab.filter_text = L"*.keep"; tab.SelectOnly(1);
        app::CapturePendingListingSelection(tab);
        auto restore = app::TakeListingSelection(tab, tab.current_path);
        tab.SetSnapshot(snapshot({L"a.keep", L"c.skip", L"d.keep"}));
        app::RestoreListingSelection(tab, restore, &pane);
        check(names(tab) == std::set<std::wstring>{L"d.keep"}, "deleted filtered item skips invisible successor");
    }
    {
        app::Tab tab; initialize(tab); tab.SelectOnly(2);
        app::CapturePendingListingSelection(tab);
        tab.current_path = L"C:\\other-directory";
        publish(tab, {L"x", L"y"});
        check(tab.SelectedCount() == 0 && tab.selected_index < 0 && tab.HasFocusHint(),
            "navigation focuses the new folder's first row without inheriting or selecting");
        tab.SelectOnly(1);
        check(!tab.HasFocusHint() && names(tab) == std::set<std::wstring>{L"y"},
            "a real selection replaces the first-row focus hint");
    }
    if (log) { fprintf(log, "failures=%d\n", failures); fclose(log); }
    return failures == 0;
}
#endif
