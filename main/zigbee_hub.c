/*
 * SPDX-FileCopyrightText: 2024 Halo Project
 * SPDX-License-Identifier: CC0-1.0
 *
 * Zigbee Hub - Coordinator implementation for ESP32-C6
 * Based on ESP-IDF esp_zigbee_gateway example
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "esp_coexist.h"

#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"

#include "zigbee_hub.h"
#include "zigbee_devices.h"

static const char *TAG = "zigbee_hub";

/* ============================================================================
   ZIGBEE CONFIGURATION
   ============================================================================ */

#define INSTALLCODE_POLICY_ENABLE   false
#define ESP_ZB_PRIMARY_CHANNEL_MASK (1l << ZIGBEE_PRIMARY_CHANNEL)

/* Manufacturer info */
#define ESP_MANUFACTURER_CODE       0x131B              /* Espressif manufacturer code */
#define ESP_MANUFACTURER_NAME       "\x04""HALO"        /* Length-prefixed string */
#define ESP_MODEL_IDENTIFIER        "\x0B""HALO-ZB-HUB" /* Length-prefixed string */

/* Zigbee coordinator config macro */
#define ESP_ZB_ZC_CONFIG() \
    { \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR, \
        .install_code_policy = INSTALLCODE_POLICY_ENABLE, \
        .nwk_cfg.zczr_cfg = { \
            .max_children = ZIGBEE_MAX_DEVICES, \
        }, \
    }

/* ============================================================================
   STATE VARIABLES
   ============================================================================ */

static bool s_network_ready = false;
static TaskHandle_t s_zigbee_task_handle = NULL;

/* ============================================================================
   FORWARD DECLARATIONS
   ============================================================================ */

static void esp_zb_task(void *pvParameters);
static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask);

/* ============================================================================
   INITIALIZATION
   ============================================================================ */

esp_err_t zigbee_hub_init(void)
{
    ESP_LOGI(TAG, "Initializing Zigbee Hub as Coordinator...");
    
    /* Initialize device storage */
    esp_err_t ret = zigbee_devices_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize device storage");
        return ret;
    }
    
    /* Configure Zigbee platform */
    esp_zb_platform_config_t config = {
        .radio_config = {
            .radio_mode = ZB_RADIO_MODE_NATIVE,  /* Use ESP32-C6 internal radio */
        },
        .host_config = {
            .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,
        },
    };
    
    ESP_ERROR_CHECK(esp_zb_platform_config(&config));
    
    /* Create Zigbee task */
    xTaskCreate(esp_zb_task, "zigbee_main", 8192, NULL, 5, &s_zigbee_task_handle);
    
    ESP_LOGI(TAG, "Zigbee Hub initialization started");
    return ESP_OK;
}

bool zigbee_is_network_ready(void)
{
    return s_network_ready;
}

/* ============================================================================
   COMMISSIONING CALLBACK
   ============================================================================ */

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, ,
                        TAG, "Failed to start Zigbee commissioning");
}

/* ============================================================================
   DEVICE DISCOVERY CALLBACKS
   ============================================================================ */

static void zb_find_window_covering_cb(esp_zb_zdp_status_t zdo_status, uint16_t addr, 
                                        uint8_t endpoint, void *user_ctx)
{
    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Found Window Covering device at addr 0x%04x, endpoint %d", addr, endpoint);
        
        /* Store the device */
        zigbee_device_t device = {
            .short_addr = addr,
            .endpoint = endpoint,
            .device_type = ZIGBEE_DEVICE_TYPE_BLIND,
            .is_online = true,
            .current_position = 0,
        };
        esp_zb_ieee_address_by_short(addr, device.ieee_addr);
        
        zigbee_devices_add(&device);
        ESP_LOGI(TAG, "Blind device registered! Total devices: %d", zigbee_get_device_count());
    }
}

static void zb_find_on_off_cb(esp_zb_zdp_status_t zdo_status, uint16_t addr,
                               uint8_t endpoint, void *user_ctx)
{
    if (zdo_status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        ESP_LOGI(TAG, "Found On/Off device at addr 0x%04x, endpoint %d", addr, endpoint);
        
        /* Store as a light device */
        zigbee_device_t device = {
            .short_addr = addr,
            .endpoint = endpoint,
            .device_type = ZIGBEE_DEVICE_TYPE_LIGHT,
            .is_online = true,
            .current_position = 0,
        };
        esp_zb_ieee_address_by_short(addr, device.ieee_addr);
        
        zigbee_devices_add(&device);
    }
}

/* ============================================================================
   SIGNAL HANDLER - Main Zigbee event processing
   ============================================================================ */

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;
    esp_zb_zdo_signal_device_annce_params_t *dev_annce_params = NULL;
    
    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        /* Enable WiFi+Zigbee coexistence */
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
        esp_coex_wifi_i154_enable();
        ESP_LOGI(TAG, "WiFi+Zigbee coexistence enabled");
#endif
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
        
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Device started up in %sfactory-reset mode",
                     esp_zb_bdb_is_factory_new() ? "" : "non-");
            
            if (esp_zb_bdb_is_factory_new()) {
                /* First boot - form new network */
                ESP_LOGI(TAG, "Starting network formation...");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                /* Subsequent boot - network already formed */
                s_network_ready = true;
                ESP_LOGI(TAG, "Network already formed, ready for devices");
                
                /* Open network briefly for any devices that need to rejoin */
                esp_zb_bdb_open_network(60);
            }
        } else {
            ESP_LOGE(TAG, "Failed to initialize Zigbee stack: %s", esp_err_to_name(err_status));
        }
        break;
        
    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t ieee_address;
            esp_zb_get_long_address(ieee_address);
            ESP_LOGI(TAG, "Network formed successfully!");
            ESP_LOGI(TAG, "  Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                     ieee_address[7], ieee_address[6], ieee_address[5], ieee_address[4],
                     ieee_address[3], ieee_address[2], ieee_address[1], ieee_address[0]);
            ESP_LOGI(TAG, "  PAN ID: 0x%04hx", esp_zb_get_pan_id());
            ESP_LOGI(TAG, "  Channel: %d", esp_zb_get_current_channel());
            ESP_LOGI(TAG, "  Short Address: 0x%04hx", esp_zb_get_short_address());
            
            s_network_ready = true;
            
            /* Start network steering to allow devices to join */
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGW(TAG, "Network formation failed (%s), retrying...", esp_err_to_name(err_status));
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
        }
        break;
        
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Network steering started - devices can now join");
        }
        break;
        
    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
        /* New device announced itself! */
        dev_annce_params = (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        ESP_LOGI(TAG, ">>> NEW DEVICE JOINED! Short addr: 0x%04hx", dev_annce_params->device_short_addr);
        
        /* Try to find Window Covering cluster (for blinds) */
        esp_zb_zdo_match_desc_req_param_t match_req;
        match_req.dst_nwk_addr = dev_annce_params->device_short_addr;
        match_req.addr_of_interest = dev_annce_params->device_short_addr;
        
        /* Search for Window Covering cluster (0x0102) */
        uint16_t window_covering_cluster = ESP_ZB_ZCL_CLUSTER_ID_WINDOW_COVERING;
        match_req.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
        match_req.num_in_clusters = 1;
        match_req.num_out_clusters = 0;
        match_req.cluster_list = &window_covering_cluster;
        
        esp_zb_zdo_match_cluster(&match_req, zb_find_window_covering_cb, NULL);
        
        /* Also try to find On/Off cluster (lights/switches) */
        esp_zb_zdo_find_on_off_light(&match_req, zb_find_on_off_cb, NULL);
        break;
        
    case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        if (err_status == ESP_OK) {
            uint8_t *permit_duration = (uint8_t *)esp_zb_app_signal_get_params(p_sg_p);
            if (*permit_duration) {
                ESP_LOGI(TAG, "Network OPEN for joining (%d seconds)", *permit_duration);
            } else {
                ESP_LOGW(TAG, "Network CLOSED - devices cannot join");
            }
        }
        break;
        
    case ESP_ZB_ZDO_SIGNAL_PRODUCTION_CONFIG_READY:
        ESP_LOGI(TAG, "Production config %s", err_status == ESP_OK ? "ready" : "not present");
        esp_zb_set_node_descriptor_manufacturer_code(ESP_MANUFACTURER_CODE);
        break;
        
    default:
        ESP_LOGD(TAG, "ZDO signal: %s (0x%x), status: %s",
                 esp_zb_zdo_signal_to_string(sig_type), sig_type, esp_err_to_name(err_status));
        break;
    }
}

/* ============================================================================
   ZIGBEE MAIN TASK
   ============================================================================ */

static void esp_zb_task(void *pvParameters)
{
    /* Initialize Zigbee stack as coordinator */
    esp_zb_cfg_t zb_nwk_cfg = ESP_ZB_ZC_CONFIG();
    esp_zb_init(&zb_nwk_cfg);
    
    /* Set primary channel */
    esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK);
    
    /* Create endpoint list */
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    
    /* Create cluster list for our hub endpoint */
    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    
    /* Add basic cluster with manufacturer info */
    esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(NULL);
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                   ESP_MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                   ESP_MODEL_IDENTIFIER);
    esp_zb_cluster_list_add_basic_cluster(cluster_list, basic_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    
    /* Add identify cluster */
    esp_zb_cluster_list_add_identify_cluster(cluster_list, esp_zb_identify_cluster_create(NULL),
                                              ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    
    /* Configure hub endpoint */
    esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = ZIGBEE_HUB_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_REMOTE_CONTROL_DEVICE_ID,
        .app_device_version = 0,
    };
    
    esp_zb_ep_list_add_gateway_ep(ep_list, cluster_list, endpoint_config);
    
    /* Register device */
    esp_zb_device_register(ep_list);
    
    ESP_LOGI(TAG, "Zigbee stack configured, starting...");
    
    /* Start Zigbee stack */
    ESP_ERROR_CHECK(esp_zb_start(false));
    
    /* Run Zigbee main loop (does not return) */
    esp_zb_stack_main_loop();
    
    vTaskDelete(NULL);
}

/* ============================================================================
   DEVICE PAIRING
   ============================================================================ */

void zigbee_permit_join(uint8_t duration_sec)
{
    if (!s_network_ready) {
        ESP_LOGW(TAG, "Cannot permit join - network not ready");
        return;
    }
    
    ESP_LOGI(TAG, "Opening network for pairing (%d seconds)...", duration_sec);
    esp_zb_bdb_open_network(duration_sec);
}

int zigbee_get_device_count(void)
{
    return zigbee_devices_get_count();
}

const zigbee_device_t* zigbee_get_device(int index)
{
    return zigbee_devices_get_by_index(index);
}

const zigbee_device_t* zigbee_get_first_blind(void)
{
    int count = zigbee_devices_get_count();
    for (int i = 0; i < count; i++) {
        const zigbee_device_t *dev = zigbee_devices_get_by_index(i);
        if (dev && dev->device_type == ZIGBEE_DEVICE_TYPE_BLIND) {
            return dev;
        }
    }
    return NULL;
}

/* ============================================================================
   BLIND CONTROL - Window Covering Cluster Commands
   ============================================================================ */

static const zigbee_device_t* get_blind_device(uint16_t device_addr)
{
    if (device_addr == 0) {
        /* Use first blind */
        return zigbee_get_first_blind();
    }
    
    /* Find by address */
    int count = zigbee_devices_get_count();
    for (int i = 0; i < count; i++) {
        const zigbee_device_t *dev = zigbee_devices_get_by_index(i);
        if (dev && dev->short_addr == device_addr && dev->device_type == ZIGBEE_DEVICE_TYPE_BLIND) {
            return dev;
        }
    }
    return NULL;
}

esp_err_t zigbee_blind_open(uint16_t device_addr)
{
    const zigbee_device_t *blind = get_blind_device(device_addr);
    if (!blind) {
        ESP_LOGW(TAG, "No blind device found");
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "Sending OPEN command to blind 0x%04x", blind->short_addr);
    
    esp_zb_zcl_window_covering_cluster_send_cmd_req_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = blind->short_addr,
            .dst_endpoint = blind->endpoint,
            .src_endpoint = ZIGBEE_HUB_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .value = NULL,
        .cmd_id = ESP_ZB_ZCL_CMD_WINDOW_COVERING_UP_OPEN,
    };
    
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t ret = esp_zb_zcl_window_covering_cluster_send_cmd_req(&cmd_req);
    esp_zb_lock_release();
    
    return ret;
}

esp_err_t zigbee_blind_close(uint16_t device_addr)
{
    const zigbee_device_t *blind = get_blind_device(device_addr);
    if (!blind) {
        ESP_LOGW(TAG, "No blind device found");
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "Sending CLOSE command to blind 0x%04x", blind->short_addr);
    
    esp_zb_zcl_window_covering_cluster_send_cmd_req_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = blind->short_addr,
            .dst_endpoint = blind->endpoint,
            .src_endpoint = ZIGBEE_HUB_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .value = NULL,
        .cmd_id = ESP_ZB_ZCL_CMD_WINDOW_COVERING_DOWN_CLOSE,
    };
    
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t ret = esp_zb_zcl_window_covering_cluster_send_cmd_req(&cmd_req);
    esp_zb_lock_release();
    
    return ret;
}

esp_err_t zigbee_blind_stop(uint16_t device_addr)
{
    const zigbee_device_t *blind = get_blind_device(device_addr);
    if (!blind) {
        ESP_LOGW(TAG, "No blind device found");
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "Sending STOP command to blind 0x%04x", blind->short_addr);
    
    esp_zb_zcl_window_covering_cluster_send_cmd_req_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = blind->short_addr,
            .dst_endpoint = blind->endpoint,
            .src_endpoint = ZIGBEE_HUB_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .value = NULL,
        .cmd_id = ESP_ZB_ZCL_CMD_WINDOW_COVERING_STOP,
    };
    
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t ret = esp_zb_zcl_window_covering_cluster_send_cmd_req(&cmd_req);
    esp_zb_lock_release();
    
    return ret;
}

esp_err_t zigbee_blind_set_position(uint16_t device_addr, uint8_t percent)
{
    const zigbee_device_t *blind = get_blind_device(device_addr);
    if (!blind) {
        ESP_LOGW(TAG, "No blind device found");
        return ESP_ERR_NOT_FOUND;
    }
    
    /* Clamp to valid range */
    if (percent > 100) {
        percent = 100;
    }
    
    ESP_LOGI(TAG, "Setting blind 0x%04x to %d%%", blind->short_addr, percent);
    
    /* Window Covering uses "lift percentage" where 0% = fully open, 100% = fully closed
       We'll invert this so 0% = closed, 100% = open (more intuitive) */
    uint8_t lift_percent = 100 - percent;
    
    esp_zb_zcl_window_covering_cluster_send_cmd_req_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = blind->short_addr,
            .dst_endpoint = blind->endpoint,
            .src_endpoint = ZIGBEE_HUB_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .value = &lift_percent,
        .cmd_id = ESP_ZB_ZCL_CMD_WINDOW_COVERING_GO_TO_LIFT_PERCENTAGE,
    };
    
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t ret = esp_zb_zcl_window_covering_cluster_send_cmd_req(&cmd_req);
    esp_zb_lock_release();
    
    return ret;
}

