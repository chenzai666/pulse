static int SortUpdatesTest() {
    using namespace pulse;
    const auto entry = [](const wchar_t* name, DWORD key) {
        fs::DirEntry row;
        row.name = name;
        row.size = key;
        row.mtime.dwLowDateTime = key;
        return row;
    };
    const auto sorted = [](std::vector<fs::DirEntry> rows, ui::SortColumn col, ui::SortDirection dir) {
        app::SortEntries(rows, col, dir);
        return rows;
    };
    const auto names = [](const std::vector<fs::DirEntry>& rows) {
        std::vector<std::wstring> result;
        for (const auto& row : rows) result.push_back(row.name);
        return result;
    };
    const auto a = entry(L"a.txt", 10);
    const auto b = entry(L"b.txt", 20);
    const auto c = entry(L"c.txt", 30);
    for (const auto col : {ui::SortColumn::Size, ui::SortColumn::Mtime}) {
        for (const auto dir : {ui::SortDirection::Asc, ui::SortDirection::Desc}) {
            for (const auto group : {app::GroupBy::None, app::GroupBy::Size}) {
                const app::ScopedEntryGrouping grouping(static_cast<int>(group));
                for (const DWORD key : {25u, 10000000u}) {
                    const auto before = sorted({a, b, c}, col, dir);
                    const auto changed = entry(L"a.txt", key);
                    const auto expected = sorted({changed, b, c}, col, dir);
                    for (const auto& events : std::vector<std::vector<fs::DirNotifyEvent>>{
                             {{FILE_ACTION_MODIFIED, a.name, {}}},
                             {{FILE_ACTION_ADDED, a.name, {}}},
                             {{FILE_ACTION_REMOVED, a.name, {}}, {FILE_ACTION_ADDED, a.name, {}}},
                             {{FILE_ACTION_RENAMED_NEW_NAME, a.name, L"temp.tmp"}}}) {
                        app::PreparedDirNotifyBatch prepared;
                        prepared.events = events;
                        prepared.metadata.emplace(a.name, changed);
                        auto rows = before;
                        const bool applied = app::ApplyPreparedDirNotifyBatch(rows, prepared, col, dir) ==
                            app::NotifyPatch::Applied;
                        Check(applied && names(rows) == names(expected),
                              "same-name overwrite follows the active size or modified order");
                    }
                    Check(names(app::KeepEntryOrder(before, expected, {}, col, dir)) == names(expected),
                          "full refresh agrees with the active order after same-name overwrite");
                }
            }
        }
    }

    constexpr auto size = ui::SortColumn::Size;
    constexpr auto asc = ui::SortDirection::Asc;
    app::PreparedDirNotifyBatch prepared;
    const auto renamed = entry(L"z.txt", 40);
    const auto newcomer = entry(L"first.txt", 1);
    prepared.events = {{FILE_ACTION_RENAMED_NEW_NAME, renamed.name, a.name},
                       {FILE_ACTION_ADDED, newcomer.name, {}}};
    prepared.metadata = {{renamed.name, renamed}, {newcomer.name, newcomer}};
    std::vector<fs::DirEntry> rows{a, b, c};
    Check(app::ApplyPreparedDirNotifyBatch(rows, prepared, size, asc) == app::NotifyPatch::Applied &&
          names(rows) == names({renamed, b, c, newcomer}), "renames and new arrivals retain existing placement");
    Check(names(app::KeepEntryOrder({a, b, c}, {newcomer, b, c, renamed}, {{a.name, renamed.name}},
          size, asc)) == names(rows), "full refresh preserves ordinary rename and arrival placement");

    auto changed = renamed;
    changed.mtime.dwLowDateTime = 100;
    prepared.events = {{FILE_ACTION_MODIFIED, changed.name, {}}};
    prepared.metadata = {{changed.name, changed}};
    Check(app::ApplyPreparedDirNotifyBatch(rows, prepared, size, asc) == app::NotifyPatch::Applied &&
          names(rows) == names({changed, b, c, newcomer}) && rows.front().mtime.dwLowDateTime == 100,
          "metadata outside the active key updates the held row in place");
    Check(names(app::KeepEntryOrder(rows, {newcomer, b, c, changed}, {}, size, asc)) == names(rows),
          "full refresh keeps held rows without an active key change");

    const auto temporary = entry(L"temp.tmp", 5);
    const auto overwritten = entry(L"a.txt", 25);
    prepared.events = {{FILE_ACTION_RENAMED_NEW_NAME, overwritten.name, temporary.name}};
    prepared.metadata = {{overwritten.name, overwritten}};
    rows = {temporary, a, b, c};
    const std::vector<fs::DirEntry> after_overwrite{b, overwritten, c};
    Check(app::ApplyPreparedDirNotifyBatch(rows, prepared, size, asc) == app::NotifyPatch::Applied &&
          names(rows) == names(after_overwrite), "visible temporary source is merged into the overwritten destination");
    Check(names(app::KeepEntryOrder({temporary, a, b, c}, after_overwrite,
              {{temporary.name, overwritten.name}}, size, asc)) == names(after_overwrite),
          "full refresh uses the overwritten destination's new sort key");

    const auto remote = app::PrepareDirNotifyBatch(L"\\\\server\\share", prepared.events);
    Check(remote.need_full_enum && remote.metadata.empty(), "remote metadata uses directory enumeration");
    prepared.metadata.clear();
    const auto before_missing = names(rows);
    Check(app::ApplyPreparedDirNotifyBatch(rows, prepared, size, asc) == app::NotifyPatch::NeedFullEnum &&
          names(rows) == before_missing, "incomplete preparation leaves the snapshot intact");

    std::vector<fs::DirEntry> large(4096);
    large.front() = a;
    for (size_t i = 1; i < large.size(); ++i) large[i].name = std::to_wstring(i);
    prepared.events.assign(64, {FILE_ACTION_MODIFIED, a.name, {}});
    prepared.metadata = {{a.name, overwritten}};
    Check(app::ApplyPreparedDirNotifyBatch(large, prepared, size, asc) == app::NotifyPatch::NeedFullEnum &&
          large.front().name == a.name && large.front().size == a.size,
          "large reorder batches request enumeration without patching the snapshot");
    return failures ? 1 : 0;
}
