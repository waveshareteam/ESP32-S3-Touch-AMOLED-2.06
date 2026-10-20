idf_component_register(
    SRCS "VibrationTest.cpp" "assets/img_app_vibration.c"
    INCLUDE_DIRS "."
    REQUIRES brookesia_core esp_driver_gpio
    WHOLE_ARCHIVE
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
