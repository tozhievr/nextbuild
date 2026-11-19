#include "cli.hpp"
#include "server.hpp"
#include "files.hpp"
#include <iostream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int entry(const nb::Args& args) {
    try {
        if (!args.empty() && args.front() == "lsp") {
#ifdef _WIN32
            _setmode(_fileno(stdin), _O_BINARY);
            _setmode(_fileno(stdout), _O_BINARY);
#endif
            return nb::lsp(nb::Args(args.begin() + 1, args.end()));
        }
        return nb::cli(args);
    } catch (const std::exception& error) {
        std::cerr << "nextbuild: error: " << error.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    nb::Args args;
    for (int index = 1; index < argc; ++index) args.push_back(nb::utf8(nb::fs::path(argv[index])));
    return entry(args);
}
#else
int main(int argc, char** argv) { return entry(nb::Args(argv + 1, argv + argc)); }
#endif
