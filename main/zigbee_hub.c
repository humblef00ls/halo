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
#include "esp_timer.h"
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

/* Tuya cluster ID - used for MoES/Tuya devices */
#define TUYA_CLUSTER_ID             0xEF00

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
static esp_timer_handle_t s_scan_timer = NULL;
static uint16_t s_scan_interval_sec = 0;

/* Debug timer for periodic position query */
static esp_timer_handle_t s_debug_timer = NULL;
#define ZIGBEE_DEBUG_INTERVAL_SEC  5  /* Query every 5 seconds */

/* Finder mode state */
static zigbee_state_t s_zigbee_state = ZIGBEE_STATE_INITIALIZING;
static esp_timer_handle_t s_finder_timer = NULL;
static int s_finder_elapsed_sec = 0;
static bool s_finder_complete = false;
static bool s_device_paired_during_finder = false;

/* ============================================================================
   FORWARD DECLARATIONS
   ============================================================================ */

static void esp_zb_task(void *pvParameters);
static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask);
static void finder_mode_timer_callback(void *arg);
static void start_finder_mode(void);
static void stop_finder_mode(bool paired);
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message);

/* ============================================================================
   STATE HELPERS
   ============================================================================ */

const char* zigbee_state_to_string(zigbee_state_t state)
{
    switch (state) {
        case ZIGBEE_STATE_INITIALIZING:     return "INITIALIZING";
        case ZIGBEE_STATE_FORMING_NETWORK:  return "FORMING_NETWORK";
        case ZIGBEE_STATE_FINDER_MODE:      return "FINDER_MODE";
        case ZIGBEE_STATE_RECONNECTING:     return "RECONNECTING";
        case ZIGBEE_STATE_READY:            return "READY";
        case ZIGBEE_STATE_FAILED:           return "FAILED";
        default:                            return "UNKNOWN";
    }
}

zigbee_state_t zigbee_get_state(void)
{
    return s_zigbee_state;
}

bool zigbee_is_finder_complete(void)
{
    return s_finder_complete;
}

/* ============================================================================
   FINDER MODE - Actively search for new devices
   ============================================================================ */

static void finder_mode_timer_callback(void *arg)
{
    s_finder_elapsed_sec += ZIGBEE_FINDER_SCAN_INTERVAL;
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  🔍 ZIGBEE FINDER MODE - Searching for devices...        ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "  Elapsed: %d/%d seconds", s_finder_elapsed_sec, ZIGBEE_FINDER_TIMEOUT_SEC);
    
    /* Print network status */
    if (s_network_ready) {
        ESP_LOGI(TAG, "  Network: READY on channel %d, PAN: 0x%04hx",
                 esp_zb_get_current_channel(), esp_zb_get_pan_id());
    }
    
    /* Check if we've paired a device */
    int device_count = zigbee_devices_get_count();
    if (device_count > 0) {
        ESP_LOGI(TAG, "  ✅ DEVICE FOUND! %d device(s) paired!", device_count);
        stop_finder_mode(true);
        return;
    }
    
    /* Scan neighbors */
    ESP_LOGI(TAG, "  Scanning for nearby Zigbee devices...");
    
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_nwk_neighbor_info_t neighbor_info;
    esp_zb_nwk_info_iterator_t iterator = 0;  /* Start from beginning */
    bool found_any = false;
    esp_err_t ret = esp_zb_nwk_get_next_neighbor(&iterator, &neighbor_info);
    
    while (ret == ESP_OK) {
        found_any = true;
        ESP_LOGI(TAG, "    📡 Nearby: addr=0x%04x, LQI=%d, depth=%d",
                 neighbor_info.short_addr,
                 neighbor_info.lqi,
                 neighbor_info.depth);
        ret = esp_zb_nwk_get_next_neighbor(&iterator, &neighbor_info);
    }
    esp_zb_lock_release();
    
    if (!found_any) {
        ESP_LOGI(TAG, "    (no devices in range yet - put your blind in pairing mode!)");
    }
    
    /* Keep network open for joining */
    esp_zb_bdb_open_network(ZIGBEE_FINDER_SCAN_INTERVAL + 2);
    ESP_LOGI(TAG, "  Network OPEN for pairing - waiting for devices to join...");
    ESP_LOGI(TAG, "");
    
    /* Check timeout */
    if (s_finder_elapsed_sec >= ZIGBEE_FINDER_TIMEOUT_SEC) {
        ESP_LOGW(TAG, "  ⏱️ Finder mode timeout (%d seconds) - no devices paired",
                 ZIGBEE_FINDER_TIMEOUT_SEC);
        stop_finder_mode(false);
    }
}

static void start_finder_mode(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  🔍 STARTING ZIGBEE FINDER MODE                          ║");
    ESP_LOGI(TAG, "║  Looking for Zigbee devices to pair...                   ║");
    ESP_LOGI(TAG, "║  Will scan every %d seconds for up to %d seconds          ║",
             ZIGBEE_FINDER_SCAN_INTERVAL, ZIGBEE_FINDER_TIMEOUT_SEC);
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    s_zigbee_state = ZIGBEE_STATE_FINDER_MODE;
    s_finder_elapsed_sec = 0;
    s_device_paired_during_finder = false;
    s_finder_complete = false;
    
    /* Create finder timer */
    esp_timer_create_args_t timer_args = {
        .callback = finder_mode_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "zb_finder_timer",
    };
    
    esp_err_t ret = esp_timer_create(&timer_args, &s_finder_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create finder timer: %s", esp_err_to_name(ret));
        s_finder_complete = true;
        return;
    }
    
    /* Open network for devices to join */
    esp_zb_bdb_open_network(ZIGBEE_FINDER_TIMEOUT_SEC + 10);
    
    /* Start periodic timer (every ZIGBEE_FINDER_SCAN_INTERVAL seconds) */
    ret = esp_timer_start_periodic(s_finder_timer, 
                                   (uint64_t)ZIGBEE_FINDER_SCAN_INTERVAL * 1000000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start finder timer: %s", esp_err_to_name(ret));
        esp_timer_delete(s_finder_timer);
        s_finder_timer = NULL;
        s_finder_complete = true;
        return;
    }
    
    /* Do an immediate first scan */
    finder_mode_timer_callback(NULL);
}

static void stop_finder_mode(bool paired)
{
    if (s_finder_timer) {
        esp_timer_stop(s_finder_timer);
        esp_timer_delete(s_finder_timer);
        s_finder_timer = NULL;
    }
    
    s_device_paired_during_finder = paired;
    s_finder_complete = true;
    
    if (paired) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
        ESP_LOGI(TAG, "║  ✅ FINDER MODE COMPLETE - Device paired successfully!   ║");
        ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
        s_zigbee_state = ZIGBEE_STATE_READY;
        
        /* Print what we found */
        zigbee_print_network_status();
    } else {
        ESP_LOGW(TAG, "");
        ESP_LOGW(TAG, "╔══════════════════════════════════════════════════════════╗");
        ESP_LOGW(TAG, "║  ⚠️ FINDER MODE TIMEOUT - No devices paired              ║");
        ESP_LOGW(TAG, "║  Send 'blinds:pair' via MQTT to try again later          ║");
        ESP_LOGW(TAG, "╚══════════════════════════════════════════════════════════╝");
        s_zigbee_state = ZIGBEE_STATE_READY;  /* Still ready, just no devices */
    }
    ESP_LOGI(TAG, "");
}

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
   DEVICE DISCOVERY CONTEXT
   ============================================================================ */

/* Context passed to discovery callbacks */
typedef struct {
    uint16_t short_addr;        /* Device short address from announcement */
    uint8_t ieee_addr[8];       /* IEEE address from announcement */
    bool device_registered;     /* Flag to prevent duplicate registration */
} device_discovery_ctx_t;

/* Static context - only one device discovery at a time */
static device_discovery_ctx_t s_discovery_ctx = {0};

/* ============================================================================
   DEVICE DISCOVERY CALLBACKS - Using Active Endpoint + Simple Descriptor
   ============================================================================ */

/* Forward declaration */
static void zb_simple_desc_cb(esp_zb_zdp_status_t zdo_status, esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx);

/* Callback for active endpoint request */
static void zb_active_ep_cb(esp_zb_zdp_status_t zdo_status, uint8_t ep_count, uint8_t *ep_id_list, void *user_ctx)
{
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || ep_count == 0) {
        ESP_LOGW(TAG, "Active endpoint request failed or no endpoints (status=%d, count=%d)", zdo_status, ep_count);
        return;
    }
    
    ESP_LOGI(TAG, "  Device has %d endpoint(s): ", ep_count);
    for (int i = 0; i < ep_count; i++) {
        ESP_LOGI(TAG, "    - Endpoint %d", ep_id_list[i]);
    }
    
    /* Query simple descriptor for first endpoint to find out what clusters it has */
    esp_zb_zdo_simple_desc_req_param_t simple_req = {
        .addr_of_interest = s_discovery_ctx.short_addr,
        .endpoint = ep_id_list[0],  /* Query first endpoint */
    };
    
    ESP_LOGI(TAG, "  Querying clusters on endpoint %d...", ep_id_list[0]);
    esp_zb_zdo_simple_desc_req(&simple_req, zb_simple_desc_cb, (void*)(uintptr_t)ep_id_list[0]);
}

/* Callback for simple descriptor request - tells us what clusters the device has */
static void zb_simple_desc_cb(esp_zb_zdp_status_t zdo_status, esp_zb_af_simple_desc_1_1_t *simple_desc, void *user_ctx)
{
    uint8_t endpoint = (uint8_t)(uintptr_t)user_ctx;
    
    if (zdo_status != ESP_ZB_ZDP_STATUS_SUCCESS || !simple_desc) {
        ESP_LOGW(TAG, "Simple descriptor request failed (status=%d)", zdo_status);
        return;
    }
    
    /* Already registered this device? */
    if (s_discovery_ctx.device_registered) {
        return;
    }
    
    ESP_LOGI(TAG, "  Simple Descriptor for endpoint %d:", endpoint);
    ESP_LOGI(TAG, "    Profile ID: 0x%04x", simple_desc->app_profile_id);
    ESP_LOGI(TAG, "    Device ID: 0x%04x", simple_desc->app_device_id);
    ESP_LOGI(TAG, "    Input clusters: %d, Output clusters: %d", 
             simple_desc->app_input_cluster_count, simple_desc->app_output_cluster_count);
    
    /* Check input clusters for Window Covering (0x0102), On/Off (0x0006), or Tuya (0xEF00) */
    #define TUYA_PRIVATE_CLUSTER_ID  0xEF00
    
    zigbee_device_type_t detected_type = ZIGBEE_DEVICE_TYPE_UNKNOWN;
    bool has_tuya_cluster = false;
    
    for (int i = 0; i < simple_desc->app_input_cluster_count; i++) {
        uint16_t cluster_id = simple_desc->app_cluster_list[i];
        ESP_LOGI(TAG, "    Input cluster[%d]: 0x%04x", i, cluster_id);
        
        if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_WINDOW_COVERING) {
            detected_type = ZIGBEE_DEVICE_TYPE_BLIND;
            ESP_LOGI(TAG, "      ^ Window Covering cluster - this is a BLIND!");
        } else if (cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF && detected_type == ZIGBEE_DEVICE_TYPE_UNKNOWN) {
            detected_type = ZIGBEE_DEVICE_TYPE_LIGHT;
            ESP_LOGI(TAG, "      ^ On/Off cluster - this is a LIGHT/SWITCH");
        } else if (cluster_id == TUYA_PRIVATE_CLUSTER_ID) {
            has_tuya_cluster = true;
            ESP_LOGI(TAG, "      ^ Tuya Private cluster (0xEF00) detected!");
        }
    }
    
    /* If no standard cluster found but has Tuya cluster, identify by device ID */
    if (detected_type == ZIGBEE_DEVICE_TYPE_UNKNOWN && has_tuya_cluster) {
        /* Device ID 0x0051 = Smart Plug, but Tuya uses this for many things including blinds */
        /* Device ID 0x0202 = Window Covering */
        if (simple_desc->app_device_id == 0x0202 || simple_desc->app_device_id == 0x0051) {
            detected_type = ZIGBEE_DEVICE_TYPE_TUYA_BLIND;
            ESP_LOGI(TAG, "  Detected as TUYA BLIND (Device ID: 0x%04x)", simple_desc->app_device_id);
        } else {
            /* Unknown Tuya device - register as Tuya blind anyway since that's what user has */
            detected_type = ZIGBEE_DEVICE_TYPE_TUYA_BLIND;
            ESP_LOGI(TAG, "  Unknown Tuya device (ID: 0x%04x) - treating as TUYA BLIND", simple_desc->app_device_id);
        }
    }
    
    if (detected_type == ZIGBEE_DEVICE_TYPE_UNKNOWN) {
        ESP_LOGW(TAG, "  Device has no recognized clusters, skipping");
        return;
    }
    
    /* Register the device */
    const char *type_name;
    switch (detected_type) {
        case ZIGBEE_DEVICE_TYPE_BLIND:      type_name = "BLIND"; break;
        case ZIGBEE_DEVICE_TYPE_TUYA_BLIND: type_name = "TUYA_BLIND"; break;
        case ZIGBEE_DEVICE_TYPE_LIGHT:      type_name = "LIGHT"; break;
        default:                            type_name = "UNKNOWN"; break;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
    if (detected_type == ZIGBEE_DEVICE_TYPE_BLIND) {
        ESP_LOGI(TAG, "║  🪟 WINDOW COVERING (BLIND) DEVICE REGISTERED!           ║");
    } else if (detected_type == ZIGBEE_DEVICE_TYPE_TUYA_BLIND) {
        ESP_LOGI(TAG, "║  🪟 TUYA/MOES BLIND DEVICE REGISTERED!                   ║");
    } else {
        ESP_LOGI(TAG, "║  💡 ON/OFF DEVICE (LIGHT/SWITCH) REGISTERED!             ║");
    }
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "  Address: 0x%04x, Endpoint: %d, Type: %s", 
             s_discovery_ctx.short_addr, endpoint, type_name);
    
    zigbee_device_t device = {
        .short_addr = s_discovery_ctx.short_addr,
        .endpoint = endpoint,
        .device_type = detected_type,
        .is_online = true,
        .current_position = 0,
    };
    memcpy(device.ieee_addr, s_discovery_ctx.ieee_addr, 8);
    
    zigbee_devices_add(&device);
    s_discovery_ctx.device_registered = true;
    
    ESP_LOGI(TAG, "  ✅ %s registered! Total devices: %d", type_name, zigbee_get_device_count());
    ESP_LOGI(TAG, "");
    
    /* If we're in finder mode, stop it - we found our device! */
    if (s_zigbee_state == ZIGBEE_STATE_FINDER_MODE) {
        stop_finder_mode(true);
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
                s_zigbee_state = ZIGBEE_STATE_FORMING_NETWORK;
                ESP_LOGI(TAG, "Starting network formation...");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                /* Subsequent boot - network already formed */
                s_network_ready = true;
                ESP_LOGI(TAG, "Network already formed");
                
                /* Check if we have previously paired devices */
                int device_count = zigbee_devices_get_count();
                if (device_count > 0) {
                    /* We have devices - go to reconnecting mode */
                    s_zigbee_state = ZIGBEE_STATE_RECONNECTING;
                    ESP_LOGI(TAG, "Found %d previously paired device(s) - reconnecting...", device_count);
                    
                    /* Open network for longer to allow devices to rejoin */
                    ESP_LOGI(TAG, "Opening network for 60s to allow stored devices to rejoin...");
                    esp_zb_bdb_open_network(60);
                    
                    /* Consider finder complete since we have devices */
                    s_finder_complete = true;
                    s_zigbee_state = ZIGBEE_STATE_READY;
                    
                    /* Print what devices we have */
                    zigbee_print_network_status();
                    
                    /* Send a ping to each stored device to trigger reconnection */
                    ESP_LOGI(TAG, "");
                    ESP_LOGI(TAG, "Pinging stored devices to verify connectivity...");
                    for (int i = 0; i < device_count; i++) {
                        const zigbee_device_t *dev = zigbee_devices_get_by_index(i);
                        if (dev && dev->device_type == ZIGBEE_DEVICE_TYPE_TUYA_BLIND) {
                            ESP_LOGI(TAG, "  Sending ping to Tuya blind 0x%04x...", dev->short_addr);
                            /* Send a Tuya query to wake up the device */
                            zigbee_blind_query_position(dev->short_addr);
                        }
                    }
                } else {
                    /* No devices - start finder mode */
                    ESP_LOGI(TAG, "No previously paired devices - starting finder mode...");
                    start_finder_mode();
                }
            }
        } else {
            ESP_LOGE(TAG, "Failed to initialize Zigbee stack: %s", esp_err_to_name(err_status));
            s_zigbee_state = ZIGBEE_STATE_FAILED;
            s_finder_complete = true;  /* Don't block boot */
        }
        break;
        
    case ESP_ZB_BDB_SIGNAL_FORMATION:
        if (err_status == ESP_OK) {
            esp_zb_ieee_addr_t ieee_address;
            esp_zb_get_long_address(ieee_address);
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
            ESP_LOGI(TAG, "║  ✅ ZIGBEE NETWORK FORMED SUCCESSFULLY!                  ║");
            ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
            ESP_LOGI(TAG, "  Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                     ieee_address[7], ieee_address[6], ieee_address[5], ieee_address[4],
                     ieee_address[3], ieee_address[2], ieee_address[1], ieee_address[0]);
            ESP_LOGI(TAG, "  PAN ID: 0x%04hx", esp_zb_get_pan_id());
            ESP_LOGI(TAG, "  Channel: %d", esp_zb_get_current_channel());
            ESP_LOGI(TAG, "  Short Address: 0x%04hx", esp_zb_get_short_address());
            ESP_LOGI(TAG, "");
            
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
            
            /* This is a fresh network with no devices - start finder mode */
            if (zigbee_devices_get_count() == 0) {
                start_finder_mode();
            } else {
                s_finder_complete = true;
                s_zigbee_state = ZIGBEE_STATE_READY;
            }
        }
        break;
        
    case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE:
        /* Device announced itself - could be new or rejoining */
        dev_annce_params = (esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        
        /* Check if this is a known/stored device rejoining */
        {
            bool is_known_device = false;
            int device_count = zigbee_devices_get_count();
            for (int i = 0; i < device_count; i++) {
                const zigbee_device_t *dev = zigbee_devices_get_by_index(i);
                if (dev && memcmp(dev->ieee_addr, dev_annce_params->ieee_addr, 8) == 0) {
                    is_known_device = true;
                    ESP_LOGI(TAG, "");
                    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
                    ESP_LOGI(TAG, "║  🔄 STORED DEVICE REJOINED!                              ║");
                    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
                    ESP_LOGI(TAG, "  Short Address: 0x%04hx (was 0x%04hx)", 
                             dev_annce_params->device_short_addr, dev->short_addr);
                    
                    /* Update short address if it changed */
                    if (dev->short_addr != dev_annce_params->device_short_addr) {
                        ESP_LOGI(TAG, "  ⚠️ Address changed - updating stored device");
                        zigbee_device_t updated = *dev;
                        updated.short_addr = dev_annce_params->device_short_addr;
                        updated.is_online = true;
                        zigbee_devices_add(&updated);
                    }
                    
                    ESP_LOGI(TAG, "  ✅ Device is now ONLINE and ready for commands!");
                    ESP_LOGI(TAG, "");
                    break;
                }
            }
            
            if (!is_known_device) {
                ESP_LOGI(TAG, "");
                ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
                ESP_LOGI(TAG, "║  🎉 NEW ZIGBEE DEVICE JOINED!                            ║");
                ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
                ESP_LOGI(TAG, "  Short Address: 0x%04hx", dev_annce_params->device_short_addr);
                ESP_LOGI(TAG, "  IEEE Address: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                         dev_annce_params->ieee_addr[7], dev_annce_params->ieee_addr[6],
                         dev_annce_params->ieee_addr[5], dev_annce_params->ieee_addr[4],
                         dev_annce_params->ieee_addr[3], dev_annce_params->ieee_addr[2],
                         dev_annce_params->ieee_addr[1], dev_annce_params->ieee_addr[0]);
                ESP_LOGI(TAG, "  Querying device endpoints...");
                ESP_LOGI(TAG, "");
                
                /* Store device info in discovery context for callbacks to use */
                s_discovery_ctx.short_addr = dev_annce_params->device_short_addr;
                memcpy(s_discovery_ctx.ieee_addr, dev_annce_params->ieee_addr, 8);
                s_discovery_ctx.device_registered = false;
                
                /* Query active endpoints - this tells us what endpoints the device has */
                esp_zb_zdo_active_ep_req_param_t active_ep_req = {
                    .addr_of_interest = dev_annce_params->device_short_addr,
                };
                esp_zb_zdo_active_ep_req(&active_ep_req, zb_active_ep_cb, NULL);
            }
        }
        
        /* If we're in finder mode, the device callbacks will stop it once registered */
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
        /* Log ALL signals at INFO level for debugging */
        ESP_LOGI(TAG, "[ZDO] Signal: %s (0x%x), status: %s",
                 esp_zb_zdo_signal_to_string(sig_type), sig_type, esp_err_to_name(err_status));
        break;
    }
}

/* ============================================================================
   TUYA DATA HANDLER - Parse incoming Tuya 0xEF00 reports
   ============================================================================ */

/* Current blind position (updated from device reports) */
static uint8_t s_blind_position_percent = 0;
static bool s_blind_position_known = false;

/* Parse Tuya data frame and log position updates */
static void tuya_parse_report(const uint8_t *data, uint16_t len, uint16_t src_addr)
{
    if (len < 7) {
        ESP_LOGW(TAG, "Tuya report too short: %d bytes", len);
        return;
    }
    
    /* Tuya frame format:
       [0-1] Sequence number (2 bytes)
       [2]   Data Point ID
       [3]   Data Type
       [4-5] Data Length (2 bytes, big endian)
       [6+]  Data
    */
    uint8_t dp_id = data[2];
    uint8_t data_type = data[3];
    uint16_t data_len = (data[4] << 8) | data[5];
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "┌─── TUYA REPORT from 0x%04x ───", src_addr);
    ESP_LOGI(TAG, "│ DP ID: %d, Type: %d, Len: %d", dp_id, data_type, data_len);
    
    if (len < 6 + data_len) {
        ESP_LOGW(TAG, "│ Data truncated!");
        return;
    }
    
    switch (dp_id) {
        case 1:  /* Control state */
            if (data_len >= 1) {
                const char *state = (data[6] == 0) ? "OPEN/OPENING" :
                                    (data[6] == 1) ? "STOPPED" :
                                    (data[6] == 2) ? "CLOSE/CLOSING" : "UNKNOWN";
                ESP_LOGI(TAG, "│ Control State: %d (%s)", data[6], state);
            }
            break;
            
        case 2:  /* Position percentage */
            if (data_len >= 4) {
                uint32_t pos = (data[6] << 24) | (data[7] << 16) | (data[8] << 8) | data[9];
                s_blind_position_percent = (uint8_t)pos;
                s_blind_position_known = true;
                ESP_LOGI(TAG, "│ Position: %d%% (0%%=closed, 100%%=open)", s_blind_position_percent);
            } else if (data_len >= 1) {
                s_blind_position_percent = data[6];
                s_blind_position_known = true;
                ESP_LOGI(TAG, "│ Position: %d%% (0%%=closed, 100%%=open)", s_blind_position_percent);
            }
            break;
            
        case 3:  /* Sometimes motor direction */
            ESP_LOGI(TAG, "│ Motor Direction: %d", data[6]);
            break;
            
        case 5:  /* Sometimes limit reached */
            ESP_LOGI(TAG, "│ Limit Status: %d", data[6]);
            break;
            
        case 7:  /* Work state */
            ESP_LOGI(TAG, "│ Work State: %d", data[6]);
            break;
            
        default:
            ESP_LOGI(TAG, "│ Unknown DP %d, data:", dp_id);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, &data[6], data_len, ESP_LOG_INFO);
            break;
    }
    
    ESP_LOGI(TAG, "└────────────────────────────────");
    ESP_LOGI(TAG, "");
}

/* Core action handler - receives all ZCL messages */
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    switch (callback_id) {
        case ESP_ZB_CORE_REPORT_ATTR_CB_ID: {
            esp_zb_zcl_report_attr_message_t *report = (esp_zb_zcl_report_attr_message_t *)message;
            ESP_LOGI(TAG, "Attribute Report: cluster=0x%04x, attr=0x%04x from 0x%04x",
                     report->cluster, report->attribute.id, report->src_address.u.short_addr);
            
            /* Check if it's from the Tuya cluster */
            if (report->cluster == TUYA_CLUSTER_ID) {
                ESP_LOGI(TAG, "Tuya cluster attribute report received!");
            }
            break;
        }
        
        case ESP_ZB_CORE_CMD_CUSTOM_CLUSTER_REQ_CB_ID: {
            /* Custom cluster command received (Tuya uses this) */
            esp_zb_zcl_custom_cluster_command_message_t *cmd = 
                (esp_zb_zcl_custom_cluster_command_message_t *)message;
            
            if (cmd->info.cluster == TUYA_CLUSTER_ID) {
                ESP_LOGI(TAG, "Tuya command received: cmd_id=%d, len=%d", 
                         cmd->info.command.id, cmd->data.size);
                if (cmd->data.value && cmd->data.size > 0) {
                    tuya_parse_report(cmd->data.value, cmd->data.size, 
                                      cmd->info.src_address.u.short_addr);
                }
            }
            break;
        }
        
        case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID: {
            esp_zb_zcl_set_attr_value_message_t *set_msg = 
                (esp_zb_zcl_set_attr_value_message_t *)message;
            ESP_LOGD(TAG, "Set Attribute: cluster=0x%04x, attr=0x%04x",
                     set_msg->info.cluster, set_msg->attribute.id);
            break;
        }
        
        default:
            ESP_LOGD(TAG, "ZCL action: callback_id=%d", callback_id);
            break;
    }
    
    return ESP_OK;
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
    
    /* Note: We tried adding Tuya cluster (0xEF00) as CLIENT to receive position reports,
       but it broke command sending. Commands work without it registered.
       The "cannot find custom client cluster" errors are annoying but harmless -
       the blind still receives and executes commands correctly. */
    
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
    
    /* Register action handler to receive reports from devices */
    esp_zb_core_action_handler_register(zb_action_handler);
    
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
        /* Match both standard blinds and Tuya blinds */
        if (dev && (dev->device_type == ZIGBEE_DEVICE_TYPE_BLIND || 
                    dev->device_type == ZIGBEE_DEVICE_TYPE_TUYA_BLIND)) {
            return dev;
        }
    }
    return NULL;
}

/* ============================================================================
   BLIND CONTROL - Window Covering & Tuya Commands
   ============================================================================ */

/* Helper to check if device is any type of blind */
static bool is_blind_device(const zigbee_device_t *dev)
{
    return dev && (dev->device_type == ZIGBEE_DEVICE_TYPE_BLIND || 
                   dev->device_type == ZIGBEE_DEVICE_TYPE_TUYA_BLIND);
}

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
        if (dev && dev->short_addr == device_addr && is_blind_device(dev)) {
            return dev;
        }
    }
    return NULL;
}

/* ============================================================================
   TUYA PROTOCOL - 0xEF00 Cluster Commands
   ============================================================================ */

/* TUYA_CLUSTER_ID defined at top of file */
#define TUYA_CMD_SET_DATA       0x00    /* Set data point command */

/* Tuya Data Point IDs for blinds (may vary by device) */
#define TUYA_DP_CONTROL         0x01    /* Control: 0=open, 1=stop, 2=close */
#define TUYA_DP_PERCENT         0x02    /* Position percentage 0-100 */

/* Tuya Data Types */
#define TUYA_TYPE_RAW           0x00
#define TUYA_TYPE_BOOL          0x01
#define TUYA_TYPE_VALUE         0x02    /* 4-byte integer */
#define TUYA_TYPE_STRING        0x03
#define TUYA_TYPE_ENUM          0x04    /* 1-byte enum */

/* Tuya control commands - standard Tuya protocol values */
#define TUYA_BLIND_OPEN         0x00    /* Open command */
#define TUYA_BLIND_STOP         0x01    /* Stop command */
#define TUYA_BLIND_CLOSE        0x02    /* Close command */

static uint8_t s_tuya_seq = 0;  /* Tuya sequence number */

/* Send a Tuya command via the 0xEF00 cluster */
static esp_err_t tuya_send_command(const zigbee_device_t *device, uint8_t dp_id, 
                                    uint8_t data_type, const uint8_t *data, uint8_t data_len)
{
    /* Tuya frame format:
       [0-1] Sequence number (2 bytes, big endian)
       [2]   Data Point ID
       [3]   Data Type
       [4-5] Data Length (2 bytes, big endian)
       [6+]  Data
    */
    uint8_t frame_len = 6 + data_len;
    uint8_t frame[16];  /* Max reasonable size */
    
    if (frame_len > sizeof(frame)) {
        ESP_LOGE(TAG, "Tuya frame too large");
        return ESP_ERR_INVALID_SIZE;
    }
    
    s_tuya_seq++;
    frame[0] = 0x00;           /* Sequence high byte */
    frame[1] = s_tuya_seq;     /* Sequence low byte */
    frame[2] = dp_id;          /* Data Point ID */
    frame[3] = data_type;      /* Data Type */
    frame[4] = 0x00;           /* Data length high byte */
    frame[5] = data_len;       /* Data length low byte */
    
    if (data && data_len > 0) {
        memcpy(&frame[6], data, data_len);
    }
    
    ESP_LOGI(TAG, "Sending Tuya command: DP=%d, type=%d, len=%d, seq=%d",
             dp_id, data_type, data_len, s_tuya_seq);
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, frame, frame_len, ESP_LOG_DEBUG);
    
    /* Send via custom cluster command */
    esp_zb_zcl_custom_cluster_cmd_req_t cmd_req = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = device->short_addr,
            .dst_endpoint = device->endpoint,
            .src_endpoint = ZIGBEE_HUB_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .cluster_id = TUYA_CLUSTER_ID,
        .custom_cmd_id = TUYA_CMD_SET_DATA,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
        .data = {
            .type = ESP_ZB_ZCL_ATTR_TYPE_SET,
            .size = frame_len,
            .value = frame,
        },
    };
    
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t ret = esp_zb_zcl_custom_cluster_cmd_req(&cmd_req);
    esp_zb_lock_release();
    
    /* Note: esp_zb_zcl_custom_cluster_cmd_req often returns error even when 
       the command is successfully sent. This is a known quirk - ignore it. */
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Tuya command send returned: %s (command likely sent anyway)", esp_err_to_name(ret));
    }
    
    return ESP_OK;  /* Always return OK since command usually works */
}

/* Send Tuya blind control command (open/stop/close) */
static esp_err_t tuya_blind_control(const zigbee_device_t *blind, uint8_t control_cmd)
{
    const char *cmd_name = (control_cmd == TUYA_BLIND_OPEN) ? "OPEN" :
                           (control_cmd == TUYA_BLIND_STOP) ? "STOP" : "CLOSE";
    ESP_LOGI(TAG, "Sending Tuya %s command to blind 0x%04x", cmd_name, blind->short_addr);
    
    return tuya_send_command(blind, TUYA_DP_CONTROL, TUYA_TYPE_ENUM, &control_cmd, 1);
}

/* Send Tuya blind position command (0-100%) */
static esp_err_t tuya_blind_position(const zigbee_device_t *blind, uint8_t percent)
{
    ESP_LOGI(TAG, "Sending Tuya position %d%% to blind 0x%04x", percent, blind->short_addr);
    
    /* Tuya uses 4-byte big-endian integer for VALUE type */
    uint8_t data[4] = {0, 0, 0, percent};
    return tuya_send_command(blind, TUYA_DP_PERCENT, TUYA_TYPE_VALUE, data, 4);
}

/* ============================================================================
   PUBLIC BLIND CONTROL FUNCTIONS
   ============================================================================ */

esp_err_t zigbee_blind_open(uint16_t device_addr)
{
    const zigbee_device_t *blind = get_blind_device(device_addr);
    if (!blind) {
        ESP_LOGW(TAG, "No blind device found");
        return ESP_ERR_NOT_FOUND;
    }
    
    /* Use Tuya protocol for Tuya devices */
    if (blind->device_type == ZIGBEE_DEVICE_TYPE_TUYA_BLIND) {
        return tuya_blind_control(blind, TUYA_BLIND_OPEN);
    }
    
    /* Standard Window Covering cluster */
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
    
    /* Use Tuya protocol for Tuya devices */
    if (blind->device_type == ZIGBEE_DEVICE_TYPE_TUYA_BLIND) {
        return tuya_blind_control(blind, TUYA_BLIND_CLOSE);
    }
    
    /* Standard Window Covering cluster */
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
    
    /* Use Tuya protocol for Tuya devices */
    if (blind->device_type == ZIGBEE_DEVICE_TYPE_TUYA_BLIND) {
        return tuya_blind_control(blind, TUYA_BLIND_STOP);
    }
    
    /* Standard Window Covering cluster */
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
    
    /* Use Tuya protocol for Tuya devices */
    if (blind->device_type == ZIGBEE_DEVICE_TYPE_TUYA_BLIND) {
        /* Tuya blinds: 0% = closed, 100% = open (same as our convention) */
        return tuya_blind_position(blind, percent);
    }
    
    /* Standard Window Covering cluster */
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

esp_err_t zigbee_blind_query_position(uint16_t device_addr)
{
    const zigbee_device_t *blind = get_blind_device(device_addr);
    if (!blind) {
        ESP_LOGW(TAG, "No blind device found");
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "┌─── QUERYING BLIND POSITION ───");
    ESP_LOGI(TAG, "│ Device: 0x%04x, Endpoint: %d", blind->short_addr, blind->endpoint);
    
    if (s_blind_position_known) {
        ESP_LOGI(TAG, "│ Last known position: %d%%", s_blind_position_percent);
    } else {
        ESP_LOGI(TAG, "│ Position: UNKNOWN (no report received yet)");
    }
    ESP_LOGI(TAG, "└────────────────────────────────");
    
    /* For Tuya devices, send a query command */
    if (blind->device_type == ZIGBEE_DEVICE_TYPE_TUYA_BLIND) {
        /* Tuya uses command 0x00 to get data - send empty payload to request all DPs */
        ESP_LOGI(TAG, "Sending Tuya query request...");
        
        /* Just request DP 2 (position) */
        uint8_t query_frame[3] = {0x00, s_tuya_seq++, 0x02};  /* Seq, DP ID */
        
        esp_zb_zcl_custom_cluster_cmd_req_t cmd_req = {
            .zcl_basic_cmd = {
                .dst_addr_u.addr_short = blind->short_addr,
                .dst_endpoint = blind->endpoint,
                .src_endpoint = ZIGBEE_HUB_ENDPOINT,
            },
            .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
            .profile_id = ESP_ZB_AF_HA_PROFILE_ID,
            .cluster_id = TUYA_CLUSTER_ID,
            .custom_cmd_id = 0x00,  /* Query/get data */
            .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV,
            .data = {
                .type = ESP_ZB_ZCL_ATTR_TYPE_SET,
                .size = sizeof(query_frame),
                .value = query_frame,
            },
        };
        
        esp_zb_lock_acquire(portMAX_DELAY);
        esp_zb_zcl_custom_cluster_cmd_req(&cmd_req);
        esp_zb_lock_release();
    }
    
    return ESP_OK;
}

/* ============================================================================
   DEVICE SCANNING & NETWORK STATUS
   ============================================================================ */

static const char* device_type_to_string(zigbee_device_type_t type)
{
    switch (type) {
        case ZIGBEE_DEVICE_TYPE_BLIND:      return "BLIND";
        case ZIGBEE_DEVICE_TYPE_TUYA_BLIND: return "TUYA_BLIND";
        case ZIGBEE_DEVICE_TYPE_LIGHT:      return "LIGHT";
        case ZIGBEE_DEVICE_TYPE_SWITCH:     return "SWITCH";
        default:                            return "UNKNOWN";
    }
}

void zigbee_print_network_status(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║              ZIGBEE NETWORK STATUS                       ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
    
    if (!s_network_ready) {
        ESP_LOGW(TAG, "  Network: NOT READY");
        return;
    }
    
    /* Get network info */
    esp_zb_ieee_addr_t ieee_address;
    esp_zb_get_long_address(ieee_address);
    
    ESP_LOGI(TAG, "  Network: READY");
    ESP_LOGI(TAG, "  PAN ID: 0x%04hx", esp_zb_get_pan_id());
    ESP_LOGI(TAG, "  Channel: %d", esp_zb_get_current_channel());
    ESP_LOGI(TAG, "  Short Address: 0x%04hx", esp_zb_get_short_address());
    ESP_LOGI(TAG, "  Extended PAN ID: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
             ieee_address[7], ieee_address[6], ieee_address[5], ieee_address[4],
             ieee_address[3], ieee_address[2], ieee_address[1], ieee_address[0]);
    
    /* List all paired devices */
    int count = zigbee_devices_get_count();
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  Paired Devices: %d", count);
    ESP_LOGI(TAG, "  ─────────────────────────────────────────────────────────");
    
    if (count == 0) {
        ESP_LOGI(TAG, "  (no devices paired - send 'blinds:pair' to start pairing)");
    } else {
        for (int i = 0; i < count; i++) {
            const zigbee_device_t *dev = zigbee_devices_get_by_index(i);
            if (dev) {
                ESP_LOGI(TAG, "  [%d] Addr: 0x%04x  Endpoint: %d  Type: %-7s  %s",
                         i, dev->short_addr, dev->endpoint,
                         device_type_to_string(dev->device_type),
                         dev->is_online ? "ONLINE" : "OFFLINE");
                ESP_LOGI(TAG, "      IEEE: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
                         dev->ieee_addr[7], dev->ieee_addr[6], dev->ieee_addr[5], dev->ieee_addr[4],
                         dev->ieee_addr[3], dev->ieee_addr[2], dev->ieee_addr[1], dev->ieee_addr[0]);
            }
        }
    }
    ESP_LOGI(TAG, "");
}

void zigbee_scan_neighbors(void)
{
    if (!s_network_ready) {
        ESP_LOGW(TAG, "Cannot scan neighbors - network not ready");
        return;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║              ZIGBEE NEIGHBOR SCAN                        ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
    
    /* The Zigbee stack maintains a neighbor table of devices in radio range */
    /* We can iterate through it to see what's nearby */
    
    esp_zb_lock_acquire(portMAX_DELAY);
    
    /* Get the neighbor table iterator */
    esp_zb_nwk_neighbor_info_t neighbor_info;
    bool found_any = false;
    esp_zb_nwk_info_iterator_t iterator = 0;  /* Start from beginning */
    
    /* Start iteration from beginning */
    esp_err_t ret = esp_zb_nwk_get_next_neighbor(&iterator, &neighbor_info);
    
    while (ret == ESP_OK) {
        found_any = true;
        ESP_LOGI(TAG, "  Neighbor: addr=0x%04x, relationship=%d, rx_on=%d, depth=%d, LQI=%d",
                 neighbor_info.short_addr,
                 neighbor_info.relationship,
                 neighbor_info.rx_on_when_idle,
                 neighbor_info.depth,
                 neighbor_info.lqi);
        
        /* Get next neighbor */
        ret = esp_zb_nwk_get_next_neighbor(&iterator, &neighbor_info);
    }
    
    esp_zb_lock_release();
    
    if (!found_any) {
        ESP_LOGI(TAG, "  (no neighbors found in radio range)");
    }
    ESP_LOGI(TAG, "");
}

/* Timer callback for periodic scanning */
static void scan_timer_callback(void *arg)
{
    if (!s_network_ready) {
        return;
    }
    
    /* Print network status and device list */
    zigbee_print_network_status();
    
    /* Scan neighbors in radio range */
    zigbee_scan_neighbors();
    
    /* Keep network open for devices that want to join */
    /* This allows new devices in pairing mode to be discovered */
    ESP_LOGI(TAG, "  Keeping network open for new devices...");
    esp_zb_bdb_open_network(s_scan_interval_sec + 5);  /* Keep open until next scan */
}

void zigbee_start_device_scan(uint16_t interval_sec)
{
    /* Stop existing timer if running */
    zigbee_stop_device_scan();
    
    if (interval_sec == 0) {
        ESP_LOGI(TAG, "Device scanning disabled");
        return;
    }
    
    s_scan_interval_sec = interval_sec;
    
    /* Create timer */
    esp_timer_create_args_t timer_args = {
        .callback = scan_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "zb_scan_timer",
    };
    
    esp_err_t ret = esp_timer_create(&timer_args, &s_scan_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create scan timer: %s", esp_err_to_name(ret));
        return;
    }
    
    /* Start periodic timer */
    ret = esp_timer_start_periodic(s_scan_timer, (uint64_t)interval_sec * 1000000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start scan timer: %s", esp_err_to_name(ret));
        esp_timer_delete(s_scan_timer);
        s_scan_timer = NULL;
        return;
    }
    
    ESP_LOGI(TAG, "Device scanning started (every %d seconds)", interval_sec);
    
    /* Do an immediate scan */
    scan_timer_callback(NULL);
}

void zigbee_stop_device_scan(void)
{
    if (s_scan_timer) {
        esp_timer_stop(s_scan_timer);
        esp_timer_delete(s_scan_timer);
        s_scan_timer = NULL;
        ESP_LOGI(TAG, "Device scanning stopped");
    }
    s_scan_interval_sec = 0;
}

/* ============================================================================
   DEBUG: Periodic Position Query (for debugging Tuya blinds)
   ============================================================================ */

static void debug_query_timer_callback(void *arg)
{
    if (!s_network_ready) {
        return;
    }
    
    int device_count = zigbee_devices_get_count();
    if (device_count == 0) {
        ESP_LOGW(TAG, "[DEBUG] No devices paired - send 'blinds:pair' first");
        return;
    }
    
    /* Query position of first blind */
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  [DEBUG] PERIODIC ZIGBEE STATUS CHECK");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════════");
    
    zigbee_blind_query_position(0);
}

/* Start debug mode - queries blind every 5 seconds */
void zigbee_start_debug_mode(void)
{
    /* Stop existing timer if running */
    if (s_debug_timer) {
        esp_timer_stop(s_debug_timer);
        esp_timer_delete(s_debug_timer);
        s_debug_timer = NULL;
    }
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  🔧 ZIGBEE DEBUG MODE ENABLED                            ║");
    ESP_LOGI(TAG, "║  Querying blind position every %d seconds                ║", ZIGBEE_DEBUG_INTERVAL_SEC);
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    esp_timer_create_args_t timer_args = {
        .callback = debug_query_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "zb_debug_timer",
    };
    
    esp_err_t ret = esp_timer_create(&timer_args, &s_debug_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create debug timer: %s", esp_err_to_name(ret));
        return;
    }
    
    ret = esp_timer_start_periodic(s_debug_timer, (uint64_t)ZIGBEE_DEBUG_INTERVAL_SEC * 1000000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start debug timer: %s", esp_err_to_name(ret));
        esp_timer_delete(s_debug_timer);
        s_debug_timer = NULL;
        return;
    }
    
    /* Do an immediate query */
    debug_query_timer_callback(NULL);
}

void zigbee_stop_debug_mode(void)
{
    if (s_debug_timer) {
        esp_timer_stop(s_debug_timer);
        esp_timer_delete(s_debug_timer);
        s_debug_timer = NULL;
        ESP_LOGI(TAG, "Zigbee debug mode stopped");
    }
}

