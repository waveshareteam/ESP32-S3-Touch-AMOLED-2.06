idf_component_register(
    SRCS
        "Recorder.cpp"
        "RecorderIcon.cpp"
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}
    REQUIRES brookesia_core bsp_extra storage_service esp_timer
)

set_source_files_properties(
    "Recorder.cpp"
    "RecorderIcon.cpp"
    PROPERTIES COMPILE_FLAGS "-Wno-missing-field-initializers"
)
