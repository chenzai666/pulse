#include "../common/preview_packs.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace pulse::packs;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

void Write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

void MakePack(const fs::path& root, PackId id, const char* version) {
    const fs::path base = root / PackKey(id);
    Write(base / L"installed.json", std::string("{\"version\":\"") + version + "\",\"dir\":\"" + version + "\"}");
    Write(base / fs::path(version) / PackMainTool(id), "MZ");
}

int RunFixture(const fs::path& root) {
    const fs::path profile = root / L"private profile";
    SetEnvironmentVariableW(L"LOCALAPPDATA", profile.c_str());
    const fs::path bundles = root / L"packs";
    Check(BundledPacksRoot() == bundles.wstring(), "bundled directory follows executable, including spaces");
    Check(PacksRoot() == (profile / pulse::edition::kDataFolder / L"packs").wstring(), "user directory follows edition identity");
    const fs::path other = root / L"unrelated working directory";
    fs::create_directories(other);
    SetCurrentDirectoryW(other.c_str());
    for (uint32_t i = 0; i < kPackCount; ++i) {
        const auto id = static_cast<PackId>(i);
        MakePack(bundles, id, "1.0.1");
        const InstalledPack pack = ReadInstalledPack(id);
        Check(pack.present && pack.bundled && pack.version == L"1.0.1", "bundled pack detected without user installation");
        Check(ResolvePackUncached(id).source == ToolSource::Pack &&
              PackToolPath(id, PackMainTool(id)) == (bundles / PackKey(id) / L"1.0.1" / PackMainTool(id)).wstring(),
              "bundled helper resolves independently of working directory");
    }
    auto settings = LoadPackSettings();
    settings.enabled[1] = false;
    Check(SavePackSettings(settings), "settings persist only in the private user profile");
    Check(ResolvePack(PackId::Images).source == ToolSource::None && ReadInstalledPack(PackId::Images).bundled,
          "disabled bundled image pack stays present and is not run");
    settings.enabled[1] = true;
    SavePackSettings(settings);
    Check(ResolvePack(PackId::Images).source == ToolSource::Pack, "cached bundled pack responds to settings changes");
    settings.use_custom_ffmpeg = true;
    settings.custom_ffmpeg = (profile / L"missing" / L"ffmpeg.exe").wstring();
    SavePackSettings(settings);
    Check(ResolvePackUncached(PackId::Media).source == ToolSource::None,
          "invalid explicitly selected FFmpeg does not fall back to bundled tools");
    settings.use_custom_ffmpeg = false;
    SavePackSettings(settings);
    MakePack(PacksRoot(), PackId::Media, "2.0.0");
    const InstalledPack updated = ReadInstalledPack(PackId::Media);
    Check(updated.present && !updated.bundled && updated.version == L"2.0.0" &&
          ResolvePackUncached(PackId::Media).directory == updated.directory, "per-user update has priority over bundled copy");
    fs::remove(fs::path(updated.directory) / L"ffmpeg.exe");
    Check(ReadInstalledPack(PackId::Media).bundled, "incomplete user update falls back to complete bundled copy");
    const fs::path marker = bundles / L"images" / L"installed.json";
    Write(root / L"escape" / L"pulse-imgpack.exe", "MZ");
    Write(marker, "{\"version\":\"evil\",\"dir\":\"..\\\\..\\\\escape\"}");
    Check(!ReadInstalledPack(PackId::Images).present, "bundled marker cannot escape through path separators");
    Write(marker, "{\"version\":\"evil\",\"dir\":\"..\"}");
    Check(!ReadInstalledPack(PackId::Images).present, "bundled marker rejects parent directory");
    MakePack(bundles, PackId::Images, "1.0.1");
    fs::remove(bundles / L"images" / L"1.0.1" / L"pulse-imgpack.exe");
    Check(!ReadInstalledPack(PackId::Images).present, "bundled marker without main tool is unavailable");
    SetEnvironmentVariableW(L"LOCALAPPDATA", nullptr);
    Check(ReadInstalledPack(PackId::Raw).bundled && ResolvePackUncached(PackId::Raw).source == ToolSource::Pack,
          "bundled packs remain available when user profile variable is missing");
    return failures ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--fixture") return RunFixture(argv[2]);
    const fs::path root = fs::temp_directory_path() / (L"pulse-bundled-packs-" + std::to_wstring(GetCurrentProcessId()));
    const fs::path program = root / L"program dir";
    fs::create_directories(program);
    wchar_t module[32768]{};
    const DWORD count = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
    if (!count || count >= std::size(module)) return 1;
    const fs::path child = program / L"bundle-test.exe";
    fs::copy_file(module, child);
    std::wstring command = L"\"" + child.wstring() + L"\" --fixture \"" + program.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    bool passed = false;
    if (CreateProcessW(child.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        const bool exited = WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0;
        DWORD code = 1;
        if (exited) GetExitCodeProcess(process.hProcess, &code);
        else TerminateProcess(process.hProcess, 1);
        passed = exited && code == 0;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    fs::remove_all(root);
    return passed ? 0 : 1;
}
