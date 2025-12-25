/* ============================================================================
   HAMURABI LED CONTROLLER
   ============================================================================
   
   A smart LED controller for ESP32-C6 with voice control via IFTTT/Adafruit IO.
   
   Hardware:
   - Waveshare ESP32-C6-DEV-KIT-N8
   - 60× RGBW NeoPixels (SK6812) on GPIO4
   - Onboard RGB LED on GPIO8 (status indicator)
   - Charlieplex Matrix via I2C (planned)
   - nOOds 12V LED Filament via PWM (planned)
   
   Features:
   - Multiple animation modes (meteor, rainbow, breathing, solid)
   - MQTT voice control via Adafruit IO + IFTTT
   - WiFi connectivity with status indication
   - Persistent settings via NVS
   - Master brightness control
   
   License: Public Domain (CC0)
   
   ============================================================================ */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mqtt_client.h"
#include "credentials.h"  /* WiFi and Adafruit IO credentials (gitignored) */

/* Logging tags for different components */
static const char *TAG = "main";
static const char *TAG_ONBOARD = "onboard_led";
static const char *TAG_RGBW = "rgbw_neopixel";
static const char *TAG_NVS = "nvs_storage";
static const char *TAG_WIFI = "wifi";

/* ============================================================================
   WIFI STATION CONFIGURATION
   ============================================================================
   Credentials are loaded from credentials.h (gitignored).
   Copy credentials.h.template to credentials.h and fill in your values.
   ============================================================================ */

/* WIFI_SSID and WIFI_PASSWORD come from credentials.h */
#define WIFI_MAX_RETRY 10

/* Event group for WiFi status */
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int wifi_retry_count = 0;
static bool wifi_connected = false;

/* ============================================================================
   MQTT / ADAFRUIT IO CONFIGURATION
   ============================================================================
   Connects to Adafruit IO to receive voice commands from IFTTT/Google Home.
   Credentials are loaded from credentials.h (gitignored).
   ============================================================================ */

/* ADAFRUIT_IO_USERNAME, ADAFRUIT_IO_KEY, and ADAFRUIT_IO_FEED come from credentials.h */

/* Full topic path for Adafruit IO */
#define MQTT_TOPIC              ADAFRUIT_IO_USERNAME "/feeds/" ADAFRUIT_IO_FEED

static const char *TAG_MQTT = "mqtt";
static esp_mqtt_client_handle_t mqtt_client = NULL;

/* ============================================================================
   ANIMATION MODES
   ============================================================================
   Controlled via MQTT commands from voice or app.
   ============================================================================ */

typedef enum {
    ANIM_METEOR,        /* Default meteor spinner */
    ANIM_RAINBOW,       /* Rainbow cycle */
    ANIM_BREATHING,     /* Breathing/pulsing effect */
    ANIM_SOLID,         /* Solid color (no animation) */
    ANIM_OFF            /* All LEDs off */
} animation_mode_t;

/* Current animation state (volatile because modified from MQTT callback) */
static volatile animation_mode_t current_animation = ANIM_METEOR;
static volatile float animation_speed = 0.2f;

/* Current color (can be changed via MQTT) */
static volatile uint8_t strip_color_r = 128;  /* Purple default */
static volatile uint8_t strip_color_g = 0;
static volatile uint8_t strip_color_b = 255;
static volatile uint8_t strip_color_w = 0;

/* ============================================================================
   PERSISTENT STORAGE (NVS)
   ============================================================================
   Stores the rotation count in flash memory so it survives reboots.
   ============================================================================ */

static nvs_handle_t my_nvs_handle;
static uint32_t lifetime_rotations = 0;

/* Initialize NVS and load the saved rotation count */
static void init_persistent_storage(void)
{
    ESP_LOGI(TAG_NVS, "Initializing NVS flash storage...");
    
    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG_NVS, "NVS partition needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    /* Open NVS namespace */
    ret = nvs_open("meteor", NVS_READWRITE, &my_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return;
    }
    
    /* Load the saved rotation count (default to 0 if not found) */
    ret = nvs_get_u32(my_nvs_handle, "rotations", &lifetime_rotations);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG_NVS, "No saved rotation count found, starting at 0");
        lifetime_rotations = 0;
    } else if (ret == ESP_OK) {
        ESP_LOGI(TAG_NVS, "Loaded lifetime rotation count: %lu", (unsigned long)lifetime_rotations);
    } else {
        ESP_LOGE(TAG_NVS, "Error reading rotation count: %s", esp_err_to_name(ret));
    }
}

/* Save the current rotation count to flash */
static void save_rotation_count(void)
{
    esp_err_t ret = nvs_set_u32(my_nvs_handle, "rotations", lifetime_rotations);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Failed to save rotation count: %s", esp_err_to_name(ret));
        return;
    }
    
    /* Commit to flash (this actually writes it) */
    ret = nvs_commit(my_nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_NVS, "Failed to commit to NVS: %s", esp_err_to_name(ret));
    }
}

/* Increment rotation count and save periodically */
static void increment_rotation_count(void)
{
    lifetime_rotations++;
    
    /* Save every 10 rotations to reduce flash wear */
    if (lifetime_rotations % 10 == 0) {
        save_rotation_count();
        ESP_LOGD(TAG_NVS, "Saved rotation count to flash: %lu", (unsigned long)lifetime_rotations);
    }
}

/* ============================================================================
   WIFI CONNECTION
   ============================================================================ */

/* WiFi event handler */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG_WIFI, "WiFi started, connecting to %s...", WIFI_SSID);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            wifi_retry_count++;
            ESP_LOGW(TAG_WIFI, "Connection failed, retrying... (%d/%d)", wifi_retry_count, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG_WIFI, "Failed to connect after %d attempts", WIFI_MAX_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_WIFI, "Connected! IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;
        wifi_connected = true;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Initialize and connect to WiFi (non-blocking, returns immediately) */
static void wifi_init_start(void)
{
    ESP_LOGI(TAG_WIFI, "Initializing WiFi...");
    
    wifi_event_group = xEventGroupCreate();
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    /* Register event handlers */
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));
    
    /* Configure WiFi */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG_WIFI, "WiFi initialization complete, connecting...");
}

/* Check if WiFi is connected */
static bool wifi_is_connected(void)
{
    return wifi_connected;
}

/* Check WiFi connection status (non-blocking) */
static int wifi_check_status(void)
{
    EventBits_t bits = xEventGroupGetBits(wifi_event_group);
    if (bits & WIFI_CONNECTED_BIT) {
        return 1;  /* Connected */
    } else if (bits & WIFI_FAIL_BIT) {
        return -1; /* Failed */
    }
    return 0;  /* Still connecting */
}

/* ============================================================================
   MQTT COMMAND HANDLER
   ============================================================================
   Parses incoming MQTT messages and updates animation state.
   
   Supported commands:
   - "meteor"     → Meteor spinner animation
   - "rainbow"    → Rainbow cycle
   - "breathing"  → Breathing/pulsing effect
   - "solid"      → Solid color (use color command to set)
   - "off"        → Turn off all LEDs
   - "speed:slow" → Slow animation
   - "speed:fast" → Fast animation
   - "color:RRGGBB" → Set color (hex, e.g., "color:FF00FF" for purple)
   ============================================================================ */

static void handle_mqtt_command(const char *data, int data_len)
{
    /* Null-terminate for string operations */
    char command[64];
    int len = (data_len < 63) ? data_len : 63;
    memcpy(command, data, len);
    command[len] = '\0';
    
    ESP_LOGI(TAG_MQTT, ">>> COMMAND RECEIVED: '%s'", command);
    
    /* Animation mode commands */
    if (strcmp(command, "meteor") == 0) {
        current_animation = ANIM_METEOR;
        ESP_LOGI(TAG_MQTT, "Animation: METEOR SPINNER");
    }
    else if (strcmp(command, "rainbow") == 0) {
        current_animation = ANIM_RAINBOW;
        ESP_LOGI(TAG_MQTT, "Animation: RAINBOW");
    }
    else if (strcmp(command, "breathing") == 0) {
        current_animation = ANIM_BREATHING;
        ESP_LOGI(TAG_MQTT, "Animation: BREATHING");
    }
    else if (strcmp(command, "solid") == 0) {
        current_animation = ANIM_SOLID;
        ESP_LOGI(TAG_MQTT, "Animation: SOLID COLOR");
    }
    else if (strcmp(command, "off") == 0) {
        current_animation = ANIM_OFF;
        ESP_LOGI(TAG_MQTT, "Animation: OFF");
    }
    else if (strcmp(command, "on") == 0) {
        current_animation = ANIM_METEOR;  /* Default to meteor when turned on */
        ESP_LOGI(TAG_MQTT, "Animation: ON (meteor)");
    }
    /* Speed commands */
    else if (strcmp(command, "speed:slow") == 0 || strcmp(command, "slow") == 0) {
        animation_speed = 0.08f;
        ESP_LOGI(TAG_MQTT, "Speed: SLOW (%.2f)", animation_speed);
    }
    else if (strcmp(command, "speed:medium") == 0 || strcmp(command, "medium") == 0) {
        animation_speed = 0.2f;
        ESP_LOGI(TAG_MQTT, "Speed: MEDIUM (%.2f)", animation_speed);
    }
    else if (strcmp(command, "speed:fast") == 0 || strcmp(command, "fast") == 0) {
        animation_speed = 0.5f;
        ESP_LOGI(TAG_MQTT, "Speed: FAST (%.2f)", animation_speed);
    }
    /* Color command: "color:RRGGBB" or "color:RRGGBBWW" */
    else if (strncmp(command, "color:", 6) == 0) {
        const char *hex = command + 6;
        unsigned int r, g, b, w = 0;
        
        if (strlen(hex) >= 6) {
            /* Parse RGB */
            char rs[3] = {hex[0], hex[1], 0};
            char gs[3] = {hex[2], hex[3], 0};
            char bs[3] = {hex[4], hex[5], 0};
            r = strtol(rs, NULL, 16);
            g = strtol(gs, NULL, 16);
            b = strtol(bs, NULL, 16);
            
            /* Parse W if present */
            if (strlen(hex) >= 8) {
                char ws[3] = {hex[6], hex[7], 0};
                w = strtol(ws, NULL, 16);
            }
            
            strip_color_r = (uint8_t)r;
            strip_color_g = (uint8_t)g;
            strip_color_b = (uint8_t)b;
            strip_color_w = (uint8_t)w;
            
            ESP_LOGI(TAG_MQTT, "Color set: R=%d G=%d B=%d W=%d", r, g, b, w);
            
            /* Switch to solid mode to show the color */
            if (current_animation == ANIM_OFF) {
                current_animation = ANIM_SOLID;
            }
        }
    }
    /* Named color shortcuts */
    else if (strcmp(command, "red") == 0) {
        strip_color_r = 255; strip_color_g = 0; strip_color_b = 0; strip_color_w = 0;
        current_animation = ANIM_SOLID;
        ESP_LOGI(TAG_MQTT, "Color: RED");
    }
    else if (strcmp(command, "green") == 0) {
        strip_color_r = 0; strip_color_g = 255; strip_color_b = 0; strip_color_w = 0;
        current_animation = ANIM_SOLID;
        ESP_LOGI(TAG_MQTT, "Color: GREEN");
    }
    else if (strcmp(command, "blue") == 0) {
        strip_color_r = 0; strip_color_g = 0; strip_color_b = 255; strip_color_w = 0;
        current_animation = ANIM_SOLID;
        ESP_LOGI(TAG_MQTT, "Color: BLUE");
    }
    else if (strcmp(command, "purple") == 0) {
        strip_color_r = 128; strip_color_g = 0; strip_color_b = 255; strip_color_w = 0;
        current_animation = ANIM_SOLID;
        ESP_LOGI(TAG_MQTT, "Color: PURPLE");
    }
    else if (strcmp(command, "white") == 0) {
        strip_color_r = 0; strip_color_g = 0; strip_color_b = 0; strip_color_w = 255;
        current_animation = ANIM_SOLID;
        ESP_LOGI(TAG_MQTT, "Color: WHITE (using W channel)");
    }
    else if (strcmp(command, "warm") == 0) {
        strip_color_r = 255; strip_color_g = 150; strip_color_b = 50; strip_color_w = 100;
        current_animation = ANIM_SOLID;
        ESP_LOGI(TAG_MQTT, "Color: WARM WHITE");
    }
    else {
        ESP_LOGW(TAG_MQTT, "Unknown command: '%s'", command);
    }
}

/* MQTT event handler */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, 
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG_MQTT, "Connected to Adafruit IO!");
            /* Subscribe to our feed */
            esp_mqtt_client_subscribe(mqtt_client, MQTT_TOPIC, 0);
            ESP_LOGI(TAG_MQTT, "Subscribed to: %s", MQTT_TOPIC);
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG_MQTT, "Disconnected from Adafruit IO");
            break;
            
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG_MQTT, "Subscription confirmed");
            break;
            
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG_MQTT, "Message received on topic: %.*s", 
                     event->topic_len, event->topic);
            handle_mqtt_command(event->data, event->data_len);
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG_MQTT, "MQTT Error");
            break;
            
        default:
            break;
    }
}

/* Initialize MQTT connection to Adafruit IO */
static void mqtt_init(void)
{
    ESP_LOGI(TAG_MQTT, "Initializing MQTT connection to Adafruit IO...");
    ESP_LOGI(TAG_MQTT, "Username: %s", ADAFRUIT_IO_USERNAME);
    ESP_LOGI(TAG_MQTT, "Feed: %s", ADAFRUIT_IO_FEED);
    
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://io.adafruit.com:1883",  /* Non-SSL for simplicity */
        .credentials.username = ADAFRUIT_IO_USERNAME,
        .credentials.authentication.password = ADAFRUIT_IO_KEY,
    };
    
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    
    ESP_LOGI(TAG_MQTT, "MQTT client started, connecting...");
}

/* Use project configuration menu (idf.py menuconfig) to choose the GPIO to blink,
   or you can edit the following line and set a number here.
*/
#define BLINK_GPIO CONFIG_BLINK_GPIO

/* ============================================================================
   EXTERNAL RGBW NEOPIXEL ON GPIO4
   ============================================================================
   This is your Adafruit RGBW NeoPixel connected to GPIO4.
   It uses the SK6812 chip which supports 4 channels: Red, Green, Blue, White.
   NeoPixels use a specific timing protocol - we use the RMT peripheral to generate it.
   ============================================================================ */

#define RGBW_LED_GPIO 4
#define RGBW_LED_COUNT 15  // Your Adafruit RGBW strip (60 LEDs / 4 = 15 RGBW pixels)

/* Handle for the external RGBW NeoPixel */
static led_strip_handle_t rgbw_strip = NULL;

/* Configure the RGBW NeoPixel on GPIO4 */
static void configure_rgbw_led(void)
{
    ESP_LOGI(TAG_RGBW, "========================================");
    ESP_LOGI(TAG_RGBW, "Initializing RGBW NeoPixel (SK6812)");
    ESP_LOGI(TAG_RGBW, "========================================");
    ESP_LOGI(TAG_RGBW, "GPIO Pin: %d", RGBW_LED_GPIO);
    ESP_LOGI(TAG_RGBW, "LED Count: %d", RGBW_LED_COUNT);
    ESP_LOGI(TAG_RGBW, "LED Model: SK6812 (for RGBW NeoPixels)");
    ESP_LOGI(TAG_RGBW, "Color Format: GRBW (Green-Red-Blue-White order)");

    /* LED strip configuration for SK6812 RGBW
       - SK6812 is the chip used in RGBW NeoPixels
       - GRBW means data is sent in Green-Red-Blue-White order (NeoPixel standard)
    */
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGBW_LED_GPIO,
        .max_leds = RGBW_LED_COUNT,
        .led_model = LED_MODEL_SK6812,  // SK6812 for RGBW NeoPixels
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRBW, // NeoPixels use GRB order
        .flags.invert_out = false,
    };

    /* RMT (Remote Control Transceiver) configuration
       - RMT is a hardware peripheral that generates precise timing signals
       - 10MHz resolution gives us 100ns precision, enough for NeoPixel timing
    */
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz = 100ns per tick
        .flags.with_dma = false,
    };

    ESP_LOGI(TAG_RGBW, "Creating RMT device for LED strip...");
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &rgbw_strip);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_RGBW, "FAILED to create LED strip! Error: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG_RGBW, "Check wiring: DIN->GPIO4, VCC->3.3V/5V, GND->GND");
        return;
    }
    
    ESP_LOGI(TAG_RGBW, "LED strip created successfully!");
    ESP_LOGI(TAG_RGBW, "Clearing LED (turning off)...");
    led_strip_clear(rgbw_strip);
    ESP_LOGI(TAG_RGBW, "RGBW NeoPixel ready on GPIO%d!", RGBW_LED_GPIO);
    ESP_LOGI(TAG_RGBW, "========================================");
}

/* Set a single pixel on the RGBW strip */
static void set_pixel_rgbw(int index, uint8_t red, uint8_t green, uint8_t blue, uint8_t white)
{
    if (rgbw_strip == NULL || index < 0 || index >= RGBW_LED_COUNT) {
        return;
    }
    led_strip_set_pixel_rgbw(rgbw_strip, index, red, green, blue, white);
}

/* Refresh the strip to display changes */
static void refresh_strip(void)
{
    if (rgbw_strip == NULL) {
        return;
    }
    led_strip_refresh(rgbw_strip);
}

/* ============================================================================
   METEOR SPINNER ANIMATION
   ============================================================================
   A continuous spinner effect:
   - One brightest pixel (the "head")
   - All other pixels form a tail that wraps around the entire strip
   - Brightness decreases smoothly from head to tail
   - The pixel just before the head is the dimmest (near zero)
   - Creates a smooth gradient that suddenly jumps to bright at the head
   
   Color: PURPLE (fixed)
   ============================================================================ */

#include <math.h>

/* Gamma correction for perceptually smooth brightness falloff */
#define GAMMA 2.2f

/* Master brightness control (0.0 to 1.0) - scales all output */
#define MASTER_BRIGHTNESS 0.50f

/* Apply gamma correction for perceptually smooth brightness */
static float gamma_correct(float linear_value)
{
    return powf(linear_value, GAMMA);
}

/* Draw the meteor spinner at a given head position (floating-point for smoothness) */
static void draw_meteor_spinner(float head_pos)
{
    if (rgbw_strip == NULL) return;
    
    /* Get current color (may be changed by MQTT) */
    uint8_t cr = strip_color_r;
    uint8_t cg = strip_color_g;
    uint8_t cb = strip_color_b;
    uint8_t cw = strip_color_w;
    
    for (int i = 0; i < RGBW_LED_COUNT; i++) {
        float distance_behind = head_pos - (float)i;
        
        while (distance_behind < 0) distance_behind += RGBW_LED_COUNT;
        while (distance_behind >= RGBW_LED_COUNT) distance_behind -= RGBW_LED_COUNT;
        
        float linear_brightness = 1.0f - (distance_behind / (float)RGBW_LED_COUNT);
        if (linear_brightness < 0.0f) linear_brightness = 0.0f;
        if (linear_brightness > 1.0f) linear_brightness = 1.0f;
        
        float corrected_brightness = gamma_correct(linear_brightness);
        
        uint8_t pr = (uint8_t)(cr * corrected_brightness * MASTER_BRIGHTNESS);
        uint8_t pg = (uint8_t)(cg * corrected_brightness * MASTER_BRIGHTNESS);
        uint8_t pb = (uint8_t)(cb * corrected_brightness * MASTER_BRIGHTNESS);
        uint8_t pw = (uint8_t)(cw * corrected_brightness * MASTER_BRIGHTNESS);
        
        set_pixel_rgbw(i, pr, pg, pb, pw);
    }
    refresh_strip();
}

/* Rainbow animation - cycles through hues */
static void draw_rainbow(float phase)
{
    if (rgbw_strip == NULL) return;
    
    for (int i = 0; i < RGBW_LED_COUNT; i++) {
        /* Calculate hue for this pixel (0-360 degrees, spread across strip) */
        float hue = fmodf(phase + (float)i * 360.0f / RGBW_LED_COUNT, 360.0f);
        
        /* HSV to RGB (simplified, saturation=1, value=1) */
        float h = hue / 60.0f;
        int hi = (int)h % 6;
        float f = h - (int)h;
        float q = 1.0f - f;
        
        float r, g, b;
        switch (hi) {
            case 0: r = 1; g = f; b = 0; break;
            case 1: r = q; g = 1; b = 0; break;
            case 2: r = 0; g = 1; b = f; break;
            case 3: r = 0; g = q; b = 1; break;
            case 4: r = f; g = 0; b = 1; break;
            default: r = 1; g = 0; b = q; break;
        }
        
        set_pixel_rgbw(i, 
            (uint8_t)(r * 255 * MASTER_BRIGHTNESS),
            (uint8_t)(g * 255 * MASTER_BRIGHTNESS),
            (uint8_t)(b * 255 * MASTER_BRIGHTNESS),
            0);
    }
    refresh_strip();
}

/* Breathing animation - pulses brightness up and down */
static void draw_breathing(float phase)
{
    if (rgbw_strip == NULL) return;
    
    /* Sine wave for smooth breathing (0 to 1) */
    float brightness = 0.5f + 0.5f * sinf(phase);
    brightness = gamma_correct(brightness);
    
    uint8_t cr = strip_color_r;
    uint8_t cg = strip_color_g;
    uint8_t cb = strip_color_b;
    uint8_t cw = strip_color_w;
    
    for (int i = 0; i < RGBW_LED_COUNT; i++) {
        set_pixel_rgbw(i,
            (uint8_t)(cr * brightness * MASTER_BRIGHTNESS),
            (uint8_t)(cg * brightness * MASTER_BRIGHTNESS),
            (uint8_t)(cb * brightness * MASTER_BRIGHTNESS),
            (uint8_t)(cw * brightness * MASTER_BRIGHTNESS));
    }
    refresh_strip();
}

/* Solid color - all pixels same color */
static void draw_solid(void)
{
    if (rgbw_strip == NULL) return;
    
    uint8_t cr = (uint8_t)(strip_color_r * MASTER_BRIGHTNESS);
    uint8_t cg = (uint8_t)(strip_color_g * MASTER_BRIGHTNESS);
    uint8_t cb = (uint8_t)(strip_color_b * MASTER_BRIGHTNESS);
    uint8_t cw = (uint8_t)(strip_color_w * MASTER_BRIGHTNESS);
    
    for (int i = 0; i < RGBW_LED_COUNT; i++) {
        set_pixel_rgbw(i, cr, cg, cb, cw);
    }
    refresh_strip();
}

/* Turn off all LEDs */
static void draw_off(void)
{
    if (rgbw_strip == NULL) return;
    
    for (int i = 0; i < RGBW_LED_COUNT; i++) {
        set_pixel_rgbw(i, 0, 0, 0, 0);
    }
    refresh_strip();
}

/* ============================================================================
   ONBOARD LED (from original blink example)
   ============================================================================
   The Waveshare ESP32-C6-DEV-KIT-N8 has an onboard addressable RGB LED on GPIO8.
   This is controlled separately from your external NeoPixel.
   ============================================================================ */

#ifdef CONFIG_BLINK_LED_STRIP

static led_strip_handle_t led_strip;
static uint8_t s_led_state = 0;

/* Current LED color state for smooth transitions */
static float current_r = 0, current_g = 0, current_b = 0;

/* ============================================================================
   ONBOARD LED TRANSITION HELPERS
   ============================================================================
   Helper functions for smooth fade transitions on the onboard LED.
   - jump_to_color: instant change
   - fade_to_color: smooth ease-in-out transition
   ============================================================================ */

/* Ease-in-out function (smoothstep) */
static float ease_in_out(float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* Set onboard LED to a specific RGB color (internal) */
static void set_onboard_led_rgb_internal(uint8_t r, uint8_t g, uint8_t b)
{
    if (led_strip) {
        led_strip_set_pixel(led_strip, 0, r, g, b);
        led_strip_refresh(led_strip);
    }
}

/* Jump instantly to a color (no transition) */
static void jump_to_color(uint8_t r, uint8_t g, uint8_t b)
{
    current_r = r;
    current_g = g;
    current_b = b;
    set_onboard_led_rgb_internal(r, g, b);
    ESP_LOGD(TAG_ONBOARD, "Jump to: R=%d G=%d B=%d", r, g, b);
}

/* Fade smoothly to a color over a duration (blocking) */
static void fade_to_color(uint8_t target_r, uint8_t target_g, uint8_t target_b, int duration_ms)
{
    float start_r = current_r;
    float start_g = current_g;
    float start_b = current_b;
    
    int steps = duration_ms / 20;  /* 20ms per frame = 50fps */
    if (steps < 1) steps = 1;
    
    ESP_LOGD(TAG_ONBOARD, "Fade: (%d,%d,%d) -> (%d,%d,%d) over %dms",
             (int)start_r, (int)start_g, (int)start_b,
             target_r, target_g, target_b, duration_ms);
    
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float eased = ease_in_out(t);
        
        current_r = start_r + (target_r - start_r) * eased;
        current_g = start_g + (target_g - start_g) * eased;
        current_b = start_b + (target_b - start_b) * eased;
        
        set_onboard_led_rgb_internal((uint8_t)current_r, (uint8_t)current_g, (uint8_t)current_b);
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
    
    /* Ensure we land exactly on target */
    current_r = target_r;
    current_g = target_g;
    current_b = target_b;
}

/* Convenience: set onboard LED (legacy compatibility) */
static void set_onboard_led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    jump_to_color(r, g, b);
}

/* Turn off onboard LED */
static void clear_onboard_led(void)
{
    jump_to_color(0, 0, 0);
}

static void blink_led(void)
{
    if (s_led_state) {
        set_onboard_led_rgb_internal(16, 16, 16);
    } else {
        led_strip_clear(led_strip);
    }
}

static void configure_led(void)
{
    ESP_LOGI(TAG_ONBOARD, "========================================");
    ESP_LOGI(TAG_ONBOARD, "Initializing ONBOARD addressable LED");
    ESP_LOGI(TAG_ONBOARD, "========================================");
    ESP_LOGI(TAG_ONBOARD, "GPIO Pin: %d", BLINK_GPIO);
    ESP_LOGI(TAG_ONBOARD, "Backend: RMT");
    
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = 1,
    };
#if CONFIG_BLINK_LED_STRIP_BACKEND_RMT
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };
    ESP_LOGI(TAG_ONBOARD, "Creating RMT device...");
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
#elif CONFIG_BLINK_LED_STRIP_BACKEND_SPI
    led_strip_spi_config_t spi_config = {
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };
    ESP_LOGI(TAG_ONBOARD, "Creating SPI device...");
    ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_config, &spi_config, &led_strip));
#else
#error "unsupported LED strip backend"
#endif
    led_strip_clear(led_strip);
    ESP_LOGI(TAG_ONBOARD, "Onboard LED ready on GPIO%d!", BLINK_GPIO);
    ESP_LOGI(TAG_ONBOARD, "========================================");
}

#elif CONFIG_BLINK_LED_GPIO

static uint8_t s_led_state = 0;

static void blink_led(void)
{
    ESP_LOGD(TAG_ONBOARD, "Setting GPIO%d to %d", BLINK_GPIO, s_led_state);
    gpio_set_level(BLINK_GPIO, s_led_state);
}

static void configure_led(void)
{
    ESP_LOGI(TAG_ONBOARD, "========================================");
    ESP_LOGI(TAG_ONBOARD, "Initializing ONBOARD GPIO LED");
    ESP_LOGI(TAG_ONBOARD, "========================================");
    ESP_LOGI(TAG_ONBOARD, "GPIO Pin: %d", BLINK_GPIO);
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    ESP_LOGI(TAG_ONBOARD, "GPIO LED ready!");
    ESP_LOGI(TAG_ONBOARD, "========================================");
}

#else
#error "unsupported LED type"
#endif

/* ============================================================================
   MAIN APPLICATION
   ============================================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║     ESP32-C6 LED Control Demo                            ║");
    ESP_LOGI(TAG, "║     Onboard LED (GPIO%d) + External RGBW NeoPixel (GPIO%d) ║", BLINK_GPIO, RGBW_LED_GPIO);
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");

    /* Step 0: Initialize persistent storage */
    ESP_LOGI(TAG, ">>> STEP 0: Initializing persistent storage...");
    init_persistent_storage();

    /* Step 1: Configure the onboard LED */
    ESP_LOGI(TAG, ">>> STEP 1: Configuring onboard LED...");
    configure_led();

    /* ========================================================================
       STARTUP SEQUENCE
       1. Solid white for 1 second
       2. Fade to black
       3. Breathing light blue (0 to 50%) while connecting WiFi
       4. Fade to solid blue (success) or blinking red (failure)
       ======================================================================== */
    
    /* Step 1: Solid white for 1 second */
    ESP_LOGI(TAG, ">>> STARTUP: Solid white for 1 second...");
    jump_to_color(255, 255, 255);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    
    /* Step 2: Fade to black */
    ESP_LOGI(TAG, ">>> STARTUP: Fading to black...");
    fade_to_color(0, 0, 0, 500);

    /* ========================================================================
       WIFI CONNECTION: Breathing light blue (0 to 50%) while connecting
       ======================================================================== */
    ESP_LOGI(TAG, ">>> STEP 2: Connecting to WiFi...");
    wifi_init_start();

    /* Light blue color for WiFi connecting (soft sky blue) - max 50% brightness = 127 */
    const uint8_t wifi_max_r = 50;   /* ~50% of light blue */
    const uint8_t wifi_max_g = 90;
    const uint8_t wifi_max_b = 127;

    /* Breathing animation while waiting for WiFi */
    float breath_t = 0.0f;
    const float breath_speed = 0.02f;  /* Speed of breathing cycle */
    int wifi_status = 0;
    bool breath_rising = true;

    ESP_LOGI(TAG, "    Breathing light blue (0-50%%) while connecting to '%s'...", WIFI_SSID);

    while (wifi_status == 0) {
        /* Use ease-in-out for smooth breathing (0 to 1 to 0) */
        float eased = ease_in_out(breath_t);
        
        /* Apply to light blue color (from 0 to max) */
        uint8_t r = (uint8_t)(wifi_max_r * eased);
        uint8_t g = (uint8_t)(wifi_max_g * eased);
        uint8_t b = (uint8_t)(wifi_max_b * eased);
        
        /* Update LED (use jump since we're animating manually) */
        current_r = r; current_g = g; current_b = b;
        set_onboard_led_rgb_internal(r, g, b);
        
        /* Advance breathing phase (triangle wave with easing) */
        if (breath_rising) {
            breath_t += breath_speed;
            if (breath_t >= 1.0f) {
                breath_t = 1.0f;
                breath_rising = false;
            }
        } else {
            breath_t -= breath_speed;
            if (breath_t <= 0.0f) {
                breath_t = 0.0f;
                breath_rising = true;
            }
        }
        
        /* Check WiFi status */
        wifi_status = wifi_check_status();
        
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }

    /* Show result with smooth fade */
    if (wifi_status == 1) {
        ESP_LOGI(TAG, ">>> WiFi CONNECTED! Fading to solid blue...");
        fade_to_color(0, 0, 255, 800);  /* Fade to 100% blue */
        
        /* Start MQTT connection to Adafruit IO */
        ESP_LOGI(TAG, ">>> STEP 3: Starting MQTT connection...");
        mqtt_init();
    } else {
        ESP_LOGE(TAG, ">>> WiFi FAILED! Fading to blinking red...");
        fade_to_color(255, 0, 0, 500);  /* Fade to 100% red */
        
        /* Blink red on/off forever in a background-like manner */
        while (1) {
            fade_to_color(0, 0, 0, 400);
            vTaskDelay(200 / portTICK_PERIOD_MS);
            fade_to_color(255, 0, 0, 400);
            vTaskDelay(200 / portTICK_PERIOD_MS);
        }
    }
    
    /* Step 4: Configure the external RGBW NeoPixel on GPIO4 */
    ESP_LOGI(TAG, ">>> STEP 4: Configuring external RGBW NeoPixel...");
    configure_rgbw_led();

    /* Animation state */
    float head_position = 0.0f;      /* For meteor animation */
    float rainbow_phase = 0.0f;      /* For rainbow animation */
    float breathing_phase = 0.0f;    /* For breathing animation */
    int animation_delay_ms = 25;     /* Frame time in ms */
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, ">>> STEP 5: Starting animation loop...");
    ESP_LOGI(TAG, "    - %d pixels in ring", RGBW_LED_COUNT);
    ESP_LOGI(TAG, "    - Master brightness: %.0f%%", MASTER_BRIGHTNESS * 100);
    ESP_LOGI(TAG, "    - Lifetime rotations: %lu", (unsigned long)lifetime_rotations);
    ESP_LOGI(TAG, "    - MQTT: Listening for voice commands on '%s'", MQTT_TOPIC);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "    Voice commands available:");
    ESP_LOGI(TAG, "      meteor, rainbow, breathing, solid, off, on");
    ESP_LOGI(TAG, "      slow, medium, fast");
    ESP_LOGI(TAG, "      red, green, blue, purple, white, warm");
    ESP_LOGI(TAG, "      color:RRGGBB (hex)");
    ESP_LOGI(TAG, "");

    while (1) {
        /* Get current animation mode (may change from MQTT at any time) */
        animation_mode_t mode = current_animation;
        float speed = animation_speed;
        
        switch (mode) {
            case ANIM_METEOR:
                draw_meteor_spinner(head_position);
                head_position += speed;
                if (head_position >= RGBW_LED_COUNT) {
                    head_position -= RGBW_LED_COUNT;
                    increment_rotation_count();
                }
                break;
                
            case ANIM_RAINBOW:
                draw_rainbow(rainbow_phase);
                rainbow_phase += speed * 5.0f;  /* Faster for rainbow */
                if (rainbow_phase >= 360.0f) rainbow_phase -= 360.0f;
                break;
                
            case ANIM_BREATHING:
                draw_breathing(breathing_phase);
                breathing_phase += speed * 0.3f;  /* Slower for breathing */
                if (breathing_phase >= 2 * 3.14159f) breathing_phase -= 2 * 3.14159f;
                break;
                
            case ANIM_SOLID:
                draw_solid();
                break;
                
            case ANIM_OFF:
                draw_off();
                break;
        }
        
        /* Wait before next frame */
        vTaskDelay(animation_delay_ms / portTICK_PERIOD_MS);
    }
}
