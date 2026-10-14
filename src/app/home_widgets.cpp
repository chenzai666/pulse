#include "home_widgets.h"
#include "app_model.h"
#include "places.h"
#include "../common/localization.h"
#include "../common/display_path.h"
#include <algorithm>

namespace pulse::app {
namespace {
ui::HomeWidgetItem FromSidebar(const SidebarEntry& entry) {
    ui::HomeWidgetItem item;
    item.title=entry.label;item.detail=entry.detail;item.path=entry.path;item.glyph=entry.glyph;
    item.drive=entry.is_drive;item.used_ratio=entry.used_ratio;
    item.icon_color=entry.color;item.danger=entry.danger;
    if(entry.is_drive) {
        if(entry.capacity_valid) {
            wchar_t capacity[160]{};
            swprintf_s(capacity,l10n::Pick(L"%.2f GiB 可用，共 %.2f GiB",L"%.2f GiB free of %.2f GiB"),
                static_cast<double>(entry.free_bytes)/1073741824.0,
                static_cast<double>(entry.total_bytes)/1073741824.0);
            item.detail=capacity;
        } else if(item.detail.empty()) {
            item.detail=l10n::Pick(L"容量不可用",L"Capacity unavailable");
        }
    }
    return item;
}
}
ui::HomeWidgets BuildHomeWidgets(const SidebarModel& sidebar, const PlacesCatalog& places,
                                 uint32_t hidden) {
    using I=l10n::StringId;
    ui::HomeWidgets result;
    result.sections.resize(5);
    const std::wstring titles[]={l10n::Get(I::SidebarQuickAccess),l10n::Pick(L"驱动器",L"Drives"),
        l10n::Get(I::SidebarNetworkLocations),l10n::Get(I::SidebarTags),l10n::Pick(L"最近访问",L"Recent activity")};
    const std::wstring empty[]={l10n::Pick(L"固定常用文件夹后将在此显示",L"Pin folders to see them here"),
        l10n::Pick(L"暂无驱动器",L"No drives"),l10n::Pick(L"暂无网络位置",L"No network locations"),
        l10n::Pick(L"创建标签后将在此显示",L"Create tags to see them here"),
        l10n::Pick(L"访问文件或文件夹后将在此显示",L"Files and folders you visit will appear here")};
    for(size_t i=0;i<5;++i) {
        result.sections[i].title=titles[i];result.sections[i].empty_text=empty[i];
        result.sections[i].visible=(hidden&(1u<<i))==0;
    }
    auto& quick=result.sections[0].items;
    const auto duplicate=[&](const std::wstring& path) {
        return std::any_of(quick.begin(),quick.end(),[&](const auto& item){return _wcsicmp(item.path.c_str(),path.c_str())==0;});
    };
    const auto& home_folders=sidebar.home_folders.empty() ? sidebar.quick_access : sidebar.home_folders;
    for(const auto& entry:home_folders) {
        if(entry.path.empty() || IsHomePath(entry.path) || entry.path==MakeRecentPath() || duplicate(entry.path)) continue;
        auto item=FromSidebar(entry);item.pinned=true;quick.push_back(std::move(item));
    }
    for(const auto& path:places.quick_access_paths) {
        if(quick.size()>=24) break;
        if(path.empty() || duplicate(path)) continue;
        ui::HomeWidgetItem item;item.title=TabTitle(path);item.path=path;item.glyph=L"\xE8B7";item.pinned=true;
        quick.push_back(std::move(item));
    }
    for(const auto& entry:sidebar.drives) result.sections[1].items.push_back(FromSidebar(entry));
    for(const auto& place:places.networks) {
        if(result.sections[2].items.size()>=24) break;
        ui::HomeWidgetItem item;item.title=place.name;item.detail=place.unc;item.path=place.unc;item.glyph=L"\xE968";
        result.sections[2].items.push_back(std::move(item));
    }
    for(const auto& entry:sidebar.system_networks) {
        if(result.sections[2].items.size()>=24) break;
        const auto& current=result.sections[2].items;
        if(std::none_of(current.begin(),current.end(),[&](const auto& item){return _wcsicmp(item.path.c_str(),entry.path.c_str())==0;}))
            result.sections[2].items.push_back(FromSidebar(entry));
    }
    for(const auto& tag:places.tags) {
        if(result.sections[3].items.size()>=24) break;
        ui::HomeWidgetItem item;item.title=tag.name;item.path=MakeTagPath(tag.id);item.glyph=L"\xE8EC";
        item.color=tag.rgb;item.detail=std::to_wstring(tag.paths.size())+l10n::Pick(L" 个项目",L" items");
        result.sections[3].items.push_back(std::move(item));
    }
    for(const auto& recent:places.recent_items) {
        // 已知类型才提供打开操作；不为未知记录同步探测文件系统。
        if(recent.kind==PlaceItemKind::Unknown) continue;
        ui::HomeWidgetItem item;item.title=TabTitle(recent.path);item.detail=path::FriendlyPathText(recent.path);
        item.path=recent.path;item.file=recent.kind==PlaceItemKind::File;
        item.glyph=item.file ? L"\xE8A5" : L"\xE8B7";
        result.sections[4].items.push_back(std::move(item));
        if(result.sections[4].items.size()==12) break;
    }
    return result;
}
}
