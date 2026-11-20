#include "address_completion.h"
#include "../common/path_utils.h"
#include "../fs/fs_enum.h"
#include <algorithm>
#include <utility>

namespace pulse {
namespace {

bool DrivePath(const std::wstring& path) {
    return path.size() >= 2 && path[1] == L':' &&
        ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'));
}

bool NetworkPath(const std::wstring& path) {
    return path.starts_with(L"\\\\") && !path.starts_with(L"\\\\?\\") &&
        !path.starts_with(L"\\\\.\\");
}

std::wstring CombineAddressPath(std::wstring text, const std::wstring& base) {
    std::replace(text.begin(), text.end(), L'/', L'\\');
    if (text.starts_with(L"\\\\?\\")) {
        text = path::StripExtendedPathPrefix(text);
        if (!DrivePath(text) && !NetworkPath(text)) return {};
    }
    if (text.empty() || fs::IsVirtualPath(text) || text.find(L'\0') != std::wstring::npos) return {};
    if (DrivePath(text) || NetworkPath(text)) return text;
    if (text.starts_with(L"\\\\") || text.find(L':') != std::wstring::npos ||
        base.empty() || fs::IsVirtualPath(base)) return {};
    const std::wstring root = path::StripExtendedPathPrefix(base);
    if (!DrivePath(root) && !NetworkPath(root)) return {};
    if (text.front() == L'\\') {
        if (DrivePath(root)) return root.substr(0, 2) + text;
        const auto server_end = root.find(L'\\', 2);
        if (server_end == std::wstring::npos || server_end + 1 == root.size()) return {};
        return root.substr(0, root.find(L'\\', server_end + 1)) + text;
    }
    return root + (root.ends_with(L'\\') ? L"" : L"\\") + text;
}

} // namespace

std::wstring ResolveAddressFilesystemPath(std::wstring text, const std::wstring& base) {
    // A typed device namespace is not a regular file target.
    if (text.find(L'\0') != std::wstring::npos) return {};
    std::replace(text.begin(), text.end(), L'/', L'\\');
    if (text.starts_with(L"\\\\?\\")) {
        const auto plain = path::StripExtendedPathPrefix(text);
        return DrivePath(plain) || NetworkPath(plain) ? fs::NormalizePath(std::move(text)) : std::wstring{};
    }
    text = CombineAddressPath(std::move(text), base);
    return text.empty() ? std::wstring{} : fs::NormalizePath(std::move(text));
}

bool ParseAddressCompletion(std::wstring text, const std::wstring& base,
                            AddressCompletionQuery& query) {
    const auto begin = text.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) return false;
    text.erase(0, begin);
    if (text.front() == L'"') {
        text.erase(0, 1);
        if (text.ends_with(L'"')) text.pop_back();
    }
    if (text.empty() || text.front() == L'%') return false;
    text = CombineAddressPath(std::move(text), base);
    if (text.empty() || text.find_first_of(L"*?\"") != std::wstring::npos) return false;
    const auto cut = text.find_last_of(L'\\');
    if (cut == std::wstring::npos || (DrivePath(text) && (text.size() < 3 || text[2] != L'\\')) ||
        (NetworkPath(text) && (cut <= 2 || cut == text.find(L'\\', 2)))) return false;
    auto parent = fs::NormalizePath(text.substr(0, cut + 1));
    if (parent.empty()) return false;
    query = {std::move(parent), text.substr(cut + 1)};
    return true;
}

std::vector<AddressCompletionEntry> EnumerateAddressCompletion(
    const AddressCompletionQuery& query, const std::atomic_bool& cancelled) {
    std::vector<AddressCompletionEntry> paths;
    if (cancelled) return paths;
    std::wstring parent = query.parent;
    if (!parent.ends_with(L'\\')) parent += L'\\';
    WIN32_FIND_DATAW data{};
    const HANDLE handle = FindFirstFileExW((parent + query.prefix + L"*").c_str(),
        FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (handle == INVALID_HANDLE_VALUE) return paths;
    struct FindHandle {
        HANDLE value;
        ~FindHandle() { FindClose(value); }
    } find{handle};
    do {
        if (cancelled) break;
        const std::wstring_view name(data.cFileName);
        if (name == L"." || name == L".." || name.size() < query.prefix.size() ||
            !path::EqualInsensitive(name.substr(0, query.prefix.size()), query.prefix)) continue;
        auto candidate = parent + data.cFileName;
        const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        paths.push_back({std::wstring(name), path::StripExtendedPathPrefix(candidate), directory});
        if (paths.size() == 32) break;
    } while (FindNextFileW(find.value, &data));
    std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) {
        return CompareStringOrdinal(left.name.c_str(), -1, right.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    if (paths.size() > 6) paths.resize(6);
    return paths;
}

} // namespace pulse
