#include "home_widgets.h"
#include "app_model.h"
#include "app_prefs.h"
#include "places.h"
#include <filesystem>
#include <fstream>

#ifdef PULSE_WITH_SELFTEST
#include "app_internal.h"
#include "app_commands.h"
#include "startup_location.h"
#include "app_input.h"
#include "../ui/ui_renderer_internal.h"
#include <algorithm>
#include <memory>
#include <optional>

namespace {
int VerifyHomeNavigation(pulse::AppState& state,const std::filesystem::path& directory,std::ofstream& log) {
    using namespace pulse;
    int failed=0;
    auto check=[&](bool ok,const char* label){log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<'\n';if(!ok)++failed;};
    const auto fixture=directory/L"home-navigation-fixture";
    std::filesystem::create_directories(fixture/L"visited-folder");
    const auto folder=fs::NormalizePath((fixture/L"visited-folder").wstring());
    const auto file=fs::NormalizePath((fixture/L"opened-file.txt").wstring());
    std::ofstream(fixture/L"opened-file.txt")<<"recent fixture";
    state.places.recent_items.clear();
    state.worker.Start([&](app::WorkResult result) {
        std::lock_guard lock(state.resultMutex);state.results.push(std::move(result));
    });
    const auto wait=[&](const std::function<bool()>& ready) {
        const auto end=GetTickCount64()+5000;
        while(GetTickCount64()<end) {
            ProcessPendingResults(state);
            if(ready()) return true;
            Sleep(10);
        }
        return false;
    };
    NavigateTo(state,folder);
    check(wait([&]{return ActiveTab(state) && !ActiveTab(state)->loading;}),"真实目录导航完成");
    check(state.places.FindRecent(folder)!=nullptr,"真实导航立即写入最近访问记录");
    OpenNewTab(state);
    check(ActiveTab(state) && app::IsHomePath(ActiveTab(state)->current_path) && BuildVm(state,false).pane.is_home,
        "从文件夹点击新标签页真正打开默认主页");
    const auto home=BuildVm(state,false).home_widgets;
    check(home.sections[4].items.size()==1 && home.sections[4].items[0].path==folder,"真实访问记录进入主页组件");
    OpenTabAt(state,app::MakeRecentPath());
    check(wait([&]{return ActiveTab(state) && !ActiveTab(state)->loading && ActiveTab(state)->EntryCount()==1;}),
        "最近访问页面从真实记录异步加载目录");
    auto* recent=ActiveTab(state);
    TickRecentViews(state);
    check(wait([&]{return !recent->loading;}) && !state.recentViewsRefreshPending,
        "先完成目录访问触发的刷新，再独立验证新文件记录");
    const auto snapshot=recent->snapshot;
    RecordRecentOpen(state,file,app::PlaceItemKind::File);
    check(state.recentViewsRefreshPending && recent->snapshot==snapshot,"记录新文件时保留正在显示的快照并排队刷新");
    check(TickRecentViews(state) && !state.recentViewsRefreshPending && recent->snapshot==snapshot,
        "下一帧合并刷新最近访问且加载期间没有空白");
    check(wait([&]{return !recent->loading && recent->EntryCount()==2;}),"无需重开标签即可看到新访问记录");
    const auto entries=recent->snapshot;
    check(entries && (*entries)[0].full_path==file && !(*entries)[0].is_dir && (*entries)[1].is_dir,
        "最近访问真实列表按最新记录排序并区分文件和目录");
    check(!TickRecentViews(state),"没有新记录时不重复枚举最近访问");
    state.worker.Stop();
    return failed;
}
int CaptureHomeWidgets(const std::filesystem::path& directory, std::ofstream& log) {
    using namespace pulse;
    int failures=0;
    auto check=[&](bool ok,const char* label){log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<'\n';if(!ok)++failures;return ok;};
    const auto language=l10n::preference();
    l10n::SetLanguage(L"zh-CN");
    WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=DefWindowProcW;
    wc.lpszClassName=L"PulseHomeWidgetsTest";RegisterClassW(&wc);
    for(float scale:{1.0f,1.5f,2.0f}) {
        auto state=std::make_unique<AppState>();
        state->isolatedTest=true;state->scale=scale;
        state->places.persist=state->appPrefs.persist=state->ctxMenuPrefs.persist=state->searchHistory.persist=false;
        state->hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,L"",WS_POPUP,
            -30000,-30000,static_cast<int>(1320*scale),static_cast<int>(920*scale),nullptr,nullptr,wc.hInstance,nullptr);
        if(!check(state->hwnd && state->compositor.Init(state->hwnd),"初始化隔离主页渲染器")) {
            if(state->hwnd)DestroyWindow(state->hwnd);
            state->hwnd=nullptr;continue;
        }
        state->renderer.SetCompositor(&state->compositor);state->renderer.SetScale(scale);
        state->compositor.RecreateTextFormats(scale);
        state->window_tabs.NewTab(std::wstring(app::kHomePath));BindCurrentLayout(*state);
        auto* tab=ActiveTab(*state);tab->loading=false;
        tab->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
        app::SidebarEntry quick;quick.label=L"桌面";quick.path=L"C:\\fixture\\Desktop";quick.glyph=L"\xE8B7";
        state->sidebar.quick_access.push_back(quick);
        const wchar_t* names[]={L"桌面",L"下载",L"文档",L"图片",L"音乐",L"视频",L"回收站"};
        const wchar_t* glyphs[]={L"\xE7F4",L"\xE896",L"\xE8A5",L"\xEB9F",L"\xE8D6",L"\xE714",L"\xE75C"};
        for(int i=0;i<7;++i) {
            auto folder=quick;folder.label=names[i];folder.glyph=glyphs[i];folder.path=L"C:\\fixture\\"+std::wstring(names[i]);
            state->sidebar.home_folders.push_back(std::move(folder));
        }
        state->places.quick_access_paths={L"C:\\fixture\\wallpaper",L"C:\\fixture\\工作资料"};
        app::SidebarEntry drive;drive.label=L"本地磁盘 (C:)";drive.path=L"C:\\";drive.detail=L"128 GB 可用，共 256 GB";
        drive.glyph=L"\xE7F1";drive.is_drive=true;drive.used_ratio=0.5f;
        drive.capacity_valid=true;drive.free_bytes=128ull<<30;drive.total_bytes=256ull<<30;
        for(int i=0;i<4;++i) {
            auto disk=drive;disk.label=L"本地磁盘 ("+std::wstring(1,static_cast<wchar_t>(L'C'+i))+L":)";
            disk.path=std::wstring(1,static_cast<wchar_t>(L'C'+i))+L":\\";
            state->sidebar.drives.push_back(std::move(disk));
        }
        state->places.networks.push_back({L"资料共享",L"\\\\offline.invalid\\data"});
        state->places.tags.push_back({L"work",L"工作",0x0078D4,{L"C:\\fixture\\report.txt"}});
        state->places.recent_items.push_back({L"C:\\fixture\\项目说明.txt",app::PlaceItemKind::File,0});
        state->places.recent_items.push_back({L"C:\\fixture\\最近访问的文件夹",app::PlaceItemKind::Folder,0});
        const auto bounds=D2D1::RectF(0,0,static_cast<float>(state->compositor.Width()),static_cast<float>(state->compositor.Height()));
        const uint32_t all_sections=(1u<<app::kSidebarSectionCount)-1;
        for(int section=0;section<app::kSidebarSectionCount;++section) {
            const uint32_t bit=1u<<section;
            // 与右键“隐藏分组”相同的状态，并保留其原来的折叠状态。
            state->sidebarHiddenMask=all_sections;
            state->sidebarCollapsedMask=all_sections;
            state->sidebarQuickAccessHiddenMask=0xffffffffu;
            auto vm=BuildVm(*state,false);
            check(vm.settings_sidebar_hidden==all_sections,"设置开关读取右键隐藏的真实侧边栏状态");
            ui::HitTestResult action;action.region=ui::HitTestResult::SettingsToggle;
            action.index=ui::kSettingsSidebarToggleBase+section;
            check(HandleSettingsControl(*state,action),"侧边栏恢复开关进入真实输入处理");
            vm=BuildVm(*state,false);
            check(vm.settings_sidebar_hidden==(all_sections&~bit) && state->sidebarCollapsedMask==(all_sections&~bit),
                "恢复当前分组同时展开，其他分组保持原状");
            const int group=app::SidebarSectionIndex(vm,section);
            check(group>=0 && !vm.sidebar[group].hidden && !vm.sidebar[group].collapsed,"恢复后的真实侧边栏可见且展开");
            check(state->appPrefs.home_widgets_hidden==0 && vm.home_widgets.sections[3].visible,
                "侧边栏恢复不改变主页小组件开关");
            if(section==static_cast<int>(app::SidebarSectionId::QuickAccess))
                check(state->sidebarQuickAccessHiddenMask==0,"重新显示快速访问恢复内置入口");
            HandleSettingsControl(*state,action);
            check(BuildVm(*state,false).settings_sidebar_hidden==all_sections,"设置中再次关闭正确隐藏分组");
        }
        state->sidebarHiddenMask=state->sidebarCollapsedMask=state->sidebarQuickAccessHiddenMask=0;
        for(bool dark:{false,true}) {
            auto vm=BuildVm(*state,false);vm.dark=dark;
            check(vm.pane.is_home && vm.home_widgets.sections.size()==5,"真实视图模型绑定主页缓存");
            auto* dc=state->compositor.Dc();dc->BeginDraw();
            state->renderer.Render(vm,bounds,ui::MakeTheme(dark,ui::HexColor(0x0078D4)));
            const auto name=L"home-"+std::to_wstring(static_cast<int>(scale*100))+(dark ? L"-dark.png" : L"-light.png");
            check(SUCCEEDED(dc->EndDraw()) && state->compositor.SaveSnapshot((directory/name).c_str()),"中文主页两主题截图");
            vm.settings_open=true;vm.settings_page=0;vm.settings_scroll=0;vm.settings_expanded=0;
            ui::fluent::Painter painter(&state->compositor);painter.SetScale(scale);painter.BeginFrame(ui::MakeTheme(dark,ui::HexColor(0x0078D4)));
            auto layout=ui::MakeSettingsLayout(vm,bounds,scale,state->renderer.TitleBarHeight(),28*scale,&painter);
            vm.settings_scroll=layout.home_widgets_section.top-layout.content.top-12*scale;
            layout=ui::MakeSettingsLayout(vm,bounds,scale,state->renderer.TitleBarHeight(),28*scale,&painter);
            for(int i=0;i<5;++i) {
                const auto r=layout.home_widget_row[i];
                const auto hit=state->renderer.HitTest(vm,bounds,r.right-38*scale,(r.top+r.bottom)/2);
                check(hit.region==ui::HitTestResult::SettingsToggle && hit.index==40+i,"主页开关真实命中映射");
            }
            dc->BeginDraw();state->renderer.Render(vm,bounds,ui::MakeTheme(dark,ui::HexColor(0x0078D4)));
            const auto settings=L"home-settings-"+std::to_wstring(static_cast<int>(scale*100))+(dark ? L"-dark.png" : L"-light.png");
            check(SUCCEEDED(dc->EndDraw()) && state->compositor.SaveSnapshot((directory/settings).c_str()),"中文主页设置两主题截图");
            vm.settings_scroll=0;
            layout=ui::MakeSettingsLayout(vm,bounds,scale,state->renderer.TitleBarHeight(),28*scale,&painter);
            vm.settings_scroll=layout.home_folder_card.top-layout.content.top-12*scale;
            layout=ui::MakeSettingsLayout(vm,bounds,scale,state->renderer.TitleBarHeight(),28*scale,&painter);
            const auto home_button=layout.home_folder_home;
            check(home_button.top>=layout.home_folder_card.top+56*scale,"默认位置按钮避开说明文字");
            const auto home_hit=state->renderer.HitTest(vm,bounds,(home_button.left+home_button.right)/2,(home_button.top+home_button.bottom)/2);
            check(home_hit.region==ui::HitTestResult::SettingsHomeFolder && home_hit.index==2,"默认位置主页按钮命中正确");
            dc->BeginDraw();state->renderer.Render(vm,bounds,ui::MakeTheme(dark,ui::HexColor(0x0078D4)));
            const auto location=L"home-default-location-"+std::to_wstring(static_cast<int>(scale*100))+(dark ? L"-dark.png" : L"-light.png");
            check(SUCCEEDED(dc->EndDraw()) && state->compositor.SaveSnapshot((directory/location).c_str()),"主页默认位置和新标签页设置截图");
            vm.settings_sidebar_hidden=1u<<static_cast<int>(app::SidebarSectionId::Tags);
            vm.settings_scroll=0;
            layout=ui::MakeSettingsLayout(vm,bounds,scale,state->renderer.TitleBarHeight(),28*scale,&painter);
            vm.settings_scroll=layout.sidebar_sections_section.top-layout.content.top-12*scale;
            layout=ui::MakeSettingsLayout(vm,bounds,scale,state->renderer.TitleBarHeight(),28*scale,&painter);
            for(int i=0;i<ui::kSettingsSidebarSectionCount;++i) {
                const auto r=layout.sidebar_section_row[i];
                const auto hit=state->renderer.HitTest(vm,bounds,r.right-38*scale,(r.top+r.bottom)/2);
                check(hit.region==ui::HitTestResult::SettingsToggle && hit.index==ui::kSettingsSidebarToggleBase+i,
                    "八个侧边栏开关真实命中与分组标识一致");
            }
            check(layout.sidebar_sections_card.top>=layout.home_widgets_card.bottom,"侧边栏设置与主页设置没有重叠");
            dc->BeginDraw();state->renderer.Render(vm,bounds,ui::MakeTheme(dark,ui::HexColor(0x0078D4)));
            const auto sidebar=L"home-sidebar-settings-"+std::to_wstring(static_cast<int>(scale*100))+(dark ? L"-dark.png" : L"-light.png");
            check(SUCCEEDED(dc->EndDraw()) && state->compositor.SaveSnapshot((directory/sidebar).c_str()),"侧边栏恢复设置明暗主题截图");
        }
        state->compositor.Resize(static_cast<int>(640*scale),static_cast<int>(920*scale));
        const auto narrow_bounds=D2D1::RectF(0,0,static_cast<float>(state->compositor.Width()),static_cast<float>(state->compositor.Height()));
        for(bool dark:{false,true}) {
            const auto vm=BuildVm(*state,false);
            auto* dc=state->compositor.Dc();dc->BeginDraw();
            state->renderer.Render(vm,narrow_bounds,ui::MakeTheme(dark,ui::HexColor(0x0078D4)));
            const auto name=L"home-narrow-"+std::to_wstring(static_cast<int>(scale*100))+(dark ? L"-dark.png" : L"-light.png");
            check(SUCCEEDED(dc->EndDraw()) && state->compositor.SaveSnapshot((directory/name).c_str()),"窄窗口主页卡片换行截图");
        }
        if(scale==1.0f) failures+=VerifyHomeNavigation(*state,directory,log);
        const HWND hwnd=state->hwnd;state->hwnd=nullptr;DestroyWindow(hwnd);
    }
    UnregisterClassW(wc.lpszClassName,wc.hInstance);
    l10n::SetLanguage(l10n::LanguageId(language));
    return failures;
}
}
int RunHomeMenuTest(const wchar_t* output) {
    using namespace pulse;
    const auto directory = std::filesystem::absolute(output).parent_path();
    std::filesystem::create_directories(directory);
    std::ofstream log{std::filesystem::path(output)};
    int failed = 0;
    const auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        log.flush();
        if (!ok) ++failed;
        return ok;
    };
    const auto previous_language = l10n::preference();
    l10n::SetLanguage(L"zh-CN");
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        auto state = std::make_unique<AppState>();
        state->isolatedTest = true;
        state->scale = scale;
        state->places.persist = state->appPrefs.persist = state->ctxMenuPrefs.persist = state->searchHistory.persist = false;
        state->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
            L"STATIC", L"Pulse 最近访问菜单测试", WS_POPUP, -30000, -30000,
            static_cast<int>(1100 * scale), static_cast<int>(850 * scale),
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!check(state->hwnd && state->compositor.Init(state->hwnd), "初始化隔离菜单渲染器")) {
            if (state->hwnd) DestroyWindow(state->hwnd);
            state->hwnd = nullptr;
            continue;
        }
        state->renderer.SetCompositor(&state->compositor);
        state->renderer.SetScale(scale);
        state->compositor.RecreateTextFormats(scale);
        state->window_tabs.NewTab(std::wstring(app::kHomePath));
        BindCurrentLayout(*state);
        auto* tab = ActiveTab(*state);
        tab->loading = false;
        tab->SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
        const std::wstring folder = L"C:\\fixture\\最近访问的文件夹";
        const std::wstring file = L"C:\\fixture\\工作资料\\说明文档.txt";
        const auto parent = fs::ParentPath(file);
        state->places.RecordRecent(folder, app::PlaceItemKind::Folder);
        state->places.RecordRecent(file, app::PlaceItemKind::File);
        const auto contains_command = [](const auto& menu, int command) {
            return std::any_of(menu.begin(), menu.end(), [&](const auto& item) { return item.command == command; });
        };
        auto menu = BuildHomeItemMenu(*state, folder, false, true);
        check(contains_command(menu, app::CmdPinQuickAccess) && contains_command(menu, app::CmdRemoveRecent) &&
            contains_command(menu, app::CmdOpenInNewTab), "最近访问目录菜单提供固定、移除记录和新标签打开");
        check(BuildHomeItemMenu(*state, std::wstring(app::kHomePath), false, true).empty() &&
            !HandleHomeItemMenuCommand(*state, std::wstring(app::kHomePath), false, true, app::CmdPinQuickAccess),
            "虚拟位置不能误固定为普通文件夹");
        const auto bit = 1u << static_cast<unsigned>(app::SidebarSectionId::QuickAccess);
        state->sidebarHiddenMask = state->sidebarCollapsedMask = bit;
        auto other = std::make_unique<AppState>(state->core);
        other->isolatedTest = true;
        other->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP,
            -30000, -30000, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        other->sidebarHiddenMask = other->sidebarCollapsedMask = bit;
        state->core.windows.Add(state->hwnd, state.get());
        state->core.windows.Add(other->hwnd, other.get());
        const auto bounds = D2D1::RectF(0, 0, static_cast<float>(state->compositor.Width()),
            static_cast<float>(state->compositor.Height()));
        const auto find_point = [&](const std::wstring& path, bool home) -> std::optional<POINT> {
            const auto vm = BuildVm(*state, false);
            for (int y = 0; y < static_cast<int>(bounds.bottom); y += 8)
                for (int x = 0; x < static_cast<int>(bounds.right); x += 8) {
                    const auto hit = state->renderer.HitTest(vm, bounds, static_cast<float>(x), static_cast<float>(y));
                    if ((home && hit.region == ui::HitTestResult::HomeWidget && hit.path == path) ||
                        (!home && hit.region == ui::HitTestResult::Row && hit.index >= 0 &&
                            EntryFullPath(*ActiveTab(*state), hit.index) == path)) return POINT{x, y};
                }
            return {};
        };
        int popup_calls = 0;
        SetHomeMenuCallbackForTest([&](const auto& items) {
            ++popup_calls;
            check(contains_command(items, app::CmdPinQuickAccess), "真实右键菜单收到固定命令");
            return app::CmdPinQuickAccess;
        });
        const auto folder_point = find_point(folder, true);
        if (check(folder_point.has_value(), "真实主页渲染命中最近访问目录卡片"))
            HandleRButtonUp(state.get(), state->hwnd, WM_RBUTTONUP, 0, MAKELPARAM(folder_point->x, folder_point->y));
        check(popup_calls == 1 && state->places.IsQuickAccessPinned(folder), "主页右键输入路径实际固定目录");
        check(!(state->sidebarHiddenMask & bit) && !(state->sidebarCollapsedMask & bit), "固定后显示并展开快速访问分组");
        check(other->hwnd && !(other->sidebarHiddenMask & bit) && !(other->sidebarCollapsedMask & bit) &&
            other->places.IsQuickAccessPinned(folder), "同进程另一窗口同步固定数据并显示快速访问");
        auto vm = BuildVm(*state, false);
        const int section = app::SidebarSectionIndex(vm, static_cast<int>(app::SidebarSectionId::QuickAccess));
        check(section >= 0 && std::any_of(vm.sidebar[section].items.begin(), vm.sidebar[section].items.end(),
            [&](const auto& item) { return item.path == folder; }), "固定后左侧快速访问立即包含目录");
        check(std::any_of(vm.home_widgets.sections[0].items.begin(), vm.home_widgets.sections[0].items.end(),
            [&](const auto& item) { return item.path == folder; }), "固定后主页快速访问立即包含同一目录");
        menu = BuildHomeItemMenu(*state, folder, false, true);
        check(contains_command(menu, app::CmdUnpinQuickAccess) && !contains_command(menu, app::CmdPinQuickAccess),
            "已固定目录改为取消固定");
        HandleHomeItemMenuCommand(*state, folder, false, true, app::CmdPinQuickAccess);
        check(state->places.quick_access_paths.size() == 1, "重复固定不产生重复入口");
        HandleHomeItemMenuCommand(*state, folder, false, true, app::CmdUnpinQuickAccess);
        check(!state->places.IsQuickAccessPinned(folder) && state->places.FindRecent(folder), "取消固定保留访问历史");
        menu = BuildHomeItemMenu(*state, file, true, true);
        check(std::any_of(menu.begin(), menu.end(), [](const auto& item) {
            return item.command == app::CmdPinQuickAccess && item.text == L"将所在文件夹固定到快速访问";
        }), "文件菜单明确说明固定所在文件夹");
        const auto file_point = find_point(file, true);
        if (check(file_point.has_value(), "真实主页命中最近访问文件卡片"))
            HandleRButtonUp(state.get(), state->hwnd, WM_RBUTTONUP, 0, MAKELPARAM(file_point->x, file_point->y));
        check(popup_calls == 2 && state->places.IsQuickAccessPinned(parent) && !state->places.IsQuickAccessPinned(file),
            "文件右键只固定父目录，不将文件作为目录保存");
        for (bool dark : {false, true}) {
            ui::FluentMenu popup;
            if (check(popup.Create(state->hwnd, &state->compositor, scale), "建立隔离菜单截图器")) {
                popup.SetTheme(dark, ui::HexColor(0x0078D4));
                for (bool is_file : {false, true}) {
                    const auto name = L"home-menu-" + std::to_wstring(static_cast<int>(scale * 100)) +
                        (dark ? L"-dark-" : L"-light-") + (is_file ? L"file.png" : L"folder.png");
                    check(popup.SaveDebugSnapshot((directory / name).c_str(),
                        BuildHomeItemMenu(*state, is_file ? file : folder, is_file, true), 1), "中文右键菜单渲染截图");
                }
            }
        }
        HandleHomeItemMenuCommand(*state, file, true, true, app::CmdUnpinQuickAccess);
        tab->current_path = app::MakeRecentPath();
        auto entries = std::make_shared<std::vector<fs::DirEntry>>();
        fs::DirEntry entry; entry.name = L"说明文档.txt"; entry.full_path = file; entries->push_back(entry);
        tab->SetSnapshot(entries);
        const auto row_point = find_point(file, false);
        if (check(row_point.has_value(), "真实最近访问列表命中文件行"))
            HandleRButtonUp(state.get(), state->hwnd, WM_RBUTTONUP, 0, MAKELPARAM(row_point->x, row_point->y));
        check(popup_calls == 3 && state->places.IsQuickAccessPinned(parent), "独立最近访问列表右键复用固定父目录功能");
        SetHomeMenuCallbackForTest({});
        HandleHomeItemMenuCommand(*state, file, true, true, app::CmdRemoveRecent);
        check(!state->places.FindRecent(file) && state->places.IsQuickAccessPinned(parent) && state->recentViewsRefreshPending,
            "移除访问记录保留固定入口，并排队刷新最近访问");
        check(other->recentViewsRefreshPending, "另一窗口最近访问列表同时排队刷新");
        if (scale == 1.0f) failed += VerifyHomeNavigation(*state, directory, log);
        check(!IsWindowVisible(state->hwnd), "测试窗口未显示且没有修改用户配置");
        state->core.windows.Remove(other->hwnd);
        if (other->hwnd) DestroyWindow(other->hwnd);
        other->hwnd = nullptr;
        state->core.windows.Remove(state->hwnd);
        const HWND hwnd = state->hwnd; state->hwnd = nullptr; DestroyWindow(hwnd);
    }
    l10n::SetLanguage(l10n::LanguageId(previous_language));
    return failed ? 1 : 0;
}

int RunHomeWidgetsTest(const wchar_t* output) {
    using namespace pulse;
    std::ofstream log{std::filesystem::path(output)};
    int failed=0;
    auto check=[&](bool ok,const char* label){log<<(ok ? "[PASS] " : "[FAIL] ")<<label<<'\n';if(!ok)++failed;};
    app::AppPrefs prefs;prefs.persist=false;
    check(prefs.new_tab_open==1 && app::IsHomePath(app::DefaultLocation(prefs)),"新配置默认新标签页打开主页");
    app::AppPrefs explicit_prefs;explicit_prefs.persist=false;
    explicit_prefs.FromJson(L"{\"new_tab_open\":0,\"home_folder\":\"\"}");
    check(explicit_prefs.new_tab_open==0 && app::DefaultLocation(explicit_prefs).empty(),"保留用户显式选择的当前目录和此电脑");
    explicit_prefs.home_folder=L"D:\\Chosen";explicit_prefs.new_tab_open=1;
    check(app::NewTabLocation(explicit_prefs,L"C:\\Current")==L"D:\\Chosen","自定义默认目录仍被新标签页使用");
    check(prefs.FromJson(L"{}") && prefs.home_widgets_hidden==0,"旧配置默认启用全部主页组件");
    for(uint32_t mask=0;mask<32;++mask) {
        prefs.home_widgets_hidden=mask;
        app::AppPrefs restored;restored.persist=false;
        check(restored.FromJson(prefs.ToJson()) && restored.home_widgets_hidden==mask,"五组件任意组合可往返保存");
    }
    prefs.home_widgets_hidden=31;prefs.ResetToDefaults();
    check(prefs.home_widgets_hidden==0,"恢复默认重新显示五组件");
    app::SidebarModel sidebar;
    app::SidebarEntry shortcut;shortcut.label=L"桌面";shortcut.path=L"C:\\fixture\\Desktop";
    sidebar.quick_access.push_back(shortcut);
    app::SidebarEntry drive;drive.label=L"C:";drive.path=L"C:\\";drive.is_drive=true;
    sidebar.drives.push_back(drive);
    app::PlacesCatalog places;places.persist=false;
    places.networks.push_back({L"离线共享",L"\\\\offline.invalid\\data"});
    places.tags.push_back({L"tag-1",L"工作",0x0078D4,{L"C:\\fixture\\report.txt"}});
    places.recent_items.push_back({L"C:\\fixture\\folder",app::PlaceItemKind::Folder,0});
    places.recent_items.push_back({L"C:\\fixture\\report.txt",app::PlaceItemKind::File,0});
    const auto all=app::BuildHomeWidgets(sidebar,places,0);
    check(all.sections.size()==5,"主页包含五个真实组件");
    check(all.sections[2].items.size()==1 && all.sections[2].items[0].path==places.networks[0].unc,"网络组件读取缓存，不要求共享在线");
    check(all.sections[3].items[0].path==app::MakeTagPath(L"tag-1"),"标签链接使用稳定标签标识");
    check(all.sections[4].items.size()==2 && !all.sections[4].items[0].file && all.sections[4].items[1].file,
        "最近访问同时包含文件夹和文件并保留正确打开方式");
    sidebar.home_folders={shortcut};
    for(const wchar_t* name:{L"Downloads",L"Documents",L"Pictures",L"Music",L"Videos"}) {
        auto folder=shortcut;folder.label=name;folder.path=L"C:\\fixture\\"+std::wstring(name);
        sidebar.home_folders.push_back(std::move(folder));
    }
    auto recycle=shortcut;recycle.path=app::MakeRecyclePath();sidebar.home_folders.push_back(recycle);
    places.quick_access_paths={shortcut.path,L"C:\\fixture\\wallpaper"};
    const auto folders=app::BuildHomeWidgets(sidebar,places,0);
    check(folders.sections[0].items.size()==8,"主页六个常用目录及回收站、固定文件夹去重后显示");
    sidebar.drives[0].capacity_valid=true;sidebar.drives[0].free_bytes=128ull<<30;sidebar.drives[0].total_bytes=256ull<<30;
    const auto capacity=app::BuildHomeWidgets(sidebar,places,0).sections[1].items[0].detail;
    check(capacity.find(L"128.00 GiB")!=std::wstring::npos && capacity.find(L"256.00 GiB")!=std::wstring::npos,
        "磁盘卡片由缓存显示完整可用空间和总容量");
    const auto wide=ui::HitHomeWidgets(folders,D2D1::RectF(0,0,1100,700),0,1,1050,100);
    check(wide.index==5 && wide.path==sidebar.home_folders[5].path,"宽主页六列卡片绘制与点击位置一致");
    const auto recent_hit=ui::HitHomeWidgets(all,D2D1::RectF(0,0,1100,900),0,1,40,344);
    check(recent_hit.index==400000,"最近访问紧跟磁盘并在首屏提供实际点击入口");
    for(float scale:{1.0f,1.25f,2.0f}) {
        const auto bounds=D2D1::RectF(20,30,20+320*scale,30+600*scale);
        const auto hit=ui::HitHomeWidgets(all,bounds,0,scale,bounds.left+30*scale,bounds.top+100*scale);
        check(hit.index==0 && hit.path==shortcut.path,"窄窗口及不同缩放的图块命中与布局一致");
        const auto hidden=app::BuildHomeWidgets(sidebar,places,1);
        const auto after=ui::HitHomeWidgets(hidden,bounds,0,scale,bounds.left+30*scale,bounds.top+100*scale);
        check(after.index==100000 && after.path==drive.path,"隐藏快速访问立即重排图块和命中");
        check(ui::HomeWidgetsHeight(hidden,320*scale,scale)<ui::HomeWidgetsHeight(all,320*scale,scale),"隐藏组件缩短滚动范围");
        const auto scrolled=ui::HitHomeWidgets(all,bounds,80*scale,scale,bounds.left+30*scale,bounds.top+20*scale);
        check(scrolled.index==0,"滚动后命中仍对应原图块");
    }
    const auto none=app::BuildHomeWidgets(sidebar,places,31);
    check(ui::HitHomeWidgets(none,D2D1::RectF(0,0,400,600),0,1,40,120).index==-1,"全部关闭没有残留可点击图块");
    failed+=CaptureHomeWidgets(std::filesystem::absolute(output).parent_path(),log);
    return failed ? 1 : 0;
}
#endif
