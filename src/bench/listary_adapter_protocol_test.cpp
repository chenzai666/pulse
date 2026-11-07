#include "../app/listary_adapter_protocol.h"

#include <cstdio>
#include <vector>

namespace {

std::vector<uint8_t> Message(std::wstring path, uint32_t operation = 1, uint64_t tab = 7) {
    const pulse::app::ListaryAdapterHeader header{
        operation, static_cast<uint32_t>(path.size() + 1), tab};
    std::vector<uint8_t> bytes(sizeof(header) + (path.size() + 1) * sizeof(wchar_t));
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(bytes.data() + sizeof(header), path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    return bytes;
}

} // namespace

int main() {
    using namespace pulse::app;
    unsigned failures = 0, checks = 0;
    const auto check = [&](bool passed, const char* name) {
        ++checks;
        if (!passed) ++failures;
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
    };
    const auto parse = [](const std::vector<uint8_t>& bytes) {
        return ParseListaryAdapterRequest(kListaryAdapterData, bytes.data(), bytes.size());
    };
    auto valid = Message(L"C:\\目录\\子目录");
    auto request = parse(valid);
    check(request && request->tab == 7 && request->path == L"C:\\目录\\子目录", "folder and tab lease preserved");
    check(parse(Message(L"C:\\")).has_value(), "drive root");
    check(parse(Message(L"\\\\server\\share\\folder")).has_value(), "UNC directory");
    check(parse(Message(L"C:\\emoji-\xD83D\xDE80")).has_value(), "valid UTF-16 surrogate pair");
    check(!ParseListaryAdapterRequest(0x504C5354, valid.data(), valid.size()), "picker tag rejected");
    check(!parse(Message(L"C:\\folder", 2)), "file operation rejected");
    check(!parse(Message(L"C:\\folder", 0)), "unknown operation rejected");
    check(!parse(Message(L"C:\\folder", 1, 0)), "zero tab lease rejected");
    check(!parse(Message(L"C:\\folder", 1, UINT64_C(0x100000000))), "cross-bitness tab overflow rejected");
    for (const auto* path : {L"", L"folder", L"C:folder", L"pulse:home", L"shell:downloads",
            L"C:\\..\\folder", L"C:\\folder.\\child", L"C:\\folder ", L"C:\\NUL",
            L"C:\\data:stream", L"C:/folder", L"C:\\*.txt", L"\\\\?\\C:\\folder",
            L"\\\\.\\pipe\\test", L"\\\\server", L"\\\\server\\share\\\\folder",
            L"C:\\bad-\xD800", L"C:\\bad-\xDC00"}) {
        check(!parse(Message(path)), "non-directory syntax rejected");
    }
    check(!parse(Message(std::wstring(L"C:\\one\0two", 10))), "embedded NUL rejected");
    auto missing_nul = valid;
    missing_nul[missing_nul.size() - 2] = 'x';
    check(!parse(missing_nul), "missing terminal NUL rejected");
    auto extra_nul = valid;
    extra_nul.resize(extra_nul.size() + 2);
    check(!parse(extra_nul), "trailing UTF-16 beyond declared length rejected");
    auto odd = valid;
    odd.pop_back();
    check(!parse(odd), "odd UTF-16 byte count rejected");
    auto truncated = valid;
    truncated.resize(15);
    check(!parse(truncated), "truncated header rejected");
    check(!ParseListaryAdapterRequest(kListaryAdapterData, nullptr, valid.size()), "null payload rejected");
    check(!parse(Message(L"C:\\" + std::wstring(32768, L'x'))), "oversized message rejected");
    // WM_COPYDATA may supply an unaligned buffer.
    auto unaligned = valid;
    unaligned.insert(unaligned.begin(), 0);
    check(ParseListaryAdapterRequest(kListaryAdapterData, unaligned.data() + 1, valid.size()).has_value(),
        "unaligned payload accepted without pointer casts");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
