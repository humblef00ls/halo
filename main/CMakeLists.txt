idf_component_register(SRCS "halo.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_driver_gpio mqtt esp_adc esp_driver_ledc)
