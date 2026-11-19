#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace nb {
namespace fs = std::filesystem;
using Args = std::vector<std::string>;

std::string utf8(const fs::path& file);
fs::path path(std::string_view text);
std::string read(const fs::path& file);
void write(const fs::path& file, const std::string& data);
fs::path root_from(fs::path start);
std::string lower(std::string text);
bool within(const fs::path& file, const fs::path& directory);
}
