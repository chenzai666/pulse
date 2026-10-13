#pragma once

#include <windows.h>

namespace pulse {
struct AppState;

// Keep the upstream edition and its windows outside this integration.
inline constexpr wchar_t kListaryWindowClass[] = L"PulseCustomMainWindow";
inline constexpr wchar_t kListaryBridgeProperty[] = L"PulseCustom.Listary.Bridge.v1";
inline constexpr UINT kListaryBridgeMessage = WM_APP + 87;

// UI thread only. Attach after the initial pane exists; detach before its
// model/worker teardown. Failure leaves normal Pulse navigation available.
bool AttachListaryWindow(AppState& state);
void DetachListaryWindow(AppState& state) noexcept;

// Call before the ordinary WndProc dispatch. Queued work contains an opaque
// request id, never a caller-provided pointer. Return true when consumed.
bool ListaryWindowMessage(AppState& state, UINT message, WPARAM wparam,
                         LPARAM lparam, LRESULT& result);
}
