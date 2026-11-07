#pragma once

#include "../ui/folder_picker_listary_protocol.h"

namespace pulse::app {

inline constexpr uintptr_t kListaryAdapterData = 0x504C534D;
inline constexpr wchar_t kListaryAdapterMarker[] = L"Pulse.Listary.Adapter.v1";
inline constexpr wchar_t kListaryAdapterTab[] = L"Pulse.Listary.Tab.v1";

struct ListaryAdapterHeader {
    uint32_t operation = 0;
    uint32_t chars = 0; // Includes the single terminating UTF-16 NUL.
    uint64_t tab = 0;
};
static_assert(sizeof(ListaryAdapterHeader) == 16);

struct ListaryAdapterRequest {
    uint32_t tab = 0;
    std::wstring path;
};

inline std::optional<ListaryAdapterRequest> ParseListaryAdapterRequest(
    uintptr_t tag, const void* bytes, size_t size) {
    static_assert(sizeof(wchar_t) == 2, "Listary messages use Windows UTF-16");
    if (tag != kListaryAdapterData || !bytes || size < sizeof(ListaryAdapterHeader) + 4 ||
        size > ui::kPickerListaryMaxBytes) return std::nullopt;
    ListaryAdapterHeader header;
    std::memcpy(&header, bytes, sizeof(header));
    if (header.operation != 1 || !header.tab || header.tab > UINT32_MAX ||
        header.chars != (size - sizeof(header)) / sizeof(wchar_t) ||
        (size - sizeof(header)) % sizeof(wchar_t)) return std::nullopt;
    std::wstring path(header.chars, L'\0');
    std::memcpy(path.data(), static_cast<const uint8_t*>(bytes) + sizeof(header),
        size - sizeof(header));
    if (path.back() != L'\0') return std::nullopt;
    path.pop_back();
    if (!ui::IsPickerListaryPath(path)) return std::nullopt;
    return ListaryAdapterRequest{static_cast<uint32_t>(header.tab), std::move(path)};
}

} // namespace pulse::app
