set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

# IDF evaluates component requirements in CMake script mode, where
# CONFIGURE_DEPENDS is not supported.  This component has a fixed source set.
file(GLOB_RECURSE PROJ_SRCS_C ${PROJ_SRC}/*.c)
file(GLOB_RECURSE PROJ_SRCS_CPP ${PROJ_SRC}/*.cpp)

idf_component_register(
    SRCS ${PROJ_SRCS_C} ${PROJ_SRCS_CPP}
    INCLUDE_DIRS ${PROJ_SRC}
    REQUIRES brookesia_core esp32_s3_touch_amoled_2_06 esp_driver_gpio qmi8658
    WHOLE_ARCHIVE
)

set_source_files_properties(
    ${PROJ_SRCS_CPP}
    PROPERTIES COMPILE_FLAGS "-Wno-missing-field-initializers"
)
