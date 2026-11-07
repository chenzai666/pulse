#include "../common/windows_compat.h"
#include "folder_picker_listary_search.h"

#include "../common/localization.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <limits>
#include <set>
#include <thread>
#include <utility>

namespace pulse::ui {
namespace {
constexpr size_t kPageSize = 50;
constexpr float kRowDip = 44.0f;
bool ContainsSearchRect(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

std::wstring Status(integrations::ListarySearchError error, bool empty_query,
                    bool no_results) {
    using integrations::ListarySearchError;
    switch (error) {
    case ListarySearchError::Disabled:
        return l10n::Pick(L"请在 Listary 设置 → HTTP API 中开启接口", L"Enable HTTP API in Listary settings");
    case ListarySearchError::Unavailable:
        return l10n::Pick(L"Listary 未运行或接口不可用", L"Listary is not running or unavailable");
    case ListarySearchError::Timeout:
        return l10n::Pick(L"Listary 搜索超时，请重试", L"Listary search timed out; try again");
    case ListarySearchError::InvalidResponse:
        return l10n::Pick(L"Listary 返回内容无效", L"Invalid Listary response");
    case ListarySearchError::Cancelled:
        return {};
    default:
        if (empty_query) return l10n::Pick(L"Listary 全盘搜索", L"Search all files with Listary");
        return no_results ? l10n::Pick(L"未找到结果", L"No results")
                          : l10n::Pick(L"↑↓ 选择 · Enter 定位", L"↑↓ Select · Enter to locate");
    }
}

} // namespace

std::wstring ListaryResultName(const integrations::ListarySearchItem& item) {
    if (!item.name.empty()) return item.name;
    std::wstring_view path = item.path;
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.remove_suffix(1);
    const size_t slash = path.find_last_of(L"\\/");
    return std::wstring(slash == std::wstring_view::npos || slash + 1 == path.size()
        ? path : path.substr(slash + 1));
}

std::wstring ListaryResultParent(std::wstring_view path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.remove_suffix(1);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos) return {};
    if (path.size() >= 3 && path[1] == L':' && slash == 2) return std::wstring(path.substr(0, 3));
    if (path.starts_with(L"\\\\") || path.starts_with(L"//")) {
        const size_t server_end = path.find_first_of(L"\\/", 2);
        // A UNC share root has no higher filesystem folder to display.
        if (server_end == slash) return std::wstring(path);
    }
    return std::wstring(path.substr(0, slash));
}

std::vector<ListaryNameMatch> ListaryNameMatches(std::wstring_view name,
    std::wstring_view query, std::wstring_view shown) {
    auto high = [](wchar_t ch) { return ch >= 0xd800 && ch <= 0xdbff; };
    auto low = [](wchar_t ch) { return ch >= 0xdc00 && ch <= 0xdfff; };
    auto valid = [&](std::wstring_view text) {
        for (size_t i = 0; i < text.size(); ++i) {
            if (high(text[i])) {
                if (++i >= text.size() || !low(text[i])) return false;
            } else if (low(text[i])) return false;
        }
        return true;
    };
    if (name.empty() || query.empty() || !valid(name) || !valid(query)) return {};
    size_t prefix = 0;
    while (prefix < name.size() && prefix < shown.size() && name[prefix] == shown[prefix]) ++prefix;
    if (prefix && high(name[prefix - 1])) --prefix;
    std::vector<bool> mask(prefix, false);
    for (size_t at = 0; at < query.size();) {
        while (at < query.size() && std::iswspace(query[at])) ++at;
        std::wstring term;
        bool quoted = false;
        while (at < query.size()) {
            const wchar_t ch = query[at];
            if (ch == L'"') { quoted = !quoted; ++at; continue; }
            if (!quoted && std::iswspace(ch)) break;
            term += ch;
            ++at;
        }
        if (term.empty() || term.front() == L'-' || term.find(L':') != std::wstring::npos) continue;
        auto letter = [](wchar_t ch) {
            return (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
        };
        auto digit = [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; };
        size_t letters = 0, digit_run = 0;
        bool compact_model = true, has_number = false;
        while (letters < term.size() && letter(term[letters])) ++letters;
        for (const auto ch : term) {
            if (!letter(ch) && !digit(ch)) { compact_model = false; break; }
            digit_run = digit(ch) ? digit_run + 1 : 0;
            if (digit_run >= 2) has_number = true;
        }
        compact_model = compact_model && letters >= 2 && has_number;
        if (compact_model) for (size_t start = 0; start < prefix; ++start) {
            if (start && (letter(name[start - 1]) || digit(name[start - 1]))) continue;
            size_t source = start;
            std::vector<size_t> matched;
            for (size_t query_at = 0; query_at < term.size(); ++query_at) {
                if (query_at) while (source < name.size() &&
                    (name[source] == L' ' || name[source] == L'-' || name[source] == L'_')) ++source;
                if (source >= name.size() || CompareStringOrdinal(name.data() + source, 1,
                        term.data() + query_at, 1, TRUE) != CSTR_EQUAL) break;
                matched.push_back(source++);
            }
            // Only the exact model characters are marked. A following order
            // number belongs to the rest of the name and is never consumed.
            if (matched.size() == term.size() &&
                (source == name.size() || (!letter(name[source]) && !digit(name[source]))))
                for (const auto pos : matched) if (pos < prefix) mask[pos] = true;
        }
        // Wildcard punctuation is syntax; only its literal portions are marked.
        for (size_t first = 0; first < term.size();) {
            while (first < term.size() && (term[first] == L'*' || term[first] == L'?')) ++first;
            const size_t end = term.find_first_of(L"*?", first);
            const auto literal = std::wstring_view(term).substr(first,
                end == std::wstring::npos ? end : end - first);
            if (!literal.empty()) for (size_t pos = 0; pos < prefix && literal.size() <= name.size() - pos; ++pos) {
                if (low(name[pos])) continue;
                if (CompareStringOrdinal(name.data() + pos, static_cast<int>(literal.size()),
                        literal.data(), static_cast<int>(literal.size()), TRUE) != CSTR_EQUAL) continue;
                const size_t finish = std::min(prefix, pos + literal.size());
                std::fill(mask.begin() + pos, mask.begin() + finish, true);
            }
            if (end == std::wstring::npos) break;
            first = end + 1;
        }
    }
    std::vector<ListaryNameMatch> ranges;
    for (size_t i = 0; i < mask.size();) {
        if (!mask[i]) { ++i; continue; }
        const size_t start = i;
        while (i < mask.size() && mask[i]) ++i;
        ranges.push_back({static_cast<UINT32>(start), static_cast<UINT32>(i - start)});
    }
    return ranges;
}

struct PickerListarySearch::Mailbox {
    struct Request {
        uint64_t generation = 0;
        std::wstring query;
        uint64_t offset = 0;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };
    struct Reply {
        uint64_t generation = 0;
        integrations::ListarySearchResult result;
    };
    std::mutex mutex;
    std::condition_variable ready;
    bool stopping = false;
    bool folders_only = false;
    std::optional<Request> pending;
    std::optional<Reply> reply;
    std::shared_ptr<std::atomic<bool>> active_cancel;

    void CancelLocked() {
        if (active_cancel) active_cancel->store(true);
        if (pending) pending->cancelled->store(true);
        pending.reset();
        reply.reset();
    }

    void Run() {
        for (;;) {
            Request request;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return stopping || pending.has_value(); });
                if (stopping) return;
                request = std::move(*pending);
                pending.reset();
                active_cancel = request.cancelled;
            }
            integrations::ListarySearchResult result;
            try {
                const auto config = integrations::ReadListarySearchConfig();
                result.error = config.enabled ? integrations::ListarySearchError::None
                                              : integrations::ListarySearchError::Disabled;
                if (config.enabled && !request.query.empty() && !request.cancelled->load())
                    result = integrations::SearchListary(config, request.query, folders_only,
                                                        *request.cancelled, request.offset);
            } catch (...) {
                result.error = integrations::ListarySearchError::Unavailable;
                result.items.clear();
            }
            {
                std::lock_guard lock(mutex);
                active_cancel.reset();
                if (stopping) return;
                if (!request.cancelled->load()) reply = Reply{request.generation, std::move(result)};
            }
        }
    }
};

PickerListarySearch::PickerListarySearch(bool folders_only)
    : mailbox_(std::make_shared<Mailbox>()),
      status_(l10n::Pick(L"正在检查 Listary…", L"Checking Listary…")) {
    mailbox_->folders_only = folders_only;
    // The mailbox owns all worker data. Closing the picker cancels the bounded
    // HTTP call without joining it on the UI thread; the worker then releases
    // its mailbox and exits, with no HWND access or posted heap payloads.
    Queue();
    std::thread([mailbox = mailbox_] { mailbox->Run(); }).detach();
}

PickerListarySearch::~PickerListarySearch() {
    {
        std::lock_guard lock(mailbox_->mutex);
        mailbox_->stopping = true;
        mailbox_->CancelLocked();
    }
    mailbox_->ready.notify_one();
}

void PickerListarySearch::SetQuery(std::wstring query) {
    if (query == query_) return;
    query_ = std::move(query);
    ++generation_;
    {
        std::lock_guard lock(mailbox_->mutex);
        mailbox_->CancelLocked();
    }
    items_.clear();
    total_ = next_offset_ = requested_offset_ = 0;
    has_more_ = page_loading_ = request_append_ = false;
    selected_ = -1;
    dismissed_ = false;
    due_ = GetTickCount64() + 220;
    status_ = query_.empty() ? l10n::Pick(L"Listary 全盘搜索", L"Search all files with Listary")
                             : l10n::Pick(L"正在搜索…", L"Searching…");
}

void PickerListarySearch::Queue(bool append) {
    ++generation_;
    requested_offset_ = append ? next_offset_ : 0;
    request_append_ = append;
    page_loading_ = true;
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard lock(mailbox_->mutex);
        mailbox_->CancelLocked();
        mailbox_->pending = Mailbox::Request{generation_, query_, requested_offset_, std::move(cancelled)};
    }
    mailbox_->ready.notify_one();
    due_ = 0;
}

void PickerListarySearch::SearchNow() {
    if (page_loading_ && !due_) return;
    items_.clear();
    selected_ = -1;
    has_more_ = false;
    total_ = next_offset_ = 0;
    if (!query_.empty()) status_ = l10n::Pick(L"正在搜索…", L"Searching…");
    Queue();
}

bool PickerListarySearch::Tick() {
    if (due_ && GetTickCount64() >= due_) Queue();
    std::optional<Mailbox::Reply> reply;
    {
        std::lock_guard lock(mailbox_->mutex);
        reply = std::move(mailbox_->reply);
        mailbox_->reply.reset();
    }
    if (!reply || reply->generation != generation_) return false;
    page_loading_ = false;
    auto& result = reply->result;
    if (result.error == integrations::ListarySearchError::None && !query_.empty()) {
        const uint64_t count = static_cast<uint64_t>(result.items.size());
        const bool overflow = requested_offset_ > (std::numeric_limits<uint64_t>::max)() - count;
        const uint64_t end = overflow ? 0 : requested_offset_ + count;
        if (result.offset != requested_offset_ || count > kPageSize || overflow ||
            end > result.total || (result.has_more && (!count || end >= result.total)) ||
            (!result.has_more && end != result.total) ||
            items_.size() > static_cast<size_t>((std::numeric_limits<int>::max)()) - result.items.size())
            result.error = integrations::ListarySearchError::InvalidResponse;
    }
    if (result.error != integrations::ListarySearchError::None) {
        if (!request_append_) { items_.clear(); selected_ = -1; has_more_ = false; }
        UpdatePageStatus(Status(result.error, query_.empty(), items_.empty()));
        return true;
    }
    // Path identities are case-insensitive on the Windows filesystem. Offset
    // always advances by the raw page size, including duplicate path entries.
    struct PathLess {
        bool operator()(const std::wstring& a, const std::wstring& b) const {
            return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
        }
    };
    std::set<std::wstring, PathLess> paths;
    for (const auto& item : items_) paths.insert(item.path);
    for (auto& item : result.items) if (paths.insert(item.path).second) items_.push_back(std::move(item));
    total_ = result.total;
    next_offset_ = result.offset + static_cast<uint64_t>(result.items.size());
    has_more_ = result.has_more;
    if (!request_append_) selected_ = items_.empty() ? -1 : 0;
    UpdatePageStatus();
    return true;
}

void PickerListarySearch::UpdatePageStatus(std::wstring error) {
    if (items_.empty()) {
        status_ = error.empty() ? Status(integrations::ListarySearchError::None, query_.empty(), true)
                                : std::move(error);
        return;
    }
    status_ = l10n::Pick(L"已加载 ", L"Loaded ") + std::to_wstring(items_.size()) +
        l10n::Pick(L" / 共 ", L" / Total ") + std::to_wstring(total_);
    if (!error.empty()) status_ += L" · " + error;
    if (page_loading_) status_ += l10n::Pick(L" · 正在加载…", L" · Loading…");
    else if (has_more_) status_ += l10n::Pick(L" · 继续向下或点击加载更多", L" · Down or click to load more");
    else status_ += l10n::Pick(L" · 全部已加载", L" · All results loaded");
}

void PickerListarySearch::LoadMore() {
    if (query_.empty() || !has_more_ || page_loading_ || !next_offset_) return;
    Queue(true);
    UpdatePageStatus();
}

bool PickerListarySearch::HitLoadMore(float x, float y) const {
    return has_more_ && !page_loading_ && x >= input_.left && x < input_.right &&
        y >= input_.bottom && y < band_.bottom;
}

void PickerListarySearch::SetFocused(bool focused) {
    focused_ = focused;
    if (focused) dismissed_ = false;
}

bool PickerListarySearch::Dismiss() {
    if (!PopupVisible()) return false;
    dismissed_ = true;
    return true;
}

bool PickerListarySearch::PopupVisible() const {
    return focused_ && !dismissed_ && !query_.empty() && !items_.empty();
}

void PickerListarySearch::MoveSelection(int direction) {
    if (items_.empty()) return;
    dismissed_ = false;
    const int count = static_cast<int>(items_.size());
    if (direction > 0 && selected_ >= count - 1) { LoadMore(); return; }
    const auto target = static_cast<int64_t>(selected_) + direction;
    selected_ = static_cast<int>(std::clamp(target, int64_t{0}, static_cast<int64_t>(count - 1)));
    if (direction > 0 && target >= count) LoadMore();
}

void PickerListarySearch::SelectRow(int index) {
    if (index >= 0 && index < static_cast<int>(items_.size())) selected_ = index;
}

std::optional<integrations::ListarySearchItem> PickerListarySearch::Selected() const {
    if (!PopupVisible() || selected_ < 0 || selected_ >= static_cast<int>(items_.size())) return {};
    return items_[static_cast<size_t>(selected_)];
}

void PickerListarySearch::Layout(float width, float footer_top, float content_top, float scale) {
    scale_ = scale;
    content_top_ = content_top;
    band_ = D2D1::RectF(0, footer_top - FooterDip() * scale, width, footer_top);
    input_ = D2D1::RectF(100.0f * scale, band_.top + 4.0f * scale,
                        width - 16.0f * scale, band_.bottom - 18.0f * scale);
}

D2D1_RECT_F PickerListarySearch::PanelBounds() const {
    const float max_height = std::max(0.0f, band_.top - content_top_ - 8.0f * scale_);
    const float rows = std::floor(max_height / (kRowDip * scale_));
    const float height = std::min({static_cast<float>(items_.size()), rows, 8.0f}) * kRowDip * scale_;
    return D2D1::RectF(input_.left, band_.top - height, input_.right, band_.top);
}

int PickerListarySearch::HitRow(float x, float y) const {
    if (!PopupVisible()) return -1;
    const auto panel = PanelBounds();
    if (!ContainsSearchRect(panel, x, y)) return -1;
    // A short dialog displays fewer rows, with the selection scrolled into view.
    const int first = FirstVisible();
    const int index = first + static_cast<int>((y - panel.top) / (kRowDip * scale_));
    return index < static_cast<int>(items_.size()) ? index : -1;
}

int PickerListarySearch::VisibleRows() const {
    const auto panel = PanelBounds();
    return std::max(0, static_cast<int>(std::lround((panel.bottom - panel.top) / (kRowDip * scale_))));
}

int PickerListarySearch::FirstVisible() const {
    return std::max(0, selected_ - VisibleRows() + 1);
}

bool PickerListarySearch::Contains(float x, float y) const {
    return ContainsSearchRect(band_, x, y) || (PopupVisible() && ContainsSearchRect(PanelBounds(), x, y));
}

} // namespace pulse::ui
