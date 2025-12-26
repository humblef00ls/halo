/*
 * SPDX-FileCopyrightText: 2024 Halo Project
 * SPDX-License-Identifier: CC0-1.0
 *
 * Zigbee Hub - Coordinator for controlling Zigbee devices (blinds, etc.)
 */

#ifndef ZIGBEE_HUB_H
#define ZIGBEE_HUB_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ============================================================================
   ZIGBEE HUB CONFIGURATION
   ============================================================================ */

#define ZIGBEE_MAX_DEVICES          10      /* Maximum number of paired devices */
#define ZIGBEE_HUB_ENDPOINT         1       /* Hub's own endpoint */
#define ZIGBEE_PRIMARY_CHANNEL      13      /* Zigbee channel (11-26, 13 is common) */
#define ZIGBEE_PAIRING_TIMEOUT      180     /* Default pairing timeout in seconds */
#define ZIGBEE_FINDER_TIMEOUT_SEC   60      /* Finder mode timeout in seconds */
#define ZIGBEE_FINDER_SCAN_INTERVAL 5       /* Scan interval during finder mode (seconds) */

/* ============================================================================
   ZIGBEE BOOT STATES
   ============================================================================ */

typedef enum {
    ZIGBEE_STATE_INITIALIZING = 0,  /* Stack initializing */
    ZIGBEE_STATE_FORMING_NETWORK,   /* Forming the network */
    ZIGBEE_STATE_FINDER_MODE,       /* Actively searching for devices to pair */
    ZIGBEE_STATE_RECONNECTING,      /* Trying to reconnect to known devices */
    ZIGBEE_STATE_READY,             /* Normal operation - devices paired */
    ZIGBEE_STATE_FAILED,            /* Initialization failed */
} zigbee_state_t;

/* ============================================================================
   DEVICE TYPES
   ============================================================================ */

typedef enum {
    ZIGBEE_DEVICE_TYPE_UNKNOWN = 0,
    ZIGBEE_DEVICE_TYPE_BLIND,           /* Window covering / blind */
    ZIGBEE_DEVICE_TYPE_LIGHT,           /* On/Off light */
    ZIGBEE_DEVICE_TYPE_SWITCH,          /* On/Off switch */
} zigbee_device_type_t;

/* ============================================================================
   DEVICE STRUCTURE
   ============================================================================ */

typedef struct {
    uint16_t short_addr;                /* Network short address */
    uint8_t ieee_addr[8];               /* IEEE 64-bit address */
    uint8_t endpoint;                   /* Device endpoint */
    zigbee_device_type_t device_type;   /* Type of device */
    bool is_online;                     /* Is device currently reachable */
    uint8_t current_position;           /* For blinds: 0=closed, 100=open */
} zigbee_device_t;

/* ============================================================================
   INITIALIZATION
   ============================================================================ */

/**
 * @brief Initialize the Zigbee hub as a coordinator
 * 
 * This creates a FreeRTOS task that runs the Zigbee stack.
 * Should be called after WiFi is connected (for coexistence).
 * 
 * @return ESP_OK on success
 */
esp_err_t zigbee_hub_init(void);

/**
 * @brief Check if Zigbee network is formed and ready
 * 
 * @return true if network is formed, false otherwise
 */
bool zigbee_is_network_ready(void);

/**
 * @brief Get current Zigbee boot state
 * 
 * @return Current state of the Zigbee subsystem
 */
zigbee_state_t zigbee_get_state(void);

/**
 * @brief Check if Zigbee finder mode is complete
 * 
 * Finder mode completes when:
 * - A device is successfully paired, OR
 * - The finder timeout (60s) expires
 * 
 * @return true if finder mode is complete (or was skipped because devices exist)
 */
bool zigbee_is_finder_complete(void);

/**
 * @brief Get the state as a human-readable string
 * 
 * @param state The state to convert
 * @return String representation of the state
 */
const char* zigbee_state_to_string(zigbee_state_t state);

/* ============================================================================
   DEVICE PAIRING
   ============================================================================ */

/**
 * @brief Open network for device pairing
 * 
 * @param duration_sec Duration in seconds to keep network open (0 to close)
 */
void zigbee_permit_join(uint8_t duration_sec);

/**
 * @brief Get count of paired devices
 * 
 * @return Number of paired devices
 */
int zigbee_get_device_count(void);

/**
 * @brief Get device by index
 * 
 * @param index Device index (0 to device_count-1)
 * @return Pointer to device info, or NULL if invalid index
 */
const zigbee_device_t* zigbee_get_device(int index);

/**
 * @brief Get first paired blind device
 * 
 * @return Pointer to blind device, or NULL if no blind paired
 */
const zigbee_device_t* zigbee_get_first_blind(void);

/* ============================================================================
   BLIND CONTROL (Window Covering Cluster)
   ============================================================================ */

/**
 * @brief Open the blind (move up)
 * 
 * @param device_addr Short address of the blind (0 for first blind)
 * @return ESP_OK on success
 */
esp_err_t zigbee_blind_open(uint16_t device_addr);

/**
 * @brief Close the blind (move down)
 * 
 * @param device_addr Short address of the blind (0 for first blind)
 * @return ESP_OK on success
 */
esp_err_t zigbee_blind_close(uint16_t device_addr);

/**
 * @brief Stop blind movement
 * 
 * @param device_addr Short address of the blind (0 for first blind)
 * @return ESP_OK on success
 */
esp_err_t zigbee_blind_stop(uint16_t device_addr);

/**
 * @brief Set blind to specific position
 * 
 * @param device_addr Short address of the blind (0 for first blind)
 * @param percent Position percentage (0=closed, 100=fully open)
 * @return ESP_OK on success
 */
esp_err_t zigbee_blind_set_position(uint16_t device_addr, uint8_t percent);

/* ============================================================================
   DEVICE SCANNING & MONITORING
   ============================================================================ */

/**
 * @brief Start periodic device scanning/listing
 * 
 * Logs all known devices every interval_sec seconds.
 * Also keeps network open for new devices to join.
 * 
 * @param interval_sec Interval between scans (0 to stop scanning)
 */
void zigbee_start_device_scan(uint16_t interval_sec);

/**
 * @brief Stop periodic device scanning
 */
void zigbee_stop_device_scan(void);

/**
 * @brief Print current network status and all known devices
 * 
 * Logs to console: network info, channel, PAN ID, and list of all devices
 */
void zigbee_print_network_status(void);

/**
 * @brief Scan for devices in the neighbor table
 * 
 * Iterates through the Zigbee stack's neighbor table and logs all entries.
 * This shows devices that are currently in radio range.
 */
void zigbee_scan_neighbors(void);

#endif /* ZIGBEE_HUB_H */

