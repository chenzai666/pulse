#include "../app/native_dialog_folder_link.h"

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void Check(bool pass, const char* description) {
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", description);
    if (!pass) ++failures;
}

uintptr_t Parse(const wchar_t* text) {
    wchar_t* end = nullptr;
    const unsigned long long value = _wcstoui64(text, &end, 10);
    return end && !*end ? static_cast<uintptr_t>(value) : 0;
}

// Query only the child just created by the product, to obtain its random
// marker. A failed query fails the test rather than guessing a marker.
std::wstring ChildCommand(HANDLE child) {
    struct UnicodeString { USHORT length, maximum; wchar_t* buffer; };
    using Query = LONG (WINAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto query = ntdll ? reinterpret_cast<Query>(GetProcAddress(ntdll, "NtQueryInformationProcess")) : nullptr;
    if (!query) return {};
    std::vector<BYTE> buffer(65536);
    ULONG needed = 0;
    if (query(child, 60, buffer.data(), static_cast<ULONG>(buffer.size()), &needed) < 0) return {};
    const auto* value = reinterpret_cast<const UnicodeString*>(buffer.data());
    const uintptr_t begin = reinterpret_cast<uintptr_t>(buffer.data());
    const uintptr_t end = begin + buffer.size();
    const uintptr_t text = reinterpret_cast<uintptr_t>(value->buffer);
    if (!value->buffer || value->length % sizeof(wchar_t) || text < begin || text > end ||
        value->length > end - text) return {};
    return std::wstring(value->buffer, value->length / sizeof(wchar_t));
}

struct Children { DWORD helper = 0, supervisor = 0; std::wstring marker; };
Children FindChildren(DWORD owner) {
    Children found;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ParentProcessID != owner) continue;
        HANDLE child = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID);
        if (!child) continue;
        const std::wstring command = ChildCommand(child);
        CloseHandle(child);
        const bool helper = command.find(L"--native-folder-link-helper") != std::wstring::npos;
        const bool supervisor = command.find(L"--native-folder-link-supervisor") != std::wstring::npos;
        const auto start = command.find(L"Pulse.FolderLink.{");
        const auto finish = start == std::wstring::npos ? std::wstring::npos : command.find(L'}', start);
        if (helper && !found.helper) found.helper = entry.th32ProcessID;
        else if (supervisor && !found.supervisor) found.supervisor = entry.th32ProcessID;
        else continue;
        if (start != std::wstring::npos && finish != std::wstring::npos) {
            const auto marker = command.substr(start, finish - start + 1);
            GUID id{};
            if (marker.size() == 55 && SUCCEEDED(CLSIDFromString(marker.c_str() + 17, &id))) {
                if (found.marker.empty()) found.marker = marker;
                else if (found.marker != marker) found.marker.clear();
            }
        }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return found;
}
Children WaitForChildren(DWORD owner) {
    Children found;
    const ULONGLONG deadline = GetTickCount64() + 5000;
    do { found = FindChildren(owner); if (found.helper && found.supervisor && !found.marker.empty()) break; Sleep(20); }
    while (GetTickCount64() < deadline);
    return found;
}
void Stop(pulse::app::NativeDialogFolderLink& link) {
    const ULONGLONG deadline = GetTickCount64() + 6000;
    bool stopped = false;
    while (GetTickCount64() < deadline && !(stopped = link.Stop())) Sleep(20);
    Check(stopped, "Stop completed and supervisor cleanup was acknowledged");
}
void RunCase(const char* label, int scenario) {
    pulse::app::NativeDialogFolderLink link;
    const auto children = WaitForChildren(GetCurrentProcessId());
    HANDLE helper = children.helper ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | PROCESS_SUSPEND_RESUME | SYNCHRONIZE, FALSE, children.helper) : nullptr;
    HANDLE supervisor = children.supervisor ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, children.supervisor) : nullptr;
    Check(helper && supervisor && !children.marker.empty(), "created distinct helper and supervisor with shared unique marker");
    if (!helper || !supervisor || children.marker.empty()) { if(helper)CloseHandle(helper); if(supervisor)CloseHandle(supervisor); return; }
    // The helper waits for supervisor readiness and exits on handshake failure.
    // Both remaining alive after the handshake window is observable proof that
    // they have passed identity validation and monitor each other.
    Sleep(5500);
    Check(WaitForSingleObject(helper,0)==WAIT_TIMEOUT && WaitForSingleObject(supervisor,0)==WAIT_TIMEOUT,
          "helper and supervisor remain alive after readiness handshake");
    HWND window=CreateWindowExW(0,L"STATIC",L"owned lifecycle fixture",WS_OVERLAPPED,0,0,200,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    const wchar_t* other=L"Pulse.FolderLink.TestUnrelated";
    bool tagged=window && SetPropW(window,children.marker.c_str(),reinterpret_cast<HANDLE>(1)) && SetPropW(window,other,reinterpret_cast<HANDLE>(1));
    Check(tagged,"marked only the owned fixture window");
    if (scenario==0) {
        using Suspend=LONG (NTAPI*)(HANDLE);
        HMODULE ntdll=GetModuleHandleW(L"ntdll.dll");
        auto suspend=ntdll ? reinterpret_cast<Suspend>(GetProcAddress(ntdll,"NtSuspendProcess")) : nullptr;
        Check(suspend && suspend(helper)>=0,"suspended owned helper to simulate blocked call");
    } else if (scenario==1) {
        Check(TerminateProcess(helper,77)!=FALSE,"terminated only owned helper unexpectedly");
    } else {
        Check(TerminateProcess(supervisor,78)!=FALSE,"terminated only owned supervisor unexpectedly");
    }
    Stop(link);
    Check(WaitForSingleObject(helper,3000)==WAIT_OBJECT_0 && WaitForSingleObject(supervisor,3000)==WAIT_OBJECT_0,
          "both owned child processes exited");
    if(tagged){Check(!GetPropW(window,children.marker.c_str()),"owned marker cleaned after both processes exit");
        Check(GetPropW(window,other)==reinterpret_cast<HANDLE>(1),"unrelated marker retained");}
    if(window){RemovePropW(window,other);RemovePropW(window,children.marker.c_str());DestroyWindow(window);}
    CloseHandle(helper); CloseHandle(supervisor);
    std::printf("case=%s complete\n",label);
}
void RunParentDeath() {
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!length || length >= ARRAYSIZE(executable)) { Check(false, "fixture executable path"); return; }
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --owned-parent";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION owner{};
    const bool created = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &owner) != FALSE;
    Check(created, "started the dedicated owned parent process");
    if (!created) return;
    CloseHandle(owner.hThread);
    const auto children = WaitForChildren(owner.dwProcessId);
    HANDLE helper = children.helper ? OpenProcess(SYNCHRONIZE, FALSE, children.helper) : nullptr;
    HANDLE supervisor = children.supervisor ? OpenProcess(SYNCHRONIZE, FALSE, children.supervisor) : nullptr;
    Check(helper && supervisor && !children.marker.empty(), "owned parent started its helper and supervisor");
    if (helper && supervisor && !children.marker.empty()) {
        Sleep(5500);
        Check(WaitForSingleObject(helper,0)==WAIT_TIMEOUT && WaitForSingleObject(supervisor,0)==WAIT_TIMEOUT,
              "parent-death pair passed the readiness window");
        HWND window=CreateWindowExW(0,L"STATIC",L"owned parent-death fixture",WS_OVERLAPPED,0,0,200,100,
                                    nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        const wchar_t* other=L"Pulse.FolderLink.TestUnrelated";
        const bool tagged=window && SetPropW(window,children.marker.c_str(),reinterpret_cast<HANDLE>(1)) &&
            SetPropW(window,other,reinterpret_cast<HANDLE>(1));
        Check(tagged, "tagged only the owned parent-death fixture");
        Check(TerminateProcess(owner.hProcess,79)!=FALSE, "terminated only the owned test parent");
        const bool stopped=WaitForSingleObject(owner.hProcess,5000)==WAIT_OBJECT_0 &&
            WaitForSingleObject(helper,6000)==WAIT_OBJECT_0 && WaitForSingleObject(supervisor,6000)==WAIT_OBJECT_0;
        Check(stopped, "parent death reaped its helper and supervisor");
        if (tagged && stopped) {
            Check(!GetPropW(window,children.marker.c_str()), "supervisor cleaned marker after parent death");
            Check(GetPropW(window,other)==reinterpret_cast<HANDLE>(1), "parent death retained unrelated marker");
        }
        if(window){RemovePropW(window,other);RemovePropW(window,children.marker.c_str());DestroyWindow(window);}
    }
    if (WaitForSingleObject(owner.hProcess,0)==WAIT_TIMEOUT) TerminateProcess(owner.hProcess,79);
    WaitForSingleObject(owner.hProcess,5000);
    if(helper) CloseHandle(helper);
    if(supervisor) CloseHandle(supervisor);
    CloseHandle(owner.hProcess);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 7 && std::wstring_view(argv[1]) == L"--native-folder-link-helper") {
        return pulse::app::NativeDialogFolderLinkHelper(reinterpret_cast<HANDLE>(Parse(argv[2])),
            reinterpret_cast<HANDLE>(Parse(argv[3])), reinterpret_cast<HANDLE>(Parse(argv[4])),
            reinterpret_cast<HANDLE>(Parse(argv[5])), argv[6]);
    }
    if (argc == 8 && std::wstring_view(argv[1]) == L"--native-folder-link-supervisor") {
        return pulse::app::NativeDialogFolderLinkSupervisor(reinterpret_cast<HANDLE>(Parse(argv[2])),
            reinterpret_cast<HANDLE>(Parse(argv[3])), reinterpret_cast<HANDLE>(Parse(argv[4])),
            reinterpret_cast<HANDLE>(Parse(argv[5])), reinterpret_cast<HANDLE>(Parse(argv[6])), argv[7]);
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"--owned-parent") {
        pulse::app::NativeDialogFolderLink link;
        Sleep(30000);
        return 0;
    }
    if (argc != 1) return 2;
    RunCase("blocked-helper", 0);
    RunCase("helper-exits", 1);
    RunCase("supervisor-exits", 2);
    RunParentDeath();
    std::printf("SUMMARY failures=%d\n", failures);
    return failures ? 1 : 0;
}
