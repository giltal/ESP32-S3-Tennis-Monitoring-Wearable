idf_component_register(
    SRCS "qmi8658.c"
    INCLUDE_DIRS "include"
    REQUIRES driver
    PRIV_REQUIRES esp_driver_i2c
)
