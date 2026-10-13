#include "sidebar_navigation.h"
#include "app_commands.h"
#include "app_hosted_edit.h"
#include "app_navigation.h"
#include "app_network_locations.h"
#include "context_menu.h"
#include "drop_navigation_model.h"
#include "window_factory.h"
#include "../common/localization.h"
#include "../fs/bounded_enumeration.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace pulse {

void OpenSidebarPath(AppState& state, const std::wstring& path) {
    if (state.addressEditing) HideAddressEditor(state, false);
    OpenTabAt(state, path);
}

bool HandleSidebarNavigationCommand(AppState& state, const std::wstring& path, int command) {
    if (command == app::CmdOpen || command == app::CmdOpenRecycle || command == app::CmdOpenInNewTab) {
        OpenSidebarPath(state, path);
        return true;
    }
    if (command == app::CmdOpenInNewWindow) {
        WindowInit init;
        init.path = path;
        OpenPulseWindow(state, init);
        return true;
    }
    return false;
}

void ShowSidebarPathMenu(AppState& state, const std::wstring& path, POINT screen_point) {
    if (!EnsureMenu(state)) return;
    state.context_menu.Close();
    const int command = state.menu->TrackPopup(screen_point, app::BuildSidebarNavigationMenu());
    HandleSidebarNavigationCommand(state, path, command);
}

bool IsTabNavigationDropTarget(AppState& state, POINT point) {
    const auto width = static_cast<float>(state.compositor.Width());
    const auto height = static_cast<float>(state.compositor.Height());
    const auto hit = state.renderer.HitTest(BuildVm(state), D2D1::RectF(0, 0, width, height),
        static_cast<float>(point.x), static_cast<float>(point.y));
    return IsTabNavigationDropTarget(state, hit, point);
}

bool IsTabNavigationDropTarget(const AppState& state, const ui::HitTestResult& hit, POINT point) {
    if (point.x < 0 || point.y < 0 || static_cast<float>(point.x) >= static_cast<float>(state.compositor.Width()) ||
        static_cast<float>(point.y) >= static_cast<float>(state.compositor.Height())) return false;
    using R = ui::HitTestResult;
    return hit.region == R::Tab || hit.region == R::TabClose || hit.region == R::TabNew ||
        hit.region == R::TabGroup || hit.region == R::TabScroll ||
        (hit.region == R::None && static_cast<float>(point.y) < state.renderer.TitleBarHeight());
}

namespace {

struct DropResolution {
    std::vector<app::DroppedNavigationGroup> groups;
    size_t failures = 0;
    DWORD error = ERROR_SUCCESS;
};
struct DropNavigationState {
    std::atomic_bool cancelled = false;
    uintptr_t token = 0;
    HWND window = nullptr;
    std::mutex mutex;
    std::deque<DropResolution> ready;
};
std::atomic_uintptr_t next_drop_token{1};
// UI-thread-owned. Workers retain only this independent object, never AppState.
std::unordered_map<HWND, std::shared_ptr<DropNavigationState>> drop_states;

struct AttributeResult { DWORD attributes = INVALID_FILE_ATTRIBUTES, error = ERROR_SUCCESS; };

DropResolution ResolveDroppedPaths(const std::vector<std::wstring>& paths,
                                  const std::shared_ptr<DropNavigationState>& state) {
    DropResolution result;
    std::vector<app::DroppedNavigationTarget> targets;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (size_t index = 0; index < paths.size() && !state->cancelled; ++index) {
        const auto source = app::NormalizeDroppedNavigationPath(paths[index]);
        AttributeResult metadata;
        if (source.empty()) metadata.error = ERROR_INVALID_NAME;
        else {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                result.failures += paths.size() - index;
                result.error = ERROR_TIMEOUT;
                break;
            }
            try {
                metadata = fs::RunBoundedEnumeration<AttributeResult>(fs::IsUncPath(source),
                    [source](const std::atomic_bool& cancelled) {
                        AttributeResult value;
                        if (cancelled) return value;
                        DWORD previous = 0;
                        const BOOL changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous);
                        value.attributes = GetFileAttributesW(source.c_str());
                        value.error = value.attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
                        if (changed) SetThreadErrorMode(previous, nullptr);
                        return value;
                    }, [state] { return state->cancelled.load(); },
                    (std::min)(remaining, std::chrono::milliseconds(1500)));
            } catch (const fs::EnumerationCancelled&) { break; }
            catch (...) { metadata.error = ERROR_TIMEOUT; }
        }
        auto target = app::PlanDroppedNavigation(source, metadata.attributes);
        if (target.folder.empty()) {
            ++result.failures;
            result.error = metadata.error ? metadata.error : ERROR_PATH_NOT_FOUND;
        } else targets.push_back(std::move(target));
    }
    result.groups = app::GroupDroppedNavigation(targets);
    return result;
}

void SelectDroppedNames(AppState& state, app::Tab& tab, const std::vector<std::wstring>& names) {
    if (names.empty()) return;
    SelectNameInTab(state, tab, names.front());
    if (names.size() == 1) return;
    tab.pending_selected_names = names;
    if (tab.snapshot) tab.RemapSelection(names, names.front(), false);
    tab.pending_selection_revision = tab.selection_revision;
}

} // namespace

bool QueueDroppedPathsInTabs(AppState& state, std::vector<std::wstring> paths) {
    if (!state.hwnd || paths.empty()) return false;
    auto& attached = drop_states[state.hwnd];
    if (!attached) {
        attached = std::make_shared<DropNavigationState>();
        attached->window = state.hwnd;
        attached->token = next_drop_token.fetch_add(1);
    }
    const auto lifetime = attached;
    state.worker.EnqueueSerialIo([lifetime, paths = std::move(paths)] {
        if (lifetime->cancelled) return;
        auto result = ResolveDroppedPaths(paths, lifetime);
        if (lifetime->cancelled) return;
        {
            std::lock_guard lock(lifetime->mutex);
            lifetime->ready.push_back(std::move(result));
        }
        PostMessageW(lifetime->window, kDroppedNavigationMessage, lifetime->token, 0);
    });
    return true;
}

bool HandleDroppedNavigationMessage(AppState& state, UINT message, WPARAM wparam, LPARAM) {
    if (message == WM_SHOWWINDOW && wparam == FALSE) {
        CancelDroppedNavigation(state);
        return false;
    }
    if (message != kDroppedNavigationMessage) return false;
    const auto found = drop_states.find(state.hwnd);
    if (found == drop_states.end() || found->second->token != wparam || found->second->cancelled) return true;
    std::deque<DropResolution> ready;
    {
        std::lock_guard lock(found->second->mutex);
        ready.swap(found->second->ready);
    }
    for (auto& resolution : ready) {
        for (const auto& group : resolution.groups) {
            // OpenTabAt can otherwise launch a cached NetHood .url/.lnk.
            // A navigation drop must never execute the dropped file.
            if (IsSystemNetworkShortcut(state, group.folder) || state.renameIndex >= 0 || !IsWindowEnabled(state.hwnd)) {
                resolution.failures += group.source_count;
                resolution.error = ERROR_BUSY;
                continue;
            }
            OpenSidebarPath(state, group.folder);
            if (auto* tab = ActiveTab(state)) SelectDroppedNames(state, *tab, group.names);
        }
        if (resolution.failures) {
            const auto message_text = std::wstring(l10n::Pick(L"无法打开拖入的项目：", L"Unable to open dropped items: ")) +
                std::to_wstring(resolution.failures) + L"\n" +
                l10n::Pick(L"错误码：", L"Error: ") + std::to_wstring(resolution.error);
            state.notification_toast.Show(state.hwnd, l10n::Get(l10n::StringId::CannotOpen), message_text, false);
        }
    }
    return true;
}

void CancelDroppedNavigation(AppState& state) noexcept {
    const auto found = drop_states.find(state.hwnd);
    if (found == drop_states.end()) return;
    found->second->cancelled = true;
    drop_states.erase(found);
}

} // namespace pulse
