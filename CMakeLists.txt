cmake_minimum_required(VERSION 3.30)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NEXTBUILD_VERSION LIMIT_COUNT 1)
project(nextbuild VERSION "${NEXTBUILD_VERSION}" LANGUAGES C CXX)

include(GNUInstallDirs)
include(CTest)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED YES)
set(CMAKE_C_EXTENSIONS NO)

if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
elseif(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-finput-charset=UTF-8>")
endif()

add_library(nextbuild_manifest_generated STATIC
    generated/manifest/parser.c
    generated/manifest/nextbuildmanifest_parse.c
    generated/manifest/nextbuildmanifest_sem.c
)
target_include_directories(nextbuild_manifest_generated PUBLIC generated/manifest)
target_compile_definitions(nextbuild_manifest_generated PUBLIC PARSLEY_STATIC)
target_compile_definitions(nextbuild_manifest_generated INTERFACE PARSLEY_NO_COMPAT)
if(UNIX AND NOT APPLE)
    target_link_libraries(nextbuild_manifest_generated PRIVATE m)
endif()

add_library(nextbuild_core STATIC)
target_sources(nextbuild_core PUBLIC FILE_SET CXX_MODULES FILES
    src/support/files.cppm
    src/support/json.cppm
    src/manifest/manifest.cppm
    src/platform/process.cppm
    src/build/build.cppm
    src/lsp/server.cppm
)
target_link_libraries(nextbuild_core PRIVATE nextbuild_manifest_generated)
target_compile_definitions(nextbuild_core PRIVATE NEXTBUILD_VERSION="${PROJECT_VERSION}")

add_executable(nextbuild src/app/main.cpp)
target_link_libraries(nextbuild PRIVATE nextbuild_core)
if(MINGW)
    target_link_options(nextbuild PRIVATE -municode)
endif()
install(TARGETS nextbuild RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES generated/manifest/LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/nextbuild"
        RENAME Parsley-LICENSE)

find_program(PARSLEY_EXECUTABLE NAMES parsley DOC "Parsley 2.35.1 executable for regenerating the manifest parser")
if(PARSLEY_EXECUTABLE)
    foreach(mode IN ITEMS regenerate check)
        add_custom_target(manifest-${mode}
            COMMAND "${CMAKE_COMMAND}"
                "-DPARSLEY=${PARSLEY_EXECUTABLE}"
                "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/parsley-${mode}"
                "-DMODE=${mode}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ManifestParser.cmake"
            VERBATIM
        )
    endforeach()
endif()

if(BUILD_TESTING)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/manifest_test_main.cpp"
         CONTENT "import nextbuild.manifest_tests;\nint main() { return manifest_tests(); }\n")
    add_executable(nextbuild_manifest_tests "${CMAKE_CURRENT_BINARY_DIR}/manifest_test_main.cpp")
    target_sources(nextbuild_manifest_tests PRIVATE FILE_SET CXX_MODULES FILES tests/manifest.cppm)
    target_link_libraries(nextbuild_manifest_tests PRIVATE nextbuild_core)
    add_test(NAME nextbuild.manifest COMMAND nextbuild_manifest_tests)
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
