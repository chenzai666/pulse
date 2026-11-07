#include "listary_search_client.h"

#include "../common/json_utils.h"
#include "../common/utf8_file.h"
#include "../ui/folder_picker_listary_protocol.h"

#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <array>
#include <unordered_map>

namespace pulse::integrations {
namespace {

constexpr size_t kMaximumBytes = 1024 * 1024;
constexpr size_t kMaximumQuery = 4096;
constexpr unsigned kMaximumResults = 50;
constexpr ULONGLONG kBudgetMs = 2000;
using Members = std::unordered_map<std::wstring, std::wstring>;

struct HttpHandle {
    HINTERNET value = nullptr;
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
    explicit HttpHandle(HINTERNET handle) : value(handle) {}
};

struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    explicit FileHandle(HANDLE handle) : value(handle) {}
};

bool Decode(std::string_view utf8, std::wstring& text) {
    if (utf8.empty() || utf8.size() > kMaximumBytes) return false;
    const std::vector<uint8_t> bytes(utf8.begin(), utf8.end());
    return DecodeUtf8Bytes(bytes, text) && json::ValidConfigObject(text);
}

bool Object(const std::wstring& text, Members& members) {
    if (!json::ValidConfigObject(text)) return false;
    bool unique = true;
    return json::ForEachMember(text, [&](const std::wstring& key, const std::wstring& value) {
        if (!members.emplace(key, value).second) unique = false;
    }) && unique;
}

bool Unsigned(const std::wstring& raw, uint64_t& value) {
    if (raw.empty()) return false;
    value = 0;
    for (const auto c : raw) {
        if (c < L'0' || c > L'9') return false;
        const auto digit = static_cast<uint64_t>(c - L'0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}

bool String(const std::wstring& raw, std::wstring& value) {
    size_t position = 0;
    return json::ConfigSyntax(raw, false).DecodeString(position, value) && position == raw.size();
}

bool Name(const std::wstring& name) {
    if (name.empty() || name.size() > 32767) return false;
    for (const auto c : name) if (c < 32 || (c >= 0x7F && c <= 0x9F) ||
        c == L'\\' || c == L'/' || c == L':' || c == L'"' || c == L'<' || c == L'>' ||
        c == L'|' || c == L'*' || c == L'?') return false;
    return true;
}

ListarySearchResult Error(ListarySearchError error) { return {error, {}}; }

ListarySearchError Interrupted(const std::atomic<bool>& cancelled, ULONGLONG deadline) {
    if (cancelled.load()) return ListarySearchError::Cancelled;
    return GetTickCount64() >= deadline ? ListarySearchError::Timeout : ListarySearchError::None;
}

bool Timeouts(HINTERNET request, ULONGLONG deadline) {
    const auto now = GetTickCount64();
    if (now >= deadline) return false;
    // Short setup waits leave the rest of the total budget for Listary's
    // search. The absolute deadline is recomputed before every response read.
    const auto remaining = static_cast<int>(deadline - now);
    const auto setup = (std::min)(remaining, 250);
    return WinHttpSetTimeouts(request, setup, setup, setup, remaining) != FALSE;
}

ListarySearchResult TransportError(const std::atomic<bool>& cancelled, ULONGLONG deadline, DWORD error) {
    const auto stopped = Interrupted(cancelled, deadline);
    if (stopped != ListarySearchError::None) return Error(stopped);
    return Error(error == ERROR_WINHTTP_TIMEOUT ? ListarySearchError::Timeout : ListarySearchError::Unavailable);
}

bool SimpleNameQuery(std::wstring_view query) {
    if (query.empty() || query.size() > kMaximumQuery || query.front() == L'-') return false;
    std::vector<WORD> character_types(query.size());
    if (!GetStringTypeW(CT_CTYPE1, query.data(), static_cast<int>(query.size()), character_types.data()))
        return false;
    bool has_name = false;
    for (size_t i = 0; i < query.size(); ++i) {
        const auto c = query[i];
        if (character_types[i] & (C1_SPACE | C1_CNTRL)) return false;
        if (c < 128 && !((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
            (c >= L'0' && c <= L'9') || c == L'.' || c == L'-' || c == L'_')) return false;
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (++i >= query.size() || query[i] < 0xDC00 || query[i] > 0xDFFF) return false;
        } else if (c >= 0xDC00 && c <= 0xDFFF) return false;
        if (c != L'.' && c != L'-' && c != L'_') has_name = true;
    }
    return has_name;
}

bool EqualName(std::wstring_view left, std::wstring_view right) {
    return left.size() == right.size() && (left.empty() ||
        CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL);
}

std::wstring_view Leaf(std::wstring_view path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == path.npos ? path : path.substr(slash + 1);
}

bool ProgramResult(const ListarySearchItem& item) {
    if (item.directory) return false;
    const auto name = item.path.empty() ? std::wstring_view(item.name) : Leaf(item.path);
    const auto dot = name.find_last_of(L'.');
    if (dot == name.npos) return false;
    const auto extension = name.substr(dot);
    for (const auto* candidate : {L".exe", L".com", L".bat", L".cmd", L".lnk", L".appref-ms", L".msc"})
        if (EqualName(extension, candidate)) return true;
    return false;
}

unsigned MatchTier(std::wstring_view query, const ListarySearchItem& item) {
    const auto name = item.name.empty() ? Leaf(item.path) : std::wstring_view(item.name);
    if (name.size() > 32767) return 0;
    if (EqualName(name, query)) return 4;
    if (!item.directory && query.find(L'.') == query.npos) {
        const auto dot = name.find_last_of(L'.');
        if (dot != name.npos && EqualName(name.substr(0, dot), query)) return 4;
    }
    if (query.size() <= name.size()) {
        if (EqualName(name.substr(0, query.size()), query)) return 3;
        if (FindStringOrdinal(FIND_FROMSTART, name.data(), static_cast<int>(name.size()),
            query.data(), static_cast<int>(query.size()), TRUE) >= 0) return 2;
    }
    size_t matched = 0;
    for (size_t i = 0; i < name.size() && matched < query.size(); ++i) {
        size_t units = 1;
        if (query[matched] >= 0xD800 && query[matched] <= 0xDBFF) units = 2;
        if (i + units <= name.size() && EqualName(name.substr(i, units), query.substr(matched, units))) {
            matched += units;
            i += units - 1;
        }
    }
    return matched == query.size() ? 1 : 0;
}

} // namespace

void RankListarySearchResults(const std::wstring& query, std::vector<ListarySearchItem>& items) {
    if (!SimpleNameQuery(query) || items.size() < 2) return;
    // Extensionless names normally express app-launch intent. Only an exact
    // stem or prefix program match crosses relevance tiers; fuzzy programs do
    // not outrank more relevant documents. No Shell resolution is performed.
    const bool extensionless = query.find(L'.') == query.npos;
    struct RankedItem { unsigned rank; size_t index; };
    std::vector<RankedItem> ranked;
    ranked.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        const auto tier = MatchTier(query, item);
        const bool program = tier && ProgramResult(item);
        const unsigned rank = tier * 2 + (program ? 1u : 0u) +
            (extensionless && program && tier >= 3 ? 10u : 0u);
        ranked.push_back({rank, i});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        return left.rank > right.rank;
    });
    std::vector<ListarySearchItem> sorted;
    sorted.reserve(items.size());
    for (const auto& entry : ranked) sorted.push_back(std::move(items[entry.index]));
    items.swap(sorted);
}

namespace detail {

ListarySearchConfig ParseListarySearchConfig(std::string_view utf8) {
    ListarySearchConfig config;
    std::wstring text;
    Members root, settings;
    if (!Decode(utf8, text) || !Object(text, root) || !root.contains(L"Settings") ||
        !Object(root.at(L"Settings"), settings)) return config;
    const auto enabled = settings.find(L"General.HttpApi.Enabled");
    if (enabled == settings.end() || enabled->second != L"true") return config;
    if (const auto port = settings.find(L"General.HttpApi.Port"); port != settings.end()) {
        uint64_t number = 0;
        if (!Unsigned(port->second, number) || !number || number > UINT16_MAX) return config;
        config.port = static_cast<uint16_t>(number);
    }
    config.enabled = true;
    return config;
}

std::optional<std::string> BuildListarySearchBody(const std::wstring& query, bool folders_only) {
    if (query.empty() || query.size() > kMaximumQuery || query.find(L'\0') != query.npos) return std::nullopt;
    std::vector<uint8_t> bytes;
    if (!EncodeUtf8Bytes(query, bytes)) return std::nullopt;
    std::wstring body = L"{\"query\":\"";
    json::Escape(query, body);
    body += L"\",\"limit\":50,\"offset\":0";
    if (folders_only) body += L",\"types\":[\"folder\"]";
    body += L"}";
    if (!EncodeUtf8Bytes(body, bytes)) return std::nullopt;
    return std::string(bytes.begin(), bytes.end());
}

ListarySearchResult ParseListarySearchResponse(std::string_view utf8, bool folders_only) {
    const auto invalid = [] { return Error(ListarySearchError::InvalidResponse); };
    std::wstring text;
    Members root, data;
    if (!Decode(utf8, text) || !Object(text, root) || !root.contains(L"ok") ||
        !root.contains(L"data")) return invalid();
    if (root.at(L"ok") == L"false") {
        std::wstring error;
        if (root.contains(L"error") && String(root.at(L"error"), error) && error == L"SEARCH_UNAVAILABLE")
            return Error(ListarySearchError::Unavailable);
        return invalid();
    }
    if (root.at(L"ok") != L"true" || !Object(root.at(L"data"), data)) return invalid();
    uint64_t count = 0, total = 0, offset = 0, limit = 0;
    if (!data.contains(L"count") || !Unsigned(data.at(L"count"), count) || count > kMaximumResults ||
        !data.contains(L"total") || !Unsigned(data.at(L"total"), total) || total < count ||
        !data.contains(L"offset") || !Unsigned(data.at(L"offset"), offset) || offset != 0 ||
        !data.contains(L"limit") || !Unsigned(data.at(L"limit"), limit) || !limit || limit > 100 || limit < count ||
        !data.contains(L"results") || !json::ValidConfigArray(data.at(L"results"))) return invalid();
    ListarySearchResult result;
    bool valid = true;
    size_t actual = 0;
    const bool array = json::ForEachElement(data.at(L"results"), [&](const std::wstring& raw) {
        if (++actual > kMaximumResults || !valid) { valid = false; return; }
        Members item;
        ListarySearchItem parsed;
        std::wstring type;
        if (!Object(raw, item) || !item.contains(L"path") || !String(item.at(L"path"), parsed.path) ||
            parsed.path.size() > 32767 || !ui::IsPickerListaryPath(parsed.path) ||
            !item.contains(L"name") || !String(item.at(L"name"), parsed.name) || !Name(parsed.name) ||
            !item.contains(L"type") || !String(item.at(L"type"), type) ||
            (type != L"file" && type != L"folder") || (folders_only && type != L"folder") ||
            (type == L"file" && parsed.path.back() == L'\\')) { valid = false; return; }
        parsed.directory = type == L"folder";
        result.items.push_back(std::move(parsed));
    });
    return array && valid && actual == count ? result : invalid();
}

} // namespace detail

ListarySearchConfig ReadListarySearchConfig() {
    // Read only the current user's profile. A missing/invalid opt-in stays off.
    const DWORD required = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (!required || required > 32768) return {};
    std::wstring directory(required, L'\0');
    const DWORD chars = GetEnvironmentVariableW(L"APPDATA", directory.data(), required);
    if (!chars || chars >= required) return {};
    directory.resize(chars);
    if (!ui::IsPickerListaryPath(directory)) return {};
    const auto path = directory + L"\\Listary\\UserProfile\\Settings\\Preferences.json";
    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    std::string contents;
    bool read_ok = GetFileSizeEx(file.value, &size) && size.QuadPart > 0 &&
        static_cast<uint64_t>(size.QuadPart) <= kMaximumBytes;
    if (read_ok) {
        contents.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        read_ok = ReadFile(file.value, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr) &&
            read == contents.size();
        char extra = 0;
        DWORD extra_read = 0;
        if (read_ok) read_ok = ReadFile(file.value, &extra, 1, &extra_read, nullptr) && !extra_read;
    }
    return read_ok ? detail::ParseListarySearchConfig(contents) : ListarySearchConfig{};
}

ListarySearchResult SearchListary(const ListarySearchConfig& config, const std::wstring& query,
                                  bool folders_only, const std::atomic<bool>& cancelled) {
    if (cancelled.load()) return Error(ListarySearchError::Cancelled);
    if (!config.enabled) return Error(ListarySearchError::Disabled);
    if (!config.port) return Error(ListarySearchError::Unavailable);
    const auto body = detail::BuildListarySearchBody(query, folders_only);
    if (!body) return Error(ListarySearchError::InvalidResponse);
    const auto deadline = GetTickCount64() + kBudgetMs;
    HttpHandle session(WinHttpOpen(L"Pulse Listary Search/1", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.value) return TransportError(cancelled, deadline, GetLastError());
    if (!Timeouts(session.value, deadline)) return TransportError(cancelled, deadline, GetLastError());
    HttpHandle connection(WinHttpConnect(session.value, L"127.0.0.1", config.port, 0));
    if (!connection.value) return TransportError(cancelled, deadline, GetLastError());
    HttpHandle request(WinHttpOpenRequest(connection.value, L"POST", L"/api/v1/search", L"HTTP/1.1",
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!request.value) return TransportError(cancelled, deadline, GetLastError());
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION |
        WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_KEEP_ALIVE;
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)))
        return TransportError(cancelled, deadline, GetLastError());
    auto stopped = Interrupted(cancelled, deadline);
    if (stopped != ListarySearchError::None) return Error(stopped);
    constexpr wchar_t headers[] = L"Content-Type: application/json\r\nAccept: application/json\r\nConnection: close\r\n";
    if (!Timeouts(request.value, deadline) || !WinHttpSendRequest(request.value, headers, static_cast<DWORD>(-1),
            const_cast<char*>(body->data()), static_cast<DWORD>(body->size()), static_cast<DWORD>(body->size()), 0))
        return TransportError(cancelled, deadline, GetLastError());
    stopped = Interrupted(cancelled, deadline);
    if (stopped != ListarySearchError::None) return Error(stopped);
    if (!Timeouts(request.value, deadline) || !WinHttpReceiveResponse(request.value, nullptr))
        return TransportError(cancelled, deadline, GetLastError());
    stopped = Interrupted(cancelled, deadline);
    if (stopped != ListarySearchError::None) return Error(stopped);
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return Error(ListarySearchError::InvalidResponse);
    if (status == 503 || status == 429) return Error(ListarySearchError::Unavailable);
    if (status != 200) return Error(ListarySearchError::InvalidResponse);
    wchar_t content_type[128]{};
    size = sizeof(content_type);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
        content_type, &size, WINHTTP_NO_HEADER_INDEX)) return Error(ListarySearchError::InvalidResponse);
    std::wstring media(content_type);
    if (const auto semicolon = media.find(L';'); semicolon != media.npos) media.resize(semicolon);
    while (!media.empty() && (media.back() == L' ' || media.back() == L'\t')) media.pop_back();
    if (_wcsicmp(media.c_str(), L"application/json") != 0) return Error(ListarySearchError::InvalidResponse);
    DWORD length = 0;
    size = sizeof(length);
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &length, &size, WINHTTP_NO_HEADER_INDEX) && length > kMaximumBytes)
        return Error(ListarySearchError::InvalidResponse);
    std::string response;
    std::array<char, 8192> buffer{};
    for (;;) {
        stopped = Interrupted(cancelled, deadline);
        if (stopped != ListarySearchError::None) return Error(stopped);
        DWORD read = 0;
        if (!Timeouts(request.value, deadline) ||
            !WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read))
            return TransportError(cancelled, deadline, GetLastError());
        if (read > kMaximumBytes - response.size()) return Error(ListarySearchError::InvalidResponse);
        if (!read) break;
        response.append(buffer.data(), read);
    }
    stopped = Interrupted(cancelled, deadline);
    if (stopped != ListarySearchError::None) return Error(stopped);
    auto result = detail::ParseListarySearchResponse(response, folders_only);
    if (result.error == ListarySearchError::None) RankListarySearchResults(query, result.items);
    stopped = Interrupted(cancelled, deadline);
    return stopped == ListarySearchError::None ? result : Error(stopped);
}

} // namespace pulse::integrations
