#include "MarkerParser.h"

#include <cctype>

namespace RTECodeEmitter {

namespace {

constexpr std::string_view kMarkerPrefix = "// RTE_EMIT:";

}  // namespace

std::string_view MarkerParser::Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::string MarkerParser::ToLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

bool MarkerParser::IsValidSection(std::string_view section) {
    const std::string lower = ToLower(section);
    return lower == "state" || lower == "init" || lower == "step" ||
           lower == "start" || lower == "stop";
}

std::optional<Marker> MarkerParser::ParseLine(std::string_view line, size_t lineNumber) {
    const std::string_view trimmed = Trim(line);
    if (!trimmed.starts_with(kMarkerPrefix)) {
        return std::nullopt;
    }

    std::string_view payload = Trim(trimmed.substr(kMarkerPrefix.size()));
    if (payload.empty()) {
        return std::nullopt;
    }

    // Split on whitespace: first token is domain, rest is section.
    const size_t spacePos = payload.find(' ');
    if (spacePos == std::string_view::npos) {
        return std::nullopt;
    }

    std::string_view domain = Trim(payload.substr(0, spacePos));
    std::string_view section = Trim(payload.substr(spacePos + 1));

    if (domain.empty() || section.empty()) {
        return std::nullopt;
    }

    if (!IsValidSection(section)) {
        return std::nullopt;
    }

    return Marker{std::string(domain), ToLower(section), lineNumber};
}

}  // namespace RTECodeEmitter
