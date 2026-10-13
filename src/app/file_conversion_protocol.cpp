#include "file_conversion_protocol.h"
#include "../common/json_utils.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace pulse::app::file_conversion {
namespace {

using Object = std::map<std::wstring, std::wstring>;
constexpr size_t kMaxTargets = 256;
constexpr size_t kMaxOutputs = 256;

bool Wide(std::string_view input, std::wstring& output) {
    if (input.empty() || input.size() > kMaxProtocolLine) return false;
    const int bytes = static_cast<int>(input.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), bytes, nullptr, 0);
    if (!count) return false;
    output.resize(static_cast<size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), bytes, output.data(), count) == count;
}

bool Utf8(const std::wstring& input, std::string& output) {
    if (input.empty() || input.size() > kMaxProtocolLine) return false;
    const int chars = static_cast<int>(input.size());
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), chars, nullptr, 0, nullptr, nullptr);
    if (!count || static_cast<size_t>(count) > kMaxProtocolLine) return false;
    output.resize(static_cast<size_t>(count));
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), chars, output.data(), count, nullptr, nullptr) == count;
}

bool ReadString(const std::wstring& raw, std::wstring& value, size_t maximum = 32767) {
    size_t at = 0;
    if (!json::ConfigSyntax(raw, false).DecodeString(at, value) || value.size() > maximum) return false;
    json::SkipWhitespace(raw, at);
    return at == raw.size();
}

// The existing syntax reader checks structure/numbers; decode every string,
// including ignored members, so malformed UTF-16 escapes cannot pass through.
bool ValidObject(const std::wstring& text) {
    if (!json::ValidConfigObject(text)) return false;
    for (size_t at = 0; at < text.size();) {
        if (text[at] != L'"') { ++at; continue; }
        std::wstring value;
        if (!json::ConfigSyntax(text, false).DecodeString(at, value)) return false;
    }
    return true;
}

bool Members(const std::wstring& text, Object& object) {
    if (!json::ValidConfigObject(text)) return false;
    size_t at = 0;
    json::SkipWhitespace(text, at);
    ++at;
    for (;;) {
        json::SkipWhitespace(text, at);
        if (at >= text.size()) return false;
        if (text[at] == L'}') return true;
        std::wstring name, raw;
        if (!json::ConfigSyntax(text, false).DecodeString(at, name)) return false;
        json::SkipWhitespace(text, at);
        if (at == text.size() || text[at++] != L':' || !json::NextValue(text, at, raw)) return false;
        if (!object.emplace(std::move(name), std::move(raw)).second) return false;
        json::SkipWhitespace(text, at);
        if (at < text.size() && text[at] == L',') ++at;
    }
}

bool StringMember(const Object& object, const wchar_t* key, std::wstring& value, bool required = true,
                  size_t maximum = 32767) {
    const auto found = object.find(key);
    if (found == object.end()) return !required;
    if (!required && found->second == L"null") return true;
    return ReadString(found->second, value, maximum);
}

bool Strings(const std::wstring& text, std::vector<std::wstring>& values, size_t maximum) {
    if (!json::ValidConfigArray(text)) return false;
    bool valid = true;
    json::ForEachElement(text, [&](const std::wstring& raw) {
        std::wstring value;
        if (values.size() >= maximum || !ReadString(raw, value)) valid = false;
        else values.push_back(std::move(value));
    });
    return valid;
}

bool StringArrayMember(const Object& object, const wchar_t* key, std::vector<std::wstring>& values,
                       size_t maximum, bool required = false) {
    const auto found = object.find(key);
    return found == object.end() ? !required : Strings(found->second, values, maximum);
}

template<class T, class Read>
bool Objects(const Object& parent, const wchar_t* key, std::vector<T>& values, size_t maximum, Read read,
             bool required = false) {
    const auto found = parent.find(key);
    if (found == parent.end()) return !required;
    if (!json::ValidConfigArray(found->second)) return false;
    bool valid = true;
    json::ForEachElement(found->second, [&](const std::wstring& raw) {
        Object object;
        T value;
        if (values.size() >= maximum || !Members(raw, object) || !read(object, value)) valid = false;
        else values.push_back(std::move(value));
    });
    return valid;
}

template<class Number>
bool NumberValue(const std::wstring& raw, Number& value) {
    if (raw.empty() || raw.size() > 64) return false;
    std::string ascii;
    for (const auto c : raw) {
        if (c > 0x7F) return false;
        ascii.push_back(static_cast<char>(c));
    }
    const auto result = std::from_chars(ascii.data(), ascii.data() + ascii.size(), value);
    return result.ec == std::errc{} && result.ptr == ascii.data() + ascii.size();
}

bool Files(const Object& object, std::vector<FileConversionFileResult>& files, bool required) {
    return Objects(object, L"files", files, kMaxInputs, [](const Object& member, FileConversionFileResult& file) {
        const auto progress = member.find(L"progress");
        if (progress != member.end() && (!NumberValue(progress->second, file.progress) ||
            !std::isfinite(file.progress) || file.progress < 0 || file.progress > 1)) return false;
        return StringMember(member, L"source", file.source) && AbsolutePathShape(file.source) &&
            StringMember(member, L"state", file.state, true, 64) && !file.state.empty() &&
            StringArrayMember(member, L"outputs", file.outputs, kMaxOutputs) &&
            std::all_of(file.outputs.begin(), file.outputs.end(), [](const auto& path) { return AbsolutePathShape(path); }) &&
            StringMember(member, L"errorKey", file.error_key, false, 128) &&
            StringMember(member, L"error", file.error, false);
    }, required);
}

bool Targets(const Object& object, std::vector<FileConversionTarget>& targets) {
    // Route steps are host-owned engine objects. UI needs the target identity,
    // label and extension; syntax/Unicode are validated before ignoring routes.
    return Objects(object, L"targets", targets, kMaxTargets, [](const Object& member, FileConversionTarget& target) {
        return StringMember(member, L"id", target.id, true, 128) && !target.id.empty() &&
            StringMember(member, L"label", target.label, true, 1024) &&
            StringMember(member, L"extension", target.extension, false, 128);
    }, true);
}

std::wstring Quote(const std::wstring& value) {
    std::wstring output = L"\"";
    json::Escape(value, output);
    output += L'"';
    return output;
}

} // namespace

bool AbsolutePathShape(std::wstring_view path) {
    if (path.empty() || path.size() > 32767) return false;
    bool extended_unc = false;
    if (path.starts_with(L"\\\\?\\UNC\\")) { extended_unc = true; path.remove_prefix(8); }
    else if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
    const bool drive = path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    const bool unc = extended_unc || (path.starts_with(L"\\\\") && !path.starts_with(L"\\\\.\\"));
    if (!drive && !unc) return false;
    if (unc) {
        const size_t start = extended_unc ? 0 : 2;
        const size_t slash = path.find(L'\\', start);
        if (slash == std::wstring_view::npos || slash == start || slash + 1 >= path.size() || path[slash + 1] == L'\\')
            return false;
    }
    for (size_t i = 0; i < path.size(); ++i) {
        const auto c = path[i];
        if (c < L' ' || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c == L'*' || c == L'?' ||
            (c == L':' && (!drive || i != 1))) return false;
    }
    return true;
}

bool EncodeRequest(const Request& request, std::string& line) {
    if (!request.generation || request.inputs.size() > kMaxInputs) return false;
    std::wstring output = L"{\"id\":\"" + std::to_wstring(request.generation) + L"\",\"action\":";
    const bool input_action = request.action == Action::Capabilities || request.action == Action::Convert;
    output += Quote(request.action == Action::Capabilities ? L"capabilities" : request.action == Action::Convert ?
        L"convert" : request.action == Action::Cancel ? L"cancel" : L"shutdown");
    if (input_action) {
        if (request.inputs.empty()) return false;
        output += L",\"inputs\":[";
        for (size_t i = 0; i < request.inputs.size(); ++i) {
            if (i) output += L',';
            output += Quote(request.inputs[i]);
            if (output.size() > kMaxRequestCharacters) return false;
        }
        output += L']';
    }
    if (request.action == Action::Convert) {
        if (request.target.empty() || request.target.size() > 128 || request.output_directory.empty() ||
            request.quality < 1 || request.quality > 100) return false;
        output += L",\"target\":" + Quote(request.target) + L",\"outputDirectory\":" + Quote(request.output_directory) +
            L",\"quality\":" + std::to_wstring(request.quality);
    } else if (request.action == Action::Cancel) {
        if (!request.job_id) return false;
        output += L",\"jobId\":\"" + std::to_wstring(request.job_id) + L'"';
    }
    output += L'}';
    if (output.size() > kMaxRequestCharacters || !ValidObject(output) || !Utf8(output, line) || line.size() >= kMaxProtocolLine)
        return false;
    line += '\n';
    return true;
}

bool DecodeEvent(std::string_view line, FileConversionEvent& event) {
    std::wstring text;
    if (!Wide(line, text) || !ValidObject(text)) return false;
    Object object;
    FileConversionEvent result;
    std::wstring id, type;
    if (!Members(text, object) || !StringMember(object, L"type", type, true, 64)) return false;
    if (type == L"ready" || type == L"cancelled" || type == L"stopping") {
        if (!StringMember(object, L"id", id, true, 128)) return false;
        event = std::move(result);
        return true;
    }
    if (!StringMember(object, L"id", id, true, 20) ||
        !NumberValue(id, result.generation) || !result.generation || std::to_wstring(result.generation) != id ||
        !StringArrayMember(object, L"limitations", result.limitations, 1024)) return false;
    if (type == L"capabilities") {
        result.kind = FileConversionEventKind::Capabilities;
        if (!Targets(object, result.targets)) return false;
    } else if (type == L"started") {
        result.kind = FileConversionEventKind::Started;
        const auto files = object.find(L"files");
        if (files != object.end()) {
            if (!files->second.empty() && files->second.front() == L'[') {
                if (!Files(object, result.files, false)) return false;
                result.file_count = static_cast<uint32_t>(result.files.size());
            } else if (!NumberValue(files->second, result.file_count) || result.file_count > kMaxInputs) return false;
        }
    } else if (type == L"progress") {
        result.kind = FileConversionEventKind::Progress;
        const auto progress = object.find(L"progress");
        if (progress == object.end() || !NumberValue(progress->second, result.progress) ||
            !std::isfinite(result.progress) || result.progress < 0 || result.progress > 1 ||
            !Files(object, result.files, false)) return false;
    } else if (type == L"completed") {
        result.kind = FileConversionEventKind::Completed;
        result.progress = 1.0;
        if (!Files(object, result.files, true) || !StringMember(object, L"state", result.state, false, 64)) return false;
        result.file_count = static_cast<uint32_t>(result.files.size());
    } else if (type == L"error") {
        result.kind = FileConversionEventKind::Failed;
        result.error = ERROR_GEN_FAILURE;
        if (!StringMember(object, L"errorKey", result.error_key, false, 128) ||
            !StringMember(object, L"error", result.error_text, false) || !Files(object, result.files, false)) return false;
        if (result.error_key.empty() && !StringMember(object, L"code", result.error_key, false, 128)) return false;
        if (result.error_text.empty() && !StringMember(object, L"detail", result.error_text, false)) return false;
    } else return false;
    event = std::move(result);
    return true;
}

} // namespace pulse::app::file_conversion
