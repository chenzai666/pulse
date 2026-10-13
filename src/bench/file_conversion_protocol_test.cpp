#include "../app/file_conversion_integration.h"
#include "../app/file_conversion_protocol.h"
#include "../common/json_utils.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pulse::app;
using namespace pulse::app::file_conversion;

namespace {

constexpr UINT kNotify = WM_APP + 90;
int failures = 0;
std::ofstream* check_log = nullptr;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (check_log) *check_log << (ok ? "[PASS] " : "[FAIL] ") << what << std::endl;
    if (!ok) ++failures;
}

std::wstring Module() {
    wchar_t module[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    return count && count < std::size(module) ? std::wstring(module, count) : std::wstring{};
}

std::wstring Wide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string Utf8(const std::wstring& text) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::string Quote(const std::wstring& text) {
    std::wstring escaped;
    pulse::json::Escape(text, escaped);
    return "\"" + Utf8(escaped) + "\"";
}

void ProtocolTests() {
    Request request;
    request.generation = 42;
    request.inputs = {L"C:\\目录\\quote\" [a] {b}\n\t.txt", L"\\\\server\\share\\表情\xD83D\xDE03.txt"};
    std::string encoded;
    Check(EncodeRequest(request, encoded) && std::count(encoded.begin(), encoded.end(), '\n') == 1 &&
              encoded.ends_with("\n") && pulse::json::ValidConfigObject(Wide(encoded)),
          "requests escape quotes, controls, nested-looking paths and surrogate pairs into one JSON line");
    std::vector<std::wstring> inputs;
    Check(pulse::json::ConfigSyntax(Wide(encoded), false).StringArrayMember(L"inputs", inputs) && inputs == request.inputs,
          "escaped request input paths round-trip without changing selection");
    request.action = Action::Cancel;
    request.job_id = 41;
    Check(EncodeRequest(request, encoded) && pulse::json::ExtractString(Wide(encoded), L"jobId") == L"41",
          "cancel has its own request id and points at the immutable conversion job id");
    FileConversionEvent event;
    Check(DecodeEvent(R"({"id":"42","type":"capabilities","inputs":[{"source":"C:\\test.txt","format":"txt"}],"targets":[{"id":"png","label":"图片 \"A\" \u4e2d\u6587","extension":"png","routes":[{"source":"txt","steps":[{"engine":"imagemagick","from":"txt","to":"png"}]}]}],"limitations":["line\nnext"]})", event) &&
              event.kind == FileConversionEventKind::Capabilities && event.targets.size() == 1 &&
              event.targets[0].extension == L"png" && event.targets[0].label == L"图片 \"A\" 中文" &&
              event.limitations[0] == L"line\nnext",
          "final capabilities schema accepts object engine steps and decodes Unicode labels and controls");
    Check(DecodeEvent(R"({"id":"42","type":"started","files":2})", event) && event.file_count == 2,
          "started uses the host's numeric file count");
    Check(DecodeEvent(R"({"id":"42","type":"progress","progress":0.5,"files":[{"source":"C:\\a.txt","state":"converting","progress":0.25}]})", event) &&
              event.progress == 0.5 && event.files.size() == 1 && event.files[0].progress == 0.25,
          "progress preserves overall and per-file fractions");
    Check(DecodeEvent(R"({"id":"42","type":"completed","state":"completedwitherrors","files":[{"source":"C:\\a.txt","state":"done","outputs":["C:\\out\\a.png"],"errorKey":null,"error":null},{"source":"C:\\b.txt","state":"failed","outputs":[],"errorKey":"engine_failed","error":"why"}]})", event) &&
              event.state == L"completedwitherrors" && event.files.size() == 2 && event.files[0].error.empty() &&
              event.files[1].error_key == L"engine_failed",
          "batch results retain partial failures and nullable success errors");
    Check(DecodeEvent(R"({"id":"42","type":"error","code":"output_unavailable","detail":"拒绝访问"})", event) &&
              event.kind == FileConversionEventKind::Failed && event.error_key == L"output_unavailable" && event.error_text == L"拒绝访问",
          "host code/detail errors reach the native event model");
    for (const auto* control : {R"({"id":"","type":"ready","protocol":1,"version":"1.10.2"})",
                               R"({"id":"cancel-token","type":"cancelled","jobId":"42","cancelled":true})",
                               R"({"id":"bye","type":"stopping"})"}) {
        Check(DecodeEvent(control, event) && event.generation == 0, "host control notifications are ignored safely");
    }
    for (const auto* invalid : {R"({"id":"42","id":"43","type":"started","files":1})",
                               R"({"id":"0042","type":"started","files":1})",
                               R"({"id":"42","type":"progress","progress":1.01})",
                               R"({"id":"42","type":"progress","progress":1e999})",
                               R"({"id":"42","type":"started","files":129})",
                               R"({"id":"42","type":"capabilities","targets":[{"id":"png","label":"\uD800"}]})",
                               R"({"id":"42","type":"completed","files":[{"source":"x","state":"done",}]})",
                               R"({"id":"42","type":"completed","files":[{"source":"C:\\a.txt","state":"done","outputs":["https://example.com/a.png"]}]})",
                               R"({"id":"42","type":"capabilities","targets":[{"id":"png","label":"x","metadata":"\uDC00"}]})"}) {
        Check(!DecodeEvent(invalid, event), "malformed, duplicate, out-of-range or invalid-Unicode response is rejected");
    }
    std::string malformed_utf8 = "{\"id\":\"42\",\"type\":\"error\",\"detail\":\"";
    malformed_utf8 += static_cast<char>(0xC0);
    malformed_utf8 += static_cast<char>(0xAF);
    malformed_utf8 += "\"}";
    Check(!DecodeEvent(malformed_utf8, event) && !DecodeEvent(std::string(kMaxProtocolLine + 1, 'x'), event),
          "invalid UTF-8 and oversized IPC responses are rejected before parsing");
    Check(AbsolutePathShape(L"C:\\目录\\a.txt") && AbsolutePathShape(L"\\\\?\\C:\\long\\a.txt") &&
              AbsolutePathShape(L"\\\\server\\share\\a.txt") && AbsolutePathShape(L"\\\\?\\UNC\\server\\share\\a.txt") &&
              !AbsolutePathShape(L"relative.txt") && !AbsolutePathShape(L"\\\\.\\pipe\\test") &&
              !AbsolutePathShape(L"C:\\a.txt:stream") && !AbsolutePathShape(L"https://example.com/a.txt"),
          "only absolute drive/UNC paths are accepted; devices, streams and network URLs are rejected");
}

bool WriteLine(const std::string& line) {
    DWORD written = 0;
    return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.data(), static_cast<DWORD>(line.size()), &written, nullptr) &&
        written == line.size();
}

bool ReadLine(std::string& line) {
    line.clear();
    char c = 0;
    DWORD bytes = 0;
    while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), &c, 1, &bytes, nullptr) && bytes) {
        if (c == '\n') return true;
        if (line.size() >= kMaxProtocolLine) return false;
        line += c;
    }
    return false;
}

DWORD Spawn(const std::wstring& argument) {
    std::wstring command = L"\"" + Module() + L"\" " + argument;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(Module().c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process)) return 0;
    const DWORD pid = process.dwProcessId;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return pid;
}

int FixtureHost() {
    const fs::path directory = fs::path(Module()).parent_path();
    WriteLine("{\"id\":\"\",\"type\":\"ready\",\"protocol\":1,\"version\":\"fixture\"}\n");
    std::string line;
    while (ReadLine(line)) {
        const std::wstring text = Wide(line);
        if (!pulse::json::ValidConfigObject(text)) return 2;
        std::ofstream(directory / L"trace.jsonl", std::ios::binary | std::ios::app) << line << '\n';
        const std::wstring id = pulse::json::ExtractString(text, L"id");
        const std::wstring action = pulse::json::ExtractString(text, L"action");
        if (action == L"shutdown") return 0;
        if (action == L"cancel") continue; // exercise native forced cleanup, even with an uncooperative host
        if (action == L"capabilities") {
            Sleep(500);
            const std::string response = "{\"id\":" + Quote(id) + ",\"type\":\"capabilities\",\"targets\":[{\"id\":\"png\",\"label\":\"PNG\",\"extension\":\"png\",\"routes\":[{\"source\":\"txt\",\"steps\":[{\"engine\":\"fixture\",\"from\":\"txt\",\"to\":\"png\"}]}]}]}\n";
            const size_t middle = response.size() / 2;
            if (!WriteLine(response.substr(0, middle))) return 3;
            Sleep(10);
            if (!WriteLine(response.substr(middle))) return 3;
            continue;
        }
        if (action != L"convert") return 4;
        const auto inputs = pulse::json::ExtractStringArray(text, L"inputs");
        if (inputs.empty()) return 5;
        const int quality = pulse::json::ExtractInt(text, L"quality");
        DWORD engine = 0, grandchild = 0;
        if (quality == 90) {
            const auto pid_file = directory / L"grandchild-pid.txt";
            fs::remove(pid_file);
            engine = Spawn(L"--fixture-engine \"" + pid_file.wstring() + L"\"");
            const ULONGLONG deadline = GetTickCount64() + 3000;
            while (!grandchild && GetTickCount64() < deadline) {
                if (fs::exists(pid_file)) std::ifstream(pid_file) >> grandchild;
                if (!grandchild) Sleep(10);
            }
        }
        WriteLine("{\"id\":" + Quote(id) + ",\"type\":\"started\",\"files\":1,\"limitations\":[\"host:" +
            std::to_string(GetCurrentProcessId()) + "\",\"engine:" + std::to_string(engine) + "\",\"grandchild:" +
            std::to_string(grandchild) + "\"]}\n");
        WriteLine("{\"id\":" + Quote(id) + ",\"type\":\"progress\",\"progress\":0.5,\"files\":[{\"source\":" +
            Quote(inputs[0]) + ",\"state\":\"converting\",\"progress\":0.5}]}\n");
        if (quality != 90) {
            const std::wstring output = pulse::json::ExtractString(text, L"outputDirectory") + L"\\out.png";
            WriteLine("{\"id\":" + Quote(id) + ",\"type\":\"completed\",\"state\":\"completed\",\"files\":[{\"source\":" +
                Quote(inputs[0]) + ",\"state\":\"done\",\"outputs\":[" + Quote(output) + "],\"errorKey\":null,\"error\":null}]}\n");
        }
    }
    return 0;
}

std::optional<FileConversionEvent> WaitEvent(FileConversionIntegration& client, HWND owner,
                                            FileConversionEventKind kind, uint64_t generation,
                                            std::vector<FileConversionEvent>& received) {
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, owner, kNotify, kNotify, PM_REMOVE)) {
            FileConversionEvent event;
            if (!client.TakeEvent(static_cast<UINT_PTR>(message.lParam), event)) continue;
            const bool match = event.kind == kind && event.generation == generation;
            received.push_back(event);
            if (match) return event;
        }
        Sleep(5);
    }
    return std::nullopt;
}

DWORD Pid(const FileConversionEvent& event, const wchar_t* key) {
    const std::wstring prefix = std::wstring(key) + L":";
    for (const auto& limitation : event.limitations)
        if (limitation.starts_with(prefix)) return static_cast<DWORD>(std::stoul(limitation.substr(prefix.size())));
    return 0;
}

int ClientTests(const fs::path& root) {
    std::ofstream results(root / L"client-results.log");
    check_log = &results;
    WNDCLASSW window_class{};
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpfnWndProc = DefWindowProcW;
    window_class.lpszClassName = L"PulseFileeClientTest";
    RegisterClassW(&window_class);
    HWND owner = CreateWindowExW(0, window_class.lpszClassName, L"", WS_POPUP, -30000, -30000,
        200, 200, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!owner) { Check(false, "hidden fixture owner creation"); return 1; }
    const fs::path input = root / L"选择 中文 [a].txt";
    const fs::path other = root / L"other.txt";
    const fs::path output = root / L"outputs";
    std::ofstream(input) << "fixture";
    std::ofstream(other) << "other";
    fs::create_directory(output);
    FileConversionIntegration client;
    Check(client.Initialize(owner, kNotify), "native asynchronous session initializes for its own hidden owner window");
    const ULONGLONG begin = GetTickCount64();
    const uint64_t obsolete = client.Capabilities({other.wstring()});
    const uint64_t current = client.Capabilities({input.wstring()});
    Check(obsolete && current && obsolete != current && GetTickCount64() - begin < 400,
          "capability submissions return without waiting for the delayed host response");
    std::vector<FileConversionEvent> received;
    const auto caps = WaitEvent(client, owner, FileConversionEventKind::Capabilities, current, received);
    Check(caps && caps->inputs == std::vector<std::wstring>{input.wstring()} && caps->targets[0].extension == L"png" &&
              std::none_of(received.begin(), received.end(), [obsolete](const auto& e) { return e.generation == obsolete; }),
          "fragmented ready/capability replies are decoded, and obsolete menu generations are filtered");
    std::vector<std::wstring> chosen{input.wstring()};
    const uint64_t job = client.Start(chosen, L"png", output.wstring());
    chosen[0] = other.wstring();
    const auto started = WaitEvent(client, owner, FileConversionEventKind::Started, job, received);
    Check(started && started->inputs == std::vector<std::wstring>{input.wstring()} && client.busy(),
          "a conversion owns its submitted input snapshot rather than the caller's later selection");
    const uint64_t another_caps = client.Capabilities(chosen);
    Check(WaitEvent(client, owner, FileConversionEventKind::Capabilities, another_caps, received).has_value() && client.busy(),
          "capabilities use a separate generation while conversion remains active");
    HANDLE host = started ? OpenProcess(SYNCHRONIZE, FALSE, Pid(*started, L"host")) : nullptr;
    HANDLE engine = started ? OpenProcess(SYNCHRONIZE, FALSE, Pid(*started, L"engine")) : nullptr;
    HANDLE grandchild = started ? OpenProcess(SYNCHRONIZE, FALSE, Pid(*started, L"grandchild")) : nullptr;
    Check(host && engine && grandchild, "fixture host actually creates an engine and its grandchild inside the native job");
    client.Cancel();
    const auto stopped = WaitEvent(client, owner, FileConversionEventKind::Stopped, job, received);
    Check(stopped && !client.busy() && host && engine && grandchild &&
              WaitForSingleObject(host, 5000) == WAIT_OBJECT_0 && WaitForSingleObject(engine, 5000) == WAIT_OBJECT_0 &&
              WaitForSingleObject(grandchild, 5000) == WAIT_OBJECT_0,
          "cancel forcibly terminates host, engine and grandchild even when the host ignores cancellation");
    for (const HANDLE handle : {host, engine, grandchild}) if (handle) CloseHandle(handle);
    for (int repeat = 0; repeat < 2; ++repeat) {
        const uint64_t completed_job = client.Start({input.wstring()}, L"png", output.wstring(), 91);
        const auto completed = WaitEvent(client, owner, FileConversionEventKind::Completed, completed_job, received);
        const bool had_started = std::any_of(received.begin(), received.end(), [completed_job](const auto& e) {
            return e.generation == completed_job && e.kind == FileConversionEventKind::Started;
        });
        Check(completed && completed->files.size() == 1 && completed->files[0].outputs[0] == (output / L"out.png").wstring() &&
                  completed->inputs[0] == input.wstring() && had_started && !client.busy(),
              "successive jobs on a persistent host each emit Started and retain their own output results");
    }
    const uint64_t retirement_job = client.Start({input.wstring()}, L"png", output.wstring());
    const auto retirement_started = WaitEvent(client, owner, FileConversionEventKind::Started, retirement_job, received);
    HANDLE retired_host = retirement_started ? OpenProcess(SYNCHRONIZE, FALSE, Pid(*retirement_started, L"host")) : nullptr;
    HANDLE retired_engine = retirement_started ? OpenProcess(SYNCHRONIZE, FALSE, Pid(*retirement_started, L"engine")) : nullptr;
    HANDLE retired_grandchild = retirement_started ? OpenProcess(SYNCHRONIZE, FALSE, Pid(*retirement_started, L"grandchild")) : nullptr;
    DestroyWindow(owner);
    owner = nullptr;
    Check(!client.Capabilities({input.wstring()}) && retired_host && retired_engine && retired_grandchild &&
              WaitForSingleObject(retired_host, 5000) == WAIT_OBJECT_0 &&
              WaitForSingleObject(retired_engine, 5000) == WAIT_OBJECT_0 &&
              WaitForSingleObject(retired_grandchild, 5000) == WAIT_OBJECT_0,
          "retiring the owner invalidates new tokens and automatically closes the complete host job");
    client.Shutdown();
    Check(!client.running() && !client.busy(), "Shutdown retires its UI session immediately without a thread join");
    for (const HANDLE handle : {retired_host, retired_engine, retired_grandchild}) if (handle) CloseHandle(handle);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    return failures ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && std::wstring(argv[1]) == L"--owner-pid") return FixtureHost();
    if (argc == 3 && std::wstring(argv[1]) == L"--fixture-engine") {
        const DWORD pid = Spawn(L"--fixture-grandchild");
        std::ofstream(fs::path(argv[2])) << pid;
        Sleep(60000);
        return 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--fixture-grandchild") { Sleep(60000); return 0; }
    if (argc == 3 && std::wstring(argv[1]) == L"--fixture-client") return ClientTests(argv[2]);
    ProtocolTests();
    const fs::path root = fs::temp_directory_path() /
        (L"pulse-filee-client-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const fs::path conversion = root / L"conversion";
    fs::create_directories(conversion);
    const fs::path runner = root / L"client-test.exe";
    fs::copy_file(Module(), runner);
    fs::copy_file(Module(), conversion / L"Pulse.Filee.Host.exe");
    std::wstring command = L"\"" + runner.wstring() + L"\" --fixture-client \"" + root.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    bool passed = false;
    if (CreateProcessW(runner.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        const bool done = WaitForSingleObject(process.hProcess, 55000) == WAIT_OBJECT_0;
        DWORD code = 1;
        if (done) GetExitCodeProcess(process.hProcess, &code);
        else { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000); }
        passed = done && code == 0;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    Check(passed, "isolated native client IPC and job cleanup regression");
    if (std::ifstream log{root / L"client-results.log"}; log) {
        std::string line;
        while (std::getline(log, line)) std::printf("%s\n", line.c_str());
    }
    if (root.parent_path() == fs::temp_directory_path() && root.filename().wstring().starts_with(L"pulse-filee-client-"))
        fs::remove_all(root);
    return failures ? 1 : 0;
}
