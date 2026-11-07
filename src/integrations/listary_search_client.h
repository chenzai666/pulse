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
};

struct ListarySearchConfig {
    bool enabled = false;
    uint16_t port = 38431;
};

ListarySearchConfig ReadListarySearchConfig();
// Simple names only: match quality first, program/shortcut preference within
// the same matching tier, with engine order retained for equal ranks.
void RankListarySearchResults(const std::wstring& query, std::vector<ListarySearchItem>& items);
// Call from a worker. This function returns data only and never opens a result.
ListarySearchResult SearchListary(const ListarySearchConfig& config, const std::wstring& query,
                                  bool folders_only, const std::atomic<bool>& cancelled);

// Pure protocol boundaries shared with the focused test executable.
namespace detail {
ListarySearchConfig ParseListarySearchConfig(std::string_view utf8);
std::optional<std::string> BuildListarySearchBody(const std::wstring& query, bool folders_only);
ListarySearchResult ParseListarySearchResponse(std::string_view utf8, bool folders_only);
} // namespace detail
} // namespace pulse::integrations
