#include "../index/custom_index_storage.h"
#include "../index/content_config_storage.h"
#include "../index/index_config.h"
#include "../index/index_paths.h"
#include "../index/index_protocol.h"
#include "../common/command_line.h"
#include "../ipc/deadline_pipe.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <shlobj.h>

namespace fs = std::filesystem;
using namespace pulse::index;
namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!ok) ++failures;
}
bool Under(const std::wstring& path, const std::wstring& root) {
    return path.size() > root.size() && path[root.size()] == L'\\' &&
        CompareStringOrdinal(path.data(), static_cast<int>(root.size()), root.data(), static_cast<int>(root.size()), TRUE) == CSTR_EQUAL;
}
struct Process {
    PROCESS_INFORMATION info{};
    ~Process() {
        if (info.hProcess) {
            if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) {
                // Only this test's child, never an installed helper/service.
                TerminateProcess(info.hProcess, ERROR_OPERATION_ABORTED);
                WaitForSingleObject(info.hProcess, 5000);
            }
            CloseHandle(info.hProcess); CloseHandle(info.hThread);
        }
    }
    bool Start(const fs::path& exe, const std::wstring& arguments) {
        std::wstring command = pulse::QuoteWindowsArgument(exe.wstring()) + L" " + arguments;
        STARTUPINFOW startup{sizeof(startup)};
        HANDLE token = nullptr, restricted = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, &token)) return false;
        TOKEN_ELEVATION elevation{}; DWORD bytes = 0;
        const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes) && elevation.TokenIsElevated;
        bool ok = false;
        if (elevated) {
            BYTE admin[SECURITY_MAX_SID_SIZE]{}; DWORD length = sizeof(admin);
            SID_AND_ATTRIBUTES disabled{admin, 0};
            if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin, &length) &&
                CreateRestrictedToken(token, DISABLE_MAX_PRIVILEGE, 1, &disabled, 0, nullptr, 0, nullptr, &restricted))
                ok = CreateProcessAsUserW(restricted, exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                    CREATE_NO_WINDOW, nullptr, exe.parent_path().c_str(), &startup, &info) != FALSE;
        } else {
            ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, exe.parent_path().c_str(), &startup, &info) != FALSE;
        }
        if (restricted) CloseHandle(restricted);
        CloseHandle(token);
        return ok;
    }
    bool Exit(DWORD expected = 0) {
        DWORD code = 0;
        return info.hProcess && WaitForSingleObject(info.hProcess, 15000) == WAIT_OBJECT_0 &&
            GetExitCodeProcess(info.hProcess, &code) && code == expected;
    }
};
using Snapshot = std::map<std::wstring, std::pair<uintmax_t, fs::file_time_type>>;
Snapshot LegacySnapshot() {
    Snapshot result;
    for (int kind : {CSIDL_LOCAL_APPDATA, CSIDL_COMMON_APPDATA}) {
        wchar_t path[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, kind, nullptr, SHGFP_TYPE_CURRENT, path))) continue;
        const fs::path root = fs::path(path) / L"Pulse";
        std::error_code error;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error)) {
            if (it->is_regular_file(error)) result[it->path().wstring()] = {it->file_size(error), it->last_write_time(error)};
        }
    }
    return result;
}
HANDLE Connect(const std::wstring& name) {
    const auto deadline = GetTickCount64() + 10000;
    do {
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        Sleep(20);
    } while (GetTickCount64() < deadline);
    return INVALID_HANDLE_VALUE;
}
bool Transfer(HANDLE pipe, void* data, DWORD size, bool write) {
    return pulse::ipc::DeadlinePipeIo(pipe, static_cast<uint8_t*>(data), size, write,
        GetTickCount64() + 10000, [] { return false; });
}
bool Send(HANDLE pipe, uint32_t type, uint32_t id, const std::vector<uint8_t>& payload = {}) {
    auto header = MakeIndexHdr(type, id, static_cast<uint32_t>(payload.size()));
    return Transfer(pipe, &header, sizeof(header), true) &&
        (payload.empty() || Transfer(pipe, const_cast<uint8_t*>(payload.data()), static_cast<DWORD>(payload.size()), true));
}
bool Search(HANDLE pipe) {
    pulse::ipc::PayloadWriter writer;
    writer.PutU32(1); writer.PutU32(static_cast<uint32_t>(ResultSort::Index)); writer.PutU32(20); writer.PutU32(0);
    writer.PutString(L"portable-boundary"); writer.PutString(L"");
    for (uint32_t attempt = 1; attempt <= 50; ++attempt) {
        if (!Send(pipe, REQ_IDX_SEARCH, attempt, writer.data())) return false;
        for (;;) {
            pulse::ipc::MsgHeader header{};
            if (!Transfer(pipe, &header, sizeof(header), false) || header.magic != kIndexMagic || header.payload_size > kIndexMaxPayload) return false;
            std::vector<uint8_t> bytes(header.payload_size);
            if (!bytes.empty() && !Transfer(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), false)) return false;
            if (header.type != RSP_IDX_SEARCH || header.request_id != attempt) continue;
            pulse::ipc::PayloadReader reader(bytes.data(), bytes.size()); uint32_t total = 0, count = 0;
            if (!reader.GetU32(total) || !reader.GetU32(count)) return false;
            if (total == 1 && count == 1) return true;
            break;
        }
        Sleep(100);
    }
    return false;
}
int Probe() {
    BYTE admin[SECURITY_MAX_SID_SIZE]{}; DWORD bytes = sizeof(admin); BOOL member = TRUE;
    const bool user = CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin, &bytes) &&
        CheckTokenMembership(nullptr, admin, &member) && !member;
    Check(user, "storage probe runs without an elevated token");
    const auto root = custom_storage::RootPath();
    Check(custom_storage::Writable(), "installation index directory is writable");
    Check(UserIndexRoot() == root && MachineDataRoot() == root && MachineIndexRoot() == root && DataDir() == root,
        "all filename index scopes resolve beside the executable");
    Check(Under(config_storage::DatabasePath(), root) && Under(MachineConfigPath(), root), "content database and config stay inside index");
    Check(std::wstring(kPipeName).find(L"PulseCustomNetworkIndex-") != std::wstring::npos && std::wstring(kServiceName) != L"PulseIndex",
        "custom edition never names the original shared endpoint or service");
    Check(custom_storage::InstallationKey(L"E:\\one") != custom_storage::InstallationKey(L"E:\\two"), "different installations have different endpoint namespaces");
    Check(custom_storage::InstallationKey(L"E:/ONE") == custom_storage::InstallationKey(L"e:\\one"), "installation namespace normalizes path case and separators");
    IndexConfig config; config.index_path = L"C:\\must-not-write";
    config.exclude_system = false; config.system_groups.clear(); config.system_groups_saved = true;
    Check(SaveMachineConfig(config), "save installation-local configuration");
    IndexConfig loaded;
    Check(LoadMachineConfig(loaded) && loaded.index_path == root, "copied configuration cannot redirect the index root");
    Check(!ConfigureIndexPath(L"C:\\must-not-write"), "production index relocation is rejected");
    return failures ? 1 : 0;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--probe") return Probe();
    if (argc == 2 && std::wstring_view(argv[1]) == L"--expect-unwritable") return custom_storage::Writable() ? 1 : 0;
    const auto original = LegacySnapshot();
    wchar_t temporary[32768]{};
    if (!GetTempPathW(ARRAYSIZE(temporary), temporary)) return 1;
    const fs::path scratch = fs::path(temporary) / (L"Pulse 定制版 索引验证 " + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto install = scratch / L"安装 目录";
    fs::create_directories(install);
    const auto test = install / L"storage-test.exe";
    const auto host = install / L"Pulse.Index.exe";
    const auto source = fs::path(custom_storage::InstallDirectory());
    fs::copy_file(source / L"pulse_custom_index_storage_test.exe", test);
    fs::copy_file(argc == 2 ? fs::path(argv[1]) : source / L"Pulse.Index.exe", host);
    {
        Process child;
        Check(child.Start(test, L"--probe") && child.Exit(), "real relocated executable verifies Chinese and space-containing installation path as a standard user");
    }
    const auto fixture = scratch / L"fixture";
    fs::create_directories(fixture);
    std::ofstream(fixture / L"portable-boundary.txt") << "fixture";
    for (int run = 0; run < 2; ++run) {
        const auto token = L"portable-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(run);
        Process child;
        const auto args = L"--test-host " + token + L" " + pulse::QuoteWindowsArgument(fixture.wstring()) + L" " + pulse::QuoteWindowsArgument((install / L"index").wstring());
        const bool started = child.Start(host, args);
        Check(started, "fixture helper starts without elevation");
        HANDLE pipe = started ? Connect(L"\\\\.\\pipe\\PulseIndex.Test." + token) : INVALID_HANDLE_VALUE;
        Check(pipe != INVALID_HANDLE_VALUE && Search(pipe), run ? "cached helper restart returns the fixture through the real search protocol" : "cold helper returns the fixture through the real search protocol");
        if (pipe != INVALID_HANDLE_VALUE) {
            Send(pipe, REQ_IDX_TEST_SHUTDOWN, 0); CloseHandle(pipe);
            Check(child.Exit(), "fixture helper stops cleanly");
        }
        Check(fs::exists(install / L"index" / L"pulse-index.bin"), "filename snapshot persists only under installation index");
    }
    {
        Process child;
        Check(child.Start(host, L"--install") && child.Exit(ERROR_NOT_SUPPORTED), "custom helper refuses SYSTEM service installation");
    }
    {
        const auto index = install / L"index", held = install / L"held-index";
        fs::rename(index, held);
        bool link = CreateSymbolicLinkW(index.c_str(), held.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2) != FALSE;
        if (!link && GetLastError() == ERROR_INVALID_PARAMETER)
            link = CreateSymbolicLinkW(index.c_str(), held.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY) != FALSE;
        if (link) {
            Process child;
            Check(child.Start(test, L"--expect-unwritable") && child.Exit(), "index directory reparse point is rejected");
            fs::remove(index);
        } else std::cout << "[SKIP] reparse-point fixture requires symbolic-link creation privilege\n";
        fs::rename(held, index);
    }
    const auto root = (install / L"index").wstring();
    PSECURITY_DESCRIPTOR readonly = nullptr;
    const auto sddl = L"D:P(A;OICI;FR;;;" + pulse::CurrentUserSidString() + L")";
    bool secured = ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &readonly, nullptr) != FALSE;
    PACL dacl = nullptr; BOOL present = FALSE, defaulted = FALSE;
    secured = secured && GetSecurityDescriptorDacl(readonly, &present, &dacl, &defaulted) && present &&
        SetNamedSecurityInfoW(const_cast<LPWSTR>(root.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
    Check(secured, "read-only index fixture is installed");
    if (readonly) LocalFree(readonly);
    {
        Process child;
        Check(secured && child.Start(test, L"--expect-unwritable") && child.Exit(), "read-only index fails without switching to AppData or changing its ACL");
    }
    {
        Process child;
        const auto args = L"--test-host readonly " + pulse::QuoteWindowsArgument(fixture.wstring()) + L" " + pulse::QuoteWindowsArgument(root);
        Check(secured && child.Start(host, args) && child.Exit(ERROR_ACCESS_DENIED), "real helper exits with access denied for a read-only installation index");
    }
    Check(original == LegacySnapshot(), "original LocalAppData/Pulse and ProgramData/Pulse files are unchanged");
    // Restore this test-owned tree before removing it; no user data is touched.
    PSECURITY_DESCRIPTOR writable = nullptr;
    const auto restore = L"D:P(A;OICI;FA;;;" + pulse::CurrentUserSidString() + L")";
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(restore.c_str(), SDDL_REVISION_1, &writable, nullptr)) {
        if (GetSecurityDescriptorDacl(writable, &present, &dacl, &defaulted) && present)
            SetNamedSecurityInfoW(const_cast<LPWSTR>(root.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                nullptr, nullptr, dacl, nullptr);
        LocalFree(writable);
    }
    std::error_code cleanup;
    fs::remove_all(scratch, cleanup);
    if (cleanup) std::wcerr << L"[INFO] test fixture retained at " << scratch << L"\n";
    return failures ? 1 : 0;
}
