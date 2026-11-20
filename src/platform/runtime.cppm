module;
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

export module nextbuild.runtime;

export namespace nb::runtime {
using Arguments = std::vector<std::string>;
enum class Stream { stdout_stream, stderr_stream };
enum class Mode { on_demand, stream };
using Reader = std::function<void(Stream, std::string_view)>;

struct Options {
    Mode mode = Mode::on_demand;
    Reader reader;
};

struct Result {
    unsigned exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
};

Result execute(const Arguments& args, const Options& options = {});

class Pool {
public:
    explicit Pool(unsigned jobs);
    ~Pool();
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    std::future<Result> submit(Arguments args, Options options = {});

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    bool closing_ = false;
    std::deque<std::packaged_task<Result()>> pending_;
    std::vector<std::thread> workers_;
    void worker();
    void close();
};
}

namespace nb::runtime {
namespace {
struct Output {
    const Options& options;
    Result result;
    std::exception_ptr failure;

    void append(Stream stream, std::string_view bytes) {
        if (failure) return;
        try {
            if (options.mode == Mode::stream) options.reader(stream, bytes);
            else if (stream == Stream::stdout_stream) result.stdout_text.append(bytes);
            else result.stderr_text.append(bytes);
        } catch (...) {
            failure = std::current_exception();
        }
    }
};

#ifdef _WIN32
struct Handle {
    HANDLE value{};
    ~Handle() { close(); }
    Handle() = default;
    explicit Handle(HANDLE value) : value(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void close() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = nullptr;
    }
};

[[noreturn]] void windows_error() {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
}

std::wstring widen(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end())).native();
}

std::wstring quote(const std::wstring& text) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t ch : text) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += ch;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

struct Attributes {
    std::vector<std::byte> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;

    explicit Attributes(std::array<HANDLE, 3>& handles) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (!size) windows_error();
        storage.resize(size);
        list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) windows_error();
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       handles.data(), sizeof(handles), nullptr, nullptr)) {
            const auto error = GetLastError();
            DeleteProcThreadAttributeList(list);
            SetLastError(error);
            windows_error();
        }
    }
    ~Attributes() { DeleteProcThreadAttributeList(list); }
};

struct Pipe {
    Handle reader, writer;

    explicit Pipe(SECURITY_ATTRIBUTES& security) {
        if (!CreatePipe(&reader.value, &writer.value, &security, 0)) windows_error();
        if (!SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0)) windows_error();
    }

    bool read_available(Output& output, Stream stream) {
        if (!reader.value) return false;
        DWORD available = 0;
        if (!PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) windows_error();
            reader.close();
            return false;
        }
        if (!available) return false;
        char bytes[8192];
        DWORD count = 0;
        if (!ReadFile(reader.value, bytes, std::min(available, static_cast<DWORD>(sizeof(bytes))),
                      &count, nullptr)) windows_error();
        output.append(stream, {bytes, count});
        return count != 0;
    }
};

unsigned launch(const Arguments& args, Output& output) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Pipe standard_output(security), standard_error(security);
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (input.value == INVALID_HANDLE_VALUE) windows_error();
    std::array handles{input.value, standard_output.writer.value, standard_error.writer.value};
    Attributes attributes(handles);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = handles[0];
    startup.StartupInfo.hStdOutput = handles[1];
    startup.StartupInfo.hStdError = handles[2];
    startup.lpAttributeList = attributes.list;
    std::wstring command;
    for (const auto& arg : args) {
        if (!command.empty()) command += L' ';
        command += quote(widen(arg));
    }
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, nullptr, &startup.StartupInfo, &child)) windows_error();
    Handle process(child.hProcess), thread(child.hThread);
    standard_output.writer.close();
    standard_error.writer.close();
    try {
        while (standard_output.reader.value || standard_error.reader.value) {
            const bool stdout_read = standard_output.read_available(output, Stream::stdout_stream);
            const bool stderr_read = standard_error.read_available(output, Stream::stderr_stream);
            if (!stdout_read && !stderr_read && (standard_output.reader.value || standard_error.reader.value))
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    } catch (...) {
        standard_output.reader.close();
        standard_error.reader.close();
        WaitForSingleObject(process.value, INFINITE);
        throw;
    }
    if (WaitForSingleObject(process.value, INFINITE) == WAIT_FAILED) windows_error();
    DWORD status;
    if (!GetExitCodeProcess(process.value, &status)) windows_error();
    return status;
}
#else
void check(int error) {
    if (error) throw std::system_error(error, std::generic_category());
}

struct Descriptor {
    int value = -1;
    ~Descriptor() { reset(); }
    void reset() {
        if (value >= 0) close(value);
        value = -1;
    }
};

struct Pipe {
    Descriptor reader, writer;

    Pipe() {
        int fds[2];
#ifdef __linux__
        if (pipe2(fds, O_CLOEXEC) == -1) check(errno);
#else
        if (pipe(fds) == -1) check(errno);
#endif
        reader.value = fds[0];
        writer.value = fds[1];
        for (auto* fd : {&reader, &writer}) {
            if (fd->value <= STDERR_FILENO) {
                const auto replacement = fcntl(fd->value, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
                if (replacement == -1) check(errno);
                fd->reset();
                fd->value = replacement;
            } else if (fcntl(fd->value, F_SETFD, FD_CLOEXEC) == -1) check(errno);
        }
    }
};

struct Actions {
    posix_spawn_file_actions_t actions;
    Actions() { check(posix_spawn_file_actions_init(&actions)); }
    ~Actions() { posix_spawn_file_actions_destroy(&actions); }
};

unsigned launch(const Arguments& args, Output& output) {
#ifndef __linux__
    // pipe() and FD_CLOEXEC are separate operations on macOS.
    static std::mutex spawn_mutex;
    std::unique_lock spawn_lock(spawn_mutex);
#endif
    Pipe standard_output, standard_error;
    Actions actions;
    check(posix_spawn_file_actions_addopen(&actions.actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
    check(posix_spawn_file_actions_adddup2(&actions.actions, standard_output.writer.value, STDOUT_FILENO));
    check(posix_spawn_file_actions_adddup2(&actions.actions, standard_error.writer.value, STDERR_FILENO));
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t child;
    check(posix_spawnp(&child, argv.front(), &actions.actions, nullptr, argv.data(), environ));
    standard_output.writer.reset();
    standard_error.writer.reset();
#ifndef __linux__
    spawn_lock.unlock();
#endif
    std::array<pollfd, 2> readers{{{standard_output.reader.value, POLLIN, 0},
                                   {standard_error.reader.value, POLLIN, 0}}};
    std::exception_ptr failure;
    try {
        while (readers[0].fd >= 0 || readers[1].fd >= 0) {
            if (poll(readers.data(), readers.size(), -1) < 0) {
                if (errno == EINTR) continue;
                check(errno);
            }
            for (std::size_t i = 0; i < readers.size(); ++i) {
                auto& fd = readers[i];
                if (!fd.revents) continue;
                char bytes[8192];
                const auto count = read(fd.fd, bytes, sizeof(bytes));
                if (count > 0)
                    output.append(i == 0 ? Stream::stdout_stream : Stream::stderr_stream,
                                  {bytes, static_cast<std::size_t>(count)});
                else if (count == 0) fd.fd = -1;
                else if (errno != EINTR) check(errno);
            }
        }
    } catch (...) {
        failure = std::current_exception();
        standard_output.reader.reset();
        standard_error.reader.reset();
    }
    int status;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) check(errno);
    if (failure) std::rethrow_exception(failure);
    return static_cast<unsigned>(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}
#endif
}

Result execute(const Arguments& args, const Options& options) {
    if (args.empty() || args.front().empty()) throw std::invalid_argument("process executable is empty");
    for (const auto& arg : args)
        if (arg.contains('\0')) throw std::invalid_argument("NUL in process argument");
    if (options.mode == Mode::stream && !options.reader)
        throw std::invalid_argument("stream mode requires an output reader");
    Output state{options, {}, {}};
    state.result.exit_code = launch(args, state);
    if (state.failure) std::rethrow_exception(state.failure);
    return std::move(state.result);
}

Pool::Pool(unsigned jobs) {
    if (!jobs) throw std::invalid_argument("runtime worker count must be positive");
    try {
        workers_.reserve(jobs);
        for (unsigned i = 0; i < jobs; ++i) workers_.emplace_back([this] { worker(); });
    } catch (...) {
        close();
        throw;
    }
}

Pool::~Pool() { close(); }

void Pool::close() {
    {
        std::scoped_lock lock(mutex_);
        closing_ = true;
    }
    ready_.notify_all();
    for (auto& worker : workers_) worker.join();
    workers_.clear();
}

std::future<Result> Pool::submit(Arguments args, Options options) {
    std::packaged_task<Result()> task([args = std::move(args), options = std::move(options)] {
        return execute(args, options);
    });
    auto future = task.get_future();
    {
        std::scoped_lock lock(mutex_);
        if (closing_) throw std::logic_error("runtime pool is closing");
        pending_.push_back(std::move(task));
    }
    ready_.notify_one();
    return future;
}

void Pool::worker() {
    for (;;) {
        std::packaged_task<Result()> task;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return closing_ || !pending_.empty(); });
            if (pending_.empty()) return;
            task = std::move(pending_.front());
            pending_.pop_front();
        }
        task();
    }
}
}
