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

DWORD OwnChild() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{sizeof(entry)};
    DWORD child = 0;
    if (Process32FirstW(snapshot, &entry)) do {
        if (entry.th32ParentProcessID == GetCurrentProcessId()) {
            if (child) { child = 0; break; }
            child = entry.th32ProcessID;
        }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return child;
}

// Query only the child just created by the product, to obtain its random
// marker. A failed query fails the test rather than guessing a marker.
std::wstring ChildMarker(HANDLE child) {
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
    const std::wstring command(value->buffer, value->length / sizeof(wchar_t));
    const auto start = command.find(L"Pulse.FolderLink.{");
    if (start == std::wstring::npos) return {};
    const auto finish = command.find(L'}', start);
    if (finish == std::wstring::npos) return {};
    const std::wstring marker = command.substr(start, finish - start + 1);
    GUID id{};
    return marker.size() == 55 && SUCCEEDED(CLSIDFromString(marker.c_str() + 17, &id)) ? marker : L"";
}

void RunTimeout() {
    pulse::app::NativeDialogFolderLink link;
    DWORD pid = 0;
    const ULONGLONG launch_deadline = GetTickCount64() + 3000;
    while (!pid && GetTickCount64() < launch_deadline) { pid = OwnChild(); if (!pid) Sleep(20); }
    HANDLE child = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SUSPEND_RESUME | SYNCHRONIZE, FALSE, pid) : nullptr;
    Check(child && WaitForSingleObject(child, 0) == WAIT_TIMEOUT, "owned helper started and is running");
    if (!child) return;
    const std::wstring marker = ChildMarker(child);
    Check(!marker.empty(), "helper has unique marker in its command line");
    HWND window = CreateWindowExW(0, L"STATIC", L"owned lifecycle fixture", WS_OVERLAPPED,
                                   0, 0, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const wchar_t* other = L"Pulse.FolderLink.TestUnrelated";
    const bool tagged = window && !marker.empty() &&
        SetPropW(window, marker.c_str(), reinterpret_cast<HANDLE>(1)) &&
        SetPropW(window, other, reinterpret_cast<HANDLE>(1));
    Check(tagged, "tag only the owned fixture with the helper marker");
    using Suspend = LONG (NTAPI*)(HANDLE);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto suspend = ntdll ? reinterpret_cast<Suspend>(GetProcAddress(ntdll, "NtSuspendProcess")) : nullptr;
    const bool suspended = suspend && suspend(child) >= 0;
    Check(suspended, "suspend only the just-created helper to simulate a blocked call");
    if (suspended) {
        const ULONGLONG deadline = GetTickCount64() + 5000;
        bool stopped = false;
        while (GetTickCount64() < deadline && !(stopped = link.Stop())) Sleep(20);
        Check(stopped && WaitForSingleObject(child, 0) == WAIT_OBJECT_0,
              "Stop terminates the blocked helper and completes within five seconds");
        if (tagged && stopped) {
            Check(!GetPropW(window, marker.c_str()), "Stop cleans its marker after helper exit");
            Check(GetPropW(window, other) == reinterpret_cast<HANDLE>(1), "unrelated marker remains intact");
        }
    }
    if (window) { RemovePropW(window, other); if (!marker.empty()) RemovePropW(window, marker.c_str()); DestroyWindow(window); }
    CloseHandle(child);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 6 && std::wstring_view(argv[1]) == L"--native-folder-link-helper") {
        return pulse::app::NativeDialogFolderLinkHelper(reinterpret_cast<HANDLE>(Parse(argv[2])),
            reinterpret_cast<HANDLE>(Parse(argv[3])), reinterpret_cast<HANDLE>(Parse(argv[4])), argv[5]);
    }
    if (argc != 1) return 2;
    RunTimeout();
    std::printf("SUMMARY failures=%d\n", failures);
    return failures ? 1 : 0;
}
