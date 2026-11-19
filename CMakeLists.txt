cmake_minimum_required(VERSION 3.21)
project(nextbuild VERSION 0.1.0 LANGUAGES CXX)

add_executable(nextbuild main.cpp build.cpp compile.cpp product.cpp process.cpp lsp.cpp)
if(MINGW)
    target_link_options(nextbuild PRIVATE -municode)
endif()
if(MSVC)
    target_compile_options(nextbuild PRIVATE /utf-8)
endif()
set_target_properties(nextbuild PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
install(TARGETS nextbuild RUNTIME DESTINATION bin)
