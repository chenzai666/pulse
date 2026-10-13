#pragma once
#include <windows.h>
#include <oleidl.h>
#include <algorithm>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

namespace pulse::app {

// Navigation drops never report MOVE: a source must keep the original data.
inline DWORD TabNavigationDropEffect(DWORD allowed) {
    if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
    if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    return DROPEFFECT_NONE;
}

// Path text only; safe before queueing work. Reject device namespaces/ADS/URIs and
// resolve dot segments before applying the extended Win32 path prefix.
inline std::wstring NormalizeDroppedNavigationPath(std::wstring input) {
    if (input.empty() || input.size() >= 32768) return {};
    std::replace(input.begin(), input.end(), L'/', L'\\');
    if (_wcsnicmp(input.c_str(), L"\\\\?\\UNC\\", 8) == 0) input = L"\\\\" + input.substr(8);
    else if (input.starts_with(L"\\\\?\\")) input.erase(0, 4);
    const bool drive = input.size() >= 3 &&
        ((input[0] >= L'A' && input[0] <= L'Z') || (input[0] >= L'a' && input[0] <= L'z')) &&
        input[1] == L':' && input[2] == L'\\';
    const bool unc = input.starts_with(L"\\\\") && input.size() > 4 && input[2] != L'.' && input[2] != L'?';
    if (!drive && !unc) return {};
    for (size_t i = 0; i < input.size(); ++i) {
        const auto c = input[i];
        if (c < 32 || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c == L'*' || c == L'?' ||
            (c == L':' && !(drive && i == 1))) return {};
    }
    if (unc) {
        const auto server = input.find(L'\\', 2);
        if (server == std::wstring::npos || server == 2 || server + 1 >= input.size()) return {};
        const auto share_end = input.find(L'\\', server + 1);
        const auto share = input.substr(server + 1, share_end - server - 1);
        if (share.empty() || share == L"." || share == L"..") return {};
    }
    wchar_t full[32768]{};
    const DWORD chars = GetFullPathNameW(input.c_str(), ARRAYSIZE(full), full, nullptr);
    if (!chars || chars >= ARRAYSIZE(full)) return {};
    std::wstring path(full, chars);
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
}

struct DroppedNavigationTarget {
    std::wstring folder;
    std::wstring select_name;
};

// `source` has already been normalized; attributes come from the worker.
inline DroppedNavigationTarget PlanDroppedNavigation(const std::wstring& source, DWORD attributes) {
    if (source.empty() || attributes == INVALID_FILE_ATTRIBUTES) return {};
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return {source, {}};
    const auto slash = source.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash + 1 >= source.size()) return {};
    return {source.substr(0, slash + 1), source.substr(slash + 1)};
}

struct DroppedNavigationGroup {
    std::wstring folder;
    std::vector<std::wstring> names;
    size_t source_count = 0;
};

inline std::vector<DroppedNavigationGroup> GroupDroppedNavigation(
    const std::vector<DroppedNavigationTarget>& targets) {
    struct FolderLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const {
            return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        }
    };
    std::vector<DroppedNavigationGroup> groups;
    std::map<std::wstring, size_t, FolderLess> indices;
    for (const auto& target : targets) {
        if (target.folder.empty()) continue;
        auto key = target.folder;
        while (key.size() > 7 && key.back() == L'\\') key.pop_back();
        auto found = indices.find(key);
        if (found == indices.end()) {
            found = indices.emplace(std::move(key), groups.size()).first;
            groups.push_back({target.folder, {}, 0});
        }
        auto& group = groups[found->second];
        ++group.source_count;
        if (!target.select_name.empty() && std::none_of(group.names.begin(), group.names.end(), [&](const auto& name) {
                return CompareStringOrdinal(name.c_str(), -1, target.select_name.c_str(), -1, TRUE) == CSTR_EQUAL;
            })) group.names.push_back(target.select_name);
    }
    return groups;
}

} // namespace pulse::app
