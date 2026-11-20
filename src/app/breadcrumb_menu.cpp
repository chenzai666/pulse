#include "app_internal.h"
#include "address_lookup.h"
#include "app_properties.h"
#include "context_menu.h"
#include "sidebar_navigation.h"
#include "../fs/fs_enum.h"
#include "../ops/clipboard.h"
#include "../common/localization.h"
#include <algorithm>
#include <cwchar>
#include <unordered_map>

namespace pulse {

namespace {

constexpr int kHideCloud = 32400;
constexpr size_t kMaxChildMenuRows = 128;

using BreadcrumbFolders = std::vector<std::pair<std::wstring, std::wstring>>;

BreadcrumbFolders ReadBreadcrumbChildren(const std::wstring& path, const std::atomic_bool& cancelled) {
    BreadcrumbFolders folders;
    if (path.empty()) {
        wchar_t drives[512]{};
        const DWORD length = GetLogicalDriveStringsW(ARRAYSIZE(drives), drives);
        if (!length || length >= ARRAYSIZE(drives)) throw std::runtime_error("GetLogicalDriveStringsW failed");
        for (const wchar_t* drive = drives; *drive && !cancelled; drive += std::wcslen(drive) + 1)
            folders.emplace_back(drive, drive);
        return folders;
    }

    std::vector<fs::DirEntry> entries;
    fs::EnumerateDirectory(path, entries, &cancelled);
    for (auto& entry : entries) {
        if (cancelled) break;
        if (!entry.is_dir || entry.name == L"." || entry.name == L"..") continue;
        if (entry.full_path.empty())
            entry.full_path = path + (path.ends_with(L'\\') ? L"" : L"\\") + entry.name;
        folders.emplace_back(std::move(entry.name), std::move(entry.full_path));
    }
    std::sort(folders.begin(), folders.end(), [](const auto& left, const auto& right) {
        return CompareStringOrdinal(left.first.c_str(), -1, right.first.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return folders;
}

struct BreadcrumbRefresh {
    HWND owner;
    ui::FluentMenu* menu;
    std::shared_ptr<AddressLookup<BreadcrumbFolders>> job;
    bool refreshed = false;
};

thread_local std::unordered_map<UINT_PTR, BreadcrumbRefresh> breadcrumb_timers;

struct BreadcrumbTimerRegistration {
    UINT_PTR timer;
    AddressLookup<BreadcrumbFolders>& job;

    ~BreadcrumbTimerRegistration() {
        KillTimer(nullptr, timer);
        breadcrumb_timers.erase(timer);
        job.cancelled = true;
    }
};

void CALLBACK RefreshBreadcrumbChildren(HWND, UINT, UINT_PTR timer, DWORD) {
    const auto it = breadcrumb_timers.find(timer);
    if (it == breadcrumb_timers.end() || it->second.refreshed || !it->second.job->ready ||
        !IsWindowVisible(it->second.owner)) return;
    it->second.refreshed = true;
    it->second.menu->RequestFilterRefresh();
}

// Runs a BuildBreadcrumbMenu command on `path` (a breadcrumb or a sidebar row).
void RunPathCommand(AppState& s, const std::wstring& path, bool filesystem, int command) {
    const std::wstring shell_path = ClipboardPath(path);
    switch (command) {
    case app::CmdOpenInNewTab:
        OpenFolderTab(s, path);
        break;
    case app::CmdOpen:
        NavigateTo(s, path);
        break;
    case app::CmdCopyPath:
        ops::WriteClipboardText(shell_path);
        break;
    case app::CmdCopy:
        if (filesystem) ops::WriteClipboard({shell_path}, false);
        break;
    case app::CmdOpenTerminal:
        if (filesystem) s.ops.OpenTerminal(shell_path);
        break;
    case app::CmdProperties:
        if (filesystem) ShowItemProperties(s, {shell_path});
        break;
    default:
        break;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace

void ShowBreadcrumbMenu(AppState& s, std::wstring path, POINT screen_pt) {
    if (path.empty() || !EnsureMenu(s)) return;
    s.context_menu.Close();
    const bool filesystem = !fs::IsVirtualPath(path);
    // Keep the clicked path across TrackPopup's nested message loop. Selection
    // commands target file rows and must not be used for breadcrumb ancestors.
    const int command = s.menu->TrackPopup(screen_pt, app::BuildBreadcrumbMenu(filesystem));
    RunPathCommand(s, path, filesystem, command);
}

void ShowBreadcrumbChildren(AppState& s, std::wstring path, POINT screen_pt) {
    const auto* tab = ActiveTab(s);
    if (!tab || fs::IsVirtualPath(path) || (s.menu && s.menu->IsOpen())) return;
    const HWND owner = s.hwnd;
    // The nested menu loop can dispatch owner shutdown before returning.
    ui::FluentMenu menu;
    if (!menu.Create(owner, &s.compositor, s.scale)) return;
    menu.SetTheme(s.darkMode, s.accentColor);
    menu.SetRowHeightDip(static_cast<float>(app::MenuRowHeightDip(
        app::EffectiveRowHeightDip(s.appPrefs.row_height, s.appPrefs.ui_font_scale))));
    s.context_menu.Close();
    const auto origin = tab->current_path;
    const auto generation = tab->view_generation;
    const auto job = std::make_shared<AddressLookup<BreadcrumbFolders>>();
    auto filter = [job](const std::wstring& query) {
        std::vector<ui::FluentMenuItem> items;
        const bool ready = job->ready;
        if (ready && job->result) {
            const auto& folders = *job->result;
            for (size_t i = 0; i < folders.size(); ++i) {
                const auto& name = folders[i].first;
                if (!query.empty() && FindStringOrdinal(FIND_FROMSTART, name.c_str(),
                    static_cast<int>(name.size()), query.c_str(), static_cast<int>(query.size()), TRUE) < 0) continue;
                ui::FluentMenuItem item;
                item.command = static_cast<int>(i + 1);
                item.text = name;
                item.glyph = L"\xE8B7";
                items.push_back(std::move(item));
                if (items.size() == kMaxChildMenuRows) break;
            }
        }
        if (items.empty()) {
            ui::FluentMenuItem item;
            item.enabled = false;
            const auto label = !ready ? l10n::StringId::LoadingEllipsis :
                !job->result ? l10n::StringId::DirectoryUnavailable :
                job->result->empty() ? l10n::StringId::FolderEmpty : l10n::StringId::NoMatches;
            item.text = l10n::Get(label);
            items.push_back(std::move(item));
        }
        return items;
    };
    menu.SetFilterPlaceholder(l10n::Get(l10n::StringId::FilterPlaceholder));
    menu.SetFilterMinWidth(180.0f);
    menu.SetMaxVisibleRows(10);
    int command = 0;
    {
        const UINT_PTR timer = SetTimer(nullptr, 0, 40, RefreshBreadcrumbChildren);
        if (!timer) return;
        const BreadcrumbTimerRegistration registration{timer, *job};
        breadcrumb_timers.emplace(timer, BreadcrumbRefresh{owner, &menu, job});
        const bool network = !path.empty() && fs::NeedsBoundedEnumeration(path);
        StartAddressLookup(job, network, [path = std::move(path)](const std::atomic_bool& cancelled) {
            return ReadBreadcrumbChildren(path, cancelled);
        });
        command = menu.TrackPopup({screen_pt.x - ui::FluentMenu::kShadowMargin,
            screen_pt.y - ui::FluentMenu::kShadowMargin}, filter(L""), filter);
    }

    if (!IsWindowVisible(owner) || GetAppState(owner) != &s) return;
    const auto* active = ActiveTab(s);
    if (!active || active != tab || active->view_generation != generation || active->current_path != origin) return;
    if (command > 0 && job->ready && job->result && static_cast<size_t>(command) <= job->result->size())
        NavigateTo(s, (*job->result)[static_cast<size_t>(command - 1)].second);
}

// #80: OneDrive rows form a header-less section, so the row carries what a
// header menu would: the usual folder commands, then hiding OneDrive. The toast
// says where to bring it back.
void ShowCloudPlaceMenu(AppState& s, const std::wstring& path, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    s.context_menu.Close();
    const bool filesystem = !path.empty() && !fs::IsVirtualPath(path);
    std::vector<ui::FluentMenuItem> items;
    if (!path.empty()) {
        items = app::BuildSidebarNavigationMenu();
        for (auto item : app::BuildBreadcrumbMenu(filesystem))
            if (item.command != app::CmdOpen && item.command != app::CmdOpenInNewTab)
                items.push_back(std::move(item));
    }
    if (!items.empty()) items.back().separator_after = true;
    ui::FluentMenuItem hide;
    hide.command = kHideCloud;
    hide.text = l10n::Get(l10n::StringId::SidebarHideCloud);
    items.push_back(std::move(hide));
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command == kHideCloud) {
        s.sidebarHiddenMask |= 1u << static_cast<int>(app::SidebarSectionId::Cloud);
        s.notification_toast.Show(s.hwnd, L"OneDrive",
            l10n::Get(l10n::StringId::SidebarCloudHiddenHint), false);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (!path.empty() && !HandleSidebarNavigationCommand(s, path, command))
        RunPathCommand(s, path, filesystem, command);
}

} // namespace pulse
