module;
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

export module nextbuild.runtime_tests;
import nextbuild.runtime;

namespace {
using namespace nb::runtime;
using namespace std::chrono_literals;
constexpr auto width = 32768;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

export int runtime_tests(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--capture-child") {
        std::cout << std::string(width, 'O') << std::flush;
        std::cerr << std::string(width, 'E') << std::flush;
        return 7;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--stream-child") {
        std::cout << "stdout-ready" << std::flush;
        std::cerr << "stderr-ready" << std::flush;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!std::filesystem::exists(argv[2]) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        return std::filesystem::exists(argv[2]) ? 0 : 1;
    }
    if (argc != 2) return 1;
    try {
        Pool pool(8);
        std::vector<std::future<Result>> pending;
        for (int i = 0; i < 192; ++i) pending.push_back(pool.submit({argv[0], "--capture-child"}));
        const auto stdout_expected = std::string(width, 'O');
        const auto stderr_expected = std::string(width, 'E');
        for (auto& request : pending) {
            const auto result = request.get();
            require(result.exit_code == 7, "incorrect child exit code");
            require(result.stdout_text == stdout_expected, "incorrect captured stdout");
            require(result.stderr_text == stderr_expected, "incorrect captured stderr");
        }

        const std::filesystem::path marker(argv[1]);
        std::filesystem::remove(marker);
        std::string out, err;
        const auto stream = pool.submit({argv[0], "--stream-child", argv[1]}, {
            Mode::stream, [&](Stream channel, std::string_view bytes) {
                (channel == Stream::stdout_stream ? out : err).append(bytes);
                if (out == "stdout-ready" && err == "stderr-ready") std::ofstream(marker).put('1');
            }
        }).get();
        std::filesystem::remove(marker);
        require(stream.exit_code == 0, "streams were not delivered while the process was running");
        require(stream.stdout_text.empty() && stream.stderr_text.empty(), "stream mode buffered output");
        require(out == "stdout-ready" && err == "stderr-ready", "stream mode mixed channels");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
