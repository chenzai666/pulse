#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace pulse {
struct AppState;
namespace ui { struct HitTestResult; }

// Known sidebar destinations do not need a synchronous existence probe.
void OpenSidebarPath(AppState& state, const std::wstring& path);
bool HandleSidebarNavigationCommand(AppState& state, const std::wstring& path, int command);
void ShowSidebarPathMenu(AppState& state, const std::wstring& path, POINT screen_point);

bool IsTabNavigationDropTarget(AppState& state, POINT point);
bool IsTabNavigationDropTarget(const AppState& state, const ui::HitTestResult& hit, POINT point);
bool QueueDroppedPathsInTabs(AppState& state, std::vector<std::wstring> paths);
inline constexpr UINT kDroppedNavigationMessage = WM_APP + 89;
bool HandleDroppedNavigationMessage(AppState& state, UINT message, WPARAM wparam, LPARAM lparam);
void CancelDroppedNavigation(AppState& state) noexcept;
}
