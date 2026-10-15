idf_component_register(
    SRCS "chat_history.c"
    INCLUDE_DIRS "include"
    REQUIRES storage_service nvs_flash esp_timer
)
