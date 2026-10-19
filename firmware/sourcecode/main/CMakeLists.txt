idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        brookesia_app_calculator
        bsp_extra
        ButtonTest
        draw
        Gallery
        Gravitysphere
        MusicPlayer
        Recorder
        Rtc
        Settings
        SpecAnalyzer
        VideoPlayer
        VibrationTest
        XiaozhiApp
        chat_history
        storage_service
        system_status
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

# LVGL 9.5 emits attributes that GCC 14 diagnoses more strictly than earlier
# toolchains. Apply the existing example's compatibility setting to LVGL only.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    if(TARGET ${LVGL_LIB})
        target_compile_options(${LVGL_LIB} PRIVATE -Wno-error=attributes -Wno-error=cpp)
    endif()
endif()

if("chmorgan__esp-audio-player" IN_LIST build_components AND "esp_driver_i2s" IN_LIST build_components)
    idf_component_get_property(AUDIO_PLAYER_LIB chmorgan__esp-audio-player COMPONENT_LIB)
    if(TARGET ${AUDIO_PLAYER_LIB})
        target_link_libraries(${AUDIO_PLAYER_LIB} PUBLIC idf::esp_driver_i2s)
    endif()
endif()

set(STORAGE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../spiffs")
set(STORAGE_STAGING_DIR "${CMAKE_BINARY_DIR}/storage_spiffs")
set(STORAGE_STAGING_STAMP "${CMAKE_BINARY_DIR}/storage_spiffs.stamp")
set(STORAGE_SOURCE_MANIFEST "${CMAKE_BINARY_DIR}/storage_spiffs_sources.txt")

file(GLOB_RECURSE STORAGE_SOURCE_FILES CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${STORAGE_SOURCE_DIR}/*")
list(SORT STORAGE_SOURCE_FILES)
string(JOIN "\n" STORAGE_SOURCE_MANIFEST_CONTENT ${STORAGE_SOURCE_FILES})
file(GENERATE
    OUTPUT "${STORAGE_SOURCE_MANIFEST}"
    CONTENT "${STORAGE_SOURCE_MANIFEST_CONTENT}\n"
)

# AIChats uses a large Puhui glyph set. Stage it from the Registry component
# into SPIFFS so it does not consume application-image space.
set(XIAOZHI_FONTS_DIR "")
foreach(component IN LISTS build_components)
    if(component MATCHES "xiaozhi-fonts")
        idf_component_get_property(XIAOZHI_FONTS_DIR ${component} COMPONENT_DIR)
        break()
    endif()
endforeach()
if(NOT XIAOZHI_FONTS_DIR)
    message(FATAL_ERROR "xiaozhi-fonts component was not resolved")
endif()

set(XIAOZHI_COMMON_FONT "${XIAOZHI_FONTS_DIR}/cbin/font_puhui_common_30_4.bin")
if(NOT EXISTS "${XIAOZHI_COMMON_FONT}")
    message(FATAL_ERROR "xiaozhi-fonts is missing font_puhui_common_30_4.bin")
endif()
set(XIAOZHI_COMMON_FONT_STAGED "${STORAGE_STAGING_DIR}/xiaozhi/font.bin")

add_custom_command(
    OUTPUT "${STORAGE_STAGING_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${STORAGE_STAGING_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${STORAGE_STAGING_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${STORAGE_SOURCE_DIR}" "${STORAGE_STAGING_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${STORAGE_STAGING_DIR}/xiaozhi"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${XIAOZHI_COMMON_FONT}" "${XIAOZHI_COMMON_FONT_STAGED}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${STORAGE_STAGING_STAMP}"
    DEPENDS
        ${STORAGE_SOURCE_FILES}
        "${STORAGE_SOURCE_MANIFEST}"
        "${XIAOZHI_COMMON_FONT}"
    COMMENT "Staging Brookesia SPIFFS resources"
    VERBATIM
)
add_custom_target(storage_spiffs_stage DEPENDS "${STORAGE_STAGING_STAMP}")

spiffs_create_partition_image(
    storage "${STORAGE_STAGING_DIR}"
    FLASH_IN_PROJECT
    DEPENDS storage_spiffs_stage
)
