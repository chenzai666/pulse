#pragma once
#include <memory>
#include <string>

namespace pulse {
struct AppState;
namespace app { struct Tab; }
struct NetworkLocationLoad;
inline constexpr wchar_t kNetworkLocationsPath[] = L"pulse:networks";
void RequestNetworkLocations(AppState& state);
void ApplyNetworkLocations(AppState& state);
void CancelNetworkLocations(AppState& state);
void FillNetworkLocationsView(AppState& state, app::Tab& tab);
bool IsSystemNetworkShortcut(const AppState& state, const std::wstring& path);
bool OpenSystemNetworkShortcut(AppState& state, const std::wstring& path);
} // namespace pulse
