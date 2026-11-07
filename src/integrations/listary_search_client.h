#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::integrations {

enum class ListarySearchError { None, Unavailable, Disabled, Timeout, InvalidResponse, Cancelled };

struct ListarySearchItem {
    std::wstring path, name;
    bool directory = false;
};

struct ListarySearchResult {
    ListarySearchError error = ListarySearchError::None;
    std::vector<ListarySearchItem> items;
    uint64_t total = 0, offset = 0;
    bool has_more = false;
};

struct ListarySearchConfig {
    bool enabled = false;
    uint16_t port = 38431;
};

ListarySearchConfig ReadListarySearchConfig();
// Simple names only: extensionless exact/prefix programs first; explicit
// filenames retain relevance priority. Equal ranks retain engine ordering.
void RankListarySearchResults(const std::wstring& query, std::vector<ListarySearchItem>& items);
// Call from a worker. This function returns data only and never opens a result.
ListarySearchResult SearchListary(const ListarySearchConfig& config, const std::wstring& query,
                                  bool folders_only, const std::atomic<bool>& cancelled, uint64_t offset = 0);

// Pure protocol boundaries shared with the focused test executable.
namespace detail {
std::wstring ListaryEffectiveQuery(const std::wstring& query);
ListarySearchConfig ParseListarySearchConfig(std::string_view utf8);
std::optional<std::string> BuildListarySearchBody(const std::wstring& query, bool folders_only, uint64_t offset = 0);
ListarySearchResult ParseListarySearchResponse(std::string_view utf8, bool folders_only, uint64_t offset = 0);
} // namespace detail
} // namespace pulse::integrations
