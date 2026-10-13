#pragma once

#include "file_conversion_integration.h"
#include <cstddef>
#include <string_view>

namespace pulse::app::file_conversion {

inline constexpr size_t kMaxProtocolLine = 4 * 1024 * 1024;
inline constexpr size_t kMaxRequestCharacters = 1024 * 1024;
inline constexpr size_t kMaxInputs = 128;
enum class Action { Capabilities, Convert, Cancel, Shutdown };

struct Request {
    Action action = Action::Capabilities;
    uint64_t generation = 0;
    uint64_t job_id = 0;
    std::vector<std::wstring> inputs;
    std::wstring target;
    std::wstring output_directory;
    int quality = 90;
};

// Pure protocol helpers, also used by the independent regression executable.
bool AbsolutePathShape(std::wstring_view path);
bool EncodeRequest(const Request& request, std::string& line);
// Well-formed ready/cancelled/stopping controls decode as Stopped/generation 0
// and are ignored by the transport; malformed protocol data still fails.
bool DecodeEvent(std::string_view line, FileConversionEvent& event);

} // namespace pulse::app::file_conversion
