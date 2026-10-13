#include "../fs/item_properties.h"

#include <aclapi.h>
#include <winioctl.h>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

int failures = 0;

void Check(bool ok, const char* label) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}

std::wstring Extended(const std::filesystem::path& path) {
    const std::wstring value = path.wstring();
    return value.starts_with(L"\\\\?\\") ? value : L"\\\\?\\" + value;
}

struct Fixture {
    std::filesystem::path root;
    std::vector<std::filesystem::path> links, file_links, attributed;
    std::filesystem::path restricted;
    PSECURITY_DESCRIPTOR security = nullptr;
    PACL original_acl = nullptr;
    bool owns_root = false;

    Fixture() {
        wchar_t temporary[32768]{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
        if (!length || length >= std::size(temporary)) return;
        root = std::filesystem::path(temporary) / (L"pulse-item-properties-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        owns_root = CreateDirectoryW(Extended(root).c_str(), nullptr) != FALSE;
    }

    ~Fixture() {
        if (!owns_root) return;
        RestoreAcl();
        for (const auto& path : links) RemoveDirectoryW(Extended(path).c_str());
        for (const auto& path : file_links) DeleteFileW(Extended(path).c_str());
        for (const auto& path : attributed) SetFileAttributesW(Extended(path).c_str(), FILE_ATTRIBUTE_NORMAL);
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path(Extended(root)), error);
        Check(!error, "remove only the owned temporary fixture");
    }

    void RestoreAcl() {
        if (!security) return;
        const DWORD error = SetNamedSecurityInfoW(Extended(restricted).data(), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, original_acl, nullptr);
        Check(error == ERROR_SUCCESS, "restore temporary directory permissions");
        LocalFree(security);
        security = nullptr;
    }

    bool Directory(const std::filesystem::path& path) {
        return CreateDirectoryW(Extended(path).c_str(), nullptr) != FALSE;
    }

    bool File(const std::filesystem::path& path, DWORD size) {
        HANDLE file = CreateFileW(Extended(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        std::vector<BYTE> bytes(size, 0x61);
        DWORD written = 0;
        const bool ok = WriteFile(file, bytes.data(), size, &written, nullptr) && written == size;
        CloseHandle(file);
        return ok;
    }

    bool Junction(const std::filesystem::path& path, const std::filesystem::path& target) {
        if (!Directory(path)) return false;
        // Register before attaching the reparse point, so even a failed attachment is cleaned.
        links.push_back(path);
        HANDLE handle = CreateFileW(Extended(path).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        struct MountPoint {
            DWORD tag;
            WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length;
            wchar_t path[2048];
        } data{};
        const std::wstring substitute = L"\\??\\" + target.wstring();
        const std::wstring display = target.wstring();
        if (substitute.size() + display.size() + 2 > std::size(data.path)) {
            CloseHandle(handle);
            return false;
        }
        data.tag = IO_REPARSE_TAG_MOUNT_POINT;
        data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
        data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
        data.print_length = static_cast<WORD>(display.size() * sizeof(wchar_t));
        data.length = static_cast<WORD>(8 + data.print_offset + data.print_length + sizeof(wchar_t));
        memcpy(data.path, substitute.c_str(), data.substitute_length + sizeof(wchar_t));
        memcpy(reinterpret_cast<BYTE*>(data.path) + data.print_offset, display.c_str(),
            data.print_length + sizeof(wchar_t));
        DWORD returned = 0;
        const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
            static_cast<DWORD>(data.length) + 8, nullptr, 0, &returned, nullptr) != FALSE;
        CloseHandle(handle);
        return ok;
    }

    bool DenyEnumeration(const std::filesystem::path& path) {
        restricted = path;
        if (GetNamedSecurityInfoW(Extended(path).data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &original_acl, nullptr, &security) != ERROR_SUCCESS) return false;
        BYTE sid_buffer[SECURITY_MAX_SID_SIZE]{};
        DWORD sid_size = sizeof(sid_buffer);
        if (!CreateWellKnownSid(WinWorldSid, nullptr, sid_buffer, &sid_size)) return false;
        EXPLICIT_ACCESSW deny{};
        deny.grfAccessPermissions = FILE_LIST_DIRECTORY;
        deny.grfAccessMode = DENY_ACCESS;
        deny.grfInheritance = NO_INHERITANCE;
        deny.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        deny.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        deny.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid_buffer);
        PACL replacement = nullptr;
        DWORD error = SetEntriesInAclW(1, &deny, original_acl, &replacement);
        if (error == ERROR_SUCCESS) {
            error = SetNamedSecurityInfoW(Extended(path).data(), SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                nullptr, nullptr, replacement, nullptr);
        }
        if (replacement) LocalFree(replacement);
        return error == ERROR_SUCCESS;
    }
};

uint64_t DiskSize(const std::filesystem::path& path) {
    DWORD high = 0;
    SetLastError(ERROR_SUCCESS);
    const DWORD low = GetCompressedFileSizeW(Extended(path).c_str(), &high);
    Check(low != INVALID_FILE_SIZE || GetLastError() == ERROR_SUCCESS, "query actual disk allocation independently");
    return (static_cast<uint64_t>(high) << 32) | low;
}

void TestBasic(Fixture& fixture, const std::atomic_bool& cancelled) {
    const auto folder = fixture.root / L"ordinary";
    const auto child = folder / L"child";
    const auto first = folder / L"first.txt";
    const auto second = child / L"second.bin";
    Check(fixture.Directory(folder) && fixture.Directory(child) && fixture.File(first, 7) && fixture.File(second, 19),
        "create isolated nested files");
    auto result = pulse::fs::ReadItemProperties({first.wstring()}, cancelled);
    Check(result.name == L"first.txt" && result.location == folder.wstring() && result.type == L"file" &&
        result.size_bytes == 7 && result.file_count == 1 && result.folder_count == 0 && result.has_times &&
        result.size_complete && result.first_error == ERROR_SUCCESS, "single file metadata and logical size");
    Check(result.allocated_bytes == DiskSize(first), "single file allocation matches filesystem query");
    result = pulse::fs::ReadItemProperties({folder.wstring()}, cancelled);
    Check(result.type == L"folder" && result.size_complete && result.size_bytes == 26 &&
        result.file_count == 2 && result.folder_count == 1, "folder includes descendants but excludes itself from count");
    Check(result.allocated_bytes == DiskSize(first) + DiskSize(second), "folder aggregates actual allocation");
    result = pulse::fs::ReadItemProperties({child.wstring(), first.wstring(), folder.wstring(),
        second.wstring(), folder.wstring(), (folder / L".").wstring()}, cancelled);
    Check(result.size_complete && result.size_bytes == 26 && result.file_count == 2 && result.folder_count == 1,
        "duplicates and overlapping parent selections count content once in any input order");
    Check(result.name.empty() && !result.has_times && result.type == L"mixed" && result.location.empty(),
        "mixed selection has no individual name, timestamp or shared location");
    fixture.attributed.push_back(first);
    Check(SetFileAttributesW(Extended(first).c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN) != FALSE,
        "set attributes only on fixture");
    result = pulse::fs::ReadItemProperties({first.wstring()}, cancelled);
    Check(result.readonly_state == 1 && result.hidden_state == 1, "set attribute states");
    const auto third = folder / L"third.txt";
    Check(fixture.File(third, 3), "create second selection peer");
    const DWORD before = GetFileAttributesW(Extended(first).c_str());
    WIN32_FILE_ATTRIBUTE_DATA times_before{}, times_after{};
    Check(GetFileAttributesExW(Extended(first).c_str(), GetFileExInfoStandard, &times_before) != FALSE,
        "record fixture metadata before property read");
    result = pulse::fs::ReadItemProperties({first.wstring(), third.wstring()}, cancelled);
    Check(result.type == L"file" && result.location == folder.wstring() && result.readonly_state == 2 &&
        result.hidden_state == 2 && result.size_bytes == 10, "multi-file shared location and mixed attribute states");
    Check(GetFileAttributesW(Extended(first).c_str()) == before, "property reads preserve fixture attributes");
    Check(GetFileAttributesExW(Extended(first).c_str(), GetFileExInfoStandard, &times_after) &&
        CompareFileTime(&times_before.ftCreationTime, &times_after.ftCreationTime) == 0 &&
        CompareFileTime(&times_before.ftLastWriteTime, &times_after.ftLastWriteTime) == 0,
        "property reads preserve creation and modification timestamps");
    result = pulse::fs::ReadItemProperties({third.wstring()}, cancelled);
    Check(result.readonly_state == 0 && result.hidden_state == 0, "clear attribute states");
    result = pulse::fs::ReadItemProperties({first.wstring(), (folder / L"missing").wstring()}, cancelled);
    Check(!result.size_complete && result.unreadable_count == 1 && result.first_error == ERROR_FILE_NOT_FOUND &&
        result.readonly_state == 2 && result.hidden_state == 2 && !result.has_times,
        "missing selection remains incomplete and attributes unknown");
}

void TestLinksAndErrors(Fixture& fixture, const std::atomic_bool& cancelled) {
    const auto folder = fixture.root / L"links";
    const auto link = folder / L"cycle";
    Check(fixture.Directory(folder) && fixture.File(folder / L"data.txt", 11), "create link fixture");
    Check(fixture.Junction(link, folder), "create junction cycle within owned fixture");
    auto result = pulse::fs::ReadItemProperties({folder.wstring()}, cancelled);
    Check(!result.size_complete && result.size_bytes == 11 && result.file_count == 1 &&
        result.folder_count == 1 && result.skipped_links == 1, "junction cycle is counted and never followed");
    result = pulse::fs::ReadItemProperties({link.wstring()}, cancelled);
    Check(!result.size_complete && result.type == L"folder" && result.size_bytes == 0 &&
        result.file_count == 0 && result.skipped_links == 1, "selected root junction is never followed");
    result = pulse::fs::ReadItemProperties({(link / L"data.txt").wstring()}, cancelled);
    Check(!result.size_complete && result.size_bytes == 0 && result.skipped_links == 1,
        "explicit path through junction ancestor is never followed");
    const auto file_link = fixture.root / L"file-link.txt";
    if (CreateSymbolicLinkW(Extended(file_link).c_str(), Extended(folder / L"data.txt").c_str(),
        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        fixture.file_links.push_back(file_link);
        result = pulse::fs::ReadItemProperties({file_link.wstring()}, cancelled);
        Check(!result.size_complete && result.size_bytes == 0 && result.skipped_links == 1 &&
            result.file_count == 1, "selected file symlink does not read its target");
    } else printf("[SKIP] symbolic-link fixture unavailable: %lu\n", GetLastError());
    const auto denied = fixture.root / L"denied";
    Check(fixture.Directory(denied) && fixture.File(denied / L"hidden-by-acl.txt", 101), "create access-error fixture");
    Check(fixture.DenyEnumeration(denied), "deny enumeration only on owned temporary directory");
    result = pulse::fs::ReadItemProperties({denied.wstring()}, cancelled);
    Check(!result.size_complete && result.unreadable_count == 1 && result.first_error == ERROR_ACCESS_DENIED &&
        result.size_bytes == 0, "access denied is an incomplete size rather than a complete zero");
    fixture.RestoreAcl();
    result = pulse::fs::ReadItemProperties({denied.wstring()}, cancelled);
    Check(result.size_complete && result.size_bytes == 101, "fixture readable after permission restoration");
}

void TestLongAndSparse(Fixture& fixture, const std::atomic_bool& cancelled) {
    auto long_folder = fixture.root;
    for (int i = 0; i < 5; ++i) {
        long_folder /= std::wstring(65, static_cast<wchar_t>(L'a' + i));
        Check(fixture.Directory(long_folder), "create long-path segment");
    }
    const auto long_file = long_folder / L"long.bin";
    Check(long_file.wstring().size() > MAX_PATH && fixture.File(long_file, 37), "create real path beyond MAX_PATH");
    auto result = pulse::fs::ReadItemProperties({long_file.wstring(), Extended(long_file)}, cancelled);
    Check(result.size_complete && result.size_bytes == 37 && result.file_count == 1 &&
        result.name == L"long.bin" && result.location == long_folder.wstring(),
        "ordinary and extended long-path aliases deduplicate");
    const auto sparse = fixture.root / L"sparse.bin";
    HANDLE file = CreateFileW(Extended(sparse).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create sparse fixture");
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD returned = 0;
    const bool supported = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != FALSE;
    if (supported) {
        LARGE_INTEGER end{};
        end.QuadPart = 16 * 1024 * 1024;
        Check(SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file), "extend sparse fixture without data writes");
    } else printf("[SKIP] fixture filesystem does not support sparse files: %lu\n", GetLastError());
    CloseHandle(file);
    if (supported) {
        result = pulse::fs::ReadItemProperties({sparse.wstring()}, cancelled);
        Check(result.size_complete && result.size_bytes == 16 * 1024 * 1024 &&
            result.allocated_bytes == DiskSize(sparse) && result.allocated_bytes < result.size_bytes,
            "sparse allocation reflects real disk occupancy instead of logical size");
    }
}

void TestBounds(Fixture& fixture, const std::atomic_bool& cancelled) {
    const auto root = fixture.root / L"bounded";
    Check(fixture.Directory(root), "create depth-limit fixture");
    auto deep = root;
    for (int i = 0; i < 260; ++i) {
        deep /= L"d";
        if (!fixture.Directory(deep)) {
            Check(false, "create bounded-depth segments");
            return;
        }
    }
    auto result = pulse::fs::ReadItemProperties({root.wstring()}, cancelled);
    Check(!result.size_complete && result.unreadable_count > 0 && result.first_error == ERROR_DIRECTORY,
        "bounded traversal reports incomplete at depth limit");
    std::atomic_bool stopped{true};
    result = pulse::fs::ReadItemProperties({root.wstring()}, stopped);
    Check(!result.size_complete && result.first_error == ERROR_CANCELLED && result.file_count == 0 &&
        result.folder_count == 0, "already cancelled request does no traversal");
    const auto busy = fixture.root / L"cancel";
    Check(fixture.Directory(busy), "create mid-walk cancellation fixture");
    for (int i = 0; i < 2048; ++i) {
        if (!fixture.File(busy / (std::to_wstring(i) + L".txt"), 1)) {
            Check(false, "create mid-walk fixture files");
            return;
        }
    }
    stopped.store(false);
    std::thread stopper([&] { Sleep(1); stopped.store(true); });
    result = pulse::fs::ReadItemProperties({busy.wstring()}, stopped);
    stopper.join();
    Check(!result.size_complete && result.first_error == ERROR_CANCELLED && result.file_count < 2048,
        "cancellation interrupts per-entry traversal and marks partial size");
    const auto moved = fixture.root / L"cancel-moved";
    BOOL renamed = FALSE;
    DWORD rename_error = ERROR_SUCCESS;
    std::atomic_bool scan_done{false};
    bool overlapped = false;
    std::thread renamer([&] {
        Sleep(1);
        renamed = MoveFileW(Extended(busy).c_str(), Extended(moved).c_str());
        if (!renamed) rename_error = GetLastError();
        overlapped = !scan_done.load();
    });
    result = pulse::fs::ReadItemProperties({busy.wstring()}, cancelled);
    scan_done.store(true);
    renamer.join();
    if (!renamed) printf("Rename error: %lu\n", rename_error);
    Check(renamed != FALSE, "property scan does not block fixture directory rename");
    if (overlapped) Check(!result.size_complete && result.first_error != ERROR_SUCCESS,
        "directory renamed during scan is reported incomplete");
    else printf("[SKIP] directory rename completed after scan; no overlapping mutation to verify\n");
    result = pulse::fs::ReadItemProperties({}, cancelled);
    Check(!result.size_complete && result.first_error == ERROR_INVALID_PARAMETER, "empty selection has no complete size");
    result = pulse::fs::ReadItemProperties({std::wstring(L"bad\0path", 8)}, cancelled);
    Check(!result.size_complete && result.first_error == ERROR_INVALID_NAME, "embedded NUL cannot truncate a user path");
}

}

int main() {
    {
        Fixture fixture;
        Check(fixture.owns_root, "create uniquely owned temporary fixture");
        if (!fixture.owns_root) return 1;
        const std::atomic_bool cancelled{false};
        TestBasic(fixture, cancelled);
        TestLinksAndErrors(fixture, cancelled);
        TestLongAndSparse(fixture, cancelled);
        TestBounds(fixture, cancelled);
    }
    printf("Item properties: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
