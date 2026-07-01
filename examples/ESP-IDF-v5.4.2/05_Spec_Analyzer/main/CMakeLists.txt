set(LV_DEMO_DIR ../managed_components/lvgl__lvgl/demos)
file(GLOB_RECURSE LV_DEMOS_SOURCES ${LV_DEMO_DIR}/*.c)

idf_component_register(
    SRCS main.c ${LV_DEMOS_SOURCES}
    INCLUDE_DIRS . ${LV_DEMO_DIR})

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB}
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -DLV_USE_DEMO_MUSIC
)
idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    if(TARGET ${LVGL_LIB})
        target_compile_options(${LVGL_LIB} PRIVATE -Wno-error=attributes)
    endif()
endif()
idf_build_get_property(build_components BUILD_COMPONENTS)
if("chmorgan__esp-audio-player" IN_LIST build_components AND "esp_driver_i2s" IN_LIST build_components)
    idf_component_get_property(AUDIO_PLAYER_LIB chmorgan__esp-audio-player COMPONENT_LIB)
    if(TARGET ${AUDIO_PLAYER_LIB})
        target_link_libraries(${AUDIO_PLAYER_LIB} PUBLIC idf::esp_driver_i2s)
    endif()
endif()
