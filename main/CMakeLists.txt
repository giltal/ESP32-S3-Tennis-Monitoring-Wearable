idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES qmi8658 hit_detector
    PRIV_REQUIRES esp_timer esp_wifi esp_netif esp_event nvs_flash
)
