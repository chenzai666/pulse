#include "../integrations/listary_search_client.h"

#include <cstdio>

using namespace pulse::integrations;

namespace {

std::string Item(std::string path = "C:\\\\Work\\\\invoice.pdf", std::string type = "file",
                 std::string name = "invoice.pdf") {
    return "{\"path\":\"" + path + "\",\"name\":\"" + name + "\",\"type\":\"" + type + "\"}";
}

std::string Response(std::string items, unsigned count = 1, uint64_t total = 100,
                     uint64_t offset = 0, unsigned limit = 50) {
    return "{\"ok\":true,\"data\":{\"total\":" + std::to_string(total) +
        ",\"offset\":" + std::to_string(offset) + ",\"limit\":" + std::to_string(limit) +
        ",\"count\":" + std::to_string(count) + ",\"results\":[" + items + "]},\"error\":null}";
}

ListarySearchItem LocalItem(std::wstring name, bool directory = false) {
    return {L"C:\\SearchFixture\\" + name, std::move(name), directory};
}

std::vector<std::wstring> Paths(const std::vector<ListarySearchItem>& items) {
    std::vector<std::wstring> paths;
    for (const auto& item : items) paths.push_back(item.path);
    return paths;
}

} // namespace

int main() {
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool passed, const char* label) {
        ++checks;
        if (!passed) ++failures;
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", label);
    };
    const auto parse = [](const std::string& text, bool folders = false, uint64_t offset = 0) {
        return detail::ParseListarySearchResponse(text, folders, offset);
    };
    const auto invalid = [&](const std::string& text) {
        const auto result = parse(text);
        return result.error == ListarySearchError::InvalidResponse && result.items.empty();
    };
    auto config = detail::ParseListarySearchConfig(
        R"({"Settings":{"General.HttpApi.Enabled":true,"General.HttpApi.Port":38432}})");
    check(config.enabled && config.port == 38432, "exact current-user settings keys");
    config = detail::ParseListarySearchConfig(R"({"Settings":{"General.HttpApi.Enabled":true}})");
    check(config.enabled && config.port == 38431, "enabled setting uses documented default port");
    for (const auto* text : {"", "{}", R"({"General.HttpApi.Enabled":true})",
            R"({"Settings":{"General":{"HttpApi":{"Enabled":true}}}})",
            R"({"Settings":{"General.HttpApi.Enabled":false}})",
            R"({"Settings":{"General.HttpApi.Enabled":"true"}})",
            R"({"Settings":{"General.HttpApi.Enabled":true,"General.HttpApi.Port":0}})",
            R"({"Settings":{"General.HttpApi.Enabled":true,"General.HttpApi.Port":65536}})",
            R"({"Settings":{"General.HttpApi.Enabled":true,"General.HttpApi.Port":-1}})",
            R"({"Settings":{"General.HttpApi.Enabled":true,"General.HttpApi.Port":38431.0}})",
            R"({"Settings":{"General.HttpApi.Enabled":true,"General.HttpApi.Port":"38431"}})",
            R"({"Settings":{"General.HttpApi.Enabled":true,"General.HttpApi.Enabled":false}})",
            R"({"Settings":{"General.HttpApi.Enabled":true},"Settings":{}})"}) {
        check(!detail::ParseListarySearchConfig(text).enabled, "absent or malformed opt-in stays disabled");
    }
    check(!detail::ParseListarySearchConfig(std::string(1024 * 1024 + 1, 'x')).enabled,
        "oversized configuration rejected");
    auto body = detail::BuildListarySearchBody(L"a\"b\\c", false);
    check(body && *body == R"({"query":"a\"b\\c","limit":50,"offset":0})", "query escaped as JSON data");
    body = detail::BuildListarySearchBody(L"中文\xD83D\xDE80", true);
    check(body && body->find("\"types\":[\"folder\"]") != std::string::npos &&
        body->find("\"file\"") == std::string::npos,
        "Unicode folder request has only folder type");
    check(!detail::BuildListarySearchBody(L"", false), "empty query rejected");
    check(!detail::BuildListarySearchBody(std::wstring(4097, L'x'), false), "oversized query rejected");
    check(!detail::BuildListarySearchBody(std::wstring(L"a\0b", 3), false), "NUL query rejected");
    check(!detail::BuildListarySearchBody(L"\xD800", false), "invalid UTF-16 query rejected");
    body = detail::BuildListarySearchBody(L"xterm", false, 50);
    check(body && *body == R"({"query":"xterm","limit":50,"offset":50})" &&
        body->find("search_in") == std::string::npos, "next page offset retains global search scope");
    body = detail::BuildListarySearchBody(L"xterm", true, 100);
    check(body && *body == R"({"query":"xterm","limit":50,"offset":100,"types":["folder"]})" &&
        body->find("search_in") == std::string::npos, "folder page keeps query and type without narrowing scope");
    body = detail::BuildListarySearchBody(L"xterm", false, UINT64_C(9007199254740991));
    check(body && body->find("\"offset\":9007199254740991") != std::string::npos,
        "maximum JSON-safe offset encoded as exact decimal integer");
    check(!detail::BuildListarySearchBody(L"xterm", false, UINT64_C(9007199254740992)),
        "unsafe JSON integer offset rejected");
    check(!detail::BuildListarySearchBody(L"xterm", false, UINT64_MAX), "uint64 maximum offset rejected");
    check(detail::ListaryEffectiveQuery(L"LA516") == L"LA 516" &&
        detail::ListaryEffectiveQuery(L"RTX4090") == L"RTX 4090", "compact model query separates letters and digits");
    for (const auto* unchanged : {L"mp3", L"xterm", L"LA516.pdf", L"LA 516", L"LA516*", L"file:LA516",
            L"A516", L"LA5", L"12LA516", L"LA_516"}) {
        check(detail::ListaryEffectiveQuery(unchanged) == unchanged,
            "ordinary filenames and advanced queries keep engine syntax unchanged");
    }
    body = detail::BuildListarySearchBody(L"LA516", false);
    check(body && *body == R"({"query":"LA 516","limit":50,"offset":0})",
        "compact model first page searches expanded engine query");
    body = detail::BuildListarySearchBody(L"LA516", false, 50);
    check(body && *body == R"({"query":"LA 516","limit":50,"offset":50})",
        "compact model later page retains identical effective search scope");
    check(detail::ListaryEffectiveQuery(L"OX1228D") == L"OX 1228 D" &&
        detail::ListaryEffectiveQuery(L"RX6750GRE") == L"RX 6750 GRE" &&
        detail::ListaryEffectiveQuery(L"AB12CD34E") == L"AB 12 CD 34 E",
        "compact model separates every letter-digit run including suffix letters");
    body = detail::BuildListarySearchBody(L"OX1228D", false, 50);
    check(body && *body == R"({"query":"OX 1228 D","limit":50,"offset":50})",
        "letter-suffix model pagination uses expanded engine query");
    const auto success = parse(Response(Item()));
    check(success.error == ListarySearchError::None && success.items.size() == 1 &&
        success.items[0].path == L"C:\\Work\\invoice.pdf" && !success.items[0].directory,
        "valid file result returned as data");
    const auto folder = parse(Response(Item("\\\\\\\\server\\\\share\\\\folder", "folder", "folder")), true);
    check(folder.error == ListarySearchError::None && folder.items.size() == 1 && folder.items[0].directory,
        "UNC folder result accepted");
    const auto chinese = parse(Response(Item(R"(C:\\\u4e2d\\test.txt)", "file", "test.txt")));
    check(chinese.error == ListarySearchError::None && chinese.items.size() == 1 &&
        chinese.items[0].path == L"C:\\中\\test.txt", "JSON Unicode escapes decoded");
    const auto empty = parse(Response("", 0, 0));
    check(empty.error == ListarySearchError::None && empty.items.empty() && empty.total == 0 &&
        empty.offset == 0 && !empty.has_more, "zero-result page has no more results");
    check(success.total == 100 && success.offset == 0 && success.has_more,
        "first page exposes total and next-page availability");
    const auto middle = parse(Response(Item(), 1, 101, 50), false, 50);
    check(middle.error == ListarySearchError::None && middle.total == 101 && middle.offset == 50 &&
        middle.items.size() == 1 && middle.has_more, "middle page retains requested offset and total");
    const auto last = parse(Response(Item(), 1, 101, 100), false, 100);
    check(last.error == ListarySearchError::None && last.offset == 100 && !last.has_more,
        "last page clears has-more flag");
    const auto end = parse(Response("", 0, 100, 100), false, 100);
    check(end.error == ListarySearchError::None && !end.has_more && end.items.empty(),
        "empty page exactly at total terminates pagination");
    check(invalid(Response("", 0)), "empty page before total rejected to prevent pagination loop");
    check(parse(Response(Item(), 1, 100, 0), false, 50).error == ListarySearchError::InvalidResponse,
        "server response offset must equal requested page");
    check(parse(Response(Item(), 1, 50, 50), false, 50).error == ListarySearchError::InvalidResponse,
        "page count cannot extend past total");
    check(parse(Response("", 0, 49, 50), false, 50).error == ListarySearchError::InvalidResponse,
        "offset past total rejected");
    check(invalid(Response(Item(), 1, UINT64_C(9007199254740992))), "unsafe JSON total rejected");
    check(parse(Response("", 0, UINT64_MAX, UINT64_MAX), false, UINT64_MAX).error ==
        ListarySearchError::InvalidResponse, "oversized request and response offset rejected without addition overflow");
    const auto boundary = parse(Response(Item(), 1, UINT64_C(9007199254740991), UINT64_C(9007199254740990)),
        false, UINT64_C(9007199254740990));
    check(boundary.error == ListarySearchError::None && !boundary.has_more,
        "safe integer last-page boundary calculates without overflow");
    check(invalid(Response(Item(), 1, 100, 0, 100)), "response limit must match requested page size");
    std::string full_page;
    for (unsigned i = 0; i < 50; ++i) { if (i) full_page += ','; full_page += Item(); }
    const auto full_middle = parse(Response(full_page, 50, 120, 50), false, 50);
    check(full_middle.error == ListarySearchError::None && full_middle.items.size() == 50 && full_middle.has_more,
        "full middle page advances by actual count");
    check(parse(Response(Item()), true).error == ListarySearchError::InvalidResponse, "folder filter enforces response type");
    check(invalid(Response(Item("C:\\\\Work\\\\file", "command"))), "command result rejected");
    for (const auto* path : {"relative.txt", "C:relative.txt", "shell:Downloads", "pulse:home",
            "C:\\\\file:stream", "\\\\\\\\?\\\\C:\\\\file", "\\\\\\\\.\\\\pipe\\\\name",
            "C:\\\\..\\\\file", "C:\\\\NUL", "C:/file", "C:\\\\bad\\u0000path",
            "C:\\\\bad\\ud800", "C:\\\\"}) {
        check(invalid(Response(Item(path))), "unsafe result path rejected");
    }
    check(invalid(Response(Item("C:\\\\file", "file", "bad\\nname"))), "control character in display name rejected");
    check(invalid(Response(Item("C:\\\\file", "file", "bad/name"))), "path-shaped display name rejected");
    check(invalid(Response(Item(), 0)), "response count must equal array count");
    check(invalid(Response(Item(), 51)), "response count capped at requested limit");
    check(invalid(Response(Item() + "," + Item("relative"), 2)), "invalid later result discards partial results");
    check(invalid(R"({"ok":true,"ok":false,"data":{}})"), "duplicate success field rejected");
    check(invalid(R"({"ok":true,"data":{"count":0,"count":1}})"), "duplicate data field rejected");
    check(invalid(R"({"ok":"true","data":{}})"), "success field has strict boolean type");
    check(invalid(R"({"ok":true,"data":null})"), "null data rejected");
    check(invalid(R"({"ok":true,"data":{"count":0,"total":0,"offset":1,"limit":50,"results":[]}})"),
        "offset must match first page");
    check(invalid(R"({"ok":true,"data":{"count":0,"total":-1,"offset":0,"limit":50,"results":[]}})"),
        "negative total rejected");
    check(invalid(R"({"ok":true,"data":{"count":0,"total":18446744073709551616,"offset":0,"limit":50,"results":[]}})"),
        "integer overflow rejected");
    check(invalid(Response(Item()).substr(0, 20)), "truncated JSON rejected");
    check(invalid(std::string("\xC0\xAF", 2)), "invalid UTF-8 rejected");
    check(invalid(std::string(1024 * 1024 + 1, 'x')), "response body capped at one MiB");
    check(parse(R"({"ok":false,"data":null,"error":"SEARCH_UNAVAILABLE"})").error ==
        ListarySearchError::Unavailable, "API search unavailable mapped");
    std::string excess;
    for (unsigned i = 0; i < 51; ++i) { if (i) excess += ','; excess += Item(); }
    check(invalid(Response(excess, 50)), "actual result array capped independently of count");

    std::vector<ListarySearchItem> ranked{
        LocalItem(L"xterm-data", true), LocalItem(L"XTerminal", true),
        LocalItem(L"XTerminal.exe"), LocalItem(L"XTerminal-Setup-x64.exe")};
    RankListarySearchResults(L"xterm", ranked);
    check(ranked[0].name == L"XTerminal.exe" && ranked[1].name == L"XTerminal-Setup-x64.exe" &&
        ranked[2].name == L"xterm-data" && ranked[3].name == L"XTerminal",
        "xterm screenshot: both matching executables precede equal-prefix directories");
    const auto once = Paths(ranked);
    RankListarySearchResults(L"xterm", ranked);
    check(Paths(ranked) == once, "ranking is idempotent and keeps same-tier engine order");

    ranked = {LocalItem(L"xterm", true), LocalItem(L"XTERM.EXE"), LocalItem(L"xterm-notes.txt")};
    RankListarySearchResults(L"XtErM", ranked);
    check(ranked[0].name == L"XTERM.EXE" && ranked[1].directory,
        "case-insensitive exact program stem wins exact directory tie");

    ranked = {LocalItem(L"report.pdf-editor.exe"), LocalItem(L"report.pdf"), LocalItem(L"report.pdf.exe")};
    RankListarySearchResults(L"report.pdf", ranked);
    check(ranked[0].name == L"report.pdf", "exact document filename beats prefix and double-extension programs");
    ranked = {LocalItem(L"ReportViewer.exe"), LocalItem(L"report.txt")};
    RankListarySearchResults(L"report.txt", ranked);
    check(ranked[0].name == L"report.txt", "explicit exact document filename beats program");

    ranked = {LocalItem(L"xterm", true), LocalItem(L"xterm"), LocalItem(L"XTerm.scs"),
        LocalItem(L"xterm.js"), LocalItem(L"XTerminal", true), LocalItem(L"XTerminal.exe"),
        LocalItem(L"XTerminal-Setup-x64.exe"), LocalItem(L"unrelated.exe")};
    RankListarySearchResults(L"xterm", ranked);
    check(ranked[0].name == L"XTerminal.exe" && ranked[1].name == L"XTerminal-Setup-x64.exe" &&
        ranked[2].name == L"xterm" && ranked[2].directory && ranked[3].name == L"xterm" &&
        ranked[4].name == L"XTerm.scs" && ranked[5].name == L"xterm.js",
        "real API mix: prefix programs precede exact extensionless files, directories, and documents");
    ranked = {LocalItem(L"report.txt"), LocalItem(L"ReportViewer.exe")};
    RankListarySearchResults(L"report", ranked);
    check(ranked[0].name == L"ReportViewer.exe", "extensionless app query prefers a prefix program over document stem");
    ranked = {LocalItem(L"my-xterm.exe"), LocalItem(L"xterm", true), LocalItem(L"x-t-e-r-m.exe")};
    RankListarySearchResults(L"xterm", ranked);
    check(ranked[0].name == L"xterm" && ranked[0].directory && ranked[1].name == L"my-xterm.exe",
        "substring and subsequence programs do not cross stronger relevance tiers");

    ranked = {LocalItem(L"calculator.exe"), LocalItem(L"xterm-notes.txt"), LocalItem(L"other.exe")};
    RankListarySearchResults(L"xterm", ranked);
    check(ranked[0].name == L"xterm-notes.txt" && ranked[1].name == L"calculator.exe" &&
        ranked[2].name == L"other.exe", "unrelated programs receive no mechanical priority");
    ranked = {LocalItem(L"completely-unrelated.txt"), LocalItem(L"other.exe")};
    const auto unrelated = Paths(ranked);
    RankListarySearchResults(L"xterm", ranked);
    check(Paths(ranked) == unrelated, "all-unmatched results retain engine order");

    ranked = {LocalItem(L"微信资料", true), LocalItem(L"微信.EXE"), LocalItem(L"微信使用说明.txt")};
    RankListarySearchResults(L"微信", ranked);
    check(ranked[0].name == L"微信.EXE", "Chinese simple name ranks matching program");
    ranked = {LocalItem(L"äpfel-notes", true), LocalItem(L"ÄPFEL.exe")};
    RankListarySearchResults(L"äpfel", ranked);
    check(ranked[0].name == L"ÄPFEL.exe", "ordinal Unicode casing is independent of current locale");

    for (const auto* extension : {L".exe", L".com", L".bat", L".cmd", L".lnk", L".appref-ms", L".msc"}) {
        ranked = {LocalItem(L"xterm", true), LocalItem(std::wstring(L"xterm") + extension)};
        RankListarySearchResults(L"xterm", ranked);
        check(!ranked[0].directory, "program and shortcut suffixes win equal name match without resolution");
    }
    ranked = {LocalItem(L"xterm.exe", true), LocalItem(L"xterm-notes.txt")};
    const auto named_directory = Paths(ranked);
    RankListarySearchResults(L"xterm", ranked);
    check(Paths(ranked) == named_directory, "executable-looking directory receives no file preference");

    const std::vector<ListarySearchItem> engine_order{LocalItem(L"xterm-docs", true),
        LocalItem(L"XTerminal.exe"), LocalItem(L"xterm", true)};
    for (const auto* advanced : {L"", L"C:\\Apps\\xterm", L"Apps/xterm", L"file:xterm", L"ext:exe xterm",
            L"xterm*", L"xterm?", L"\"xterm\"", L"'xterm'", L"xterm notes", L"xterm|terminal", L"+xterm",
            L"-xterm", L"!xterm", L"(xterm)", L"[xterm]", L"xterm\t", L"xterm\x3000"}) {
        ranked = engine_order;
        RankListarySearchResults(advanced, ranked);
        check(Paths(ranked) == Paths(engine_order), "advanced Listary query preserves engine ordering");
    }
    ranked = {LocalItem(L"xterm-cache", true), LocalItem(L"xterm", true), LocalItem(L"my-xterm", true)};
    RankListarySearchResults(L"xterm", ranked);
    check(ranked.size() == 3 && ranked[0].name == L"xterm" && ranked[0].directory &&
        ranked[1].directory && ranked[2].directory, "folder-only input stays folder-only");
    ranked = {LocalItem(L"x-t-e-r-m.exe"), LocalItem(L"my-xterm.txt"), LocalItem(L"xterminal", true)};
    RankListarySearchResults(L"xterm", ranked);
    check(ranked[0].name == L"xterminal" && ranked[1].name == L"my-xterm.txt" &&
        ranked[2].name == L"x-t-e-r-m.exe", "prefix and substring relevance precede fuzzy program preference");
    ranked = {LocalItem(L"Amazon_LA516_front.png"), LocalItem(L"Amazon_LA516_back.png"),
        LocalItem(L"LA 516", true), LocalItem(L"LA_516-assets", true), LocalItem(L"LA-516.pdf")};
    RankListarySearchResults(L"LA516", ranked);
    check(ranked[0].name == L"LA 516" && ranked[0].directory && ranked[1].name == L"LA-516.pdf" &&
        ranked[2].name == L"LA_516-assets" && ranked[3].name == L"Amazon_LA516_front.png",
        "compact model ranking recognizes spaced folder as exact ahead of Amazon image substring");
    ranked = {LocalItem(L"Amazon_OX1228D_front.png"), LocalItem(L"OX 1228D 68296359358.rar"),
        LocalItem(L"OX 1228D 68296359358", true), LocalItem(L"OX-1228_D", true)};
    RankListarySearchResults(L"OX1228D", ranked);
    check(ranked[0].name == L"OX-1228_D" && ranked[0].directory &&
        ranked[1].name == L"OX 1228D 68296359358.rar" && ranked[2].directory &&
        ranked[3].name == L"Amazon_OX1228D_front.png",
        "letter-suffix model ranks spaced folders and RAR ahead of image substring with stable prefix ties");
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
