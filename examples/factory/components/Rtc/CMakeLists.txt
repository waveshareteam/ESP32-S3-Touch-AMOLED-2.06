set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

idf_component_register(
    SRCS "Rtc.cpp"
    INCLUDE_DIRS ${PROJ_SRC}
    REQUIRES brookesia_core waveshare__esp32_s3_touch_amoled_2_06 pcf85063a
    WHOLE_ARCHIVE
)

set_source_files_properties(
    "Rtc.cpp"
    PROPERTIES COMPILE_FLAGS "-Wno-missing-field-initializers"
)
