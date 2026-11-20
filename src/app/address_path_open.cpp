#include "address_path_open.h"
#include "address_completion.h"
#include "address_lookup.h"
#include "app_internal.h"
#include "app_network_locations.h"

namespace pulse {

struct AddressPathOpen : AddressLookup<DWORD> {
    const app::Tab* tab = nullptr;
    uint64_t view_generation = 0;
    std::wstring origin;
    std::wstring target;
};

void CancelAddressPathOpen(AppState& s) {
    if (!s.addressPathOpen) return;
    s.addressPathOpen->cancelled = true;
    s.addressPathOpen.reset();
}

void OpenAddressPath(AppState& s, const std::wstring& path) {
    CancelAddressPathOpen(s);
    const auto* tab = ActiveTab(s);
    if (!tab) return;
    if (path.empty() || fs::IsVirtualPath(path) || IsSystemNetworkShortcut(s, path)) {
        NavigateTo(s, path);
        return;
    }
    const auto target = ResolveAddressFilesystemPath(path, tab->current_path);
    if (target.empty() || (fs::IsUncPath(target) && target.find(L'\\', 8) == std::wstring::npos)) {
        NavigateTo(s, path);
        return;
    }
    if (path.ends_with(L'\\') || path.ends_with(L'/')) {
        NavigateTo(s, target);
        return;
    }

    const auto job = std::make_shared<AddressPathOpen>();
    job->tab = tab;
    job->view_generation = tab->view_generation;
    job->origin = tab->current_path;
    job->target = target;
    s.addressPathOpen = job;
    StartAddressLookup<DWORD>(job, fs::NeedsBoundedEnumeration(target),
        [target](const std::atomic_bool&) { return GetFileAttributesW(target.c_str()); },
        s.hwnd, WM_ADDRESS_LOOKUP_READY);
}

void PollAddressPathOpen(AppState& s) {
    const auto job = s.addressPathOpen;
    if (!job || !job->ready) return;
    s.addressPathOpen.reset();
    const auto* tab = ActiveTab(s);
    if (!tab || tab != job->tab || tab->view_generation != job->view_generation ||
        tab->current_path != job->origin || s.addressEditing || !IsWindowVisible(s.hwnd)) return;
    const DWORD attrs = job->result.value_or(INVALID_FILE_ATTRIBUTES);
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        s.ops.OpenWith(job->target);
        RecordRecentOpen(s, job->target, app::PlaceItemKind::File);
    } else {
        NavigateTo(s, job->target);
    }
}

} // namespace pulse
