/*
 * SPDX-FileCopyrightText: 2024 Halo Project
 * SPDX-License-Identifier: CC0-1.0
 *
 * Matter Devices - Extended Color Light and Window Covering endpoints
 * Provides native Google Home, Apple HomeKit, and Alexa control.
 */

#ifndef MATTER_DEVICES_H
#define MATTER_DEVICES_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
   MATTER DEVICE CONFIGURATION
   ============================================================================ */

/* Matter endpoint IDs */
#define MATTER_ENDPOINT_ROOT        0       /* Root node (required) */
#define MATTER_ENDPOINT_LIGHT       1       /* Extended color light (LED ring) */
#define MATTER_ENDPOINT_BLINDS      2       /* Window covering (Zigbee blinds) */

/* Commissioning info - displayed on device label/QR code */
#define MATTER_SETUP_CODE           "12345678"   /* 8-digit setup code */
#define MATTER_DISCRIMINATOR        0x0F00       /* 12-bit unique identifier */

/* ============================================================================
   LIGHT STATE (synced with halo.c LED control)
   ============================================================================ */

typedef struct {
    bool on;                    /* On/Off state */
    uint8_t brightness;         /* 0-254 (Matter level) */
    uint8_t hue;                /* 0-254 (Matter hue) */
    uint8_t saturation;         /* 0-254 (Matter saturation) */
    uint8_t color_temp_mireds;  /* Color temperature in mireds (optional) */
} matter_light_state_t;

/* ============================================================================
   BLINDS STATE (synced with Zigbee blind control)
   ============================================================================ */

typedef struct {
    uint8_t current_position;   /* 0-100% (0=closed, 100=open) */
    uint8_t target_position;    /* Target position */
    bool is_moving;             /* Currently in motion */
} matter_blinds_state_t;

/* ============================================================================
   CALLBACKS - Called when Matter receives commands
   ============================================================================
   Implement these in halo.c to handle Matter commands.
   ============================================================================ */

/**
 * @brief Callback when light on/off state changes
 * @param on New on/off state
 */
typedef void (*matter_light_on_off_cb_t)(bool on);

/**
 * @brief Callback when light brightness changes
 * @param brightness New brightness (0-100%)
 */
typedef void (*matter_light_brightness_cb_t)(uint8_t brightness);

/**
 * @brief Callback when light color changes (RGB mode)
 * @param r Red component (0-255)
 * @param g Green component (0-255)
 * @param b Blue component (0-255)
 */
typedef void (*matter_light_color_cb_t)(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Callback when light color temperature changes (White mode)
 * @param mireds Color temperature in mireds (153=cool/6500K, 370=warm/2700K, 500=very warm/2000K)
 * 
 * For RGBW lights: Use this to control the White channel.
 * When this is called, turn OFF RGB and use only the W channel.
 * When light_color is called, turn OFF W and use RGB.
 */
typedef void (*matter_light_color_temp_cb_t)(uint16_t mireds);

/**
 * @brief Callback when blinds position changes
 * @param position Target position (0=closed, 100=open)
 */
typedef void (*matter_blinds_position_cb_t)(uint8_t position);

/**
 * @brief Callback when blinds stop command received
 */
typedef void (*matter_blinds_stop_cb_t)(void);

/* Callback registration structure */
typedef struct {
    matter_light_on_off_cb_t        light_on_off;
    matter_light_brightness_cb_t    light_brightness;
    matter_light_color_cb_t         light_color;         /* RGB mode (hue/saturation) */
    matter_light_color_temp_cb_t    light_color_temp;    /* White mode (color temperature) */
    matter_blinds_position_cb_t     blinds_position;
    matter_blinds_stop_cb_t         blinds_stop;
} matter_callbacks_t;

/* ============================================================================
   INITIALIZATION
   ============================================================================ */

/**
 * @brief Initialize Matter stack and register devices
 * 
 * Creates two Matter endpoints:
 * - Extended Color Light (endpoint 1) - LED ring
 * - Window Covering (endpoint 2) - Zigbee blinds
 * 
 * @param callbacks Pointer to callback functions for handling commands
 * @return ESP_OK on success
 */
esp_err_t matter_devices_init(const matter_callbacks_t *callbacks);

/**
 * @brief Start Matter commissioning (make device discoverable)
 * 
 * Opens the device for commissioning via QR code or setup code.
 * Called after WiFi is connected.
 * 
 * @return ESP_OK on success
 */
esp_err_t matter_start_commissioning(void);

/**
 * @brief Check if Matter is commissioned (paired with a controller)
 * 
 * @return true if commissioned, false if waiting for pairing
 */
bool matter_is_commissioned(void);

/* ============================================================================
   STATE UPDATES - Call these to sync state FROM hardware TO Matter
   ============================================================================ */

/**
 * @brief Update light on/off state in Matter
 * @param on Current on/off state
 */
void matter_update_light_on_off(bool on);

/**
 * @brief Update light brightness in Matter
 * @param brightness Current brightness (0-100%)
 */
void matter_update_light_brightness(uint8_t brightness);

/**
 * @brief Update light color in Matter
 * @param r Red (0-255)
 * @param g Green (0-255)
 * @param b Blue (0-255)
 */
void matter_update_light_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Update blinds position in Matter
 * @param position Current position (0-100%)
 * @param is_moving Whether blinds are currently moving
 */
void matter_update_blinds_position(uint8_t position, bool is_moving);

/* ============================================================================
   FACTORY RESET
   ============================================================================ */

/**
 * @brief Factory reset Matter - removes all commissioning data
 * 
 * Device will need to be re-paired after this.
 */
void matter_factory_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* MATTER_DEVICES_H */

