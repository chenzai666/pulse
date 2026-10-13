#pragma once

namespace pulse::edition {

#ifdef PULSE_CUSTOM_EDITION
inline constexpr bool kCustom = true;
inline constexpr wchar_t kDataFolder[] = L"PulseCustom";
inline constexpr wchar_t kWindowClass[] = L"PulseCustomMainWindow";
inline constexpr wchar_t kInstanceMutex[] = L"Local\\PulseCustom.Singleton";
inline constexpr wchar_t kInstanceProperty[] = L"PulseCustom.Singleton.PrimaryPid";
inline constexpr wchar_t kStartupName[] = L"PulseCustom";
inline constexpr wchar_t kWindowTitle[] = L"Pulse 定制版";
#else
inline constexpr bool kCustom = false;
inline constexpr wchar_t kDataFolder[] = L"Pulse";
inline constexpr wchar_t kWindowClass[] = L"PulseMainWindow";
inline constexpr wchar_t kInstanceMutex[] = L"Local\\Pulse.Singleton";
inline constexpr wchar_t kInstanceProperty[] = L"Pulse.Singleton.PrimaryPid";
inline constexpr wchar_t kStartupName[] = L"Pulse";
inline constexpr wchar_t kWindowTitle[] = L"Pulse";
#endif

} // namespace pulse::edition
