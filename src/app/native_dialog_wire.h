#pragma once
#include "native_dialog_takeover.h"
#include "native_file_dialog_adapter.h"
#include "../ipc/protocol.h"
#include <cmath>
#include <limits>

namespace pulse::app::dialog_wire {
constexpr uint32_t kMagic = 0x44475050;
constexpr size_t kLimit = 1024 * 1024;
enum Type : uint32_t { Request = 1, Ready = 2, Result = 3 };
struct Header { uint32_t magic = kMagic, type = 0, size = 0; };
static_assert(sizeof(Header) == 12);
struct RequestData {
    NativeFileDialog source;
    NativeDialogAppearance appearance;
    DWORD parent = 0;
};
inline bool TextValid(const std::wstring& text) {
    return text.size() <= 32768 && text.find(L'\0') == std::wstring::npos;
}
inline bool Write(HANDLE pipe, Type type, const ipc::PayloadWriter& payload) {
    if (payload.data().size() > kLimit) return false;
    const Header header{kMagic, type, static_cast<uint32_t>(payload.data().size())};
    DWORD written = 0;
    if (!WriteFile(pipe, &header, sizeof(header), &written, nullptr) || written != sizeof(header)) return false;
    return payload.data().empty() || (WriteFile(pipe, payload.data().data(), header.size, &written, nullptr) && written == header.size);
}
inline bool ReadExact(HANDLE pipe, void* buffer, DWORD size) {
    auto* cursor = static_cast<uint8_t*>(buffer);
    while (size) {
        DWORD read = 0;
        if (!ReadFile(pipe, cursor, size, &read, nullptr) || !read) return false;
        cursor += read; size -= read;
    }
    return true;
}
inline bool Read(HANDLE pipe, Header& header, std::vector<uint8_t>& payload) {
    if (!ReadExact(pipe, &header, sizeof(header)) || header.magic != kMagic || !header.size || header.size > kLimit ||
        header.type < Request || header.type > Result) return false;
    payload.resize(header.size);
    return ReadExact(pipe, payload.data(), header.size);
}
inline bool PackRequest(const RequestData& data, ipc::PayloadWriter& out) {
    const auto& source = data.source;
    const auto& style = data.appearance.style;
    if (!out.data().empty() || !source.window || !source.process_id || !data.parent ||
        static_cast<uint32_t>(source.kind) > 2 || !TextValid(source.title) || !TextValid(source.initial_path) ||
        !TextValid(source.filename) || source.filters.size() > 64 ||
        (source.filters.empty() ? source.filter_index != 0 : source.filter_index >= source.filters.size()) ||
        !TextValid(data.appearance.language) || data.appearance.language.size() > 32 ||
        !std::isfinite(style.row_height_dip) || style.row_height_dip < 0 || style.row_height_dip > 100) return false;
    out.PutU64(reinterpret_cast<uintptr_t>(source.window)); out.PutU32(source.process_id);
    out.PutU32(data.parent); out.PutU32(static_cast<uint32_t>(source.kind));
    out.PutString(source.title); out.PutString(source.initial_path); out.PutString(source.filename);
    out.PutU32(source.allow_multiselect ? 1 : 0); out.PutU32(static_cast<uint32_t>(source.filter_index));
    out.PutU32(static_cast<uint32_t>(source.filters.size()));
    for (const auto& filter : source.filters) {
        if (!TextValid(filter.label) || !TextValid(filter.pattern)) return false;
        out.PutString(filter.label); out.PutString(filter.pattern);
    }
    out.PutU32(data.appearance.dark ? 1 : 0); out.PutU32(data.appearance.accent); out.PutString(data.appearance.language);
    out.PutU32(source.window_thread_id);
    out.PutU32(source.process_created.dwLowDateTime); out.PutU32(source.process_created.dwHighDateTime);
    out.PutU64(reinterpret_cast<uintptr_t>(source.filename_window)); out.PutU64(reinterpret_cast<uintptr_t>(source.confirm_window));
    out.PutU32(source.legacy_folder ? 1 : 0);
    out.PutU32((style.selection_outline ? 1u : 0u) | (style.show_hidden ? 2u : 0u) |
        (style.list_smart_date ? 4u : 0u) | (style.list_zebra_rows ? 8u : 0u) |
        (style.list_size_bar ? 16u : 0u) | (style.thumbnail_badges ? 32u : 0u));
    out.PutF32(style.row_height_dip); out.PutU32(style.details_columns);
    return out.data().size() <= kLimit;
}
inline bool UnpackRequest(const std::vector<uint8_t>& bytes, RequestData& data) {
    ipc::PayloadReader in(bytes.data(), bytes.size());
    if (bytes.size() > kLimit) return false;
    const auto get_dword = [&](DWORD& value) {
        uint32_t decoded = 0;
        if (!in.GetU32(decoded)) return false;
        value = decoded;
        return true;
    };
    uint64_t window = 0, filename_window = 0, confirm_window = 0;
    uint32_t kind = 0, multi = 0, index = 0, count = 0, dark = 0, flags = 0, legacy = 0;
    auto& source = data.source; auto& style = data.appearance.style;
    if (!in.GetU64(window) || !get_dword(source.process_id) || !get_dword(data.parent) || !in.GetU32(kind) || kind > 2 ||
        !in.GetString(source.title) || !in.GetString(source.initial_path) || !in.GetString(source.filename) ||
        !in.GetU32(multi) || multi > 1 || !in.GetU32(index) || !in.GetU32(count) || count > 64) return false;
    source.window = reinterpret_cast<HWND>(static_cast<uintptr_t>(window)); source.kind = static_cast<NativeDialogKind>(kind);
    source.allow_multiselect = multi != 0; source.filter_index = index;
    source.filters.clear();
    for (uint32_t i = 0; i < count; ++i) {
        ui::PickerFilter filter;
        if (!in.GetString(filter.label) || !in.GetString(filter.pattern) || !TextValid(filter.label) || !TextValid(filter.pattern)) return false;
        source.filters.push_back(std::move(filter));
    }
    if (!in.GetU32(dark) || dark > 1 || !in.GetU32(data.appearance.accent) || !in.GetString(data.appearance.language) ||
        !get_dword(source.window_thread_id) || !get_dword(source.process_created.dwLowDateTime) ||
        !get_dword(source.process_created.dwHighDateTime) || !in.GetU64(filename_window) || !in.GetU64(confirm_window) ||
        !in.GetU32(legacy) || legacy > 1 || window > (std::numeric_limits<uintptr_t>::max)() ||
        filename_window > (std::numeric_limits<uintptr_t>::max)() || confirm_window > (std::numeric_limits<uintptr_t>::max)() ||
        !in.GetU32(flags) || flags > 63 || !in.GetF32(style.row_height_dip) || !std::isfinite(style.row_height_dip) ||
        style.row_height_dip < 0 || style.row_height_dip > 100 || !in.GetU32(style.details_columns) || in.remaining()) return false;
    data.appearance.dark = dark != 0;
    source.filename_window = reinterpret_cast<HWND>(static_cast<uintptr_t>(filename_window));
    source.confirm_window = reinterpret_cast<HWND>(static_cast<uintptr_t>(confirm_window));
    source.legacy_folder = legacy != 0;
    style.selection_outline = (flags & 1) != 0; style.show_hidden = (flags & 2) != 0;
    style.list_smart_date = (flags & 4) != 0; style.list_zebra_rows = (flags & 8) != 0;
    style.list_size_bar = (flags & 16) != 0; style.thumbnail_badges = (flags & 32) != 0;
    return TextValid(source.title) && TextValid(source.initial_path) && TextValid(source.filename) &&
        TextValid(data.appearance.language) && data.appearance.language.size() <= 32 && source.process_id && data.parent && source.window &&
        (source.filters.empty() ? index == 0 : index < source.filters.size());
}
}
