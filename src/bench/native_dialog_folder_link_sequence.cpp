// Compile the real implementation into this test translation unit so both
// navigations use the same Shared object and RecoveryRecord. This test adapter
// is excluded from the release build.
#include "../app/native_dialog_folder_link.cpp"

#include <cstdio>
#include <string>

namespace {
bool FixtureReached(const std::wstring& status, const std::wstring& folder) {
    HANDLE file = CreateFileW(status.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char bytes[65536]{}; DWORD read = 0;
    const bool loaded = ReadFile(file, bytes, sizeof(bytes) - 1, &read, nullptr) != FALSE;
    CloseHandle(file);
    if (!loaded || !read) return false;
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, folder.data(),
        static_cast<int>(folder.size()), nullptr, 0, nullptr, nullptr);
    if (!length) return false;
    std::string expected(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, folder.data(), static_cast<int>(folder.size()),
                            expected.data(), length, nullptr, nullptr) != length) return false;
    std::string value(bytes, read);
    if (value.starts_with("\xEF\xBB\xBF")) value.erase(0, 3);
    return value.starts_with(expected + '\n');
}
}

bool RunFolderLinkSequence(HWND dialog, HDESK desktop, const std::wstring& first,
                           const std::wstring& second, const std::wstring& status,
                           HWND initial_focus) {
    using namespace pulse::app;
    if (!IsPrivateTestDesktop(desktop) || (GetThreadDesktop(GetCurrentThreadId()) != desktop &&
        !SetThreadDesktop(desktop))) return false;
    WindowIdentity identity{};
    if (!DialogIdentity(dialog, identity) || !initial_focus || !IsChild(dialog, initial_focus) ||
        !SameThread(initial_focus, identity)) return false;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return false;
    Microsoft::WRL::ComPtr<IUIAutomation> automation;
    auto shared = std::make_shared<NativeDialogFolderLink::Shared>();
    auto record = std::make_unique<RecoveryRecord>();
    shared->recovery = record.get();
    shared->test_allow_nonforeground = true;
    bool first_issued = false, reached = false, focus = false, second_issued = false;
    LONG first_stage = -1, second_stage = -1;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&automation))) && automation) {
        Navigate(identity, first, shared, automation.Get());
        first_issued = shared->test_issued;
        first_stage = InterlockedCompareExchange(&record->stage, 0, 0);
        const ULONGLONG deadline = GetTickCount64() + 5000;
        while (first_issued && GetTickCount64() < deadline && !reached) {
            reached = FixtureReached(status, first);
            if (!reached) Sleep(50);
        }
        // The user has returned to the dialog after visiting Pulse; emulate
        // that action only on this fixture and let real production focus
        // checks reject an unready or changed control.
        if (reached && SameIdentity(identity) && IsChild(dialog, initial_focus) &&
            AttachThreadInput(GetCurrentThreadId(), identity.thread, TRUE)) {
            SetActiveWindow(dialog);
            SetFocus(initial_focus);
            AttachThreadInput(GetCurrentThreadId(), identity.thread, FALSE);
            focus = FocusIs(identity.thread, initial_focus);
        }
        if (focus) {
            shared->test_issued = false;
            Navigate(identity, second, shared, automation.Get());
            second_issued = shared->test_issued;
            second_stage = InterlockedCompareExchange(&record->stage, 0, 0);
        }
    }
    std::printf("first_issued=%d first_stage=%ld first_folder_reached=%d focus_restored=%d second_issued=%d second_stage=%ld\n",
        first_issued ? 1 : 0, first_stage, reached ? 1 : 0, focus ? 1 : 0, second_issued ? 1 : 0, second_stage);
    automation.Reset(); CoUninitialize();
    return first_issued && first_stage == 0 && reached && focus && second_issued && second_stage == 0;
}
