#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace RTECodeEmitter {

struct Marker {
    std::string domain;
    std::string section;
    size_t lineNumber;
};

class MarkerParser {
public:
    static std::optional<Marker> ParseLine(std::string_view line, size_t lineNumber);
    static bool IsValidSection(std::string_view section);

private:
    static std::string_view Trim(std::string_view s);
    static std::string ToLower(std::string_view s);
};

}  // namespace RTECodeEmitter
