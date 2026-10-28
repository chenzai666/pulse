#include "../ui/folder_picker_listary_protocol.h"

#include <cstdio>
#include <vector>

int main() {
    using namespace pulse::ui;
    int failures = 0;
    auto check = [&](const char* name, bool pass) {
        std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
        if (!pass) ++failures;
    };
    auto parse = [](const std::wstring& path, uintptr_t operation = 1) {
        return ParsePickerListaryRequest(kPickerListaryData, operation, path.c_str(),
                                         (path.size() + 1) * sizeof(wchar_t));
    };
    check("drive directory", parse(L"C:\\").has_value());
    check("UNC share", parse(L"\\\\server\\share\\目录").has_value());
    const auto file = parse(L"C:\\Users\\文件 name.txt", 2);
    check("file operation preserves path", file && file->file && file->path == L"C:\\Users\\文件 name.txt");
    check("directory operation", parse(L"D:\\Folder") && !parse(L"D:\\Folder")->file);
    for (const auto* path : {L"", L"folder", L"C:relative", L"\\rooted", L"1:\\path",
         L"\\\\?\\C:\\file", L"\\\\.\\pipe\\x", L"\\??\\C:\\file", L"\\\\server",
         L"C:\\file:stream", L"C:\\NUL.txt", L"C:\\CON .txt", L"C:\\COM1", L"C:\\CONIN$", L"C:\\a\\..\\b",
         L"C:\\double\\\\path", L"C:\\bad\nname", L"C:\\bad\"name", L"C:\\bad*name",
         L"C:/forward", L"C:\\trailing. ", L"\\\\server\\share:stream"}) {
        check("reject nonordinary or device path", !parse(path));
    }
    const wchar_t valid[] = L"C:\\ok";
    check("reject bad tag", !ParsePickerListaryRequest(0, 1, valid, sizeof(valid)));
    check("reject bad operation", !ParsePickerListaryRequest(kPickerListaryData, 3, valid, sizeof(valid)));
    check("reject null data", !ParsePickerListaryRequest(kPickerListaryData, 1, nullptr, sizeof(valid)));
    check("reject odd byte length", !ParsePickerListaryRequest(kPickerListaryData, 1, valid, sizeof(valid) - 1));
    check("reject unterminated text", !ParsePickerListaryRequest(kPickerListaryData, 1, valid, sizeof(valid) - 2));
    check("reject short data", !ParsePickerListaryRequest(kPickerListaryData, 1, valid, 2));
    check("reject overlarge data", !ParsePickerListaryRequest(kPickerListaryData, 1, valid, kPickerListaryMaxBytes + 2));
    std::wstring nul = L"C:\\ok";
    nul.push_back(L'\0');
    nul += L"hidden";
    check("reject embedded nul", !parse(nul));
    check("reject unmatched surrogate", !parse(std::wstring(L"C:\\") + wchar_t(0xD800)));
    check("accept surrogate pair", parse(std::wstring(L"C:\\") + wchar_t(0xD83D) + wchar_t(0xDE00)).has_value());
    std::vector<unsigned char> unaligned(sizeof(valid) + 1);
    std::memcpy(unaligned.data() + 1, valid, sizeof(valid));
    check("accept unaligned bounded UTF16", ParsePickerListaryRequest(kPickerListaryData, 1,
          unaligned.data() + 1, sizeof(valid)).has_value());
    return failures ? 1 : 0;
}
