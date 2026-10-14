#ifdef PULSE_WITH_SELFTEST
#include "recycle_place_commands.h"
#include "app_internal.h"
#include "context_menu.h"
#include "pane_drop.h"
#include "../ui/recycle_toolbar.h"
#include "../ui/ui_renderer_internal.h"
#include "../common/windows_compat.h"
#include <cstdio>
#include <filesystem>

namespace {
template<class Check>
void CaptureRecycleToolbar(Check&& check, FILE* log) {
    using namespace pulse;
    using namespace pulse::ui;
    struct LanguageGuard {
        l10n::Language previous = l10n::preference();
        ~LanguageGuard() { l10n::SetLanguage(l10n::LanguageId(previous)); }
    } language_guard;
    l10n::SetLanguage(L"zh-CN");
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    check(SUCCEEDED(initialized), "recycle renderer initializes COM");
    if (FAILED(initialized)) return;
    struct ComGuard { ~ComGuard() { CoUninitialize(); } } com_guard;
    compat::EnableDpiAwareness();
    // 窗口从不显示或激活；截图直接读取本测试渲染器的交换链。
    const HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"STATIC", L"Pulse 回收站渲染测试", WS_POPUP,
        0, 0, 2200, 1440, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "recycle renderer creates hidden isolated window");
    if (!window) return;
    struct WindowGuard { HWND window; ~WindowGuard() { DestroyWindow(window); } } window_guard{window};
    Compositor compositor;
    const bool ready = compositor.Init(window);
    check(ready, "recycle hidden renderer initializes compositor");
    if (!ready) return;
    MainRenderer renderer;
    renderer.SetCompositor(&compositor);
    std::filesystem::create_directories(L"bench_data/recycle-toolbar");
    for (const float scale : {1.0f, 1.5f, 2.0f}) {
        compositor.RecreateTextFormats(scale);
        renderer.SetScale(scale);
        for (const float width_dip : {1100.0f, 320.0f}) {
            const float width = width_dip * scale, height = 660 * scale;
            compositor.Resize(static_cast<int>(width), static_cast<int>(height));
            const D2D1_RECT_F bounds{0, 0, width, height};
            for (const bool dark : {false, true}) {
                WindowViewModel vm;
                vm.dark = dark;
                vm.window_effect = WindowEffect::None;
                vm.tabs.push_back({L"回收站", true});
                vm.pane.path = L"pulse:recycle";
                vm.pane.header_text = L"回收站";
                vm.pane.is_recycle = true;
                vm.pane.view_mode = ViewMode::Details;
                vm.pane.selected_index = 0;
                vm.pane.selected_count = vm.pane.visible_selected_count = 1;
                ListEntryView document;
                document.name = L"合成测试文档.txt";
                document.type_text = L"文本文档";
                document.date_text = L"2026-10-14 12:00";
                document.attrs = FILE_ATTRIBUTE_NORMAL;
                vm.pane.entries.push_back(document);
                document.name = L"合成测试图片.png";
                document.type_text = L"PNG 图像";
                vm.pane.entries.push_back(document);
                PaneSlotView slot;
                slot.rect = renderer.ContentRect(width, height);
                slot.focused = true;
                slot.pane = vm.pane;
                vm.pane_slots.push_back(slot);
                const auto toolbar = renderer.ToolbarLayoutAt(width, 100 * scale);
                std::array<float, 3> text_widths{};
                constexpr l10n::StringId labels[]{l10n::StringId::EmptyRecycleBin,
                    l10n::StringId::RestoreAll, l10n::StringId::Restore};
                for (size_t i = 0; i < text_widths.size(); ++i)
                    text_widths[i] = MeasureTextWidth(compositor.DwriteFactory(), compositor.TextFormat(), l10n::Get(labels[i]));
                const auto actions = MakeRecycleToolbarLayout({toolbar.create.left, toolbar.create.top,
                    width - 12 * scale, toolbar.create.bottom}, scale, text_widths);
                check(actions.icon_only == (width_dip < 600),
                    "real Chinese metrics choose normal labels and compact narrow icons");
                const auto capture = [&](const wchar_t* suffix) {
                    vm.pane_slots.front().pane = vm.pane;
                    auto* dc = compositor.Dc();
                    dc->BeginDraw();
                    renderer.Render(vm, bounds, MakeTheme(dark, HexColor(0x0078D4)));
                    const HRESULT drawn = dc->EndDraw();
                    const std::wstring path = L"bench_data/recycle-toolbar/zh-CN-" +
                        std::to_wstring(static_cast<int>(scale * 100)) +
                        (width_dip < 600 ? L"-narrow" : L"-normal") +
                        (dark ? L"-dark-" : L"-light-") + suffix + L".png";
                    check(SUCCEEDED(drawn) && compositor.SaveSnapshot(path.c_str()),
                        "real recycle renderer saves screenshot");
                    fprintf(log, "[IMAGE] %ls\n", path.c_str());
                };
                capture(L"selected");
                for (int action = 0; action < 3; ++action) {
                    const auto& button = actions.buttons[static_cast<size_t>(action)];
                    check(button.right <= width - 12 * scale + 0.1f && button.left >= toolbar.create.left &&
                        button.bottom <= renderer.TitleBarHeight() + renderer.ToolbarHeight(),
                        "drawn recycle command bounds remain inside the toolbar");
                    for (const float fraction : {0.15f, 0.5f, 0.85f}) {
                        const auto hit = renderer.HitTest(vm, bounds,
                            button.left + (button.right - button.left) * fraction,
                            (button.top + button.bottom) / 2);
                        check(hit.region == HitTestResult::RecycleToolbarAction && hit.index == action,
                            "real hit-test agrees with each drawn recycle command");
                    }
                    if (action < 2) {
                        const auto hit = renderer.HitTest(vm, bounds,
                            (button.right + actions.buttons[static_cast<size_t>(action + 1)].left) / 2,
                            (button.top + button.bottom) / 2);
                        check(hit.region != HitTestResult::RecycleToolbarAction,
                            "gaps between recycle buttons do not dispatch commands");
                    }
                }
                vm.pane.selected_index = -1;
                vm.pane.selected_count = vm.pane.visible_selected_count = 0;
                const auto& restore = actions.buttons[2];
                auto hit = renderer.HitTest(vm, bounds, (restore.left + restore.right) / 2,
                    (restore.top + restore.bottom) / 2);
                check(hit.region != HitTestResult::RecycleToolbarAction,
                    "real hit-test disables restore without selection");
                if (scale == 1 && width_dip == 1100) capture(L"unselected");
                vm.pane.loading = true;
                for (const auto& button : actions.buttons) {
                    hit = renderer.HitTest(vm, bounds, (button.left + button.right) / 2,
                        (button.top + button.bottom) / 2);
                    check(hit.region != HitTestResult::RecycleToolbarAction,
                        "loading recycle buttons cannot hit any action");
                }
                vm.pane.loading = false;
                vm.pane.entries.clear();
                for (const auto& button : actions.buttons) {
                    hit = renderer.HitTest(vm, bounds, (button.left + button.right) / 2,
                        (button.top + button.bottom) / 2);
                    check(hit.region != HitTestResult::RecycleToolbarAction,
                        "empty recycle buttons cannot hit any action");
                }
                if (scale == 1 && width_dip == 1100) capture(L"empty");
                check(!IsWindowVisible(window), "renderer fixture is never visible");
            }
        }
    }
}
}

bool RunRecyclePlaceMenuTest() {
    using namespace pulse;
    std::filesystem::create_directories(L"bench_data");
    FILE* log = nullptr;
    _wfopen_s(&log, L"bench_data/recycle_place_menu_test.log", L"w");
    if (!log) return false;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        fflush(log);
        failures += !ok;
    };
    const auto menu = app::BuildRecyclePlaceMenu(true, true);
    check(menu.size() == 7 && menu[0].command == app::CmdEmptyRecycle &&
        menu[1].command == app::CmdRestoreAllRecycle && menu[0].enabled && menu[1].enabled,
        "occupied recycle menu offers empty and restore all");
    check(menu[2].command == app::CmdOpenInNewTab && menu[3].command == app::CmdOpenInNewWindow &&
        menu[4].children.size() == 2 &&
        menu[4].children[0].command == app::CmdOpenInNewPaneVertical &&
        menu[4].children[1].command == app::CmdOpenInNewPaneHorizontal,
        "recycle menu exposes new tab, new window and both split orientations");
    check(menu[5].command == app::CmdUnpinQuickAccess && menu[6].command == app::CmdProperties,
        "recycle menu exposes unpin and recycle properties");
    const auto empty = app::BuildRecyclePlaceMenu(false, false);
    check(!empty[0].enabled && !empty[1].enabled && !empty[4].enabled &&
        !empty[4].children[0].enabled && !empty[4].children[1].enabled &&
        empty[2].enabled && empty[3].enabled && empty[5].enabled && empty[6].enabled,
        "empty recycle and unavailable split disable only their applicable actions");

    std::vector<fs::DirEntry> entries(3);
    entries[0].full_path = L"C:\\original\\a.txt";
    entries[0].recycle_path = L"C:\\$Recycle.Bin\\fixture\\$Ra.txt";
    entries[1].full_path = L"C:\\original\\invalid.txt";
    entries[2].recycle_path = L"D:\\$Recycle.Bin\\fixture\\$Rb";
    check(RecycleRestorePaths(entries) == std::vector<std::wstring>{entries[0].recycle_path, entries[2].recycle_path},
        "restore request uses payload paths and skips missing recycle identities");
    check(RecycleRestorePaths({}).empty(), "empty enumeration submits no restore sources");
    ui::PaneViewModel recycle;
    recycle.is_recycle = true;
    check(!ui::RecycleToolbarActionEnabled(recycle, 0) && !ui::RecycleToolbarActionEnabled(recycle, 1) &&
        !ui::RecycleToolbarActionEnabled(recycle, 2), "empty recycle toolbar actions remain disabled");
    recycle.snapshot = std::make_shared<const std::vector<fs::DirEntry>>(entries);
    recycle.filter_text = L"no matching entries";
    check(ui::RecycleToolbarActionEnabled(recycle, 0) && ui::RecycleToolbarActionEnabled(recycle, 1) &&
        !ui::RecycleToolbarActionEnabled(recycle, 2), "global recycle actions ignore filters while selection restore stays disabled");
    recycle.selected_count = 1;
    check(ui::RecycleToolbarActionEnabled(recycle, 2), "selected recycle item enables restore");
    recycle.loading = true;
    check(!ui::RecycleToolbarActionEnabled(recycle, 0) && !ui::RecycleToolbarActionEnabled(recycle, 1) &&
        !ui::RecycleToolbarActionEnabled(recycle, 2), "loading recycle toolbar cannot invoke stale actions");
    for (const float scale : {1.0f, 1.5f, 2.0f}) {
        for (const float width : {120.0f, 400.0f, 900.0f}) {
            const auto toolbar = ui::MakeRecycleToolbarLayout({0, 0, width * scale, 32 * scale}, scale,
                {80 * scale, 112 * scale, 32 * scale});
            bool fits = toolbar.buttons[0].left >= 0 && toolbar.buttons.back().right <= width * scale;
            for (size_t i = 1; i < toolbar.buttons.size(); ++i)
                fits = fits && toolbar.buttons[i - 1].right <= toolbar.buttons[i].left;
            check(fits, "recycle toolbar fits narrow and scaled rows without overlap");
        }
    }

    auto state = std::make_unique<AppState>();
    state->isolatedTest = true;
    state->shot.active = true;
    state->appPrefs.persist = state->searchHistory.persist = state->ctxMenuPrefs.persist = state->places.persist = false;
    state->sidebarQuickAccessHiddenMask = 1u << static_cast<unsigned>(app::BuiltinQuickAccess::Desktop);
    const auto before = state->sidebarQuickAccessHiddenMask;
    check(HandleRecyclePlaceCommand(*state, app::CmdUnpinQuickAccess) &&
        state->sidebarQuickAccessHiddenMask == (before | (1u << static_cast<unsigned>(app::BuiltinQuickAccess::RecycleBin))),
        "unpin targets the recycle builtin and preserves other hidden links");
    check(!HandleRecyclePlaceCommand(*state, app::CmdDelete),
        "unrelated commands cannot dispatch against the active folder selection");
    check(!HandleRecycleToolbarCommand(*state, 0) && !HandleRecycleToolbarCommand(*state, 1) &&
        !HandleRecycleToolbarCommand(*state, 2), "recycle toolbar cannot affect a missing or ordinary folder");
    check(!CanOpenRecycleInNewPane(*state), "missing layout cannot split");
    state->window_tabs.NewTab(L"C:\\synthetic-recycle-menu");
    auto* layout = state->window_tabs.Active();
    state->pane = layout->panes.front().get();
    auto* original_pane = state->pane;
    check(CanOpenRecycleInNewPane(*state), "ordinary folder can open recycle in another pane");
    for (const auto side : {app::ContentDrop::Right, app::ContentDrop::Bottom}) {
        app::PanePlacement placement;
        placement.layout = app::LayoutPreset::Custom;
        placement.pane = original_pane;
        placement.side = side;
        check(app::AddPaneFromTab(*layout, app::MakeSingleLayoutTab(app::MakeRecyclePath()), placement),
            "existing split model accepts the recycle virtual location");
    }
    check(original_pane->ActiveTab()->current_path == L"C:\\synthetic-recycle-menu" &&
        layout->VisiblePanes().size() == 3,
        "new recycle panes preserve the original folder and its pane");
    app::AddPaneFromTab(*layout, app::MakeSingleLayoutTab(app::MakeRecyclePath()), app::ContentDrop::Right);
    check(!CanOpenRecycleInNewPane(*state), "four visible panes disable further splitting");
    state->worker.Stop();
    state->watches.Stop();
    CaptureRecycleToolbar(check, log);
    fclose(log);
    return failures == 0;
}
#endif
