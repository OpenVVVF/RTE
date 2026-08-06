#include <RTEAutomation/Platform.h>

#include <cstdlib>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace RTEAutomation {

char PathListSeparator() {
#ifdef _WIN32
    return ';';
#else
    return ':';
#endif
}

std::string ExecutableName(std::string name) {
#ifdef _WIN32
    if (fs::path(name).extension().empty()) name += ".exe";
#endif
    return name;
}

fs::path ExecutablePath() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) return {};
    buffer.resize(size);
    return fs::path(buffer);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(buffer.c_str(), ec);
    return ec ? fs::path(buffer.c_str()) : canonical;
#else
    std::string buffer(4096, '\0');
    const ssize_t size = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (size <= 0) return {};
    buffer.resize(static_cast<std::size_t>(size));
    return fs::path(buffer);
#endif
}

std::vector<fs::path> PathDirectories() {
    std::vector<fs::path> result;
    const char* raw = std::getenv("PATH");
    if (!raw) return result;
    std::string value(raw);
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const std::size_t end = value.find(PathListSeparator(), begin);
        const std::string part = value.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!part.empty()) result.emplace_back(part);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

std::optional<fs::path> FindExecutable(const std::string& name) {
    const fs::path input(name);
    std::error_code ec;
    if (input.has_parent_path() && fs::is_regular_file(input, ec)) {
        return fs::absolute(input, ec);
    }

    std::vector<std::string> candidates{ExecutableName(name)};
#ifdef _WIN32
    if (!input.has_extension()) {
        if (const char* pathext = std::getenv("PATHEXT")) {
            std::string extensions(pathext);
            std::size_t begin = 0;
            while (begin <= extensions.size()) {
                const std::size_t end = extensions.find(';', begin);
                std::string ext = extensions.substr(
                    begin, end == std::string::npos ? std::string::npos : end - begin);
                if (!ext.empty()) candidates.push_back(name + ext);
                if (end == std::string::npos) break;
                begin = end + 1;
            }
        }
    }
#endif
    for (const fs::path& dir : PathDirectories()) {
        for (const std::string& candidate : candidates) {
            fs::path full = dir / candidate;
            if (fs::is_regular_file(full, ec)) return full;
        }
    }
    return std::nullopt;
}

}  // namespace RTEAutomation
