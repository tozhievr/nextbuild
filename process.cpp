#include "nextbuild.hpp"
#include <cstdlib>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace nb {
fs::path tool(std::string_view name, const fs::path& adjacent) {
    auto executable = path(name);
#ifdef _WIN32
    if (!executable.has_extension()) executable += L".exe";
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

namespace {
#ifdef _WIN32
struct Handle {
    HANDLE value{};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    explicit Handle(HANDLE value) : value(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void close() { if (value) CloseHandle(value); value = nullptr; }
};
[[noreturn]] void process_error() {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
}
std::wstring quote(const std::wstring& text) {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (const auto c : text) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        quoted.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        quoted += c;
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    return quoted + L'"';
}
std::pair<unsigned, std::string> launch(const Args& args) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle reader, writer;
    if (!CreatePipe(&reader.value, &writer.value, &security, 0)) process_error();
    if (!SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0)) process_error();
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (input.value == INVALID_HANDLE_VALUE) process_error();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.value;
    startup.hStdOutput = writer.value;
    startup.hStdError = writer.value;
    std::wstring command;
    for (const auto& arg : args) {
        if (!command.empty()) command += L' ';
        command += quote(path(arg).native());
    }
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(path(args.front()).c_str(), command.data(), nullptr, nullptr, TRUE,
                        0, nullptr, nullptr, &startup, &child)) process_error();
    Handle process(child.hProcess), thread(child.hThread);
    writer.close();
    std::string output;
    char chunk[4096];
    DWORD count;
    while (ReadFile(reader.value, chunk, sizeof(chunk), &count, nullptr) && count)
        output.append(chunk, count);
    if (WaitForSingleObject(process.value, INFINITE) == WAIT_FAILED) process_error();
    DWORD status;
    if (!GetExitCodeProcess(process.value, &status)) process_error();
    return {status, std::move(output)};
}
#else
struct Pipe {
    int reader = -1, writer = -1;
    ~Pipe() {
        if (reader >= 0) close(reader);
        if (writer >= 0) close(writer);
    }
};
void check(int error) {
    if (error) throw std::system_error(error, std::generic_category());
}
struct Actions {
    posix_spawn_file_actions_t actions;
    Actions() { check(posix_spawn_file_actions_init(&actions)); }
    ~Actions() { posix_spawn_file_actions_destroy(&actions); }
};
std::pair<unsigned, std::string> launch(const Args& args) {
    int fds[2];
    if (pipe(fds) == -1) check(errno);
    Pipe pipe{fds[0], fds[1]};
    Actions actions;
    check(posix_spawn_file_actions_addopen(&actions.actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
    check(posix_spawn_file_actions_addclose(&actions.actions, pipe.reader));
    check(posix_spawn_file_actions_adddup2(&actions.actions, pipe.writer, STDOUT_FILENO));
    check(posix_spawn_file_actions_adddup2(&actions.actions, pipe.writer, STDERR_FILENO));
    check(posix_spawn_file_actions_addclose(&actions.actions, pipe.writer));
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t child;
    check(posix_spawnp(&child, argv.front(), &actions.actions, nullptr, argv.data(), environ));
    close(pipe.writer);
    pipe.writer = -1;
    std::string output;
    char chunk[4096];
    while (true) {
        const auto count = ::read(pipe.reader, chunk, sizeof(chunk));
        if (count > 0) output.append(chunk, count);
        else if (count == 0 || errno != EINTR) break;
    }
    int status;
    while (waitpid(child, &status, 0) == -1) {
        if (errno != EINTR) check(errno);
    }
    return {static_cast<unsigned>(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status)),
            std::move(output)};
}
#endif
}
std::string run(const Args& args) {
    std::string command;
    for (const auto& arg : args) {
        if (arg.contains('\0')) throw std::runtime_error("NUL in process argument");
        if (!command.empty()) command += ' ';
        command += arg;
    }
    const auto [status, output] = launch(args);
    if (status) throw std::runtime_error("command failed (" + std::to_string(status) + "): " +
                                         command + "\n" + output);
    return output;
}
}
