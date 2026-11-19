#pragma once
#include "files.hpp"

namespace nb {
std::string run(const Args& args);
fs::path tool(std::string_view name, const fs::path& adjacent = {});
}
