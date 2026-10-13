#include "app_quicklook_routing.h"
#include "app_internal.h"

namespace pulse {

void HandleEmbeddedQuickLookEvent(AppState& state, UINT_PTR token) {
    app::QuickLookEvent event;
    if (!state.quickLook.TakeEvent(token, event)) return;
    if (event.kind == app::QuickLookEventKind::Failed) {
        ui::QuickPreviewItem selected;
        if (SelectedQuickPreviewItem(state, selected) && selected.path == event.path &&
            !state.quickPreview.visible()) {
            ShowNativeQuickPreview(state, selected);
        }
    } else if (event.kind == app::QuickLookEventKind::Navigate) {
        NavigateQuickPreview(state, event.direction);
    }
    InvalidateRect(state.hwnd, nullptr, FALSE);
}

} // namespace pulse
