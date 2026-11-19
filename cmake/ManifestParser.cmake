execute_process(COMMAND "${PARSLEY}" --version OUTPUT_VARIABLE version
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT version STREQUAL "parsley 2.35.1")
    message(FATAL_ERROR "Manifest output is pinned to Parsley 2.35.1. Found ${version}")
endif()

set(grammar "${ROOT}/grammar/NextbuildManifest.parsley")
execute_process(COMMAND "${PARSLEY}" test "${grammar}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${PARSLEY}" build "${grammar}" -o "${OUTPUT}" --no-compile-shared
    COMMAND_ERROR_IS_FATAL ANY)

foreach(file IN ITEMS parser.c parser.h)
    set(generated "${ROOT}/generated/manifest/${file}")
    if(MODE STREQUAL "regenerate")
        file(COPY_FILE "${OUTPUT}/${file}" "${generated}" ONLY_IF_DIFFERENT)
    elseif(MODE STREQUAL "check")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files --ignore-eol
            "${OUTPUT}/${file}" "${generated}" RESULT_VARIABLE differs)
        if(NOT differs STREQUAL "0")
            message(FATAL_ERROR "Outdated generated file: ${file}. Build target manifest-regenerate.")
        endif()
    else()
        message(FATAL_ERROR "Unknown parser generation mode: ${MODE}")
    endif()
endforeach()
