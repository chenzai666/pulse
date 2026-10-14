#include "home_widgets.h"
#include "app_internal.h"

namespace pulse {
void HandleHomeWidgetClick(AppState& state, const std::wstring& path, bool file) {
    if(path.empty()) return;
    if(file) {
        // 记录中的文件类型已知，沿用正常打开路径，避免在 UI 线程探测离线共享。
        state.ops.OpenWith(path);
        RecordRecentOpen(state,path,app::PlaceItemKind::File);
    } else NavigateTo(state,path);
}
}
