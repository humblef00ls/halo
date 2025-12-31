idf_component_register(SRCS "matter_devices.cpp" "rotary_encoder.c" "halo.c" "zigbee_hub.c" "zigbee_devices.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_driver_gpio mqtt esp_driver_ledc esp_coex esp_matter)

# ============================================================================
# Matter Device Information - Override default "TEST_PRODUCT" names
# ============================================================================
# These appear in Google Home, Apple HomeKit, and Alexa
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME="Halo"
    CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME="Halo\ Hub"
)
