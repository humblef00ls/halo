/*
 * SPDX-FileCopyrightText: 2024 Halo Project
 * SPDX-License-Identifier: CC0-1.0
 *
 * Zigbee Devices - Storage and persistence for paired Zigbee devices
 */

#ifndef ZIGBEE_DEVICES_H
#define ZIGBEE_DEVICES_H

#include "esp_err.h"
#include "zigbee_hub.h"

/* ============================================================================
   DEVICE STORAGE API
   ============================================================================ */

/**
 * @brief Initialize device storage
 * 
 * Loads previously paired devices from NVS.
 * 
 * @return ESP_OK on success
 */
esp_err_t zigbee_devices_init(void);

/**
 * @brief Add or update a device
 * 
 * If device with same IEEE address exists, updates it.
 * Otherwise adds as new device.
 * Automatically saves to NVS.
 * 
 * @param device Device to add/update
 * @return ESP_OK on success
 */
esp_err_t zigbee_devices_add(const zigbee_device_t *device);

/**
 * @brief Remove a device by short address
 * 
 * @param short_addr Short address of device to remove
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t zigbee_devices_remove(uint16_t short_addr);

/**
 * @brief Get count of stored devices
 * 
 * @return Number of devices
 */
int zigbee_devices_get_count(void);

/**
 * @brief Get device by index
 * 
 * @param index Index (0 to count-1)
 * @return Pointer to device, or NULL if invalid index
 */
const zigbee_device_t* zigbee_devices_get_by_index(int index);

/**
 * @brief Get device by short address
 * 
 * @param short_addr Short address to search
 * @return Pointer to device, or NULL if not found
 */
const zigbee_device_t* zigbee_devices_get_by_addr(uint16_t short_addr);

/**
 * @brief Save all devices to NVS
 * 
 * @return ESP_OK on success
 */
esp_err_t zigbee_devices_save(void);

/**
 * @brief Clear all stored devices
 * 
 * @return ESP_OK on success
 */
esp_err_t zigbee_devices_clear_all(void);

/**
 * @brief Log all devices to console
 */
void zigbee_devices_print_all(void);

#endif /* ZIGBEE_DEVICES_H */

