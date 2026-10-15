cmake_minimum_required(VERSION 3.16)

get_filename_component(REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(TO_CMAKE_PATH "${REPOSITORY_ROOT}" REPOSITORY_ROOT)
add_compile_options(
    "-ffile-prefix-map=${REPOSITORY_ROOT}=."
    "-fmacro-prefix-map=${REPOSITORY_ROOT}=."
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-s3-touch-amoled-2-06-brookesia)

include("${CMAKE_CURRENT_LIST_DIR}/patches/esp-audio-player/override.cmake")

# mcp-c-sdk 2.0.1 contains bounded strncpy calls that GCC 14 cannot prove are
# terminated. Keep this compatibility exception on that managed target only.
if(TARGET __idf_espressif__mcp-c-sdk AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(__idf_espressif__mcp-c-sdk PRIVATE
        -Wno-error=stringop-truncation
    )
endif()
