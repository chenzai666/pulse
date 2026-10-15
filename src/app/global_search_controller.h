#pragma once
#include <windows.h>
namespace pulse {
struct AppState;
inline constexpr UINT kListarySearchResultMessage = WM_APP + 94;
void ApplyGlobalSearchSettings(AppState& s);
void ToggleGlobalSearch(AppState& s);
void OpenListarySearch(AppState& s);
void ShutdownGlobalSearch(AppState& s);
bool HandleGlobalSearchHotkeyCapture(AppState& s, unsigned key);
void HandleGlobalSearchResult(AppState& s, WPARAM token);
void CancelGlobalSearchForWindow(AppState& s);
}
