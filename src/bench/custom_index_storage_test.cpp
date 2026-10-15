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
#include <iterator>
#include <map>
#include <shlobj.h>
#include <shellapi.h>

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
    fs::path executable, output_path;
    std::string operation;
    void Output() const {
        auto print = [](const fs::path& path) {
            std::ifstream input(path, std::ios::binary);
            if (input) std::cout << std::string(std::istreambuf_iterator<char>(input), {});
        };
        print(output_path);
        if (executable.filename() == L"storage-test.exe") print(executable.parent_path() / L"probe-result.log");
    }
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
        executable = exe;
        if (exe.filename() == L"storage-test.exe") DeleteFileW((exe.parent_path() / L"probe-result.log").c_str());
        const auto option = arguments.substr(0, arguments.find(L' '));
        operation.clear();
        for (const wchar_t ch : option) operation += static_cast<char>(ch);
        std::wstring command = pulse::QuoteWindowsArgument(exe.wstring()) + L" " + arguments;
        static uint32_t next_log = 0;
        output_path = exe.parent_path() / (L"child-" + std::to_wstring(++next_log) + L".log");
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE output = CreateFileW(output_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &inherit, OPEN_EXISTING, 0, nullptr);
        SIZE_T attribute_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
        std::vector<BYTE> attribute_memory(attribute_size);
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = output; startup.StartupInfo.hStdError = output; startup.StartupInfo.hStdInput = input;
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_memory.data());
        const bool initialized = InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size) != FALSE;
        HANDLE inherited[]{output, input};
        const bool ready = initialized && output != INVALID_HANDLE_VALUE && input != INVALID_HANDLE_VALUE &&
            UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);
        HANDLE token = nullptr, restricted = nullptr;
        if (!ready || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, &token)) {
            const auto error = GetLastError();
            if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
            if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
            if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
            std::cout << "[INFO] child setup failed, Win32=" << error << '\n';
            return false;
        }
        TOKEN_ELEVATION elevation{}; DWORD bytes = 0;
        const bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes) && elevation.TokenIsElevated;
        bool ok = false;
        if (elevated) {
            BYTE admin[SECURITY_MAX_SID_SIZE]{}; DWORD length = sizeof(admin);
            SID_AND_ATTRIBUTES disabled{admin, 0};
            if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin, &length) &&
                CreateRestrictedToken(token, DISABLE_MAX_PRIVILEGE, 1, &disabled, 0, nullptr, 0, nullptr, &restricted))
                ok = CreateProcessAsUserW(restricted, exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                    CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, exe.parent_path().c_str(), &startup.StartupInfo, &info) != FALSE;
        } else {
            ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, exe.parent_path().c_str(), &startup.StartupInfo, &info) != FALSE;
        }
        const auto error = ok ? ERROR_SUCCESS : GetLastError();
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        CloseHandle(output); CloseHandle(input);
        if (restricted) CloseHandle(restricted);
        CloseHandle(token);
        if (!ok) std::cout << "[INFO] child launch failed, Win32=" << error << ", exe=" << executable.filename().string() << ", mode=" << operation << '\n';
        return ok;
    }
    bool Exit(DWORD expected = 0) {
        DWORD code = 0;
        const DWORD wait = info.hProcess ? WaitForSingleObject(info.hProcess, 15000) : WAIT_FAILED;
        const DWORD wait_error = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        const bool queried = info.hProcess && GetExitCodeProcess(info.hProcess, &code);
        const DWORD query_error = queried ? ERROR_SUCCESS : GetLastError();
        Output();
        if (wait != WAIT_OBJECT_0 || !queried || code != expected)
            std::cout << "[INFO] child result, wait=" << wait << ", exit=0x" << std::hex << code
                << ", expected=0x" << expected << std::dec << ", Win32=" << (wait_error ? wait_error : query_error)
                << ", exe=" << executable.filename().string() << ", mode=" << operation << '\n';
        return wait == WAIT_OBJECT_0 && queried && code == expected;
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
bool Search(HANDLE pipe, const std::wstring& needle = L"portable-boundary", uint32_t expected = 1) {
    pulse::ipc::PayloadWriter writer;
    writer.PutU32(1); writer.PutU32(static_cast<uint32_t>(ResultSort::Index)); writer.PutU32(20); writer.PutU32(0);
    writer.PutString(needle); writer.PutString(L"");
    static uint32_t next_id = 0;
    uint32_t stable = 0;
    for (uint32_t attempt = 1; attempt <= 50; ++attempt) {
        const auto id = ++next_id;
        if (!Send(pipe, REQ_IDX_SEARCH, id, writer.data())) return false;
        for (;;) {
            pulse::ipc::MsgHeader header{};
            if (!Transfer(pipe, &header, sizeof(header), false) || header.magic != kIndexMagic || header.payload_size > kIndexMaxPayload) return false;
            std::vector<uint8_t> bytes(header.payload_size);
            if (!bytes.empty() && !Transfer(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), false)) return false;
            if (header.type != RSP_IDX_SEARCH || header.request_id != id) continue;
            pulse::ipc::PayloadReader reader(bytes.data(), bytes.size()); uint32_t total = 0, count = 0;
            if (!reader.GetU32(total) || !reader.GetU32(count)) return false;
            stable = total == expected && count == expected ? stable + 1 : 0;
            if (stable >= (expected ? 1u : 3u)) return true;
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

int StorageTestMain(int argc, wchar_t** argv) {
    std::cout << std::unitbuf;
    if (argc == 2 && (std::wstring_view(argv[1]) == L"--probe" || std::wstring_view(argv[1]) == L"--expect-unwritable")) {
        std::ofstream result(fs::path(custom_storage::InstallDirectory()) / L"probe-result.log", std::ios::binary | std::ios::trunc);
        if (!result) { std::cerr << "[INFO] cannot open probe-result.log, Win32=" << GetLastError() << '\n'; return 2; }
        auto* output = std::cout.rdbuf(result.rdbuf());
        std::cout << "[INFO] probe entered main\n";
        int code = 0;
        if (std::wstring_view(argv[1]) == L"--probe") code = Probe();
        else {
            const bool writable = custom_storage::Writable();
            const DWORD error = GetLastError();
            std::cout << "[INFO] unwritable probe, writable=" << writable << ", Win32=" << error << '\n';
            code = writable ? 1 : 0;
        }
        std::cout << "[INFO] probe exit=" << code << '\n';
        std::cout.rdbuf(output);
        return code;
    }
    const auto original = LegacySnapshot();
    wchar_t temporary[32768]{};
    if (!GetTempPathW(ARRAYSIZE(temporary), temporary)) return 1;
    const fs::path scratch = fs::path(temporary) / (L"Pulse 定制版 索引验证 " + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto install = scratch / L"安装 目录";
    fs::create_directories(install);
    const auto test = install / L"storage-test.exe";
    const auto host = install / L"Pulse.Index.exe";
    const auto source = fs::path(custom_storage::InstallDirectory());
    fs::copy_file(source / L"pulse_custom_index_storage_probe.exe", test);
    fs::copy_file(argc == 2 ? fs::path(argv[1]) : source / L"Pulse.Index.exe", host);
    {
        Process child;
        Check(child.Start(test, L"--probe") && child.Exit(), "real relocated executable verifies Chinese and space-containing installation path as a standard user");
    }
    const auto fixture = scratch / L"fixture";
    fs::create_directories(fixture);
    std::ofstream(fixture / L"portable-boundary.txt") << "fixture";
    const auto excluded = fixture / L"excluded";
    fs::create_directories(excluded);
    std::ofstream(excluded / L"excluded-initial-token.txt") << "excluded";
    {
        Process child;
        Check(child.Start(host, L"--configure-exclude " + pulse::QuoteWindowsArgument(excluded.wstring()) + L" --enable") && child.Exit(),
            "explicit fixture exclusion is configured without elevation");
    }
    for (int run = 0; run < 2; ++run) {
        const auto token = L"portable-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(run);
        Process child;
        const auto args = L"--test-host " + token + L" " + pulse::QuoteWindowsArgument(fixture.wstring()) + L" " + pulse::QuoteWindowsArgument((install / L"index").wstring());
        const bool started = child.Start(host, args);
        Check(started, "fixture helper starts without elevation");
        HANDLE pipe = started ? Connect(L"\\\\.\\pipe\\PulseIndex.Test." + token) : INVALID_HANDLE_VALUE;
        Check(pipe != INVALID_HANDLE_VALUE && Search(pipe), run ? "cached helper restart returns the fixture through the real search protocol" : "cold helper returns the fixture through the real search protocol");
        if (pipe != INVALID_HANDLE_VALUE) {
            Check(Search(pipe, L"excluded-initial-token", 0), "cold and cached searches honor the configured fixture exclusion");
            if (run == 0) {
                const auto snapshot = install / L"index" / L"pulse-index.bin";
                const auto before_empty = fs::last_write_time(snapshot);
                fs::create_directory(fixture / L"live-empty-directory-token");
                Check(Search(pipe, L"live-empty-directory-token"), "live empty directory is indexed incrementally");
                Sleep(750);
                Check(fs::last_write_time(snapshot) == before_empty, "creating an empty directory never rewrites the full filename snapshot");

                const auto incoming = scratch / L"prepared-tree";
                fs::create_directories(incoming / L"nested");
                std::ofstream(incoming / L"nested" / L"incoming-descendant-token.txt") << "prepared outside the watch";
                const auto before_incoming = fs::last_write_time(snapshot);
                fs::rename(incoming, fixture / L"incoming-tree");
                Check(Search(pipe, L"incoming-descendant-token"), "moving in a populated directory discovers missing descendants incrementally");
                Check(fs::last_write_time(snapshot) == before_incoming, "subtree reconciliation does not rebuild the full filename snapshot");

                const auto created = fixture / L"live-created-token.txt", renamed = fixture / L"live-renamed-token.txt";
                std::ofstream(created) << "created while helper runs";
                Check(Search(pipe, L"live-created-token"), "live file creation is returned without restarting the helper");
                fs::rename(created, renamed);
                Check(Search(pipe, L"live-renamed-token") && Search(pipe, L"live-created-token", 0), "live rename updates the new and old search names");
                fs::remove(renamed);
                Check(Search(pipe, L"live-renamed-token", 0), "live deletion removes the search hit");

                const auto hidden = excluded / L"excluded-live-token.txt";
                const auto marker = fixture / L"after-excluded-token.txt";
                std::ofstream(hidden) << "must remain excluded";
                std::ofstream(marker) << "notification ordering marker";
                Check(Search(pipe, L"after-excluded-token") && Search(pipe, L"excluded-live-token", 0),
                    "live creation inside an excluded directory never becomes searchable");
                fs::rename(marker, excluded / marker.filename());
                const auto confirmed = fixture / L"rename-confirmation-token.txt";
                std::ofstream(confirmed) << "notification ordering marker";
                Check(Search(pipe, L"rename-confirmation-token") && Search(pipe, L"after-excluded-token", 0),
                    "moving an indexed file into an excluded directory hides its search hit");
                fs::rename(hidden, fixture / hidden.filename());
                Check(Search(pipe, L"excluded-live-token"), "moving a file out of an excluded directory makes it searchable");

                const auto tree = fixture / L"live-tree";
                fs::create_directories(tree);
                std::ofstream(tree / L"directory-crossing-token.txt") << "directory subtree";
                Check(Search(pipe, L"directory-crossing-token"), "live directory population is reconciled");
                fs::rename(tree, excluded / tree.filename());
                Check(Search(pipe, L"directory-crossing-token", 0), "moving a populated directory into an exclusion hides descendants");
                fs::rename(excluded / tree.filename(), tree);
                Check(Search(pipe, L"directory-crossing-token"), "moving a populated directory out of an exclusion restores descendants");
                fs::remove_all(tree);
                Check(Search(pipe, L"directory-crossing-token", 0), "live directory deletion removes descendant search hits");
            }
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

#ifdef PULSE_STORAGE_WIN32_PROBE
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return static_cast<int>(GetLastError());
    const bool valid = argc == 2 && (std::wstring_view(argv[1]) == L"--probe" || std::wstring_view(argv[1]) == L"--expect-unwritable");
    const int result = valid ? StorageTestMain(argc, argv) : ERROR_INVALID_PARAMETER;
    LocalFree(argv);
    return result;
}
#else
int wmain(int argc, wchar_t** argv) {
    return StorageTestMain(argc, argv);
}
#endif
