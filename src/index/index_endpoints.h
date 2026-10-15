#pragma once
#include "../common/edition.h"
#ifdef PULSE_CUSTOM_EDITION
#include "network_agent_security.h"
#endif

namespace pulse::index {
#ifdef PULSE_CUSTOM_EDITION
inline const std::wstring kPipeNameStorage = agent::PipeName() + L".Files";
inline const std::wstring kMutexNameStorage = agent::SingletonName() + L".Files";
inline const std::wstring kConfigurationMutexStorage = L"Local\\PulseCustomIndexConfiguration-" + custom_storage::InstallationKey();
inline const wchar_t* const kPipeName = kPipeNameStorage.c_str();
inline const wchar_t* const kMutexName = kMutexNameStorage.c_str();
inline const wchar_t* const kConfigurationMutex = kConfigurationMutexStorage.c_str();
inline constexpr wchar_t kServiceName[] = L"PulseCustomIndex";
#else
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\PulseIndex";
inline constexpr wchar_t kServiceName[] = L"PulseIndex";
inline constexpr wchar_t kMutexName[] = L"Local\\Pulse.Index.Singleton";
inline constexpr wchar_t kConfigurationMutex[] = L"Global\\PulseIndexConfiguration";
#endif
}
