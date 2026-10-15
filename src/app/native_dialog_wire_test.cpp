#include "native_dialog_wire.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

namespace pulse::app {
bool PackNativeDialogPickerResult(bool picked, const ui::FilePickerResult& result, ipc::PayloadWriter& payload);
}

namespace {
class TestPipe {
public:
    TestPipe() { valid = CreatePipe(&input, &output, nullptr, 4096) != FALSE; }
    ~TestPipe() { if (input) CloseHandle(input); if (output) CloseHandle(output); }
    TestPipe(const TestPipe&) = delete;
    TestPipe& operator=(const TestPipe&) = delete;
    void Finish() { if (output) CloseHandle(output); output = nullptr; }
    bool Raw(const void* bytes, DWORD size) {
        DWORD written = 0;
        return valid && WriteFile(output, bytes, size, &written, nullptr) && written == size;
    }
    HANDLE input = nullptr, output = nullptr;
    bool valid = false;
};

void PutAt(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    if (offset + sizeof(value) <= bytes.size()) std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
}

int RunNativeDialogWireTest(const wchar_t* output) {
    using namespace pulse;
    using namespace app;
    namespace wire = app::dialog_wire;
    std::ofstream log{std::filesystem::path(output)};
    if (!log) return 1;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        log << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        if (!ok) ++failures;
    };
    wire::RequestData request;
    request.source.window = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x123456));
    request.source.process_id = 42;
    request.source.window_thread_id = 44;
    request.source.process_created = FILETIME{123, 456};
    request.source.filename_window = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x123457));
    request.source.confirm_window = reinterpret_cast<HWND>(static_cast<uintptr_t>(0x123458));
    request.source.legacy_folder = true;
    request.parent = 43;
    request.source.kind = NativeDialogKind::Save;
    request.source.title = L"保存中文文档";
    request.source.initial_path = LR"(\\server\共享目录\草稿)";
    request.source.filename = L"文档 final.txt";
    request.source.filters = {{L"文本文档", L"*.txt;*.md"}, {L"所有文件", L"*.*"}};
    request.source.filter_index = 1;
    request.source.allow_multiselect = true;
    request.appearance.dark = true;
    request.appearance.accent = 0x3366CC;
    request.appearance.language = L"zh-TW";
    auto& style = request.appearance.style;
    style.selection_outline = true; style.show_hidden = true;
    style.list_smart_date = false; style.list_zebra_rows = false;
    style.list_size_bar = true; style.thumbnail_badges = false;
    style.row_height_dip = 35.5f; style.details_columns = 0x21;
    ipc::PayloadWriter packed;
    check(wire::PackRequest(request, packed), "request packs Unicode paths and filters");
    wire::RequestData loaded;
    check(wire::UnpackRequest(packed.data(), loaded) && loaded.source.window == request.source.window &&
        loaded.source.process_id == 42 && loaded.parent == 43 && loaded.source.kind == NativeDialogKind::Save &&
        loaded.source.window_thread_id == request.source.window_thread_id &&
        loaded.source.process_created.dwLowDateTime == request.source.process_created.dwLowDateTime &&
        loaded.source.process_created.dwHighDateTime == request.source.process_created.dwHighDateTime &&
        loaded.source.filename_window == request.source.filename_window &&
        loaded.source.confirm_window == request.source.confirm_window && loaded.source.legacy_folder &&
        loaded.source.title == request.source.title && loaded.source.initial_path == request.source.initial_path &&
        loaded.source.filename == request.source.filename && loaded.source.allow_multiselect &&
        loaded.source.filter_index == 1 && loaded.source.filters.size() == 2 &&
        loaded.source.filters[0].label == request.source.filters[0].label &&
        loaded.source.filters[0].pattern == request.source.filters[0].pattern &&
        loaded.source.filters[1].label == request.source.filters[1].label &&
        loaded.source.filters[1].pattern == request.source.filters[1].pattern &&
        loaded.appearance.dark && loaded.appearance.accent == 0x3366CC && loaded.appearance.language == L"zh-TW" &&
        loaded.appearance.style.selection_outline && loaded.appearance.style.show_hidden &&
        !loaded.appearance.style.list_smart_date && !loaded.appearance.style.list_zebra_rows &&
        loaded.appearance.style.list_size_bar && !loaded.appearance.style.thumbnail_badges &&
        loaded.appearance.style.row_height_dip == 35.5f && loaded.appearance.style.details_columns == 0x21,
        "request round-trips source identity, filter selection, and appearance");
    bool modes = true;
    for (const auto kind : {NativeDialogKind::Open, NativeDialogKind::Save, NativeDialogKind::Folder}) {
        auto candidate = request; candidate.source.kind = kind;
        ipc::PayloadWriter value;
        wire::RequestData copy;
        modes &= wire::PackRequest(candidate, value) && wire::UnpackRequest(value.data(), copy) && copy.source.kind == kind;
    }
    check(modes, "open, save, and folder modes round-trip independently");
    {
        TestPipe pipe;
        const bool sent = wire::Write(pipe.output, wire::Request, packed);
        pipe.Finish();
        wire::Header header;
        std::vector<uint8_t> bytes;
        check(sent && wire::Read(pipe.input, header, bytes) && header.type == wire::Request &&
            bytes == packed.data(), "request transport preserves the complete payload");
    }
    auto rejects = [&](std::vector<uint8_t> bytes) {
        wire::RequestData rejected;
        return !wire::UnpackRequest(bytes, rejected);
    };
    bool truncated = true;
    for (size_t size = 0; size < packed.data().size(); ++size)
        truncated &= rejects(std::vector<uint8_t>(packed.data().begin(), packed.data().begin() + size));
    check(truncated, "every truncated request is rejected");
    auto bytes = packed.data(); bytes.push_back(0);
    check(rejects(bytes), "trailing request data is rejected");
    const size_t selection = 20 + 12 + 2 * (request.source.title.size() +
        request.source.initial_path.size() + request.source.filename.size());
    auto mutation_rejected = [&](size_t offset, uint32_t value) {
        auto changed = packed.data(); PutAt(changed, offset, value); return rejects(std::move(changed));
    };
    check(mutation_rejected(16, 3) && mutation_rejected(selection, 2) &&
        mutation_rejected(selection + 4, 2) && mutation_rejected(selection + 8, 65),
        "invalid mode, multi-select flag, filter index, and filter count are rejected");
    check(mutation_rejected(8, 0) && mutation_rejected(12, 0) && mutation_rejected(20, UINT32_MAX),
        "missing process identity and forged string lengths are rejected");
    auto no_window = packed.data(); PutAt(no_window, 0, 0); PutAt(no_window, 4, 0);
    check(rejects(no_window), "missing source window is rejected");
    check(mutation_rejected(packed.data().size() - 12, 64) &&
        mutation_rejected(packed.data().size() - 8, 0x7FC00000) &&
        mutation_rejected(packed.data().size() - 8, 0xBF800000) &&
        mutation_rejected(packed.data().size() - 8, 0x42CA0000),
        "unknown appearance flags, nonfinite and out-of-range row heights are rejected");
    size_t appearance = selection + 12;
    for (const auto& filter : request.source.filters)
        appearance += 8 + 2 * (filter.label.size() + filter.pattern.size());
    check(mutation_rejected(appearance, 2), "invalid dark-theme flag is rejected");
    check(mutation_rejected(packed.data().size() - 16, 2), "invalid legacy-folder flag is rejected");
    auto accepts_request = [&](wire::RequestData candidate) {
        ipc::PayloadWriter value;
        wire::RequestData copy;
        return wire::PackRequest(candidate, value) && wire::UnpackRequest(value.data(), copy);
    };
    auto limit = request; limit.source.title.assign(32768, L'文');
    check(accepts_request(limit), "32768-character request text is accepted");
    limit.source.title.push_back(L'文');
    ipc::PayloadWriter long_text;
    check(!wire::PackRequest(limit, long_text), "overlong request text is rejected before transport");
    limit = request; limit.source.title.assign(L"a\0b", 3);
    ipc::PayloadWriter nul_text;
    check(!wire::PackRequest(limit, nul_text), "embedded NUL in request text is rejected");
    limit = request; limit.source.filters.resize(64, {L"文本", L"*.txt"}); limit.source.filter_index = 63;
    check(accepts_request(limit), "64 request filters are accepted");
    limit.source.filters.push_back({L"额外", L"*.*"});
    ipc::PayloadWriter extra_filter;
    check(!wire::PackRequest(limit, extra_filter), "65 request filters are rejected");
    limit = request; limit.appearance.language.assign(33, L'a');
    check(!accepts_request(limit), "overlong language metadata is rejected");
    limit = request; limit.source.filters.assign(16, {std::wstring(32768, L'文'), std::wstring(32768, L'*')});
    ipc::PayloadWriter oversized;
    check(!wire::PackRequest(limit, oversized), "aggregate request payload above 1 MiB is rejected");
    {
        TestPipe pipe;
        ipc::PayloadWriter too_large;
        too_large.PutString(std::wstring(wire::kLimit / sizeof(wchar_t), L'a'));
        check(!wire::Write(pipe.output, wire::Request, too_large), "oversized payload is rejected by the transport writer");
    }
    for (int bad = 0; bad < 4; ++bad) {
        TestPipe pipe;
        wire::Header header{wire::kMagic, wire::Request, 0};
        if (bad == 0) header.magic ^= 1;
        if (bad == 1) header.size = static_cast<uint32_t>(wire::kLimit + 1);
        if (bad == 3) header.size = 16;
        const DWORD size = bad == 2 ? static_cast<DWORD>(sizeof(header) - 1) : static_cast<DWORD>(sizeof(header));
        const bool sent = pipe.Raw(&header, size);
        pipe.Finish();
        std::vector<uint8_t> received;
        check(sent && !wire::Read(pipe.input, header, received), "malformed, oversized, and truncated transport frames are rejected");
    }
    ui::FilePickerResult result;
    result.filter_index = 2;
    result.paths = {LR"(C:\文档\a.txt)", LR"(\\server\share\b.md)", LR"(\\?\C:\very long\c.txt)", LR"(\\?\UNC\server\share\d.txt)"};
    ipc::PayloadWriter response;
    check(PackNativeDialogPickerResult(true, result, response), "result packs complete local, UNC, and extended paths");
    {
        TestPipe pipe;
        const bool sent = wire::Write(pipe.output, wire::Result, response);
        pipe.Finish();
        wire::Header header;
        std::vector<uint8_t> received;
        bool round_trip = sent && wire::Read(pipe.input, header, received) && header.type == wire::Result;
        ipc::PayloadReader reader(received.data(), received.size());
        uint32_t picked = 0, filter = 0, count = 0;
        round_trip &= reader.GetU32(picked) && reader.GetU32(filter) && reader.GetU32(count) &&
            picked == 1 && filter == 2 && count == result.paths.size();
        for (const auto& expected : result.paths) {
            std::wstring path;
            round_trip &= reader.GetString(path) && path == expected;
        }
        check(round_trip && reader.remaining() == 0, "result transport round-trips every full path and filter index");
    }
    auto accepts_result = [&](const ui::FilePickerResult& candidate) {
        ipc::PayloadWriter value; return PackNativeDialogPickerResult(true, candidate, value);
    };
    result.paths.assign(256, LR"(C:\a.txt)");
    check(accepts_result(result), "256 selected paths are accepted");
    result.paths.push_back(LR"(C:\b.txt)");
    check(!accepts_result(result), "257 selected paths are rejected");
    result.paths.assign(1, std::wstring(32768, L'a'));
    check(accepts_result(result), "32768-character result path is accepted");
    result.paths[0].push_back(L'a');
    check(!accepts_result(result), "overlong result path is rejected");
    result.paths = {LR"(C:\a)" + std::wstring(1, wchar_t{}) + L"b"};
    check(!accepts_result(result), "embedded NUL in result path is rejected");
    result.paths = {L""};
    check(!accepts_result(result), "empty selected path is rejected");
    result.paths.clear();
    check(!accepts_result(result), "successful result requires at least one path");
    result.paths.assign(15, std::wstring(32768, L'a'));
    result.paths.push_back(std::wstring(32730, L'a'));
    ipc::PayloadWriter exact;
    check(PackNativeDialogPickerResult(true, result, exact) && exact.data().size() == wire::kLimit,
        "result payload at exactly 1 MiB is accepted");
    result.paths.back().push_back(L'a');
    check(!accepts_result(result), "result payload above 1 MiB is rejected");
    ipc::PayloadWriter cancelled;
    check(PackNativeDialogPickerResult(false, result, cancelled) && cancelled.data().size() == 12,
        "cancelled result omits stale paths");
    {
        TestPipe pipe;
        ipc::PayloadWriter ready; ready.PutU64(0x12345678); ready.PutU32(42); ready.PutU32(44);
        const bool sent = wire::Write(pipe.output, wire::Ready, ready);
        pipe.Finish();
        wire::Header header;
        std::vector<uint8_t> received;
        const bool read = wire::Read(pipe.input, header, received);
        ipc::PayloadReader reader(received.data(), received.size());
        uint64_t window = 0;
        uint32_t process = 0, thread = 0;
        check(sent && read && header.type == wire::Ready && header.size == 16 &&
            reader.GetU64(window) && window == 0x12345678 &&
            reader.GetU32(process) && process == 42 && reader.GetU32(thread) && thread == 44 &&
            reader.remaining() == 0, "ready frame carries picker window, process, and thread identity");
    }
    log << "failures=" << failures << '\n';
    return failures ? 1 : 0;
}
