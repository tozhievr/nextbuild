#include "files.hpp"
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace nb {
std::string utf8(const fs::path& value) {
    const auto bytes = value.u8string();
    return {bytes.begin(), bytes.end()};
}
fs::path path(std::string_view value) {
    return fs::path(std::u8string(value.begin(), value.end()));
}
std::string read(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read " + utf8(file));
    std::string result{std::istreambuf_iterator<char>(input), {}};
    if (input.bad()) throw std::runtime_error("cannot read " + utf8(file));
    return result;
}
void write(const fs::path& file, const std::string& data) {
    if (fs::is_regular_file(file) && read(file) == data) return;
    if (file.has_parent_path()) fs::create_directories(file.parent_path());
    std::ofstream output(file, std::ios::binary);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    output.close();
    if (!output) throw std::runtime_error("cannot write " + utf8(file));
}
fs::path root_from(fs::path start) {
    start = fs::weakly_canonical(fs::absolute(start));
    auto fallback = start;
    bool found_project = false;
    for (auto candidate = start;; candidate = candidate.parent_path()) {
        if (fs::is_regular_file(candidate / "tools/build.py") ||
            fs::is_regular_file(candidate / "tools/nextbuild") ||
            fs::is_regular_file(candidate / "tools/nextbuild.exe") ||
            fs::is_regular_file(candidate / "tools/nextbuild/CMakeLists.txt"))
            return candidate;
        if (!found_project && fs::is_regular_file(candidate / ".nextbuild")) {
            fallback = candidate;
            found_project = true;
        }
        if (candidate == candidate.root_path()) return fallback;
    }
}
std::string lower(std::string text) {
    for (auto& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}
bool within(const fs::path& file, const fs::path& directory) {
    const auto relative = file.lexically_relative(directory);
    return !relative.empty() && *relative.begin() != "..";
}
}
