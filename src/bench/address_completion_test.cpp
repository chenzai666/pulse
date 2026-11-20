#include "../app/address_completion.h"
#include "../fs/fs_enum.h"
#include <cstdio>

int wmain() {
    int failures = 0;
    const auto check = [&](bool passed, const char* description) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", description);
        if (!passed) ++failures;
    };
    pulse::AddressCompletionQuery query;
    const auto matches = [&](const wchar_t* text, const wchar_t* base, const wchar_t* parent, const wchar_t* prefix) {
        return pulse::ParseAddressCompletion(text, base, query) &&
            query.parent == pulse::fs::NormalizePath(parent) && query.prefix == prefix;
    };
    check(matches(L"C:\\work\\Do", L"", L"C:\\work", L"Do"), "absolute path splits at its last separator");
    check(matches(L"Do", L"\\\\?\\C:\\work", L"C:\\work", L"Do"), "relative prefix uses extended current folder");
    check(matches(L"../Docs/te", L"C:\\work\\here", L"C:\\work\\Docs", L"te"), "relative parent segments and forward slashes");
    check(matches(L"\\Docs\\te", L"\\\\host\\share\\folder", L"\\\\host\\share\\Docs", L"te"), "UNC root-relative prefix stays on its share");
    check(matches(L"\"\\\\?\\UNC\\host\\share\\my folder\\na", L"", L"\\\\host\\share\\my folder", L"na"), "quoted extended UNC input");
    check(!pulse::ParseAddressCompletion(L"\\\\host", L"", query), "bare server is left to normal navigation");
    check(!pulse::ParseAddressCompletion(L"x", L"pulse:recent", query), "virtual locations supply no relative base");
    check(!pulse::ParseAddressCompletion(L"C:folder\\f", L"D:\\work", query), "drive-relative text is not current-folder completion");
    check(!pulse::ParseAddressCompletion(L"C:\\*\\file", L"", query), "wildcards in parent are rejected");
    check(!pulse::ParseAddressCompletion(L"C:\\work\\?", L"", query), "wildcards in prefix are rejected");
    check(!pulse::ParseAddressCompletion(L"\\\\?\\GLOBALROOT\\Device\\f", L"C:\\work", query), "device namespace cannot become a relative completion");
    check(pulse::ResolveAddressFilesystemPath(L"..\\file.txt", L"\\\\?\\C:\\work\\folder") ==
        pulse::fs::NormalizePath(L"C:\\work\\file.txt"), "relative direct open resolves dot segments before extending the path");
    check(pulse::ResolveAddressFilesystemPath(L"\\folder\\file.txt", L"\\\\host\\share\\here") ==
        pulse::fs::NormalizePath(L"\\\\host\\share\\folder\\file.txt"), "direct open resolves a UNC share-relative path");
    check(pulse::ResolveAddressFilesystemPath(L"relative.txt", L"pulse:recent").empty(), "virtual views leave unresolved paths to existing navigation");

    wchar_t temp[MAX_PATH]{};
    wchar_t fixture[MAX_PATH]{};
    if (!GetTempPathW(ARRAYSIZE(temp), temp) || !GetTempFileNameW(temp, L"pac", 0, fixture)) return 1;
    if (!DeleteFileW(fixture) || !CreateDirectoryW(fixture, nullptr)) return 1;
    const std::wstring root(fixture);
    const auto document = root + L"\\Document.txt";
    const auto nested = root + L"\\Documents";
    const HANDLE file = CreateFileW(document.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    const bool initialized = CreateDirectoryW(nested.c_str(), nullptr) != FALSE && file != INVALID_HANDLE_VALUE;
    check(initialized, "create isolated fixture");
    if (initialized && matches(L"doc", fixture, fixture, L"doc")) {
        std::atomic_bool cancelled{false};
        const auto results = pulse::EnumerateAddressCompletion(query, cancelled);
        check(results.size() == 2 && results[0].name == L"Document.txt" && !results[0].directory &&
            results[1].name == L"Documents" && results[1].directory,
            "case-insensitive enumeration preserves file/folder kinds and names");
        cancelled = true;
        check(pulse::EnumerateAddressCompletion(query, cancelled).empty(), "cancelled enumeration publishes no paths");
    } else {
        check(false, "parse filesystem fixture");
    }
    if (file != INVALID_HANDLE_VALUE) check(DeleteFileW(document.c_str()) != FALSE, "remove fixture file");
    if (GetFileAttributesW(nested.c_str()) != INVALID_FILE_ATTRIBUTES)
        check(RemoveDirectoryW(nested.c_str()) != FALSE, "remove fixture subfolder");
    check(RemoveDirectoryW(fixture) != FALSE, "remove empty fixture folder");
    return failures ? 1 : 0;
}
