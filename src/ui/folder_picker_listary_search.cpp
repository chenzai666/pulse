#include "../common/windows_compat.h"
#include "folder_picker_listary_search.h"

#include "../common/localization.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <mutex>
#include <thread>
#include <utility>

namespace pulse::ui {
namespace {
constexpr size_t kMaxResults = 8;
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

struct PickerListarySearch::Mailbox {
    struct Request {
        uint64_t generation = 0;
        std::wstring query;
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
                                                        *request.cancelled);
                if (result.items.size() > kMaxResults) result.items.resize(kMaxResults);
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
    selected_ = -1;
    dismissed_ = false;
    due_ = GetTickCount64() + 220;
    status_ = query_.empty() ? l10n::Pick(L"Listary 全盘搜索", L"Search all files with Listary")
                             : l10n::Pick(L"正在搜索…", L"Searching…");
}

void PickerListarySearch::Queue() {
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard lock(mailbox_->mutex);
        mailbox_->CancelLocked();
        mailbox_->pending = Mailbox::Request{generation_, query_, std::move(cancelled)};
    }
    mailbox_->ready.notify_one();
    due_ = 0;
}

void PickerListarySearch::SearchNow() {
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
    items_ = std::move(reply->result.items);
    if (reply->result.error != integrations::ListarySearchError::None) items_.clear();
    selected_ = items_.empty() ? -1 : 0;
    status_ = Status(reply->result.error, query_.empty(), items_.empty());
    return true;
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
    selected_ = (selected_ + direction + count) % count;
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
    const float height = std::min(static_cast<float>(items_.size()), rows) * kRowDip * scale_;
    return D2D1::RectF(input_.left, band_.top - height, input_.right, band_.top);
}

int PickerListarySearch::HitRow(float x, float y) const {
    if (!PopupVisible()) return -1;
    const auto panel = PanelBounds();
    if (!ContainsSearchRect(panel, x, y)) return -1;
    // A short dialog displays fewer rows, with the selection scrolled into view.
    const int visible = std::max(1, static_cast<int>((panel.bottom - panel.top) / (kRowDip * scale_)));
    const int first = std::max(0, selected_ - visible + 1);
    const int index = first + static_cast<int>((y - panel.top) / (kRowDip * scale_));
    return index < static_cast<int>(items_.size()) ? index : -1;
}

bool PickerListarySearch::Contains(float x, float y) const {
    return ContainsSearchRect(band_, x, y) || (PopupVisible() && ContainsSearchRect(PanelBounds(), x, y));
}

} // namespace pulse::ui
