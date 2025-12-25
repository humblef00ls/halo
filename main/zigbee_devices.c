/*
 * SPDX-FileCopyrightText: 2024 Halo Project
 * SPDX-License-Identifier: CC0-1.0
 *
 * Zigbee Devices - Storage and persistence implementation
 */

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "zigbee_devices.h"

static const char *TAG = "zigbee_devices";

/* ============================================================================
   NVS CONFIGURATION
   ============================================================================ */

#define NVS_NAMESPACE       "zigbee_dev"
#define NVS_KEY_COUNT       "dev_count"
#define NVS_KEY_PREFIX      "dev_"      /* Devices stored as dev_0, dev_1, etc. */

/* ============================================================================
   DEVICE STORAGE
   ============================================================================ */

static zigbee_device_t s_devices[ZIGBEE_MAX_DEVICES];
static int s_device_count = 0;
static nvs_handle_t s_nvs_handle = 0;

/* ============================================================================
   NVS HELPERS
   ============================================================================ */

static esp_err_t nvs_save_device(int index, const zigbee_device_t *device)
{
    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_PREFIX, index);
    
    esp_err_t ret = nvs_set_blob(s_nvs_handle, key, device, sizeof(zigbee_device_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save device %d: %s", index, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t nvs_load_device(int index, zigbee_device_t *device)
{
    char key[16];
    snprintf(key, sizeof(key), "%s%d", NVS_KEY_PREFIX, index);
    
    size_t size = sizeof(zigbee_device_t);
    esp_err_t ret = nvs_get_blob(s_nvs_handle, key, device, &size);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Failed to load device %d: %s", index, esp_err_to_name(ret));
    }
    return ret;
}

/* ============================================================================
   INITIALIZATION
   ============================================================================ */

esp_err_t zigbee_devices_init(void)
{
    ESP_LOGI(TAG, "Initializing Zigbee device storage...");
    
    /* Open NVS namespace */
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Load device count */
    uint8_t count = 0;
    ret = nvs_get_u8(s_nvs_handle, NVS_KEY_COUNT, &count);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* First run, no devices stored */
        s_device_count = 0;
        ESP_LOGI(TAG, "No stored devices found");
        return ESP_OK;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read device count: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Clamp to max */
    if (count > ZIGBEE_MAX_DEVICES) {
        count = ZIGBEE_MAX_DEVICES;
    }
    
    /* Load each device */
    s_device_count = 0;
    for (int i = 0; i < count; i++) {
        if (nvs_load_device(i, &s_devices[s_device_count]) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded device %d: addr=0x%04x, type=%d",
                     s_device_count, s_devices[s_device_count].short_addr,
                     s_devices[s_device_count].device_type);
            s_device_count++;
        }
    }
    
    ESP_LOGI(TAG, "Loaded %d devices from storage", s_device_count);
    return ESP_OK;
}

/* ============================================================================
   DEVICE MANAGEMENT
   ============================================================================ */

esp_err_t zigbee_devices_add(const zigbee_device_t *device)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Check if device already exists (by IEEE address) */
    for (int i = 0; i < s_device_count; i++) {
        if (memcmp(s_devices[i].ieee_addr, device->ieee_addr, 8) == 0) {
            /* Update existing device */
            ESP_LOGI(TAG, "Updating existing device 0x%04x", device->short_addr);
            memcpy(&s_devices[i], device, sizeof(zigbee_device_t));
            return zigbee_devices_save();
        }
    }
    
    /* Add new device */
    if (s_device_count >= ZIGBEE_MAX_DEVICES) {
        ESP_LOGW(TAG, "Device storage full, cannot add more devices");
        return ESP_ERR_NO_MEM;
    }
    
    memcpy(&s_devices[s_device_count], device, sizeof(zigbee_device_t));
    s_device_count++;
    
    ESP_LOGI(TAG, "Added new device 0x%04x (type=%d), total: %d",
             device->short_addr, device->device_type, s_device_count);
    
    return zigbee_devices_save();
}

esp_err_t zigbee_devices_remove(uint16_t short_addr)
{
    for (int i = 0; i < s_device_count; i++) {
        if (s_devices[i].short_addr == short_addr) {
            /* Shift remaining devices down */
            for (int j = i; j < s_device_count - 1; j++) {
                memcpy(&s_devices[j], &s_devices[j + 1], sizeof(zigbee_device_t));
            }
            s_device_count--;
            
            ESP_LOGI(TAG, "Removed device 0x%04x, remaining: %d", short_addr, s_device_count);
            return zigbee_devices_save();
        }
    }
    
    return ESP_ERR_NOT_FOUND;
}

int zigbee_devices_get_count(void)
{
    return s_device_count;
}

const zigbee_device_t* zigbee_devices_get_by_index(int index)
{
    if (index < 0 || index >= s_device_count) {
        return NULL;
    }
    return &s_devices[index];
}

const zigbee_device_t* zigbee_devices_get_by_addr(uint16_t short_addr)
{
    for (int i = 0; i < s_device_count; i++) {
        if (s_devices[i].short_addr == short_addr) {
            return &s_devices[i];
        }
    }
    return NULL;
}

/* ============================================================================
   PERSISTENCE
   ============================================================================ */

esp_err_t zigbee_devices_save(void)
{
    if (s_nvs_handle == 0) {
        ESP_LOGE(TAG, "NVS not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    /* Save device count */
    esp_err_t ret = nvs_set_u8(s_nvs_handle, NVS_KEY_COUNT, (uint8_t)s_device_count);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save device count: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Save each device */
    for (int i = 0; i < s_device_count; i++) {
        ret = nvs_save_device(i, &s_devices[i]);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    
    /* Commit changes */
    ret = nvs_commit(s_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGD(TAG, "Saved %d devices to NVS", s_device_count);
    }
    
    return ret;
}

esp_err_t zigbee_devices_clear_all(void)
{
    s_device_count = 0;
    
    if (s_nvs_handle != 0) {
        nvs_erase_all(s_nvs_handle);
        nvs_commit(s_nvs_handle);
    }
    
    ESP_LOGI(TAG, "Cleared all devices");
    return ESP_OK;
}

/* ============================================================================
   DEBUG
   ============================================================================ */

void zigbee_devices_print_all(void)
{
    ESP_LOGI(TAG, "=== Paired Zigbee Devices (%d) ===", s_device_count);
    
    if (s_device_count == 0) {
        ESP_LOGI(TAG, "  (no devices paired)");
        return;
    }
    
    for (int i = 0; i < s_device_count; i++) {
        const zigbee_device_t *dev = &s_devices[i];
        const char *type_str = "Unknown";
        
        switch (dev->device_type) {
            case ZIGBEE_DEVICE_TYPE_BLIND:  type_str = "Blind"; break;
            case ZIGBEE_DEVICE_TYPE_LIGHT:  type_str = "Light"; break;
            case ZIGBEE_DEVICE_TYPE_SWITCH: type_str = "Switch"; break;
            default: break;
        }
        
        ESP_LOGI(TAG, "  [%d] %s @ 0x%04x (EP:%d) %s",
                 i, type_str, dev->short_addr, dev->endpoint,
                 dev->is_online ? "ONLINE" : "offline");
        ESP_LOGI(TAG, "      IEEE: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                 dev->ieee_addr[7], dev->ieee_addr[6], dev->ieee_addr[5], dev->ieee_addr[4],
                 dev->ieee_addr[3], dev->ieee_addr[2], dev->ieee_addr[1], dev->ieee_addr[0]);
    }
    
    ESP_LOGI(TAG, "================================");
}

