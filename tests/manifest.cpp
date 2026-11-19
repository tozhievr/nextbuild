#include "manifest.hpp"

#include <iostream>
#include <limits>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void manifest_values() {
    const auto document = nb::parse(
        "\xef\xbb\xbf# comment\r\n"
        "@Source : @NEXTOS_MAIN_ROOT / 'a folder' // source root\r\n"
        "Version : 23.1.3\r\n"
        "EnableCompileFlag : '-DONE=\\\"value\\\"'\r\n"
        "SetNextBuildDrawProgress : true\r\n"
        "SetExecutableLayout : {\r\n"
        "  -start = 0x10000\r\n"
        "  - last = 0xffffffffffffffff\r\n"
        "}\r\n"
        "# trailing comment");
    require(document.size() == 5, "expected five statements");
    require(document[0].name == "Source" && document[0].variable && document[0].line == 2 &&
            document[0].column == 1, "variable metadata mismatch");
    const nb::Variables vars{{"NEXTOS_MAIN_ROOT", nb::path("workspace")}};
    require(nb::path_value(nb::evaluate(document[0].expr, vars)) == nb::path("workspace/a folder"),
            "path evaluation mismatch");
    require(nb::scalar(nb::evaluate(document[1].expr, vars)) == "23.1.3", "version mismatch");
    require(nb::scalar(nb::evaluate(document[2].expr, vars)) == "-DONE=\"value\"", "quote decoding mismatch");
    require(std::get<bool>(nb::evaluate(document[3].expr, vars)), "boolean mismatch");
    const auto layout = std::get<nb::Layout>(nb::evaluate(document[4].expr, vars));
    require(layout == nb::Layout{{"start", 0x10000}, {"last", std::numeric_limits<nb::Address>::max()}},
            "layout mismatch");
}

void syntax_and_positions() {
    const auto document = nb::parse("Project : '🚀' @Name : 'x'");
    require(document[1].column == 16, "UTF-16 AST column mismatch");
    require(nb::parse("").empty() && nb::parse("// comment").empty(), "empty manifest mismatch");
    for (const auto text : {"Project :\nNext", "Project : 'Next", "Project : Next /", "Project : Next ?",
                            "SetExecutableLayout : {\n - start = 0x10000"}) {
        try {
            static_cast<void>(nb::parse(text));
        } catch (const nb::SyntaxError& error) {
            require(error.line >= 1 && error.column >= 1, "invalid error position");
            continue;
        }
        throw std::runtime_error("invalid manifest was accepted");
    }
    try {
        static_cast<void>(nb::parse("Project : '🚀' ?"));
    } catch (const nb::SyntaxError& error) {
        require(error.line == 1 && error.column == 16, "UTF-16 diagnostic column mismatch");
        return;
    }
    throw std::runtime_error("invalid UTF-8 diagnostic fixture was accepted");
}
}

int main() {
    try {
        manifest_values();
        syntax_and_positions();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
