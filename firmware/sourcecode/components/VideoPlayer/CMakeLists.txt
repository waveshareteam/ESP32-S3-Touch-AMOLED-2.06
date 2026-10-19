set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

file(GLOB_RECURSE PROJ_SRCS_CPP ${PROJ_SRC}/*.cpp)
file(GLOB_RECURSE PROJ_SRCS_C ${PROJ_SRC}/*.c)

idf_component_register(
    SRCS ${PROJ_SRCS_C} ${PROJ_SRCS_CPP}
    INCLUDE_DIRS ${PROJ_SRC}
    REQUIRES
        avi_player
        brookesia_core
        bsp_extra
        esp_new_jpeg
        storage_service
)

set_source_files_properties(
    ${PROJ_SRCS_CPP}
    PROPERTIES COMPILE_FLAGS "-Wno-missing-field-initializers"
)
