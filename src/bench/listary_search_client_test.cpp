#include "../integrations/listary_search_client.h"

#include <cstdio>

using namespace pulse::integrations;

namespace {

std::string Item(std::string path = "C:\\\\Work\\\\invoice.pdf", std::string type = "file",
                 std::string name = "invoice.pdf") {
    return "{\"path\":\"" + path + "\",\"name\":\"" + name + "\",\"type\":\"" + type + "\"}";
}

std::string Response(std::string items, unsigned count = 1) {
    return "{\"ok\":true,\"data\":{\"total\":100,\"offset\":0,\"limit\":50,\"count\":" +
        std::to_string(count) + ",\"results\":[" + items + "]},\"error\":null}";
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
    const auto parse = [](const std::string& text, bool folders = false) {
        return detail::ParseListarySearchResponse(text, folders);
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
    const auto empty = parse(Response("", 0));
    check(empty.error == ListarySearchError::None && empty.items.empty(), "empty successful result");
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
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
