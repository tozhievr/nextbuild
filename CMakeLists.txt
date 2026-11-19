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

include(CTest)
if(BUILD_TESTING)
    add_test(NAME nextbuild.lsp COMMAND "${CMAKE_COMMAND}"
        "-DNEXTBUILD=$<TARGET_FILE:nextbuild>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/lsp test"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/lsp.cmake")
    find_program(NEXTBUILD_TEST_CLANG NAMES clang++)
    if(NEXTBUILD_TEST_CLANG)
        add_test(NAME nextbuild.products COMMAND "${CMAKE_COMMAND}"
            "-DNEXTBUILD=$<TARGET_FILE:nextbuild>"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/product test"
            "-DCLANG=${NEXTBUILD_TEST_CLANG}"
            "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.cmake")
    endif()
endif()
