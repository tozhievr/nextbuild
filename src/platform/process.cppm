module;
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <mutex>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

export module nextbuild.process;
import nextbuild.files;
import nextbuild.runtime;

export namespace nb {
std::string run(const Args& args, bool stream = false);
fs::path tool(std::string_view name, const fs::path& adjacent = {});
bool terminal_color();
bool no_color_requested();
void write_output(bool error, std::string_view bytes);
}

namespace nb {
fs::path tool(std::string_view name, const fs::path& adjacent) {
    auto executable = path(name);
#ifdef _WIN32
    if (executable.has_parent_path() && fs::is_regular_file(executable))
        return fs::absolute(executable);
    if (lower(utf8(executable.extension())) != ".exe") executable += L".exe";
    if (executable.has_parent_path() && fs::is_regular_file(executable))
        return fs::absolute(executable);
    if (!adjacent.empty() && fs::is_regular_file(adjacent / executable))
        return adjacent / executable;
    std::wstring found(32768, L'\0');
    const DWORD length = SearchPathW(nullptr, executable.c_str(), nullptr,
                                     static_cast<DWORD>(found.size()), found.data(), nullptr);
    if (length && length < found.size()) return fs::path(found.substr(0, length));
#else
    const auto usable = [](const fs::path& p) {
        return fs::is_regular_file(p) && access(p.c_str(), X_OK) == 0;
    };
    if (executable.has_parent_path()) {
        if (usable(executable)) return fs::absolute(executable);
        return {};
    }
    if (!adjacent.empty() && usable(adjacent / executable)) return adjacent / executable;
    const char* env = std::getenv("PATH");
    std::string_view paths = env ? env : "/usr/bin:/bin";
    while (true) {
        const auto end = paths.find(':');
        const auto candidate = path(paths.substr(0, end)) / executable;
        if (usable(candidate)) return fs::absolute(candidate);
        if (end == std::string_view::npos) break;
        paths.remove_prefix(end + 1);
    }
#endif
    return {};
}


bool no_color_requested() {
#ifdef _WIN32
    SetLastError(ERROR_SUCCESS);
    return GetEnvironmentVariableW(L"NO_COLOR", nullptr, 0) != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
#else
    return std::getenv("NO_COLOR") != nullptr;
#endif
}

bool terminal_color() {
#ifdef _WIN32
    const auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    return GetConsoleMode(handle, &mode) &&
           SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
    return isatty(STDOUT_FILENO) == 1;
#endif
}

void write_output(bool error, std::string_view bytes) {
    static std::mutex output_mutex;
    const std::scoped_lock lock(output_mutex);
    auto& output = error ? std::cerr : std::cout;
    output << bytes << std::flush;
}

std::string run(const Args& args, bool stream) {
    runtime::Options options;
    if (stream) {
        options.mode = runtime::Mode::stream;
        options.reader = [](runtime::Stream channel, std::string_view bytes) {
            write_output(channel == runtime::Stream::stderr_stream, bytes);
        };
    }
    const auto result = runtime::execute(args, options);
    if (result.exit_code) {
        std::string command;
        for (const auto& arg : args) command += (command.empty() ? "" : " ") + arg;
        throw std::runtime_error("command failed (" + std::to_string(result.exit_code) + "): " +
                                 command + "\n" + result.stderr_text + result.stdout_text);
    }
    return result.stdout_text + result.stderr_text;
}
}
