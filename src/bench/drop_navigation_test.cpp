#include "../app/drop_navigation_model.h"
#include "../fs/bounded_enumeration.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
bool passed = true;
void Check(bool result, const char* text) {
    std::cout << (result ? "[PASS] " : "[FAIL] ") << text << '\n';
    passed &= result;
}
}

int main() {
    using namespace pulse::app;
    Check(TabNavigationDropEffect(DROPEFFECT_COPY | DROPEFFECT_MOVE) == DROPEFFECT_COPY,
          "navigation drop keeps source even when move is allowed");
    Check(TabNavigationDropEffect(DROPEFFECT_MOVE) == DROPEFFECT_NONE,
          "move-only source is not acknowledged as a move");
    Check(TabNavigationDropEffect(DROPEFFECT_LINK | DROPEFFECT_MOVE) == DROPEFFECT_LINK,
          "navigation prefers a non-mutating reference effect");
    Check(NormalizeDroppedNavigationPath(L"relative.txt").empty() &&
          NormalizeDroppedNavigationPath(L"C:relative.txt").empty() &&
          NormalizeDroppedNavigationPath(L"\\\\.\\PhysicalDrive0").empty() &&
          NormalizeDroppedNavigationPath(L"\\\\?\\GLOBALROOT\\Device\\x").empty() &&
          NormalizeDroppedNavigationPath(L"C:\\file:stream").empty() &&
          NormalizeDroppedNavigationPath(L"pulse:search:query").empty(),
          "navigation admission rejects relative paths, devices, ADS and virtual URIs");
    Check(NormalizeDroppedNavigationPath(L"\\\\server\\share\\folder") == L"\\\\?\\UNC\\server\\share\\folder",
          "UNC normalization is textual and needs no network probe");
    Check(NormalizeDroppedNavigationPath(L"C:\\one\\..\\two") == L"\\\\?\\C:\\two",
          "dot segments are resolved before extended prefix");
    const auto drive_file = PlanDroppedNavigation(L"\\\\?\\C:\\sample.exe", FILE_ATTRIBUTE_NORMAL);
    Check(drive_file.folder == L"\\\\?\\C:\\" && drive_file.select_name == L"sample.exe",
          "file at drive root becomes parent navigation and selection");
    Check(PlanDroppedNavigation(L"\\\\?\\C:\\missing.txt", INVALID_FILE_ATTRIBUTES).folder.empty(),
          "invalid source is not converted into a folder tab");

    const auto root = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"drop-navigation-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    try {
        std::filesystem::create_directories(root / L"folder");
        std::ofstream(root / L"must-not-run.exe", std::ios::binary) << "navigation fixture";
        std::ofstream(root / L"中文.txt", std::ios::binary) << "navigation fixture";
        std::vector<DroppedNavigationTarget> targets;
        for (const auto& leaf : {L"must-not-run.exe", L"中文.txt", L"folder"}) {
            const auto source = NormalizeDroppedNavigationPath((root / leaf).wstring());
            targets.push_back(PlanDroppedNavigation(source, GetFileAttributesW(source.c_str())));
        }
        targets.push_back(targets.front());
        const auto groups = GroupDroppedNavigation(targets);
        Check(groups.size() == 2 && groups[0].names.size() == 2 && groups[0].source_count == 3 &&
              groups[1].names.empty(), "mixed drop groups files by parent, deduplicates selection and opens folders");
        Check(groups[0].names.front() == L"must-not-run.exe", "program source is only a selected file name");
        std::filesystem::remove_all(root);
    } catch (...) {
        Check(false, "isolated navigation fixtures");
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::atomic_bool cancelled = false;
    std::thread cancel([&] { Sleep(20); cancelled = true; });
    const auto started = std::chrono::steady_clock::now();
    bool stopped = false;
    try {
        pulse::fs::RunBoundedEnumeration<int>(true, [](const std::atomic_bool& stop) {
            while (!stop) Sleep(5);
            return 0;
        }, [&] { return cancelled.load(); }, std::chrono::milliseconds(250));
    } catch (const pulse::fs::EnumerationCancelled&) { stopped = true; }
    cancel.join();
    Check(stopped && std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1000),
          "closing cancels bounded metadata wait without joining a stalled provider");
    return passed ? 0 : 1;
}
