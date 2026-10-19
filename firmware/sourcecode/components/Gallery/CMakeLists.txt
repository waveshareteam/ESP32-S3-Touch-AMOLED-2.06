set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

# Keep the source list fixed so ESP-IDF's component discovery script can
# evaluate this component without CONFIGURE_DEPENDS support.
idf_component_register(
    SRCS
        "Gallery.cpp"
        "assets/img_app_gallery.c"
    INCLUDE_DIRS ${PROJ_SRC}
    REQUIRES
        brookesia_core
        esp32_s3_touch_amoled_2_06
        storage_service
        espressif__esp_new_jpeg
)

set_source_files_properties(
    "Gallery.cpp"
    PROPERTIES COMPILE_FLAGS "-Wno-missing-field-initializers"
)
