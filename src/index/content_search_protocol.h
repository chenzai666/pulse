#pragma once
#include "../ipc/protocol.h"
#include "content_index.h"
namespace pulse::index::content {
inline constexpr uint32_t kMagic = 0x43535051;
inline constexpr size_t kMaximumPayload = 8 * 1024 * 1024;
enum Message : uint32_t {
    REQ_SEARCH = 1, REQ_CONFIG = 2, REQ_CANCEL = 3, REQ_STATUS = 4,
    REQ_PAUSE = 5, REQ_RESUME = 6, REQ_REBUILD = 7, REQ_SHUTDOWN = 8,
    REQ_SUBSCRIBE = 9,
    RSP_BATCH = 101, RSP_STATUS = 102,
};
inline std::wstring PipeName(std::wstring_view token) {
#ifdef PULSE_CUSTOM_EDITION
    return L"\\\\.\\pipe\\PulseCustomContentSearch." + std::wstring(token);
#else
    return L"\\\\.\\pipe\\PulseContentSearch." + std::wstring(token);
#endif
}
inline ipc::MsgHeader Header(uint32_t type, uint32_t size) {
    ipc::MsgHeader h; h.magic = kMagic; h.type = type; h.payload_size = size; return h;
}
inline void PutSubscriptionStatus(ipc::PayloadWriter& writer, const ContentSearchProgress& progress) {
    writer.PutU32(progress.subscription_error);
    writer.PutU32(static_cast<uint32_t>(progress.subscription_failure));
}
inline bool GetSubscriptionStatus(ipc::PayloadReader& reader, ContentSearchProgress& progress) {
    if (!reader.remaining()) return true;
    uint32_t failure = 0, error = 0;
    if (!reader.GetU32(error) || !reader.GetU32(failure) ||
        failure > static_cast<uint32_t>(ContentSubscriptionFailure::Reconcile) ||
        ((error == 0) != (failure == 0))) return false;
    progress.subscription_error = error;
    progress.subscription_failure = static_cast<ContentSubscriptionFailure>(failure);
    return reader.remaining() == 0;
}
inline void PutConfig(ipc::PayloadWriter& w, const ContentIndexConfig& c) {
    w.PutU64(c.maximum_file_bytes); w.PutU32(static_cast<uint32_t>(c.roots.size()));
    for (const auto& r : c.roots) { w.PutString(r.path); w.PutU32(static_cast<uint32_t>(r.encoding)); }
    w.PutU32(static_cast<uint32_t>(c.excluded_directories.size()));
    for (const auto& ex : c.excluded_directories) w.PutString(ex);
    w.PutU32(c.shared_scope ? 1u : 0u);
    w.PutU32(static_cast<uint32_t>(c.excluded_paths.size()));
    for (const auto& path : c.excluded_paths) w.PutString(path);
    w.PutU32(static_cast<uint32_t>(c.default_encoding));
    w.PutU64(c.maximum_document_bytes);
}
inline bool GetConfig(ipc::PayloadReader& r, ContentIndexConfig& c) {
    c.maximum_document_bytes = 512ull * 1024 * 1024;
    uint32_t n = 0;
    if (!r.GetU64(c.maximum_file_bytes) || c.maximum_file_bytes > 64ull * 1024 * 1024 || !r.GetU32(n) || n > 32) return false;
    c.roots.clear();
    for (uint32_t i = 0; i < n; ++i) {
        ContentIndexRoot root; uint32_t encoding = 0;
        if (!r.GetString(root.path) || root.path.size() > 32768 || !r.GetU32(encoding) || encoding > 3) return false;
        root.encoding = static_cast<text::Encoding>(encoding); c.roots.push_back(std::move(root));
    }
    if (!r.GetU32(n) || n > 256) return false;
    c.excluded_directories.clear();
    for (uint32_t i = 0; i < n; ++i) { std::wstring value; if (!r.GetString(value) || value.size() > 256) return false; c.excluded_directories.push_back(std::move(value)); }
    c.shared_scope = false; c.excluded_paths.clear(); c.default_encoding = text::Encoding::Auto;
    if (r.remaining()) {
        uint32_t shared = 0;
        if (!r.GetU32(shared) || !r.GetU32(n) || n > 256) return false;
        c.shared_scope = shared != 0;
        for (uint32_t i = 0; i < n; ++i) {
            std::wstring path;
            if (!r.GetString(path) || path.empty() || path.size() > 32768) return false;
            c.excluded_paths.push_back(std::move(path));
        }
        if (r.remaining()) {
            uint32_t encoding = 0;
            if (!r.GetU32(encoding) || encoding > 3) return false;
            c.default_encoding = static_cast<text::Encoding>(encoding);
        }
    }
    if (r.remaining() && (!r.GetU64(c.maximum_document_bytes) || !c.maximum_document_bytes ||
        c.maximum_document_bytes > 512ull * 1024 * 1024)) return false;
    return true;
}
inline void PutStatus(ipc::PayloadWriter& w, const ContentIndexStatus& s) {
    w.PutU64(s.indexed_files); w.PutU64(s.skipped_files); w.PutU64(s.errors);
    w.PutU64(s.pending_files); w.PutU64(s.indexed_bytes);
    w.PutU32((s.paused ? 1u : 0u) | (s.indexing ? 2u : 0u) | 12u); w.PutU32(s.error);
    w.PutString(s.current_root); w.PutString(s.coverage);
    w.PutU32(static_cast<uint32_t>(s.root_status.size()));
    for (const auto& root : s.root_status) {
        w.PutString(root.path); w.PutU64(root.indexed_files); w.PutU64(root.skipped_files);
        w.PutU32(root.error); w.PutU32((root.available ? 1u : 0u) | (root.indexing ? 2u : 0u) |
            (static_cast<uint32_t>(root.state) << 8) | 4u);
    }
    w.PutU64(s.revision);
    w.PutU64(s.change_sequence);
}
inline bool GetStatus(ipc::PayloadReader& r, ContentIndexStatus& s) {
    uint32_t flags = 0, error = 0;
    if (!r.GetU64(s.indexed_files) || !r.GetU64(s.skipped_files) || !r.GetU64(s.errors) ||
        !r.GetU64(s.pending_files) || !r.GetU64(s.indexed_bytes) || !r.GetU32(flags) ||
        !r.GetU32(error) || !r.GetString(s.current_root) || !r.GetString(s.coverage)) return false;
    s.error = error;
    s.paused = (flags & 1) != 0; s.indexing = (flags & 2) != 0;
    uint32_t count = 0;
    if (!r.GetU32(count) || count > 32) return false;
    s.root_status.clear();
    for (uint32_t i = 0; i < count; ++i) {
        ContentIndexRootStatus root; uint32_t state = 0, root_error = 0;
        if (!r.GetString(root.path) || !r.GetU64(root.indexed_files) || !r.GetU64(root.skipped_files) || !r.GetU32(root_error) || !r.GetU32(state)) return false;
        root.error = root_error;
        root.state = (state & 4u) ? static_cast<ContentIndexRootStatus::State>((state >> 8) & 255u)
            : (state & 2u) ? ContentIndexRootStatus::State::Scanning
            : (state & 1u) ? ContentIndexRootStatus::State::Ready : ContentIndexRootStatus::State::Waiting;
        root.available = (state & 1) != 0; root.indexing = (state & 2) != 0; s.root_status.push_back(std::move(root));
    }
    s.revision = 0;
    if ((flags & 4u) && !r.GetU64(s.revision)) return false;
    s.change_sequence = 0;
    return !(flags & 8u) || r.GetU64(s.change_sequence);
}
} // namespace pulse::index::content
