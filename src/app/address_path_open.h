#pragma once

#include <windows.h>
#include <string>

namespace pulse {

struct AppState;
struct AddressPathOpen;

inline constexpr UINT WM_ADDRESS_LOOKUP_READY = WM_APP + 97;
void OpenAddressPath(AppState& s, const std::wstring& path);
void PollAddressPathOpen(AppState& s);
void CancelAddressPathOpen(AppState& s);

} // namespace pulse
