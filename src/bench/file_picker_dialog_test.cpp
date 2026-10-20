// Explicitly opt in to short-lived, process-owned picker windows with --interaction.
#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../app/native_dialog_history.h"
#include "../ui/folder_picker_dialog.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace {
using namespace pulse::ui;
constexpr int kPathId = 101;
constexpr int kFilenameId = 102;
constexpr int kSearchId = 103;

HWND OwnPicker() {
    HWND found = nullptr;
    EnumWindows([](HWND hwnd, LPARAM context) -> BOOL {
        DWORD process = 0;
        GetWindowThreadProcessId(hwnd, &process);
        wchar_t name[80]{};
        GetClassNameW(hwnd, name, 80);
        if (process == GetCurrentProcessId() &&
            std::wstring_view(name) == L"PulseFolderPickerWindow") {
            *reinterpret_cast<HWND*>(context) = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

bool Send(HWND hwnd, UINT message, WPARAM wparam = 0, LPARAM lparam = 0) {
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process != GetCurrentProcessId()) return false;
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(hwnd, message, wparam, lparam,
                              SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result) != 0;
}

std::wstring Text(HWND hwnd) {
    wchar_t text[32768]{};
    Send(hwnd, WM_GETTEXT, std::size(text), reinterpret_cast<LPARAM>(text));
    return text;
}

bool SetText(HWND hwnd, const std::wstring& text) {
    return Send(hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

bool Until(const std::function<bool()>& ready, const std::atomic<bool>& done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (!done.load() && std::chrono::steady_clock::now() < end) {
        if (ready()) return true;
        // Poll a condition, rather than assuming an asynchronous operation has finished.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

struct Outcome { bool picked = false; bool driven = false; FilePickerResult result; };
using Driver = std::function<bool(HWND, const std::atomic<bool>&)>;

Outcome Run(const FolderPickerSpec& spec, const Driver& drive, bool legacy = false, HWND owner = nullptr) {
    Outcome outcome;
    std::atomic<bool> done = false;
    const HDESK desktop = GetThreadDesktop(GetCurrentThreadId());
    std::thread driver([&] {
        if (!SetThreadDesktop(desktop)) return;
        HWND hwnd = nullptr;
        if (Until([&] {
                hwnd = OwnPicker();
                return hwnd && IsWindowVisible(hwnd) && GetDlgItem(hwnd, kSearchId) &&
                    (spec.mode == PickerMode::Folder ||
                     (GetDlgItem(hwnd, kFilenameId) && IsWindowEnabled(GetDlgItem(hwnd, kFilenameId))));
            }, done)) {
            outcome.driven = drive(hwnd, done);
            if (outcome.driven) Until([&] { return !IsWindow(hwnd); }, done);
        }
        if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    });
    if (legacy) {
        std::wstring path;
        outcome.picked = ShowFolderPicker(owner, spec, false, {0.0f, 0.47f, 0.83f, 1.0f}, path);
        if (outcome.picked) outcome.result.paths.push_back(std::move(path));
    } else {
        outcome.picked = ShowFilePicker(owner, spec, false, {0.0f, 0.47f, 0.83f, 1.0f},
                                        outcome.result);
    }
    done = true;
    driver.join();
    return outcome;
}

struct Fixture {
    std::filesystem::path root;
    Fixture() {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        root = std::filesystem::path(temp) /
            (L"PulsePickerInteraction-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(root / L"nested");
        for (const auto* name : {L"alpha.txt", L"two words.txt", L"blocked.bin", L"background.png",
                                 L"nested\\inside.txt"}) {
            const auto path = root / name;
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("fixture creation");
            CloseHandle(file);
        }
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};

bool HasPaths(const Outcome& outcome, const std::vector<std::wstring>& expected) {
    if (!outcome.driven || !outcome.picked || outcome.result.paths.size() != expected.size())
        return false;
    for (size_t i = 0; i < expected.size(); ++i)
        if (!SamePickerPath(outcome.result.paths[i], expected[i])) return false;
    return true;
}
} // namespace

int RunFilePickerDialogTest(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--history-read")
        return SamePickerPath(pulse::app::ReadDialogHistory(argv[2], PickerMode::Save), argv[3]) ? 0 : 1;
    const bool history_mode = argc == 2 && std::wstring_view(argv[1]) == L"--history";
    const bool save_mode = argc == 2 && std::wstring_view(argv[1]) == L"--save";
    const bool input_surfaces = argc == 2 && std::wstring_view(argv[1]) == L"--input-surfaces";
    if (!history_mode && !save_mode && !input_surfaces && (argc != 2 || std::wstring_view(argv[1]) != L"--interaction")) {
        std::puts("Use --interaction, --input-surfaces or --save for scoped file picker regression cases.");
        return 2;
    }
    // Surface regression runs without activating any window on the user's desktop.
    if (history_mode || input_surfaces || save_mode) {
        const auto name = L"PulsePickerInputs_" + std::to_wstring(GetCurrentProcessId());
        HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop || !SetThreadDesktop(desktop)) return 3;
        // Windows releases this process-owned desktop when the test exits.
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 3;
    pulse::compat::EnableDpiAwareness();
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    int failures = 0;
    const auto check = [&](const char* name, bool pass) {
        std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name);
        std::fflush(stdout);
        if (!pass) ++failures;
    };
    try {
        Fixture fixture;
        if (history_mode) {
            FolderPickerSpec spec;
            spec.mode = PickerMode::Save;
            spec.initial_path = fixture.root.wstring();
            spec.filename = L"retained.txt";
            spec.listary_integration = true;
            std::wstring last;
            spec.on_directory_changed = [&](const std::wstring& path) { last = path; };
            const auto nested = (fixture.root / L"nested").wstring();
            auto outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>& done) {
                if (GetPropW(hwnd, L"Pulse.Picker.Listary.v1") != reinterpret_cast<HANDLE>(1)) return false;
                COPYDATASTRUCT data{0x504C5354, static_cast<DWORD>((nested.size() + 1) * sizeof(wchar_t)),
                    const_cast<wchar_t*>(nested.c_str())};
                DWORD_PTR accepted = 0;
                if (!SendMessageTimeoutW(hwnd, WM_COPYDATA, 1, reinterpret_cast<LPARAM>(&data),
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &accepted) || !accepted) return false;
                if (!Until([&] { return SamePickerPath(Text(GetDlgItem(hwnd, kPathId)), nested) &&
                        IsWindowEnabled(GetDlgItem(hwnd, kFilenameId)); }, done)) return false;
                if (Text(GetDlgItem(hwnd, kFilenameId)) != L"retained.txt") return false;
                wchar_t snapshot[32768]{};
                if (GetEnvironmentVariableW(L"PULSE_PICKER_TEST_SNAPSHOT", snapshot, 32768)) {
                    SetPropW(hwnd, L"Pulse.PickerTestSnapshot", snapshot);
                    Send(hwnd, WM_PAINT);
                }
                return Send(hwnd, WM_CLOSE);
            });
            check("Listary directory jump updates address and preserves Save filename", outcome.driven && !outcome.picked);
            check("last successfully visited folder survives Cancel", SamePickerPath(last, nested));
            check("write last directory", pulse::app::WriteDialogHistory(fixture.root.wstring(), PickerMode::Save, last));
            check("Open history stays independent", pulse::app::WriteDialogHistory(fixture.root.wstring(), PickerMode::File, fixture.root.wstring()) &&
                SamePickerPath(pulse::app::ReadDialogHistory(fixture.root.wstring(), PickerMode::Save), nested));
            wchar_t executable[32768]{};
            GetModuleFileNameW(nullptr, executable, 32768);
            std::wstring command = L"\"" + std::wstring(executable) + L"\" --picker-dialog-test --history-read \"" +
                fixture.root.wstring() + L"\" \"" + nested + L"\"";
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION child{};
            bool read_in_child = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startup, &child) != FALSE;
            if (read_in_child) {
                DWORD exit = 1;
                read_in_child = WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0 &&
                    GetExitCodeProcess(child.hProcess, &exit) && exit == 0;
                CloseHandle(child.hThread); CloseHandle(child.hProcess);
            }
            check("fresh process restores last directory", read_in_child);
            spec.initial_path = pulse::app::ReadDialogHistory(fixture.root.wstring(), PickerMode::Save);
            outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>&) {
                return SamePickerPath(Text(GetDlgItem(hwnd, kPathId)), nested) && Send(hwnd, WM_CLOSE);
            });
            check("next picker starts in remembered folder", outcome.driven && !outcome.picked);
            spec.initial_path = (fixture.root / L"removed-folder").wstring();
            spec.fallback_path = fixture.root.wstring();
            outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>&) {
                return SamePickerPath(Text(GetDlgItem(hwnd, kPathId)), fixture.root.wstring()) && Send(hwnd, WM_CLOSE);
            });
            check("unavailable remembered folder falls back to caller directory", outcome.driven && !outcome.picked);
            CoUninitialize();
            return failures ? 1 : 0;
        }
        if (save_mode) {
            FolderPickerSpec spec;
            spec.mode = PickerMode::Save;
            spec.initial_path = fixture.root.wstring();
            spec.filename = L"new report";
            spec.filters = {{L"Text", L"*.txt"}};
            spec.allow_multiselect = true;
            int ready_calls = 0;
            bool ready_visible = false;
            spec.on_ready = [&](HWND hwnd) {
                ++ready_calls;
                ready_visible = IsWindow(hwnd) && IsWindowVisible(hwnd) && Text(hwnd) == L"Save";
            };
            auto outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
                return Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN);
            });
            check("save ready callback receives visible window once", ready_calls == 1 && ready_visible);
            check("save accepts new filename without creating file", HasPaths(outcome, {(fixture.root / L"new report.txt").wstring()}) &&
                !std::filesystem::exists(fixture.root / L"new report.txt"));
            spec.filename = L"nested";
            outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>& done) {
                HWND filename = GetDlgItem(hwnd, kFilenameId);
                return Send(filename, WM_KEYDOWN, VK_RETURN) &&
                    Until([&] { return SamePickerPath(Text(GetDlgItem(hwnd, kPathId)), (fixture.root / L"nested").wstring()) &&
                        IsWindowEnabled(filename); }, done) &&
                    SetText(filename, L"new inside") && Send(filename, WM_KEYDOWN, VK_RETURN);
            });
            check("save directory navigates then accepts new file", HasPaths(outcome, {(fixture.root / L"nested\\new inside.txt").wstring()}));
            spec.filename = L"missing\\new.txt";
            outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>& done) {
                HWND filename = GetDlgItem(hwnd, kFilenameId);
                return Send(filename, WM_KEYDOWN, VK_RETURN) &&
                    Until([&] { return IsWindow(hwnd) && IsWindowEnabled(filename); }, done) &&
                    SetText(filename, L"alpha.txt") && Send(filename, WM_KEYDOWN, VK_RETURN);
            });
            check("save invalid parent allows retry with existing file", HasPaths(outcome, {(fixture.root / L"alpha.txt").wstring()}) &&
                std::filesystem::file_size(fixture.root / L"alpha.txt") == 0);
            spec.filename = L"\"one.txt\" \"two.txt\"";
            outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
                HWND filename = GetDlgItem(hwnd, kFilenameId);
                return Send(filename, WM_KEYDOWN, VK_RETURN) && IsWindowEnabled(filename) &&
                    SetText(filename, L"final.txt") && Send(filename, WM_KEYDOWN, VK_RETURN);
            });
            check("save ignores multiselect request and rejects multiple names", HasPaths(outcome, {(fixture.root / L"final.txt").wstring()}));
            spec.filename = L"cancelled.txt";
            outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
                return Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_ESCAPE);
            });
            check("save Escape cancels without writing", outcome.driven && !outcome.picked && outcome.result.paths.empty() &&
                !std::filesystem::exists(fixture.root / L"cancelled.txt"));
            spec.filename.clear();
            outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>& done) {
                HWND search = GetDlgItem(hwnd, kSearchId);
                HWND filename = GetDlgItem(hwnd, kFilenameId);
                return SetText(search, L"alpha") && Send(search, WM_KEYDOWN, VK_RETURN) &&
                    Until([&] { return Send(hwnd, WM_KEYDOWN, VK_HOME) && Text(filename) == L"alpha.txt"; }, done) &&
                    Send(filename, WM_KEYDOWN, VK_RETURN);
            });
            check("save selecting an existing row fills filename", HasPaths(outcome, {(fixture.root / L"alpha.txt").wstring()}));
            HWND owner = CreateWindowExW(0, L"STATIC", L"save owner fixture", WS_POPUP, 0, 0, 100, 100,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            check("save owner fixture created", owner != nullptr);
            if (owner) {
                bool owner_disabled_while_ready = false;
                spec.on_ready = [&](HWND) { owner_disabled_while_ready = !IsWindowEnabled(owner); };
                const auto cancel = [](HWND hwnd, const std::atomic<bool>&) { return Send(hwnd, WM_CLOSE); };
                EnableWindow(owner, FALSE);
                outcome = Run(spec, cancel, false, owner);
                check("save keeps originally disabled owner disabled after close", outcome.driven && !outcome.picked &&
                    owner_disabled_while_ready && !IsWindowEnabled(owner));
                EnableWindow(owner, TRUE);
                owner_disabled_while_ready = false;
                outcome = Run(spec, cancel, false, owner);
                check("save restores originally enabled owner after close", outcome.driven && !outcome.picked &&
                    owner_disabled_while_ready && IsWindowEnabled(owner));
                DestroyWindow(owner);
            }
            CoUninitialize();
            return failures ? 1 : 0;
        }
        if (input_surfaces) {
            FolderPickerSpec spec;
            spec.mode = PickerMode::File;
            spec.initial_path = fixture.root.wstring();
            const auto outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>&) {
                wchar_t snapshot[32768]{};
                if (GetEnvironmentVariableW(L"PULSE_PICKER_TEST_SNAPSHOT", snapshot, 32768)) {
                    SetPropW(hwnd, L"Pulse.PickerTestSnapshot", snapshot);
                    Send(hwnd, WM_PAINT);
                    check("embedded picker snapshot is saved", std::filesystem::exists(snapshot));
                }
                bool passed = true;
                for (const int id : {kPathId, kFilenameId, kSearchId}) {
                    HWND edit = GetDlgItem(hwnd, id);
                    DWORD flags = 0;
                    BYTE alpha = 0;
                    const bool redirected = edit && GetLayeredWindowAttributes(edit, nullptr, &alpha, &flags) &&
                        (flags & LWA_ALPHA) && alpha == 255;
                    check("picker input inherits redirected surface from shared control", redirected);
                    const bool typed = edit && SetText(edit, L"Typed 中文") && Text(edit) == L"Typed 中文";
                    check("picker input retains Unicode editing", typed);
                    passed = passed && redirected && typed;
                }
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return passed;
            });
            check("picker input check closes without selecting or changing files", outcome.driven && !outcome.picked);
            const auto picked = Run(spec, [&](HWND hwnd, const std::atomic<bool>&) {
                HWND filename = GetDlgItem(hwnd, kFilenameId);
                return filename && SetText(filename, L"alpha.txt") && Send(filename, WM_KEYDOWN, VK_RETURN);
            });
            check("embedded picker accepts a typed filename", HasPaths(picked, {(fixture.root / L"alpha.txt").wstring()}));
            CoUninitialize();
            return failures ? 1 : 0;
        }
        const auto path = [&](const wchar_t* name) { return (fixture.root / name).wstring(); };
        FolderPickerSpec spec;
        spec.mode = PickerMode::File;
        spec.initial_path = fixture.root.wstring();
        spec.filename = L"alpha.txt";
        spec.filters = {{L"Text files", L"*.txt"}};
        auto outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>& done) {
            return Until([&] { return SamePickerPath(Text(GetDlgItem(hwnd, kPathId)),
                                                     spec.initial_path); }, done) &&
                Text(GetDlgItem(hwnd, kFilenameId)) == spec.filename &&
                Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN);
        });
        check("initial directory and filename", HasPaths(outcome, {path(L"alpha.txt")}));

        spec.allow_multiselect = true;
        spec.filename = L"\"alpha.txt\" \"two words.txt\"";
        outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
            return Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN);
        });
        check("quoted multiple filenames", HasPaths(outcome,
              {path(L"alpha.txt"), path(L"two words.txt")}));

        spec.filename = L"blocked.bin";
        outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>& done) {
            HWND filename = GetDlgItem(hwnd, kFilenameId);
            return Send(filename, WM_KEYDOWN, VK_RETURN) &&
                Until([&] { return IsWindow(hwnd) && IsWindowEnabled(filename); }, done) &&
                SetText(filename, L"alpha.txt") && Send(filename, WM_KEYDOWN, VK_RETURN);
        });
        check("filter rejects disallowed file and allows retry",
              HasPaths(outcome, {path(L"alpha.txt")}));

        spec.filename = L"nested";
        outcome = Run(spec, [&](HWND hwnd, const std::atomic<bool>& done) {
            HWND filename = GetDlgItem(hwnd, kFilenameId);
            return Send(filename, WM_KEYDOWN, VK_RETURN) &&
                Until([&] { return SamePickerPath(Text(GetDlgItem(hwnd, kPathId)),
                                                 path(L"nested")) && IsWindowEnabled(filename); }, done) &&
                SetText(filename, L"inside.txt") && Send(filename, WM_KEYDOWN, VK_RETURN);
        });
        check("typed directory navigates before file submission",
              HasPaths(outcome, {path(L"nested\\inside.txt")}));

        spec.filename = L"alpha.txt";
        outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
            HWND search = GetDlgItem(hwnd, kSearchId);
            return SetText(search, L"alpha") && Send(search, WM_KEYDOWN, VK_DOWN) &&
                Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_ESCAPE);
        });
        check("search and arrow key do not submit; Escape cancels",
              outcome.driven && !outcome.picked && outcome.result.paths.empty());

        spec.filename = L"blocked.bin";
        outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
            HWND filename = GetDlgItem(hwnd, kFilenameId);
            return Send(filename, WM_KEYDOWN, VK_RETURN) && Send(hwnd, WM_CLOSE);
        });
        check("cancel rejected candidate validation", outcome.driven && !outcome.picked &&
              outcome.result.paths.empty());
        spec.filename = L"two words.txt";
        outcome = Run(spec, [](HWND hwnd, const std::atomic<bool>&) {
            return Send(GetDlgItem(hwnd, kFilenameId), WM_KEYDOWN, VK_RETURN);
        });
        check("new request excludes previous validation result",
              HasPaths(outcome, {path(L"two words.txt")}));

        FolderPickerSpec folder;
        folder.mode = PickerMode::Folder;
        folder.initial_path = fixture.root.wstring();
        outcome = Run(folder, [](HWND hwnd, const std::atomic<bool>& done) {
            // The folder mode has no filename control. Enter is harmless until
            // the directory listing has verified that the current folder exists.
            const bool closed = Until([&] {
                if (!IsWindow(hwnd)) return true;
                Send(hwnd, WM_KEYDOWN, VK_RETURN);
                return !IsWindow(hwnd);
            }, done);
            return closed || done.load();
        }, true);
        check("legacy folder entry confirms current directory",
              HasPaths(outcome, {fixture.root.wstring()}));

        FolderPickerSpec image;
        image.mode = PickerMode::Image;
        image.initial_path = fixture.root.wstring();
        image.filename = L"alpha.txt";
        image.filters = {{L"Text and images", L"*.txt;*.png"}};
        outcome = Run(image, [](HWND hwnd, const std::atomic<bool>& done) {
            HWND filename = GetDlgItem(hwnd, kFilenameId);
            return Send(filename, WM_KEYDOWN, VK_RETURN) &&
                Until([&] { return IsWindow(hwnd) && IsWindowEnabled(filename); }, done) &&
                SetText(filename, L"background.png") && Send(filename, WM_KEYDOWN, VK_RETURN);
        }, true);
        check("legacy image entry rejects text despite filter and accepts image extension",
              HasPaths(outcome, {path(L"background.png")}));
    } catch (const std::exception& error) {
        std::printf("[FAIL] fixture: %s\n", error.what());
        ++failures;
    }
    CoUninitialize();
    return failures ? 1 : 0;
}
