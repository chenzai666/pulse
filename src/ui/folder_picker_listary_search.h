#pragma once

#include "../integrations/listary_search_client.h"
#include "fluent_components.h"
#include "ui_compositor.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::ui {

class ShellIconCache;

// Lexical display helpers: never query the filesystem or invoke the Shell.
std::wstring ListaryResultName(const integrations::ListarySearchItem& item);
std::wstring ListaryResultParent(std::wstring_view path);
struct ListaryNameMatch {
    UINT32 start = 0;
    UINT32 length = 0;
};
// Ordinal, case-insensitive literal query spans mapped to the visible prefix.
// Filter operators/negative terms are not filename highlight text.
std::vector<ListaryNameMatch> ListaryNameMatches(std::wstring_view name,
    std::wstring_view query, std::wstring_view shown);

// UI-thread-owned search state. Its single worker only sees a shared mailbox,
// never a window or a callback into the picker.
class PickerListarySearch {
public:
    explicit PickerListarySearch(bool folders_only);
    ~PickerListarySearch();
    PickerListarySearch(const PickerListarySearch&) = delete;
    PickerListarySearch& operator=(const PickerListarySearch&) = delete;

    static constexpr float FooterDip() { return 56.0f; }
    void SetQuery(std::wstring query);
    void SearchNow();
    bool Tick();
    void SetFocused(bool focused);
    bool Dismiss();
    bool PopupVisible() const;
    void MoveSelection(int direction);
    void LoadMore();
    bool HitLoadMore(float x, float y) const;
    std::optional<integrations::ListarySearchItem> Selected() const;
    int HitRow(float x, float y) const;
    bool Contains(float x, float y) const;
    void SelectRow(int index);
    void Layout(float width, float footer_top, float content_top, float scale);
    // Called by the existing 80 ms UI timer. Draw only consumes cached pixels.
    void PrefetchIcons(ShellIconCache& icons) const;
    void Draw(Compositor& compositor, fluent::Painter& painter, const Theme& theme,
              bool enabled, ShellIconCache& icons) const;
    D2D1_RECT_F InputBounds() const { return input_; }
    const std::wstring& Query() const { return query_; }
    const std::wstring& StatusText() const { return status_; }

private:
    struct Mailbox;
    void Queue(bool append = false);
    void UpdatePageStatus(std::wstring error = {});
    int VisibleRows() const;
    int FirstVisible() const;
    std::shared_ptr<Mailbox> mailbox_;
    std::wstring query_;
    std::wstring status_;
    std::vector<integrations::ListarySearchItem> items_;
    uint64_t generation_ = 0;
    uint64_t due_ = 0;
    uint64_t total_ = 0;
    uint64_t next_offset_ = 0;
    uint64_t requested_offset_ = 0;
    bool has_more_ = false;
    bool page_loading_ = false;
    bool request_append_ = false;
    bool focused_ = false;
    bool dismissed_ = false;
    int selected_ = -1;
    float scale_ = 1.0f;
    float content_top_ = 0;
    D2D1_RECT_F band_{};
    D2D1_RECT_F input_{};
    D2D1_RECT_F PanelBounds() const;
};

} // namespace pulse::ui
