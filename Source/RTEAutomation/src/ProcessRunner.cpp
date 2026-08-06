#include <RTEAutomation/ProcessRunner.h>

#include <RTEAutomation/Platform.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace RTEAutomation {
namespace {

std::string DisplayQuote(const std::string& value) {
    if (value.find_first_of(" \t\"'") == std::string::npos) return value;
    std::string result = "\"";
    for (char c : value) result += c == '"' ? "\\\"" : std::string(1, c);
    return result + "\"";
}

void Deliver(std::string& pending, const char* data, std::size_t size,
             const ProcessOutput& callback) {
    pending.append(data, size);
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = pending.find('\n', begin);
        if (end == std::string::npos) break;
        std::string line = pending.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (callback) callback(line);
        begin = end + 1;
    }
    pending.erase(0, begin);
}

#ifdef _WIN32
std::wstring ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), size);
    return result;
}

std::wstring WindowsQuote(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring out = L"\"";
    unsigned slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out += L'"';
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            slashes = 0;
            out += c;
        }
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}
#endif

}  // namespace

std::string FormatCommandForDisplay(const ProcessSpec& spec) {
    std::string result = DisplayQuote(spec.executable.string());
    for (const auto& argument : spec.arguments) result += " " + DisplayQuote(argument);
    return result;
}

ProcessResult RunProcess(const ProcessSpec& spec, ProcessOutput output) {
    ProcessResult result;
    if (spec.executable.empty()) {
        result.error = "process executable is empty";
        return result;
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        result.error = "CreatePipe failed";
        return result;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    const std::wstring executable = spec.executable.wstring();
    std::wstring command = WindowsQuote(executable);
    for (const auto& argument : spec.arguments) {
        command += L" " + WindowsQuote(ToWide(argument));
    }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const std::wstring cwd = spec.workingDirectory.wstring();

    // Environment overrides are applied only around process creation and
    // restored immediately. Process launches are serialized by this runner's
    // callers, and this avoids shell syntax while preserving inherited state.
    std::map<std::wstring, std::optional<std::wstring>> previous;
    for (const auto& [key, value] : spec.environment) {
        const std::wstring wideKey = ToWide(key);
        DWORD needed = GetEnvironmentVariableW(wideKey.c_str(), nullptr, 0);
        if (needed) {
            std::wstring old(needed, L'\0');
            GetEnvironmentVariableW(wideKey.c_str(), old.data(), needed);
            if (!old.empty() && old.back() == L'\0') old.pop_back();
            previous[wideKey] = old;
        } else {
            previous[wideKey] = std::nullopt;
        }
        SetEnvironmentVariableW(wideKey.c_str(), ToWide(value).c_str());
    }
    const BOOL created = CreateProcessW(
        executable.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &startup, &process);
    for (const auto& [key, value] : previous) {
        SetEnvironmentVariableW(key.c_str(), value ? value->c_str() : nullptr);
    }
    CloseHandle(writePipe);
    if (!created) {
        CloseHandle(readPipe);
        result.error = "CreateProcessW failed with error " + std::to_string(GetLastError());
        return result;
    }
    result.started = true;
    std::array<char, 4096> buffer{};
    std::string pending;
    DWORD count = 0;
    while (ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &count,
                    nullptr) && count > 0) {
        Deliver(pending, buffer.data(), count, output);
    }
    if (!pending.empty() && output) output(pending);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);
    CloseHandle(readPipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#else
    int pipes[2]{};
    if (pipe(pipes) != 0) {
        result.error = std::string("pipe failed: ") + std::strerror(errno);
        return result;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipes[0]);
        close(pipes[1]);
        result.error = std::string("fork failed: ") + std::strerror(errno);
        return result;
    }
    if (pid == 0) {
        dup2(pipes[1], STDOUT_FILENO);
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        if (!spec.workingDirectory.empty()
            && chdir(spec.workingDirectory.c_str()) != 0) {
            _exit(126);
        }
        for (const auto& [key, value] : spec.environment) {
            setenv(key.c_str(), value.c_str(), 1);
        }
        std::vector<std::string> storage;
        storage.reserve(spec.arguments.size() + 1);
        storage.push_back(spec.executable.string());
        storage.insert(storage.end(), spec.arguments.begin(), spec.arguments.end());
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& value : storage) argv.push_back(value.data());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(errno == ENOENT ? 127 : 126);
    }
    result.started = true;
    close(pipes[1]);
    std::array<char, 4096> buffer{};
    std::string pending;
    ssize_t count = 0;
    while ((count = read(pipes[0], buffer.data(), buffer.size())) > 0) {
        Deliver(pending, buffer.data(), static_cast<std::size_t>(count), output);
    }
    close(pipes[0]);
    if (!pending.empty() && output) output(pending);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exitCode = 128 + WTERMSIG(status);
#endif
    if (result.exitCode != 0) {
        result.error = "process exited with code " + std::to_string(result.exitCode);
    }
    return result;
}

}  // namespace RTEAutomation
