#include "item_properties.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <winternl.h>

namespace pulse::fs {
namespace {

constexpr size_t kMaxDepth = 256;

class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
        static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

void FirstError(ItemProperties& result, DWORD error) {
    result.size_complete = false;
    if (result.first_error == ERROR_SUCCESS) result.first_error = error;
}

void Unreadable(ItemProperties& result, DWORD error) {
    FirstError(result, error);
    if (result.unreadable_count != UINT32_MAX) ++result.unreadable_count;
}

void SkipLink(ItemProperties& result) {
    FirstError(result, ERROR_CANT_ACCESS_FILE);
    if (result.skipped_links != UINT32_MAX) ++result.skipped_links;
}

bool Cancelled(ItemProperties& result, const std::atomic_bool& cancelled) {
    if (!cancelled.load(std::memory_order_relaxed)) return false;
    FirstError(result, ERROR_CANCELLED);
    return true;
}

void Add(ItemProperties& result, uint64_t& total, uint64_t value) {
    if (value > std::numeric_limits<uint64_t>::max() - total) {
        total = std::numeric_limits<uint64_t>::max();
        FirstError(result, ERROR_ARITHMETIC_OVERFLOW);
    } else total += value;
}

std::wstring DisplayPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + path.substr(8);
    return path.starts_with(L"\\\\?\\") ? path.substr(4) : path;
}

size_t RootLength(const std::wstring& path) {
    if (path.size() >= 7 && path.starts_with(L"\\\\?\\") && path[5] == L':' && path[6] == L'\\')
        return 7;
    if (!path.starts_with(L"\\\\?\\UNC\\")) return 0;
    const size_t server = path.find(L'\\', 8);
    if (server == std::wstring::npos || server == 8) return 0;
    const size_t share = path.find(L'\\', server + 1);
    const size_t end = share == std::wstring::npos ? path.size() : share;
    return end == server + 1 ? 0 : end;
}

std::wstring Normalize(const std::wstring& input) {
    if (input.empty() || input.find(L'\0') != std::wstring::npos) {
        SetLastError(ERROR_INVALID_NAME);
        return {};
    }
    std::wstring path = input;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.starts_with(L"\\\\.\\")) {
        SetLastError(ERROR_INVALID_NAME);
        return {};
    }
    if (path.starts_with(L"\\\\?\\") && !RootLength(path)) {
        SetLastError(ERROR_INVALID_NAME);
        return {};
    }
    // Canonicalize dot segments before adding the prefix that disables Win32 normalization.
    path = DisplayPath(path);
    if (path.size() == 2 && path[1] == L':') path += L'\\';
    std::vector<wchar_t> full(32768);
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (!length) return {};
    if (length >= full.size()) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return {};
    }
    path.assign(full.data(), length);
    path = path.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
    const size_t root = RootLength(path);
    if (!root || path.size() >= 32767) {
        SetLastError(ERROR_INVALID_NAME);
        return {};
    }
    while (path.size() > root && path.back() == L'\\') path.pop_back();
    return path;
}

std::wstring Parent(const std::wstring& path) {
    const size_t root = RootLength(path);
    if (path.size() <= root) return {};
    const size_t slash = path.find_last_of(L'\\');
    return path.substr(0, std::max(root, slash));
}

bool Descendant(const std::wstring& child, const std::wstring& parent) {
    if (child.size() <= parent.size() || !SamePath(child.substr(0, parent.size()), parent)) return false;
    return parent.back() == L'\\' || child[parent.size()] == L'\\';
}

Handle OpenMetadata(const std::wstring& path, DWORD access = FILE_READ_ATTRIBUTES) {
    return Handle(CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
}

Handle OpenRelative(HANDLE parent, const std::wstring& name, DWORD access = FILE_READ_ATTRIBUTES) {
    using Create = NTSTATUS (NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
        PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    using ConvertError = ULONG (WINAPI*)(NTSTATUS);
    static const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    static const auto create = reinterpret_cast<Create>(GetProcAddress(ntdll, "NtCreateFile"));
    static const auto convert = reinterpret_cast<ConvertError>(GetProcAddress(ntdll, "RtlNtStatusToDosError"));
    if (!create || !convert || name.size() > 32767) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return Handle();
    }
    UNICODE_STRING unicode{};
    unicode.Buffer = const_cast<LPWSTR>(name.c_str());
    unicode.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    unicode.MaximumLength = unicode.Length;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &unicode;
    attributes.Attributes = 0x40; // OBJ_CASE_INSENSITIVE, matching ordinary Win32 opens.
    IO_STATUS_BLOCK io{};
    HANDLE handle = INVALID_HANDLE_VALUE;
    constexpr ULONG options = 0x00200000 | 0x00004000 | 0x00000020;
    // FILE_OPEN_REPARSE_POINT | FILE_OPEN_FOR_BACKUP_INTENT | FILE_SYNCHRONOUS_IO_NONALERT.
    const NTSTATUS status = create(&handle, access | SYNCHRONIZE, &attributes, &io, nullptr,
        0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 1, options, nullptr, 0);
    if (status < 0) {
        SetLastError(convert(status));
        return Handle();
    }
    return Handle(handle);
}

bool BasicInfo(HANDLE handle, FILE_BASIC_INFO& info) {
    return GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof(info)) != FALSE;
}

// Open one component at a time relative to the previous directory handle. Sharing DELETE
// allows normal renames/removal without redirecting subsequent opens through a replacement.
Handle OpenSelection(const std::wstring& path, ItemProperties& result,
    const std::atomic_bool& cancelled, DWORD access = FILE_READ_ATTRIBUTES) {
    const size_t root = RootLength(path);
    Handle current = OpenMetadata(path.substr(0, root), path.size() == root ? access : FILE_READ_ATTRIBUTES);
    if (!current) {
        Unreadable(result, GetLastError());
        return Handle();
    }
    size_t start = root;
    if (start < path.size() && path[start] == L'\\') ++start;
    size_t depth = 0;
    while (start < path.size()) {
        if (Cancelled(result, cancelled)) return Handle();
        if (++depth >= kMaxDepth) {
            Unreadable(result, ERROR_DIRECTORY);
            return Handle();
        }
        FILE_BASIC_INFO info{};
        if (!BasicInfo(current.get(), info)) {
            Unreadable(result, GetLastError());
            return Handle();
        }
        if (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            SkipLink(result);
            return Handle();
        }
        const size_t slash = path.find(L'\\', start);
        const size_t end = slash == std::wstring::npos ? path.size() : slash;
        Handle child = OpenRelative(current.get(), path.substr(start, end - start),
            end == path.size() ? access : FILE_READ_ATTRIBUTES);
        if (!child) {
            Unreadable(result, GetLastError());
            return Handle();
        }
        current = std::move(child);
        start = end + 1;
    }
    return current;
}

bool SameObject(HANDLE first, HANDLE second) {
    BY_HANDLE_FILE_INFORMATION a{}, b{};
    return GetFileInformationByHandle(first, &a) && GetFileInformationByHandle(second, &b) &&
        a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
        a.nFileIndexLow == b.nFileIndexLow;
}

FILETIME Time(LARGE_INTEGER value) {
    return FILETIME{value.LowPart, static_cast<DWORD>(value.HighPart)};
}

struct Root {
    std::wstring path;
    FILE_BASIC_INFO basic{};
    bool known = false;
};

void FileSize(HANDLE handle, ItemProperties& result) {
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard))) {
        Unreadable(result, GetLastError());
        return;
    }
    if (standard.EndOfFile.QuadPart < 0 || standard.AllocationSize.QuadPart < 0) {
        Unreadable(result, ERROR_INVALID_DATA);
        return;
    }
    Add(result, result.size_bytes, static_cast<uint64_t>(standard.EndOfFile.QuadPart));
    FILE_COMPRESSION_INFO compressed{};
    // This is the handle-based equivalent of GetCompressedFileSizeW, including sparse files.
    if (GetFileInformationByHandleEx(handle, FileCompressionInfo, &compressed, sizeof(compressed))) {
        if (compressed.CompressedFileSize.QuadPart < 0) Unreadable(result, ERROR_INVALID_DATA);
        else Add(result, result.allocated_bytes, static_cast<uint64_t>(compressed.CompressedFileSize.QuadPart));
    } else {
        const DWORD error = GetLastError();
        FILE_BASIC_INFO basic{};
        if (!BasicInfo(handle, basic)) Unreadable(result, GetLastError());
        else if (basic.FileAttributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE))
            Unreadable(result, error);
        else Add(result, result.allocated_bytes, static_cast<uint64_t>(standard.AllocationSize.QuadPart));
    }
}

void Walk(const std::wstring& path, ItemProperties& result, const std::atomic_bool& cancelled,
    size_t depth, bool selected_root, HANDLE parent = INVALID_HANDLE_VALUE,
    const std::wstring& relative_name = {}) {
    if (Cancelled(result, cancelled)) return;
    Handle metadata = parent == INVALID_HANDLE_VALUE ? OpenSelection(path, result, cancelled) :
        OpenRelative(parent, relative_name);
    FILE_BASIC_INFO basic{};
    if (!metadata) {
        if (parent != INVALID_HANDLE_VALUE) Unreadable(result, GetLastError());
        return;
    }
    if (!BasicInfo(metadata.get(), basic)) {
        Unreadable(result, GetLastError());
        return;
    }
    const bool directory = (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (directory) {
        if (!selected_root) Add(result, result.folder_count, 1);
    } else Add(result, result.file_count, 1);
    if (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        SkipLink(result);
        return;
    }
    if (!directory) {
        if (!Cancelled(result, cancelled)) FileSize(metadata.get(), result);
        return;
    }
    if (depth >= kMaxDepth) {
        Unreadable(result, ERROR_DIRECTORY);
        return;
    }
    if (Cancelled(result, cancelled)) return;
    Handle enumeration = parent == INVALID_HANDLE_VALUE ?
        OpenSelection(path, result, cancelled, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY) :
        OpenRelative(parent, relative_name, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY);
    if (!enumeration) {
        if (parent != INVALID_HANDLE_VALUE) Unreadable(result, GetLastError());
        return;
    }
    FILE_BASIC_INFO enumeration_basic{};
    if (!SameObject(metadata.get(), enumeration.get()) || !BasicInfo(enumeration.get(), enumeration_basic) ||
        (enumeration_basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        Unreadable(result, ERROR_RETRY);
        return;
    }
    // Heap storage keeps the bounded recursive walk from consuming large stack frames.
    auto buffer = std::make_unique<BYTE[]>(16384);
    bool restart = true;
    for (;;) {
        if (Cancelled(result, cancelled)) return;
        if (!GetFileInformationByHandleEx(enumeration.get(),
            restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo, buffer.get(), 16384)) {
            const DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_FILES) Unreadable(result, error);
            else {
                FILE_BASIC_INFO after{};
                if (!BasicInfo(enumeration.get(), after)) Unreadable(result, GetLastError());
                else if (after.LastWriteTime.QuadPart != enumeration_basic.LastWriteTime.QuadPart ||
                    after.FileAttributes != enumeration_basic.FileAttributes) Unreadable(result, ERROR_RETRY);
                if (selected_root && !Cancelled(result, cancelled)) {
                    Handle current = OpenSelection(path, result, cancelled);
                    if (current && !SameObject(metadata.get(), current.get())) Unreadable(result, ERROR_RETRY);
                }
            }
            return;
        }
        restart = false;
        size_t offset = 0;
        for (;;) {
            if (Cancelled(result, cancelled)) return;
            constexpr size_t header = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
            if (offset > 16384 - header) {
                Unreadable(result, ERROR_INVALID_DATA);
                return;
            }
            const auto* item = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.get() + offset);
            if (item->FileNameLength % sizeof(wchar_t) || item->FileNameLength > 16384 - offset - header) {
                Unreadable(result, ERROR_INVALID_DATA);
                return;
            }
            const std::wstring name(item->FileName, item->FileNameLength / sizeof(wchar_t));
            if (name != L"." && name != L"..") {
                if (name.empty() || name.find_first_of(L"\\/\0", 0, 3) != std::wstring::npos) {
                    Unreadable(result, ERROR_INVALID_DATA);
                    return;
                }
                Walk(path + (path.back() == L'\\' ? L"" : L"\\") + name,
                    result, cancelled, depth + 1, false, enumeration.get(), name);
                if (Cancelled(result, cancelled)) return;
            }
            if (!item->NextEntryOffset) break;
            if (item->NextEntryOffset < header || item->NextEntryOffset > 16384 - offset) {
                Unreadable(result, ERROR_INVALID_DATA);
                return;
            }
            offset += item->NextEntryOffset;
        }
    }
}

}

ItemProperties ReadItemProperties(const std::vector<std::wstring>& paths, const std::atomic_bool& cancelled) {
    ItemProperties result;
    if (Cancelled(result, cancelled)) return result;
    std::vector<Root> roots;
    bool unknown = false;
    for (const auto& input : paths) {
        if (Cancelled(result, cancelled)) return result;
        std::wstring path = Normalize(input);
        if (path.empty()) {
            Unreadable(result, GetLastError());
            unknown = true;
            continue;
        }
        if (std::any_of(roots.begin(), roots.end(), [&](const Root& root) { return SamePath(root.path, path); })) continue;
        Root root{std::move(path)};
        Handle handle = OpenSelection(root.path, result, cancelled);
        if (handle && BasicInfo(handle.get(), root.basic)) root.known = true;
        else if (handle) Unreadable(result, GetLastError());
        unknown |= !root.known;
        roots.push_back(std::move(root));
    }
    if (roots.empty()) {
        if (paths.empty()) FirstError(result, ERROR_INVALID_PARAMETER);
        result.readonly_state = result.hidden_state = 2;
        return result;
    }
    result.location = DisplayPath(Parent(roots.front().path));
    int readonly_state = -1, hidden_state = -1;
    for (const auto& root : roots) {
        if (!SamePath(result.location, DisplayPath(Parent(root.path)))) result.location.clear();
        if (!root.known) continue;
        const std::wstring type = root.basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY ? L"folder" : L"file";
        if (result.type.empty()) result.type = type;
        else if (result.type != type) result.type = L"mixed";
        const int readonly = (root.basic.FileAttributes & FILE_ATTRIBUTE_READONLY) ? 1 : 0;
        const int hidden = (root.basic.FileAttributes & FILE_ATTRIBUTE_HIDDEN) ? 1 : 0;
        readonly_state = readonly_state == -1 || readonly_state == readonly ? readonly : 2;
        hidden_state = hidden_state == -1 || hidden_state == hidden ? hidden : 2;
    }
    result.readonly_state = unknown ? 2 : readonly_state;
    result.hidden_state = unknown ? 2 : hidden_state;
    if (unknown) result.type = L"mixed";
    if (roots.size() == 1 && !unknown) {
        const Root& root = roots.front();
        const size_t root_length = RootLength(root.path);
        result.name = root.path.size() == root_length ? DisplayPath(root.path) : root.path.substr(root.path.find_last_of(L'\\') + 1);
        result.created = Time(root.basic.CreationTime);
        result.modified = Time(root.basic.LastWriteTime);
        result.accessed = Time(root.basic.LastAccessTime);
        result.has_times = true;
    }
    for (const auto& root : roots) {
        if (Cancelled(result, cancelled)) break;
        if (!root.known) continue;
        const bool covered = std::any_of(roots.begin(), roots.end(), [&](const Root& candidate) {
            return candidate.known && (candidate.basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(candidate.basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && Descendant(root.path, candidate.path);
        });
        if (covered) continue;
        Walk(root.path, result, cancelled, 0, true);
    }
    return result;
}

}
