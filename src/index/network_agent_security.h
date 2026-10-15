#pragma once
#include <windows.h>
#include <sddl.h>
#include <string>
#include <cstdint>
#include <vector>
#include "custom_index_storage.h"

namespace pulse::index::agent {
struct Identity {
    std::wstring user;
    std::wstring logon;
    DWORD session = 0;
    uint64_t authentication = 0;
    bool operator==(const Identity&) const = default;
    std::wstring Suffix() const { return user + L"-" + logon + L"-" + std::to_wstring(session) + L"-" + std::to_wstring(authentication); }
    bool valid() const { return !user.empty() && !logon.empty(); }
};
inline std::wstring SidText(PSID sid) {
    LPWSTR value = nullptr;
    if (!IsValidSid(sid) || !ConvertSidToStringSidW(sid, &value)) return {};
    std::wstring text(value);
    LocalFree(value);
    return text;
}
inline Identity TokenIdentity(HANDLE token) {
    Identity result;
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> user(bytes);
    if (!bytes || !GetTokenInformation(token, TokenUser, user.data(), bytes, &bytes)) return {};
    result.user = SidText(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
    if (!GetTokenInformation(token, TokenSessionId, &result.session, sizeof(result.session), &bytes)) return {};
    bytes = 0;
    GetTokenInformation(token, TokenGroups, nullptr, 0, &bytes);
    std::vector<BYTE> groups(bytes);
    if (!bytes || !GetTokenInformation(token, TokenGroups, groups.data(), bytes, &bytes)) return {};
    const auto* entries = reinterpret_cast<TOKEN_GROUPS*>(groups.data());
    for (DWORD i = 0; i < entries->GroupCount; ++i) {
        if ((entries->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
            result.logon = SidText(entries->Groups[i].Sid);
            break;
        }
    }
    TOKEN_STATISTICS statistics{};
    if (!GetTokenInformation(token, TokenStatistics, &statistics, sizeof(statistics), &bytes)) return {};
    result.authentication = (static_cast<uint64_t>(static_cast<DWORD>(statistics.AuthenticationId.HighPart)) << 32) |
        statistics.AuthenticationId.LowPart;
    return result;
}
inline Identity ProcessIdentity(HANDLE process = GetCurrentProcess()) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return {};
    const auto identity = TokenIdentity(token);
    CloseHandle(token);
    return identity;
}
inline bool SameIdentity(const Identity& a, const Identity& b) {
    return a.valid() && b.valid() && a.user == b.user && a.logon == b.logon && a.session == b.session && a.authentication == b.authentication;
}
inline std::wstring EndpointSuffix(const Identity& owner) {
    if (!owner.valid()) return {};
#ifdef PULSE_CUSTOM_EDITION
    return custom_storage::InstallationKey() + L"-" + owner.Suffix();
#else
    return owner.Suffix();
#endif
}
inline std::wstring PipeName(const Identity& owner = ProcessIdentity()) {
    const auto suffix = EndpointSuffix(owner);
    return suffix.empty() ? std::wstring{} :
#ifdef PULSE_CUSTOM_EDITION
        L"\\\\.\\pipe\\PulseCustomNetworkIndex-" + suffix;
#else
        L"\\\\.\\pipe\\PulseNetworkIndex-" + suffix;
#endif
}
inline std::wstring SingletonName(const Identity& owner = ProcessIdentity()) {
    const auto suffix = EndpointSuffix(owner);
    return suffix.empty() ? std::wstring{} :
#ifdef PULSE_CUSTOM_EDITION
        L"Local\\PulseCustom.Index.NetworkAgent-" + suffix;
#else
        L"Local\\Pulse.Index.NetworkAgent-" + suffix;
#endif
}
class EndpointSecurity {
public:
    explicit EndpointSecurity(const Identity& owner) {
        if (!owner.valid()) return;
        // A user SID alone includes the same account's other interactive logons.
        const auto sddl = L"D:P(A;;GA;;;" + owner.logon + L")";
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr))
            attributes_.lpSecurityDescriptor = descriptor_;
    }
    ~EndpointSecurity() { if (descriptor_) LocalFree(descriptor_); }
    EndpointSecurity(const EndpointSecurity&) = delete;
    EndpointSecurity& operator=(const EndpointSecurity&) = delete;
    explicit operator bool() const { return descriptor_ != nullptr; }
    SECURITY_ATTRIBUTES* get() { return descriptor_ ? &attributes_ : nullptr; }
private:
    SECURITY_ATTRIBUTES attributes_{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
};
inline bool AuthorizedClient(HANDLE pipe, const Identity& owner) {
    if (!owner.valid() || !ImpersonateNamedPipeClient(pipe)) return false;
    HANDLE token = nullptr;
    const bool opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) != FALSE;
    // Never continue running the agent with a client's token on this thread.
    if (!RevertToSelf()) TerminateProcess(GetCurrentProcess(), ERROR_ACCESS_DENIED);
    if (!opened) return false;
    const auto client = TokenIdentity(token);
    CloseHandle(token);
    return SameIdentity(owner, client);
}
inline bool AuthorizedServer(HANDLE pipe, const Identity& owner = ProcessIdentity()) {
    ULONG pid = 0;
    if (!owner.valid() || !GetNamedPipeServerProcessId(pipe, &pid)) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    const auto server = ProcessIdentity(process);
    CloseHandle(process);
    return SameIdentity(owner, server);
}
struct Security {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    explicit Security(const Identity& identity) {
        if (!identity.valid()) return;
        const auto sddl = L"D:P(A;;GA;;;" + identity.logon + L")";
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            attributes.lpSecurityDescriptor = descriptor;
    }
    ~Security() { if (descriptor) LocalFree(descriptor); }
    Security(const Security&) = delete;
    Security& operator=(const Security&) = delete;
};
inline bool AuthorizeClient(HANDLE pipe, const Identity& owner) { return AuthorizedClient(pipe, owner); }
inline bool AuthorizeServer(HANDLE pipe) { return AuthorizedServer(pipe); }
} // namespace pulse::index::agent
