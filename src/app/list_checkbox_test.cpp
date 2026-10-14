#ifdef PULSE_WITH_SELFTEST
#include "app_input.h"
#include "app_hover.h"
#include "app_runtime.h"
#include "app_navigation.h"
#include "pane_drop.h"
#include "../common/localization.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <windowsx.h>

bool RunListCheckboxTest() {
    using namespace pulse;
    using R = ui::HitTestResult;
    const auto language = l10n::preference();
    l10n::SetLanguage(L"zh-CN");
    const auto output = std::filesystem::absolute(L"bench_data/list-checkbox");
    std::filesystem::create_directories(output);
    std::ofstream log(output / L"results.log");
    bool passed = log.good();
    const auto check = [&](bool value, const char* label) {
        log << (value ? "[PASS] " : "[FAIL] ") << label << std::endl;
        passed &= value;
        return value;
    };
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"PulseListCheckboxTest";
    RegisterClassW(&wc);
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        for (int width_dip : {1000, 620}) {
            log << "scale=" << scale << " width=" << width_dip << std::endl;
            auto state = std::make_unique<AppState>();
            state->isolatedTest = true;
            state->places.persist = state->appPrefs.persist = state->searchHistory.persist = false;
            state->ctxMenuPrefs.persist = false;
            state->scale = scale;
            state->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
                wc.lpszClassName, L"", WS_POPUP, -30000, -30000,
                static_cast<int>(width_dip * scale), static_cast<int>(650 * scale),
                nullptr, nullptr, wc.hInstance, nullptr);
            if (!check(state->hwnd && state->compositor.Init(state->hwnd), "initialize isolated renderer")) {
                if (state->hwnd) DestroyWindow(state->hwnd);
                continue;
            }
            state->renderer.SetCompositor(&state->compositor);
            state->renderer.SetScale(scale);
            state->compositor.RecreateTextFormats(scale);
            state->window_tabs.NewTab(L"C:\\Pulse-checkbox-fixture");
            BindCurrentLayout(*state);
            auto* tab = ActiveTab(*state);
            auto entries = std::make_shared<std::vector<fs::DirEntry>>();
            for (int i = 0; i < 50; ++i) {
                fs::DirEntry entry;
                entry.name = i == 1 ? L"excluded.txt" : L"示例文件-" + std::to_wstring(i) + L".keep";
                entry.attrs = FILE_ATTRIBUTE_NORMAL;
                entries->push_back(std::move(entry));
            }
            tab->SetSnapshot(entries);
            tab->loading = false;
            tab->view_mode = ui::ViewMode::Details;
            const D2D1_RECT_F bounds{0, 0, static_cast<float>(state->compositor.Width()),
                static_cast<float>(state->compositor.Height())};
            const auto vm = [&] { return BuildVm(*state, false); };
            const auto point_for = [&](int index) {
                auto view = vm();
                const auto pane = FocusedPaneRect(*state);
                auto cell = state->renderer.ItemRectInPane(view.pane, pane, index);
                if (index < 0) {
                    cell = state->renderer.PaneListRect(view.pane, pane);
                    cell.bottom = cell.top;
                    cell.top -= state->renderer.ColumnHeaderHeight();
                    cell = ui::DetailsContentRect(cell, scale);
                }
                auto box = ui::DetailsSelectionRect(cell, scale);
                return POINT{static_cast<LONG>((box.left + box.right) * 0.5f),
                    static_cast<LONG>((box.top + box.bottom) * 0.5f)};
            };
            const auto click = [&](int index, bool dbl = false) {
                const auto point = point_for(index);
                const auto hit = state->renderer.HitTest(vm(), bounds, static_cast<float>(point.x), static_cast<float>(point.y));
                check(hit.region == (index < 0 ? R::SelectAllCheckbox : R::RowCheckbox), "painted checkbox has its own hit target");
                UpdatePointerHover(*state, hit, point);
                const auto lp = MAKELPARAM(point.x, point.y);
                if (dbl) HandleLButtonDblClk(state.get(), state->hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, lp);
                else HandleLButtonDown(state.get(), state->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
                HandleLButtonUp(state.get(), state->hwnd, WM_LBUTTONUP, 0, lp);
                check(!state->dragPending && !state->renameClickCandidate && !state->marqueePending,
                    "checkbox never arms file drag, delayed rename or marquee");
            };
            click(0);
            click(2);
            check(tab->SelectedCount() == 2 && tab->IsSelected(0) && tab->IsSelected(2), "checkboxes add without Ctrl");
            click(0, true);
            check(tab->SelectedCount() == 1 && tab->IsSelected(2) &&
                tab->current_path == fs::NormalizePath(L"C:\\Pulse-checkbox-fixture"),
                "double-click toggles without opening the row");
            check(vm().pane.visible_selected_count == 1, "header sees a partial selection");
            for (bool dark : {false, true}) {
                auto view = vm();
                view.dark = dark;
                auto* dc = state->compositor.Dc();
                dc->BeginDraw();
                state->renderer.Render(view, bounds, ui::MakeTheme(dark, ui::HexColor(0x0078D4)));
                const auto path = output / (L"list-" + std::to_wstring(static_cast<int>(scale * 100)) +
                    L"-" + std::to_wstring(width_dip) + (dark ? L"-dark.png" : L"-light.png"));
                check(SUCCEEDED(dc->EndDraw()) && state->compositor.SaveSnapshot(path.c_str()), "render checkbox states to PNG");
            }
            click(-1);
            check(tab->SelectedCount() == 50 && vm().pane.visible_selected_count == 50, "header selects all rows");
            click(-1);
            check(tab->SelectedCount() == 0, "header clears all selection");
            tab->SelectOnly(1);
            tab->filter_text = L"*.keep";
            check(vm().pane.visible_selected_count == 0 && vm().pane.EntryCount() == 49,
                "hidden selected items do not mark the filtered header");
            click(-1);
            check(tab->SelectedCount() == 49 && !tab->IsSelected(1), "filtered select-all excludes hidden source rows");
            click(-1);
            check(tab->SelectedCount() == 0, "filtered all-selected state can clear");
            tab->filter_text.clear();
            tab->scroll_y = 180 * scale;
            const auto point = point_for(10);
            auto hit = state->renderer.HitTest(vm(), bounds, static_cast<float>(point.x), static_cast<float>(point.y));
            check(hit.region == R::RowCheckbox && hit.index == 10, "scrolled checkbox targets its source row");
            tab->filter_text = L"no-matches";
            tab->scroll_y = 0;
            click(-1);
            check(tab->SelectedCount() == 0 && vm().pane.EntryCount() == 0, "empty view cannot select source items");
            tab->SelectOnly(1);
            click(-1);
            check(tab->IsSelected(1), "disabled empty-view checkbox preserves hidden selection");
            tab->ClearSelection();
            tab->filter_text.clear();
            auto* original_pane = state->pane;
            app::AddPaneFromTab(*state->window_tabs.Active(), app::MakeSingleLayoutTab(L"C:\\second-fixture"), app::ContentDrop::Right);
            BindCurrentLayout(*state);
            auto* second = ActiveTab(*state);
            second->SetSnapshot(entries);
            second->loading = false;
            second->view_mode = ui::ViewMode::Details;
            const auto second_point = point_for(0);
            FocusPane(*state, original_pane);
            const auto second_hit = state->renderer.HitTest(vm(), bounds,
                static_cast<float>(second_point.x), static_cast<float>(second_point.y));
            check(second_hit.region == R::RowCheckbox, "inactive split pane exposes its checkbox");
            const auto second_lp = MAKELPARAM(second_point.x, second_point.y);
            HandleLButtonDown(state.get(), state->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, second_lp);
            HandleLButtonUp(state.get(), state->hwnd, WM_LBUTTONUP, 0, second_lp);
            check(second->SelectedCount() == 1 && tab->SelectedCount() == 0 && ActiveTab(*state) == second,
                "checkbox focuses and selects only the pane clicked");
            state->renderer.SetEmbedded(48, 48);
            const auto embedded_point = point_for(0);
            const auto embedded_hit = state->renderer.HitTest(vm(), bounds,
                static_cast<float>(embedded_point.x), static_cast<float>(embedded_point.y));
            check(embedded_hit.region == R::Row,
                "embedded folder picker retains ordinary row input without unsupported checkboxes");
            const HWND hwnd = state->hwnd;
            state->hwnd = nullptr;
            DestroyWindow(hwnd);
        }
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    l10n::SetLanguage(l10n::LanguageId(language));
    return passed;
}
#endif
