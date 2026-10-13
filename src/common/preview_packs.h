// preview_packs.h - Preview packs (FFmpeg, modern images, RAW, archives).
// Packs may ship beside the executables. A user-installed pack takes priority
// over the bundled copy, so Settings' existing download/update path still works.
// Preview helpers only run when the native decoder cannot handle a file.
//
// Layout under %LOCALAPPDATA%\Pulse\packs:
//   packs.json                  user settings, written by the app
//   <key>\installed.json        {"version":"7.1.1","dir":"7.1.1"}
//   <key>\<dir>\ffmpeg.exe ...  the pack's files
// The executable's packs\ directory uses the same installed.json layout;
// settings are always per-user, including for a read-only Program Files install.
//
// Header-only on purpose: the preview host, the app and the tests are
// separate executables and must not gain link dependencies.
#pragma once

#include "json_utils.h"
#include "utf8_file.h"
#include "edition.h"
#include <windows.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace pulse::packs {

enum class PackId : uint32_t { Media = 0, Images = 1, Raw = 2, Archives = 3 };
inline constexpr uint32_t kPackCount = 4;

// Directory and settings-key name of a pack.
inline const wchar_t* PackKey(PackId id) {
    switch (id) {
    case PackId::Media: return L"ffmpeg";
    case PackId::Images: return L"images";
    case PackId::Raw: return L"raw";
    case PackId::Archives: return L"archive";
    }
    return L"";
}

// Main executable of a pack; its siblings live in the same directory.
inline const wchar_t* PackMainTool(PackId id) {
    switch (id) {
    case PackId::Media: return L"ffmpeg.exe";
    case PackId::Images: return L"pulse-imgpack.exe";
    case PackId::Raw: return L"pulse-rawpack.exe";
    case PackId::Archives: return L"7z.exe";
    }
    return L"";
}

// The edition's per-user packs, empty when LOCALAPPDATA is unavailable. Tests and
// --test-instance runs redirect LOCALAPPDATA, which isolates packs as well.
inline std::wstring PacksRoot() {
    wchar_t root[32768];
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", root, static_cast<DWORD>(std::size(root)));
    if (!count || count >= std::size(root)) return {};
    return std::wstring(root, count) + L"\\" + edition::kDataFolder + L"\\packs";
}

inline std::wstring PacksSettingsPath() {
    const std::wstring root = PacksRoot();
    return root.empty() ? std::wstring{} : root + L"\\packs.json";
}

inline bool IsRegularFile(const std::wstring& path) {
    const DWORD attrs = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline std::wstring DirectoryOf(const std::wstring& file) {
    const size_t slash = file.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : file.substr(0, slash);
}

// Resolve from the executable, never from the working directory or PATH.
inline const std::wstring& BundledPacksRoot() {
    static const std::wstring root = [] {
        wchar_t module[32768]{};
        const DWORD count = GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
        if (!count || count >= std::size(module)) return std::wstring{};
        const std::wstring directory = DirectoryOf(std::wstring(module, count));
        return directory.empty() ? std::wstring{} : directory + L"\\packs";
    }();
    return root;
}

// User settings (packs.json). Flat keys so json_utils' key search is exact.
struct PackSettings {
    bool enabled[kPackCount] = {true, true, true, true};
    // Media pack only: an FFmpeg the user already has (path to ffmpeg.exe).
    bool use_custom_ffmpeg = false;
    std::wstring custom_ffmpeg;
    bool auto_update = true;
    uint32_t download_source = 0;   // 0 automatic, 1 GitHub, 2 accelerated mirror
    bool remove_on_uninstall = true;
};

inline PackSettings ParsePackSettings(const std::wstring& text) {
    PackSettings s;
    s.enabled[0] = json::ExtractBool(text, L"ffmpeg_enabled", true);
    s.enabled[1] = json::ExtractBool(text, L"images_enabled", true);
    s.enabled[2] = json::ExtractBool(text, L"raw_enabled", true);
    s.enabled[3] = json::ExtractBool(text, L"archive_enabled", true);
    s.use_custom_ffmpeg = json::ExtractBool(text, L"ffmpeg_use_custom", false);
    s.custom_ffmpeg = json::ExtractString(text, L"ffmpeg_custom_path");
    s.auto_update = json::ExtractBool(text, L"auto_update", true);
    const int source = json::ExtractInt(text, L"download_source", 0);
    s.download_source = source >= 0 && source <= 2 ? static_cast<uint32_t>(source) : 0u;
    s.remove_on_uninstall = json::ExtractBool(text, L"remove_on_uninstall", true);
    return s;
}

inline std::wstring SerializePackSettings(const PackSettings& s) {
    std::wstring path;
    json::Escape(s.custom_ffmpeg, path);
    std::wstring out = L"{\n";
    out += L"  \"ffmpeg_enabled\": " + std::wstring(s.enabled[0] ? L"true" : L"false") + L",\n";
    out += L"  \"images_enabled\": " + std::wstring(s.enabled[1] ? L"true" : L"false") + L",\n";
    out += L"  \"raw_enabled\": " + std::wstring(s.enabled[2] ? L"true" : L"false") + L",\n";
    out += L"  \"archive_enabled\": " + std::wstring(s.enabled[3] ? L"true" : L"false") + L",\n";
    out += L"  \"ffmpeg_use_custom\": " + std::wstring(s.use_custom_ffmpeg ? L"true" : L"false") + L",\n";
    out += L"  \"ffmpeg_custom_path\": \"" + path + L"\",\n";
    out += L"  \"auto_update\": " + std::wstring(s.auto_update ? L"true" : L"false") + L",\n";
    out += L"  \"download_source\": " + std::to_wstring(s.download_source) + L",\n";
    out += L"  \"remove_on_uninstall\": " + std::wstring(s.remove_on_uninstall ? L"true" : L"false") + L"\n";
    out += L"}\n";
    return out;
}

inline PackSettings LoadPackSettings() {
    std::wstring text;
    const std::wstring path = PacksSettingsPath();
    if (path.empty() || !ReadUtf8File(path, text)) return {};
    return ParsePackSettings(text);
}

// Creates every missing directory of an absolute path.
inline bool CreateDirectoryChain(const std::wstring& directory) {
    // Intermediate failures are expected (drive roots, \\server on UNC paths);
    // only the final directory has to exist.
    for (size_t at = directory.find_first_of(L"\\/", 3); ; at = directory.find_first_of(L"\\/", at + 1)) {
        CreateDirectoryW(directory.substr(0, at).c_str(), nullptr);
        if (at == std::wstring::npos) break;
    }
    const DWORD attrs = GetFileAttributesW(directory.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline bool SavePackSettings(const PackSettings& settings) {
    const std::wstring root = PacksRoot();
    if (root.empty()) return false;
    if (!CreateDirectoryChain(root)) return false;
    return WriteUtf8FileAtomic(PacksSettingsPath(), SerializePackSettings(settings));
}

struct InstalledPack {
    bool present = false;
    bool bundled = false;
    std::wstring version;
    std::wstring directory;   // absolute directory holding the pack's files
};

// Reads <root>\<key>\installed.json and checks the main tool is there.
inline InstalledPack ReadInstalledPackAtRoot(PackId id, const std::wstring& root, bool bundled = false) {
    InstalledPack pack;
    if (root.empty()) return pack;
    const std::wstring base = root + L"\\" + PackKey(id);
    std::wstring text;
    if (!ReadUtf8File(base + L"\\installed.json", text)) return pack;
    const std::wstring dir = json::ExtractString(text, L"dir");
    // A version directory name, never a path: installed.json is user-writable.
    if (dir.empty() || dir.find_first_of(L"\\/:") != std::wstring::npos || dir == L"." || dir == L"..")
        return pack;
    pack.version = json::ExtractString(text, L"version");
    pack.directory = base + L"\\" + dir;
    pack.present = IsRegularFile(pack.directory + L"\\" + PackMainTool(id));
    pack.bundled = pack.present && bundled;
    return pack;
}

inline InstalledPack ReadInstalledPack(PackId id) {
    InstalledPack pack = ReadInstalledPackAtRoot(id, PacksRoot());
    if (pack.present) return pack;
    return ReadInstalledPackAtRoot(id, BundledPacksRoot(), true);
}

// Where a pack's tools come from right now.
enum class ToolSource : uint32_t { None = 0, Pack = 1, Custom = 2 };

struct ResolvedPack {
    ToolSource source = ToolSource::None;
    std::wstring directory;    // directory holding the tools
};

inline ResolvedPack ResolvePackUncached(PackId id) {
    ResolvedPack out;
    const PackSettings settings = LoadPackSettings();
    if (!settings.enabled[static_cast<uint32_t>(id)]) return out;
    if (id == PackId::Media && settings.use_custom_ffmpeg) {
        if (IsRegularFile(settings.custom_ffmpeg)) {
            out.source = ToolSource::Custom;
            out.directory = DirectoryOf(settings.custom_ffmpeg);
        }
        // An explicitly chosen source must not silently change if its file moves.
        return out;
    }
    const InstalledPack pack = ReadInstalledPack(id);
    if (pack.present) {
        out.source = ToolSource::Pack;
        out.directory = pack.directory;
    }
    return out;
}

// Write times of packs.json and both installed.json markers. Settings and the
// installer write one of them on every change, so comparing this stamp lets a
// change made in Settings reach the very next request.
inline uint64_t PackStamp(PackId id) {
    const std::wstring root = PacksRoot();
    const std::wstring& bundled_root = BundledPacksRoot();
    uint64_t stamp = 0;
    for (const std::wstring& file : {
        root.empty() ? std::wstring{} : root + L"\\packs.json",
        root.empty() ? std::wstring{} : root + L"\\" + PackKey(id) + L"\\installed.json",
        bundled_root.empty() ? std::wstring{} : bundled_root + L"\\" + PackKey(id) + L"\\installed.json"}) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        const uint64_t time = GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)
            ? (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime
            : 1;
        stamp = stamp * 1000003u + time;
    }
    return stamp;
}

// The preview host asks this for every candidate file, so the answer is
// cached: three attribute reads per call, the settings and pack files only when
// they changed or every few seconds (a custom ffmpeg.exe may have been deleted).
inline ResolvedPack ResolvePack(PackId id) {
    struct Slot { ULONGLONG checked = 0; uint64_t stamp = 0; ResolvedPack value; };
    static Slot slots[kPackCount];
    static std::mutex lock;
    constexpr ULONGLONG kRecheckMs = 3000;
    const ULONGLONG now = GetTickCount64();
    const uint64_t stamp = PackStamp(id);
    std::lock_guard<std::mutex> guard(lock);
    Slot& slot = slots[static_cast<uint32_t>(id)];
    if (slot.checked == 0 || now - slot.checked >= kRecheckMs || stamp != slot.stamp) {
        slot.value = ResolvePackUncached(id);
        slot.checked = now ? now : 1;
        slot.stamp = stamp;
    }
    return slot.value;
}

// Full path of one of a pack's executables, empty when unavailable.
inline std::wstring PackToolPath(PackId id, std::wstring_view exe) {
    const ResolvedPack pack = ResolvePack(id);
    if (pack.source == ToolSource::None) return {};
    std::wstring path = pack.directory + L"\\" + std::wstring(exe);
    return IsRegularFile(path) ? path : std::wstring{};
}

} // namespace pulse::packs
