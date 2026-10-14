#include "home_widgets.h"
#include "app_internal.h"
#include "quick_access.h"
#include "sidebar_navigation.h"
#include "context_menu.h"
#include "../common/localization.h"

namespace {
#ifdef PULSE_WITH_SELFTEST
std::function<int(const std::vector<pulse::ui::FluentMenuItem>&)> home_menu_test_callback;
#endif
std::wstring PinTarget(const std::wstring& path,bool file) {
    if(path.empty() || pulse::fs::IsVirtualPath(path)) return {};
    return file ? pulse::fs::ParentPath(path) : path;
}
}

namespace pulse {
void HandleHomeWidgetClick(AppState& state, const std::wstring& path, bool file) {
    if(path.empty()) return;
    if(file) {
        // 记录中的文件类型已知，沿用正常打开路径，避免在 UI 线程探测离线共享。
        state.ops.OpenWith(path);
        RecordRecentOpen(state,path,app::PlaceItemKind::File);
    } else NavigateTo(state,path);
}

std::vector<ui::FluentMenuItem> BuildHomeItemMenu(AppState& state,const std::wstring& path,bool file,bool recent) {
    if(path.empty() || fs::IsVirtualPath(path)) return {};
    auto items=app::BuildSidebarNavigationMenu();
    if(file) items.resize(1);
    const auto target=PinTarget(path,file);
    AppendQuickAccessCommand(state,items,target.empty() ? std::vector<std::wstring>{} : std::vector<std::wstring>{target});
    if(file) {
        for(auto& item:items) {
            if(item.command==app::CmdPinQuickAccess)
                item.text=l10n::Pick(L"将所在文件夹固定到快速访问",L"Pin containing folder to Quick access");
            if(item.command==app::CmdUnpinQuickAccess)
                item.text=l10n::Pick(L"从快速访问取消固定所在文件夹",L"Unpin containing folder from Quick access");
        }
    }
    if(recent) {
        ui::FluentMenuItem remove;
        remove.command=app::CmdRemoveRecent;remove.text=l10n::Get(l10n::StringId::RemoveRecent);remove.glyph=L"\xE711";
        items.push_back(std::move(remove));
    }
    return items;
}

bool HandleHomeItemMenuCommand(AppState& state,const std::wstring& path,bool file,bool recent,int command) {
    if(path.empty() || fs::IsVirtualPath(path)) return false;
    if(command==app::CmdPinQuickAccess || command==app::CmdUnpinQuickAccess) {
        const auto target=PinTarget(path,file);
        if(target.empty()) return false;
        HandleQuickAccessCommand(state,command,{target});
        app::ForEachWindowSharingCore(state,[&](AppState& window) {
            if(command==app::CmdPinQuickAccess) {
                const auto bit=1u<<static_cast<unsigned>(app::SidebarSectionId::QuickAccess);
                window.sidebarHiddenMask&=~bit;window.sidebarCollapsedMask&=~bit;
            }
            if(window.hwnd) InvalidateRect(window.hwnd,nullptr,FALSE);
        });
        return true;
    }
    if(recent && command==app::CmdRemoveRecent) {
        if(state.places.RemoveRecent(path)) {
            app::ForEachWindowSharingCore(state,[&](AppState& window) {
                window.recentViewsRefreshPending=true;
                if(window.hwnd) InvalidateRect(window.hwnd,nullptr,FALSE);
            });
        }
        return true;
    }
    if(command==app::CmdOpen) {HandleHomeWidgetClick(state,path,file);return true;}
    return !file && HandleSidebarNavigationCommand(state,path,command);
}

void ShowHomeItemMenu(AppState& state,const std::wstring& path,bool file,bool recent,POINT point) {
    auto items=BuildHomeItemMenu(state,path,file,recent);
    if(items.empty()) return;
    int command=app::CmdNone;
#ifdef PULSE_WITH_SELFTEST
    if(state.isolatedTest && home_menu_test_callback) command=home_menu_test_callback(items);
    else
#endif
    {
        if(!EnsureMenu(state)) return;
        state.context_menu.Close();
        command=state.menu->TrackPopup(point,std::move(items));
    }
    HandleHomeItemMenuCommand(state,path,file,recent,command);
}

bool HandleHomeWidgetContextMenu(AppState& state,const ui::HitTestResult& hit,POINT point) {
    if(hit.region!=ui::HitTestResult::HomeWidget || hit.path.empty()) return false;
    if(hit.path==app::MakeRecyclePath()) ShowRecyclePlaceMenu(state,point);
    else ShowHomeItemMenu(state,hit.path,hit.sub_index==1,hit.index/100000==4,point);
    return true;
}

#ifdef PULSE_WITH_SELFTEST
void SetHomeMenuCallbackForTest(std::function<int(const std::vector<ui::FluentMenuItem>&)> callback) {
    home_menu_test_callback=std::move(callback);
}
#endif
}
