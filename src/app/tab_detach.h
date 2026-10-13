#pragma once
// A tab or pane dragged away stays in its source until button release.
// Drop previews use the existing strip and pane placement model; cancellation
// clears the preview without creating a temporary window.
#include <windows.h>
#include <cstddef>

namespace pulse {
struct AppState;
namespace app { struct Pane; }
namespace ui {
struct HitTestResult;
}

// Vertical distance from the press, in DIP, that pulls a tab off the strip.
constexpr float kTabDetachDistanceDip = 24.0f;

// True once the drag has moved far enough to start an off-strip preview.
bool TabDetachDistanceReached(POINT press, POINT now, float scale);

// The new window's rect: `size`, placed so that the cursor lands on
// `grab_client` (a point in the new window's client area); `client_offset` is
// the client origin relative to the window's top-left corner.
RECT DetachedWindowRect(SIZE size, POINT cursor, POINT grab_client, POINT client_offset);

// WM_MOUSEMOVE during a press on a tab (mx, my: client px). Once it leaves the
// strip, capture follows the drop preview. Returns true when preview handles
// this move instead of the existing strip reorder.
bool TryDetachDraggedTab(AppState& s, int mx, int my);
// Starts a captured preview without moving the tab or extracting the pane.
bool BeginTabDropPreview(AppState& s, size_t index, POINT grab, app::Pane* pane = nullptr);
// WM_MOUSEMOVE while captured; returning to the source strip resumes its sorter.
bool UpdatePendingTabDrop(AppState& s, int mx, int my);
// WM_LBUTTONUP: commit the final target, or create a window only outside Pulse.
// True when handled; a last-tab transfer may destroy s, so callers return now.
bool FinishPendingTabDrop(AppState& s, int mx, int my);
// Esc, capture loss, deactivation or close: clear the preview and keep all tabs.
bool CancelPendingTabDrop(AppState& s);
// WM_LBUTTONDOWN (after the pane under the press is focused): a press on the
// header of one of at least two visible panes may drag that pane out (M6d).
void ArmPaneDrag(AppState& s, const ui::HitTestResult& hit, int mx, int my);
// WM_MOUSEMOVE: a pane-header drag past the system distance starts a preview.
// The pane remains in its split until release, so Esc preserves its origin.
bool TryDetachDraggedPane(AppState& s, int mx, int my);

// A visible tab of a strip, in strip order: its window_tabs index and rect.
struct TabSpan {
    size_t index = 0;
    RECT rect{};
};
// Where a tab dropped at `pt` (client px) lands among `spans`: before the
// first tab whose middle is right of `pt`, else at `count` (the end). The
// whole tab row counts, as in browsers: -1 only when `pt` is more than
// `slack` px above or below the tabs or left of the first one, or right of
// `strip_right` (where the caption buttons start).
int TabDropIndex(const TabSpan* spans, size_t span_count, size_t count, POINT pt, LONG slack,
                 LONG strip_right);

// WM_BEGIN_TAB_DETACH: runs the system move loop for `s` while the button is
// still down. `from` is the window the tab came from and `index` its old
// position (nullptr: the whole window was dragged by its only tab). After Esc
// the tab goes back and `s` is destroyed. Released over another Pulse
// window's tab strip, a window with one tab merges into it there (M6b).
void RunDetachedTabMove(AppState& s, HWND from, size_t index);

// WM_MOVING: while `s` is in a move loop, marks where its tabs would land on
// the tab strip of the Pulse window under the cursor (M6b, M6c).
void TrackTabDrop(AppState& s);
// WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE of a title-bar drag (M6c). Released on
// another window's tab strip (not after Esc), EndWindowMove posts
// WM_MERGE_WINDOW, whose FinishWindowMerge merges `s` into that window with
// an undo toast; `s` may be gone afterwards.
void BeginWindowMove(AppState& s);
void EndWindowMove(AppState& s);
void FinishWindowMerge(AppState& s, HWND target, size_t index);
}  // namespace pulse
