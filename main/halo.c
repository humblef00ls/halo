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
   - Multiple animation modes (cycle, fusion, wave, tetris, stars, meteor, rainbow, breathing, solid)
   - MQTT voice control via Adafruit IO + IFTTT
   - WiFi connectivity with status indication
   - Persistent settings via NVS
   - Master brightness control
   
   License: Public Domain (CC0)
   
   ============================================================================ */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mqtt_client.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/ledc.h"  /* PWM for passive buzzer */
#include "esp_random.h"   /* For esp_random() in song selection */
#include "esp_system.h"   /* For esp_get_free_heap_size(), chip info */
#include "esp_heap_caps.h" /* For heap_caps_get_free_size() */
#include "esp_timer.h"    /* For esp_timer_get_time() */
#include "credentials.h"  /* WiFi and Adafruit IO credentials (gitignored) */
#include "zigbee_hub.h"   /* Zigbee coordinator for blind control */
#include "zigbee_devices.h" /* Zigbee device storage */

/* Logging tags for different components */
static const char *TAG = "main";
static const char *TAG_ONBOARD = "onboard_led";
static const char *TAG_RGBW = "rgbw_neopixel";
static const char *TAG_NVS = "nvs_storage";
static const char *TAG_WIFI = "wifi";
static const char *TAG_METRICS = "metrics";

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
   POTENTIOMETER BRIGHTNESS CONTROL
   ============================================================================
   A 10kΩ potentiometer on GPIO1 (ADC1_CH1) provides real-time brightness control.
   Wiring: Left pin → GND, Middle pin → GPIO1, Right pin → 3.3V
   ============================================================================ */

#define POT_GPIO            GPIO_NUM_1
#define POT_ADC_CHANNEL     ADC_CHANNEL_1
#define POT_ADC_UNIT        ADC_UNIT_1

static const char *TAG_POT = "potentiometer";
static adc_oneshot_unit_handle_t pot_adc_handle = NULL;

/* Pot brightness multiplier (0.05 to 1.0) - updated by reading pot
   Default to 0.5 (50%) in case potentiometer is not connected */
static volatile float pot_brightness = 0.5f;

/* ============================================================================
   PASSIVE BUZZER (MELODY PLAYER)
   ============================================================================
   A passive buzzer on GPIO23 plays melodies using PWM.
   Wiring: GPIO23 → (+), GND → (-)
   ============================================================================ */

#define BUZZER_GPIO         GPIO_NUM_23
#define BUZZER_LEDC_TIMER   LEDC_TIMER_1
#define BUZZER_LEDC_CHANNEL LEDC_CHANNEL_1
#define BUZZER_LEDC_MODE    LEDC_LOW_SPEED_MODE

static const char *TAG_BUZZER = "buzzer";

/* Musical note frequencies (Hz) - Scientific pitch notation */
#define NOTE_REST   0
#define NOTE_C4     262
#define NOTE_CS4    277
#define NOTE_D4     294
#define NOTE_DS4    311
#define NOTE_E4     330
#define NOTE_F4     349
#define NOTE_FS4    370
#define NOTE_G4     392
#define NOTE_GS4    415
#define NOTE_A4     440
#define NOTE_AS4    466
#define NOTE_B4     494
#define NOTE_C5     523
#define NOTE_CS5    554
#define NOTE_D5     587
#define NOTE_DS5    622
#define NOTE_E5     659
#define NOTE_F5     698
#define NOTE_FS5    740
#define NOTE_G5     784
#define NOTE_GS5    831
#define NOTE_A5     880
#define NOTE_AS5    932
#define NOTE_B5     988
#define NOTE_C6     1047
#define NOTE_D6     1175
#define NOTE_E6     1319
#define NOTE_F6     1397
#define NOTE_G6     1568

/* A note in a melody: frequency (Hz) and duration (ms) */
typedef struct {
    uint16_t freq;
    uint16_t duration_ms;
} melody_note_t;

/* Pre-defined melodies */
static const melody_note_t MELODY_STARTUP[] = {
    {NOTE_C5, 100}, {NOTE_E5, 100}, {NOTE_G5, 100}, {NOTE_C6, 200},
    {NOTE_REST, 50}, {NOTE_G5, 100}, {NOTE_C6, 300}
};
#define MELODY_STARTUP_LEN (sizeof(MELODY_STARTUP) / sizeof(melody_note_t))

static const melody_note_t MELODY_SUCCESS[] = {
    {NOTE_G5, 100}, {NOTE_C6, 200}
};
#define MELODY_SUCCESS_LEN (sizeof(MELODY_SUCCESS) / sizeof(melody_note_t))

static const melody_note_t MELODY_ERROR[] = {
    {NOTE_A4, 150}, {NOTE_REST, 50}, {NOTE_A4, 150}, {NOTE_REST, 50}, {NOTE_A4, 300}
};
#define MELODY_ERROR_LEN (sizeof(MELODY_ERROR) / sizeof(melody_note_t))

static const melody_note_t MELODY_BUTTON_PRESS[] = {
    {NOTE_E5, 50}, {NOTE_G5, 50}
};
#define MELODY_BUTTON_PRESS_LEN (sizeof(MELODY_BUTTON_PRESS) / sizeof(melody_note_t))

static const melody_note_t MELODY_SHUTDOWN[] = {
    {NOTE_C6, 100}, {NOTE_G5, 100}, {NOTE_E5, 100}, {NOTE_C5, 300}
};
#define MELODY_SHUTDOWN_LEN (sizeof(MELODY_SHUTDOWN) / sizeof(melody_note_t))

/* Super Mario Bros - Ground Theme (first few notes) */
__attribute__((unused))
static const melody_note_t MELODY_MARIO[] = {
    {NOTE_E5, 100}, {NOTE_E5, 100}, {NOTE_REST, 100}, {NOTE_E5, 100},
    {NOTE_REST, 100}, {NOTE_C5, 100}, {NOTE_E5, 200},
    {NOTE_G5, 200}, {NOTE_REST, 200}, {NOTE_G4, 200}
};
#define MELODY_MARIO_LEN (sizeof(MELODY_MARIO) / sizeof(melody_note_t))

/* Star Wars - Imperial March (opening) */
__attribute__((unused))
static const melody_note_t MELODY_IMPERIAL[] = {
    {NOTE_A4, 400}, {NOTE_A4, 400}, {NOTE_A4, 400},
    {NOTE_F4, 300}, {NOTE_C5, 100},
    {NOTE_A4, 400}, {NOTE_F4, 300}, {NOTE_C5, 100}, {NOTE_A4, 600}
};
#define MELODY_IMPERIAL_LEN (sizeof(MELODY_IMPERIAL) / sizeof(melody_note_t))

/* Twinkle Twinkle Little Star */
__attribute__((unused))
static const melody_note_t MELODY_TWINKLE[] = {
    {NOTE_C5, 200}, {NOTE_C5, 200}, {NOTE_G5, 200}, {NOTE_G5, 200},
    {NOTE_A5, 200}, {NOTE_A5, 200}, {NOTE_G5, 400},
    {NOTE_F5, 200}, {NOTE_F5, 200}, {NOTE_E5, 200}, {NOTE_E5, 200},
    {NOTE_D5, 200}, {NOTE_D5, 200}, {NOTE_C5, 400}
};
#define MELODY_TWINKLE_LEN (sizeof(MELODY_TWINKLE) / sizeof(melody_note_t))

static bool buzzer_initialized = false;

/* ============================================================================
   ANIMATION MODES
   ============================================================================
   Controlled via MQTT commands from voice or app.
   ============================================================================ */

typedef enum {
    ANIM_CYCLE,         /* Default: auto-cycles between fusion, wave, tetris, stars */
    ANIM_FUSION,        /* White to purple gradient */
    ANIM_WAVE,          /* Light blue wave radiating from center */
    ANIM_TETRIS,        /* Random colored pixels stacking */
    ANIM_STARS,         /* Twinkling stars effect */
    ANIM_METEOR,        /* Meteor spinner */
    ANIM_METEOR_SHOWER, /* Multiple meteors with rainbow trails */
    ANIM_RAINBOW,       /* Rainbow cycle */
    ANIM_BREATHING,     /* Breathing/pulsing effect */
    ANIM_SOLID,         /* Solid color (no animation) */
    ANIM_OFF            /* All LEDs off */
} animation_mode_t;

/* Current animation state (volatile because modified from MQTT callback) */
static volatile animation_mode_t current_animation = ANIM_CYCLE;
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
   POTENTIOMETER FUNCTIONS
   ============================================================================ */

/* Initialize the potentiometer ADC */
static void init_potentiometer(void)
{
    ESP_LOGI(TAG_POT, "Initializing potentiometer on GPIO%d...", POT_GPIO);
    
    /* Configure ADC unit */
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = POT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_config, &pot_adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_POT, "Failed to initialize ADC unit: %s", esp_err_to_name(ret));
        return;
    }
    
    /* Configure the ADC channel */
    adc_oneshot_chan_cfg_t chan_config = {
        .bitwidth = ADC_BITWIDTH_12,   /* 0-4095 range */
        .atten = ADC_ATTEN_DB_12,      /* Full 0-3.3V range */
    };
    ret = adc_oneshot_config_channel(pot_adc_handle, POT_ADC_CHANNEL, &chan_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_POT, "Failed to configure ADC channel: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG_POT, "Potentiometer initialized successfully");
}

/* Read the potentiometer and update pot_brightness (call periodically)
   Returns brightness value from 0.05 (min, so LEDs never fully off) to 0.95 (max) */
static float read_potentiometer(void)
{
    if (pot_adc_handle == NULL) {
        return 0.5f;  /* Default to 50% brightness if not initialized */
    }
    
    int raw_value = 0;
    esp_err_t ret = adc_oneshot_read(pot_adc_handle, POT_ADC_CHANNEL, &raw_value);
    if (ret != ESP_OK) {
        return pot_brightness;  /* Keep last value on error */
    }
    
    /* Potentiometers often don't reach exact 0 or 4095 in practice.
       Use aggressive dead zones to ensure we can hit 0% and 100% */
    const int ADC_MIN_DEADZONE = 50;    /* Below this = 0% */
    const int ADC_MAX_DEADZONE = 3300;  /* Above this = 100% (lowered for real-world pots) */
    
    float raw_brightness;
    if (raw_value <= ADC_MIN_DEADZONE) {
        raw_brightness = 0.0f;
    } else if (raw_value >= ADC_MAX_DEADZONE) {
        raw_brightness = 1.0f;
    } else {
        /* Scale the middle range to 0.0-1.0 */
        raw_brightness = (float)(raw_value - ADC_MIN_DEADZONE) / 
                         (float)(ADC_MAX_DEADZONE - ADC_MIN_DEADZONE);
    }
    
    /* Map 0.0-1.0 to brightness range: 5% minimum, 100% maximum (no artificial cap!) */
    const float min_brightness = 0.05f;  /* 5% minimum */
    const float max_brightness = 1.00f;  /* 100% maximum - full brightness available */
    float new_brightness = min_brightness + raw_brightness * (max_brightness - min_brightness);
    
    /* Lighter smoothing so we can reach extremes faster */
    pot_brightness = pot_brightness * 0.5f + new_brightness * 0.5f;
    
    /* Clamp to ensure we stay in bounds */
    if (pot_brightness < min_brightness) pot_brightness = min_brightness;
    if (pot_brightness > max_brightness) pot_brightness = max_brightness;
    
    /* Only log when brightness changes significantly (>2% change) */
    static float last_logged_brightness = -1.0f;
    if (fabsf(pot_brightness - last_logged_brightness) > 0.02f) {
        last_logged_brightness = pot_brightness;
        ESP_LOGI(TAG_POT, "Brightness changed to %.0f%%", pot_brightness * 100);
    }
    
    return pot_brightness;
}

/* Track pot changes for brightness gauge display */
static float pot_prev_value = 0.5f;
static int pot_idle_frames = 999;  /* Frames since last pot change (start high to skip gauge on boot) */
static const int POT_GAUGE_TIMEOUT = 90;  /* Show gauge for ~1.5 seconds after last change (60fps) */

/* Check if pot is being adjusted (returns true if gauge should show) */
static bool is_pot_adjusting(void)
{
    float diff = fabsf(pot_brightness - pot_prev_value);
    
    if (diff > 0.01f) {  /* Pot moved more than 1% */
        pot_idle_frames = 0;
        pot_prev_value = pot_brightness;
    } else {
        pot_idle_frames++;
    }
    
    return (pot_idle_frames < POT_GAUGE_TIMEOUT);
}

/* ============================================================================
   SYSTEM METRICS LOGGING
   ============================================================================
   Logs CPU/memory metrics every 30 seconds for remote monitoring.
   ============================================================================ */

static void log_system_metrics(void)
{
    /* Only log every 30 seconds (at 60 FPS = 1800 frames) */
    static int metrics_frame_counter = 0;
    metrics_frame_counter++;
    
    if (metrics_frame_counter < 1800) {  /* 30 seconds at 60 FPS */
        return;
    }
    metrics_frame_counter = 0;
    
    /* Get heap memory stats */
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    
    /* Get uptime */
    int64_t uptime_us = esp_timer_get_time();
    int uptime_sec = (int)(uptime_us / 1000000);
    int uptime_min = uptime_sec / 60;
    int uptime_hrs = uptime_min / 60;
    
    /* Log metrics */
    ESP_LOGI(TAG_METRICS, "=== System Metrics ===");
    ESP_LOGI(TAG_METRICS, "Uptime: %dh %dm %ds", 
             uptime_hrs, uptime_min % 60, uptime_sec % 60);
    ESP_LOGI(TAG_METRICS, "Heap: %u KB free (min: %u KB, internal: %u KB)",
             (unsigned)(free_heap / 1024), 
             (unsigned)(min_free_heap / 1024),
             (unsigned)(free_internal / 1024));
    ESP_LOGI(TAG_METRICS, "Zigbee: %s, %d devices",
             zigbee_is_network_ready() ? "ready" : "not ready",
             zigbee_get_device_count());
    ESP_LOGI(TAG_METRICS, "Animation: mode %d, speed %.2f, brightness %.0f%%",
             current_animation, animation_speed, pot_brightness * 100);
}

/* ============================================================================
   BUZZER FUNCTIONS
   ============================================================================ */

/* Initialize the buzzer PWM channel */
static void init_buzzer(void)
{
    ESP_LOGI(TAG_BUZZER, "Initializing passive buzzer on GPIO%d...", BUZZER_GPIO);
    
    /* Configure timer */
    ledc_timer_config_t timer_conf = {
        .speed_mode = BUZZER_LEDC_MODE,
        .timer_num = BUZZER_LEDC_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT,  /* 0-1023 duty range */
        .freq_hz = 1000,  /* Initial frequency (will be changed per note) */
        .clk_cfg = LEDC_AUTO_CLK
    };
    esp_err_t ret = ledc_timer_config(&timer_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_BUZZER, "Failed to configure LEDC timer: %s", esp_err_to_name(ret));
        return;
    }
    
    /* Configure channel */
    ledc_channel_config_t channel_conf = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = BUZZER_LEDC_MODE,
        .channel = BUZZER_LEDC_CHANNEL,
        .timer_sel = BUZZER_LEDC_TIMER,
        .duty = 0,  /* Start silent */
        .hpoint = 0
    };
    ret = ledc_channel_config(&channel_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_BUZZER, "Failed to configure LEDC channel: %s", esp_err_to_name(ret));
        return;
    }
    
    buzzer_initialized = true;
    ESP_LOGI(TAG_BUZZER, "Buzzer initialized successfully");
}

/* Play a single tone at the given frequency for duration_ms */
static void buzzer_tone(uint16_t frequency_hz, uint16_t duration_ms)
{
    if (!buzzer_initialized) return;
    
    if (frequency_hz == NOTE_REST || frequency_hz == 0) {
        /* Rest: silence */
        ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
        ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    } else {
        /* Set frequency and 50% duty cycle for square wave */
        ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, frequency_hz);
        ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 512);  /* 50% of 1024 */
        ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    }
    
    vTaskDelay(duration_ms / portTICK_PERIOD_MS);
}

/* Stop the buzzer */
static void buzzer_stop(void)
{
    if (!buzzer_initialized) return;
    
    ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
}

/* Set buzzer frequency without blocking (for smooth sweeps) */
static void buzzer_set_freq(uint16_t frequency_hz)
{
    if (!buzzer_initialized) return;
    
    if (frequency_hz == 0) {
        ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 0);
        ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    } else {
        ledc_set_freq(BUZZER_LEDC_MODE, BUZZER_LEDC_TIMER, frequency_hz);
        ledc_set_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL, 512);  /* 50% duty */
        ledc_update_duty(BUZZER_LEDC_MODE, BUZZER_LEDC_CHANNEL);
    }
}

/* Play a melody (array of notes) */
static void buzzer_play_melody(const melody_note_t *melody, size_t length)
{
    if (!buzzer_initialized || melody == NULL) return;
    
    ESP_LOGD(TAG_BUZZER, "Playing melody (%d notes)", (int)length);
    
    for (size_t i = 0; i < length; i++) {
        buzzer_tone(melody[i].freq, melody[i].duration_ms);
        /* Small gap between notes for clarity */
        buzzer_stop();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    
    buzzer_stop();
}

/* Play a beep (simple single tone) */
static void buzzer_beep(uint16_t frequency_hz, uint16_t duration_ms)
{
    buzzer_tone(frequency_hz, duration_ms);
    buzzer_stop();
}

/* Play ascending chime (for success/connect) */
static void buzzer_chime_up(void)
{
    buzzer_play_melody(MELODY_SUCCESS, MELODY_SUCCESS_LEN);
}

/* Play descending chime (for disconnect/shutdown) */
static void buzzer_chime_down(void)
{
    buzzer_play_melody(MELODY_SHUTDOWN, MELODY_SHUTDOWN_LEN);
}

/* Play startup melody */
static void buzzer_startup(void)
{
    buzzer_play_melody(MELODY_STARTUP, MELODY_STARTUP_LEN);
}

/* Play error beeps */
static void buzzer_error(void)
{
    buzzer_play_melody(MELODY_ERROR, MELODY_ERROR_LEN);
}

/* Play button press click */
static void buzzer_click(void)
{
    buzzer_play_melody(MELODY_BUTTON_PRESS, MELODY_BUTTON_PRESS_LEN);
}

/* ============================================================================
   RTTTL (Ring Tone Text Transfer Language) PARSER
   ============================================================================
   Nokia ringtone format: name:d=duration,o=octave,b=bpm:notes
   Example: "TakeonMe:d=4,o=4,b=160:8f#5,8f#5,8f#5,8d5,8p..."
   ============================================================================ */

/* RTTTL note frequencies lookup table (octave 4 base) */
static const uint16_t rtttl_notes[] = {
    /* c, c#, d, d#, e, f, f#, g, g#, a, a#, b */
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494
};

/* Parse a single RTTTL note character to index (0-11, or -1 for pause) */
static int rtttl_note_to_index(char note) {
    switch (note) {
        case 'c': return 0;
        case 'd': return 2;
        case 'e': return 4;
        case 'f': return 5;
        case 'g': return 7;
        case 'a': return 9;
        case 'b': return 11;
        case 'p': return -1;  /* Pause/rest */
        default: return -2;   /* Invalid */
    }
}

/* Melody cancellation flag - used to interrupt song playback */
static volatile bool melody_cancel_requested = false;

/* Play RTTTL ringtone string directly (blocking) */
static void buzzer_play_rtttl(const char *rtttl)
{
    if (!buzzer_initialized || rtttl == NULL) return;
    
    const char *p = rtttl;
    
    /* Skip name (everything before first ':') */
    while (*p && *p != ':') p++;
    if (*p != ':') return;
    p++;
    
    /* Parse defaults section (d=duration, o=octave, b=bpm) */
    int default_duration = 4;
    int default_octave = 6;
    int bpm = 63;
    
    while (*p && *p != ':') {
        if (*p == 'd' && *(p+1) == '=') {
            p += 2;
            default_duration = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        } else if (*p == 'o' && *(p+1) == '=') {
            p += 2;
            default_octave = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        } else if (*p == 'b' && *(p+1) == '=') {
            p += 2;
            bpm = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        } else {
            p++;
        }
    }
    if (*p != ':') return;
    p++;
    
    /* Calculate whole note duration in ms: (60000 / bpm) * 4 */
    int whole_note_ms = (60000 / bpm) * 4;
    
    /* Parse and play notes */
    while (*p) {
        /* Check if cancellation was requested */
        if (melody_cancel_requested) {
            buzzer_stop();
            return;  /* Exit early - song cancelled */
        }
        
        /* Skip whitespace and commas */
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        
        /* Parse duration (optional, before note) */
        int duration = default_duration;
        if (*p >= '0' && *p <= '9') {
            duration = atoi(p);
            while (*p >= '0' && *p <= '9') p++;
        }
        
        /* Parse note letter */
        char note_char = *p++;
        if (note_char >= 'A' && note_char <= 'Z') {
            note_char = note_char - 'A' + 'a';  /* Convert to lowercase */
        }
        int note_idx = rtttl_note_to_index(note_char);
        if (note_idx == -2) continue;  /* Invalid note, skip */
        
        /* Check for sharp */
        bool is_sharp = false;
        if (*p == '#') {
            is_sharp = true;
            p++;
        }
        
        /* Check for dotted note */
        bool dotted = false;
        if (*p == '.') {
            dotted = true;
            p++;
        }
        
        /* Parse octave (optional, after note) */
        int octave = default_octave;
        if (*p >= '0' && *p <= '9') {
            octave = *p - '0';
            p++;
        }
        
        /* Check for dotted note (can also appear after octave) */
        if (*p == '.') {
            dotted = true;
            p++;
        }
        
        /* Calculate note duration in ms */
        int note_duration_ms = whole_note_ms / duration;
        if (dotted) {
            note_duration_ms = note_duration_ms + (note_duration_ms / 2);
        }
        
        /* Calculate frequency */
        uint16_t freq = 0;
        if (note_idx >= 0) {
            int idx = note_idx;
            if (is_sharp) idx++;
            if (idx > 11) idx = 11;
            
            /* Get base frequency (octave 4) and shift to target octave */
            freq = rtttl_notes[idx];
            int octave_shift = octave - 4;
            if (octave_shift > 0) {
                freq <<= octave_shift;  /* Multiply by 2^shift */
            } else if (octave_shift < 0) {
                freq >>= (-octave_shift);  /* Divide by 2^shift */
            }
        }
        
        /* Play the note */
        if (freq > 0) {
            buzzer_tone(freq, note_duration_ms);
        } else {
            vTaskDelay(note_duration_ms / portTICK_PERIOD_MS);
        }
    }
    
    buzzer_stop();
}

/* ============================================================================
   RTTTL SONG LIBRARY (5-10 seconds each)
   ============================================================================ */

/* Array of all songs for random selection - using string literals directly */
static const char * const SONG_LIBRARY[] = {
    /* Littleroot Town (Pokemon Ruby/Sapphire/Emerald) - lowered one octave */
    "Littleroot:d=4,o=4,b=100:8c4,8f4,8g4,4a4,8p,8g4,8a4,8g4,8a4,8a#4,8p,4c5,8d5,8a4,8g4,8a4,8c#5,4d5,4e5,4d5,8a4,8g4,8f4,8e4,8f4,8a4,4d5,8d4,8e4,2f4,8c5,8a#4,8a#4,8a4,2f4,8d5,8a4,8a4,8g4,2f4",
    /* YMCA - Village People */
    "YMCA:d=8,o=5,b=160:c#6,a#,2p,a#,g#,f#,g#,a#,4c#6,a#,4c#6,d#6,a#,2p,a#,g#,f#,g#,a#,4c#6,a#,4c#6,d#6,b,2p,b,a#,g#,a#,b,4d#6,f#6,4d#6,4f6.,4d#6.,4c#6.,4b.,4a#,4g#",
    /* Zelda Song of Storms */
    "zelda_storms:d=4,o=5,b=180:8d6,8f6,d7,p,8d6,8f6,d7,p,e7,8p,8f7,8e7,8f7,8e7,8c7,a6,8p,a6,d6,8f6,8g6,2a6,8p,a6,d6,8f6,8g6,2e6,8p,8d6,8f6,d7,p,8d6,8f6,d7,p,e7,8p,8f7,8e7,8f7,8e7,8c7,a6,8p,a6,d6,8f6,8g6,a6,8p,a6,1d6",
    /* Rudolph the Red Nosed Reindeer */
    "Rudolph:d=8,o=5,b=250:g,4a,g,4e,4c6,4a,2g.,g,a,g,a,4g,4c6,2b.,4p,f,4g,f,4d,4b,4a,2g.,g,a,g,a,4g,4a,2e.,4p,g,4a,a,4e,4c6,4a,2g.,g,a,g,a,4g,4c6,2b.,4p,f,4g,f,4d,4b,4a,2g.,g,a,g,a,4g,4d6,2c6.,4p,4a,4a,4c6,4a,4g,4e,2g,4d,4e,4g,4a,4b,4b,2b,4c6,4c6,4b,4a,4g,4f,2d,g,4a,g,4e,4c6,4a,2g.,g,a,g,a,4g,4c6,2b.,4p,f,4g,f,4d,4b,4a,2g.,4g,4a,4g,4a,2g,2d6,1c6."
};
#define SONG_LIBRARY_SIZE (sizeof(SONG_LIBRARY) / sizeof(SONG_LIBRARY[0]))

/* ============================================================================
   MELODY TASK (Non-blocking background playback)
   ============================================================================
   Uses a FreeRTOS task to play melodies in the background while the main
   animation loop continues running. This is similar to JavaScript's event loop!
   ============================================================================ */

/* Flag to track if a melody is currently playing */
static volatile bool melody_playing = false;

/* melody_cancel_requested is declared earlier (before buzzer_play_rtttl) */

/* Current song index (to avoid repeating same song) */
static volatile int current_song_index = -1;

/* Queue handle for song requests */
static QueueHandle_t melody_queue = NULL;

/* Melody task - runs in background, plays songs from queue */
static void melody_task(void *pvParameters)
{
    int song_index;
    
    ESP_LOGI(TAG_BUZZER, "Melody task started - ready for songs!");
    
    while (1) {
        /* Wait for a song request (blocks until something is in the queue) */
        if (xQueueReceive(melody_queue, &song_index, portMAX_DELAY) == pdTRUE) {
            /* Clear any pending cancel request from previous song */
            melody_cancel_requested = false;
            melody_playing = true;
            current_song_index = song_index;
            
            ESP_LOGI(TAG_BUZZER, "Playing song %d of %d", song_index + 1, (int)SONG_LIBRARY_SIZE);
            
            /* Play the RTTTL song (this blocks the melody task, but not the main loop!) */
            buzzer_play_rtttl(SONG_LIBRARY[song_index]);
            
            melody_playing = false;
            
            /* If cancelled, the new song will be queued by the caller */
            if (melody_cancel_requested) {
                ESP_LOGI(TAG_BUZZER, "Song cancelled");
                melody_cancel_requested = false;
            }
        }
    }
}

/* Initialize the melody task and queue */
static void init_melody_task(void)
{
    /* Create queue for song requests (holds 1 song at a time - no queuing multiple) */
    melody_queue = xQueueCreate(1, sizeof(int));
    
    if (melody_queue == NULL) {
        ESP_LOGE(TAG_BUZZER, "Failed to create melody queue!");
        return;
    }
    
    /* Create the melody task with its own stack */
    BaseType_t result = xTaskCreate(
        melody_task,        /* Task function */
        "melody_task",      /* Name */
        4096,               /* Stack size (bytes) */
        NULL,               /* Parameters */
        5,                  /* Priority (lower than main loop) */
        NULL                /* Task handle (not needed) */
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG_BUZZER, "Failed to create melody task!");
    } else {
        ESP_LOGI(TAG_BUZZER, "Melody task created - background playback enabled!");
    }
}

/* Request a random song to play (non-blocking - returns immediately) */
static void buzzer_play_random_song(void)
{
    if (melody_queue == NULL) {
        return;  /* Queue not initialized */
    }
    
    /* If a song is currently playing, cancel it first */
    if (melody_playing) {
        ESP_LOGI(TAG_BUZZER, "Cancelling current song...");
        melody_cancel_requested = true;
        buzzer_stop();  /* Stop buzzer immediately */
        
        /* Brief delay to let melody task process the cancellation */
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
    
    /* Pick a random song (different from current if possible) */
    int song_index;
    int attempts = 0;
    do {
        uint32_t random_val = esp_random();
        song_index = random_val % SONG_LIBRARY_SIZE;
        attempts++;
    } while (song_index == current_song_index && attempts < 5 && SONG_LIBRARY_SIZE > 1);
    
    /* Queue the song (non-blocking - if queue is full, just ignore) */
    xQueueSend(melody_queue, &song_index, 0);
}

/* ============================================================================
   WIFI CONNECTION
   ============================================================================ */

/* WiFi disconnect reason codes (for debugging) */
static const char* wifi_disconnect_reason_str(uint8_t reason)
{
    switch (reason) {
        case 1:  return "UNSPECIFIED";
        case 2:  return "AUTH_EXPIRE";
        case 3:  return "AUTH_LEAVE";
        case 4:  return "ASSOC_EXPIRE";
        case 5:  return "ASSOC_TOOMANY";
        case 6:  return "NOT_AUTHED";
        case 7:  return "NOT_ASSOCED";
        case 8:  return "ASSOC_LEAVE";
        case 9:  return "ASSOC_NOT_AUTHED";
        case 10: return "DISASSOC_PWRCAP_BAD";
        case 11: return "DISASSOC_SUPCHAN_BAD";
        case 12: return "IE_INVALID";
        case 13: return "MIC_FAILURE";
        case 14: return "4WAY_HANDSHAKE_TIMEOUT";  /* Common: wrong password */
        case 15: return "GROUP_KEY_UPDATE_TIMEOUT";
        case 16: return "IE_IN_4WAY_DIFFERS";
        case 17: return "GROUP_CIPHER_INVALID";
        case 18: return "PAIRWISE_CIPHER_INVALID";
        case 19: return "AKMP_INVALID";
        case 20: return "UNSUPP_RSN_IE_VERSION";
        case 21: return "INVALID_RSN_IE_CAP";
        case 22: return "802_1X_AUTH_FAILED";
        case 23: return "CIPHER_SUITE_REJECTED";
        case 200: return "BEACON_TIMEOUT";  /* Common: out of range */
        case 201: return "NO_AP_FOUND";     /* Common: SSID not found */
        case 202: return "AUTH_FAIL";       /* Common: wrong password */
        case 203: return "ASSOC_FAIL";
        case 204: return "HANDSHAKE_TIMEOUT";
        case 205: return "CONNECTION_FAIL";
        default: return "UNKNOWN";
    }
}

/* WiFi event handler */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG_WIFI, "WiFi started, connecting to %s...", WIFI_SSID);
        /* Note: Connection is now started manually after scan completes */
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        uint8_t reason = disconnected->reason;
        
        ESP_LOGW(TAG_WIFI, "Disconnected! Reason: %d (%s)", reason, wifi_disconnect_reason_str(reason));
        
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

/* Scan for available WiFi networks (for debugging) */
static void wifi_scan_networks(void)
{
    ESP_LOGI(TAG_WIFI, "");
    ESP_LOGI(TAG_WIFI, "╔══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG_WIFI, "║  📡 SCANNING FOR WIFI NETWORKS...                        ║");
    ESP_LOGI(TAG_WIFI, "╚══════════════════════════════════════════════════════════╝");
    
    /* Start scan */
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 300,
    };
    
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);  /* Blocking scan */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_WIFI, "Scan failed: %s", esp_err_to_name(ret));
        return;
    }
    
    /* Get results */
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    if (ap_count == 0) {
        ESP_LOGW(TAG_WIFI, "  ❌ NO NETWORKS FOUND! Check antenna/location.");
        return;
    }
    
    ESP_LOGI(TAG_WIFI, "  Found %d networks:", ap_count);
    
    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (ap_list == NULL) {
        ESP_LOGE(TAG_WIFI, "  Failed to allocate memory for scan results");
        return;
    }
    
    esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    
    bool found_target = false;
    for (int i = 0; i < ap_count; i++) {
        const char *match = "";
        if (strcmp((char*)ap_list[i].ssid, WIFI_SSID) == 0) {
            match = " ✅ TARGET";
            found_target = true;
        }
        ESP_LOGI(TAG_WIFI, "    %2d. %-32s  CH:%2d  RSSI:%4d dBm%s",
                 i + 1,
                 ap_list[i].ssid,
                 ap_list[i].primary,
                 ap_list[i].rssi,
                 match);
    }
    
    if (!found_target) {
        ESP_LOGW(TAG_WIFI, "");
        ESP_LOGW(TAG_WIFI, "  ⚠️  TARGET NETWORK '%s' NOT FOUND IN SCAN!", WIFI_SSID);
        ESP_LOGW(TAG_WIFI, "  Possible causes:");
        ESP_LOGW(TAG_WIFI, "    - SSID typo (check exact spelling/case)");
        ESP_LOGW(TAG_WIFI, "    - Router's 2.4GHz is off (ESP32 can't see 5GHz)");
        ESP_LOGW(TAG_WIFI, "    - Too far from router");
        ESP_LOGW(TAG_WIFI, "    - Hidden SSID");
    } else {
        ESP_LOGI(TAG_WIFI, "  ✅ Target network '%s' found!", WIFI_SSID);
    }
    
    free(ap_list);
    ESP_LOGI(TAG_WIFI, "");
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
            /* Use OPEN threshold to accept any security mode (WPA, WPA2, WPA3) */
            .threshold.authmode = WIFI_AUTH_OPEN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,  /* Support WPA3 if router uses it */
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    /* Do a quick scan first to see what networks are available (helps debug) */
    vTaskDelay(100 / portTICK_PERIOD_MS);  /* Brief delay to let WiFi fully start */
    wifi_scan_networks();
    
    /* Now start the actual connection */
    ESP_LOGI(TAG_WIFI, "Starting connection to %s...", WIFI_SSID);
    esp_wifi_connect();
    
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
    if (strcmp(command, "cycle") == 0) {
        current_animation = ANIM_CYCLE;
        ESP_LOGI(TAG_MQTT, "Animation: CYCLE (fusion <-> wave every 15s)");
    }
    else if (strcmp(command, "fusion") == 0) {
        current_animation = ANIM_FUSION;
        ESP_LOGI(TAG_MQTT, "Animation: FUSION (white to purple gradient)");
    }
    else if (strcmp(command, "wave") == 0) {
        current_animation = ANIM_WAVE;
        ESP_LOGI(TAG_MQTT, "Animation: WAVE (light blue pulse from center)");
    }
    else if (strcmp(command, "tetris") == 0) {
        current_animation = ANIM_TETRIS;
        ESP_LOGI(TAG_MQTT, "Animation: TETRIS (random colored pixels stacking)");
    }
    else if (strcmp(command, "stars") == 0) {
        current_animation = ANIM_STARS;
        ESP_LOGI(TAG_MQTT, "Animation: STARS (twinkling stars)");
    }
    else if (strcmp(command, "meteor") == 0) {
        current_animation = ANIM_METEOR;
        ESP_LOGI(TAG_MQTT, "Animation: METEOR SPINNER");
    }
    else if (strcmp(command, "shower") == 0 || strcmp(command, "meteor shower") == 0) {
        current_animation = ANIM_METEOR_SHOWER;
        ESP_LOGI(TAG_MQTT, "Animation: METEOR SHOWER (rainbow trails)");
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
        current_animation = ANIM_CYCLE;  /* Default to cycle when turned on */
        ESP_LOGI(TAG_MQTT, "Animation: ON (cycle)");
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
    /* ========================================================================
       ZIGBEE BLIND CONTROL COMMANDS
       ======================================================================== */
    else if (strcmp(command, "blinds:pair") == 0) {
        ESP_LOGI(TAG_MQTT, "Zigbee: Opening network for pairing (60s)...");
        zigbee_permit_join(60);
    }
    else if (strcmp(command, "blinds:open") == 0) {
        ESP_LOGI(TAG_MQTT, "Zigbee: Opening blinds");
        zigbee_blind_open(0);  /* 0 = first paired blind */
    }
    else if (strcmp(command, "blinds:close") == 0) {
        ESP_LOGI(TAG_MQTT, "Zigbee: Closing blinds");
        zigbee_blind_close(0);
    }
    else if (strcmp(command, "blinds:stop") == 0) {
        ESP_LOGI(TAG_MQTT, "Zigbee: Stopping blinds");
        zigbee_blind_stop(0);
    }
    else if (strncmp(command, "blinds:", 7) == 0) {
        /* blinds:XX where XX is a percentage (0-100) */
        int percent = atoi(command + 7);
        if (percent >= 0 && percent <= 100) {
            ESP_LOGI(TAG_MQTT, "Zigbee: Setting blinds to %d%%", percent);
            zigbee_blind_set_position(0, (uint8_t)percent);
        } else {
            ESP_LOGW(TAG_MQTT, "Invalid blind position: %d", percent);
        }
    }
    else if (strcmp(command, "zigbee:status") == 0) {
        /* Print detailed network status and device list */
        zigbee_print_network_status();
    }
    else if (strcmp(command, "zigbee:scan") == 0) {
        /* Start periodic scanning every 10 seconds */
        ESP_LOGI(TAG_MQTT, "Zigbee: Starting device scan (10s interval)");
        zigbee_start_device_scan(10);
    }
    else if (strcmp(command, "zigbee:scan:stop") == 0) {
        /* Stop periodic scanning */
        ESP_LOGI(TAG_MQTT, "Zigbee: Stopping device scan");
        zigbee_stop_device_scan();
    }
    else if (strncmp(command, "zigbee:scan:", 12) == 0) {
        /* zigbee:scan:XX where XX is interval in seconds */
        int interval = atoi(command + 12);
        if (interval > 0 && interval <= 3600) {
            ESP_LOGI(TAG_MQTT, "Zigbee: Starting device scan (%ds interval)", interval);
            zigbee_start_device_scan((uint16_t)interval);
        } else {
            ESP_LOGW(TAG_MQTT, "Invalid scan interval: %d (use 1-3600)", interval);
        }
    }
    else if (strcmp(command, "zigbee:neighbors") == 0) {
        /* Scan neighbors in radio range */
        zigbee_scan_neighbors();
    }
    else if (strcmp(command, "zigbee:finder") == 0) {
        /* Restart finder mode to search for new devices */
        ESP_LOGI(TAG_MQTT, "Zigbee: Restarting finder mode (60s search)...");
        zigbee_start_device_scan(ZIGBEE_FINDER_SCAN_INTERVAL);
        zigbee_permit_join(ZIGBEE_FINDER_TIMEOUT_SEC);
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
#define RGBW_LED_COUNT 45  // RGBW NeoPixel strip

/* Handle for the external RGBW NeoPixel */
static led_strip_handle_t rgbw_strip = NULL;

/* ============================================================================
   POWER BUTTON AND STANDBY MODE
   ============================================================================
   BOOT Button (GPIO9): Built-in button on ESP32-C6 devkit - used for power on/off
   Melody Button (GPIO5): External button - triggers random melody playback
   ============================================================================ */

#define BOOT_BUTTON_GPIO 9     /* Built-in BOOT button for power on/off */
#define MELODY_BUTTON_GPIO 5   /* External button for melody playback */

/* Forward declaration for onboard LED control (defined in LED section below) */
static void set_onboard_led_rgb_internal(uint8_t r, uint8_t g, uint8_t b);
/* Note: This function is defined inside #ifdef CONFIG_BLINK_LED_STRIP */

/* Flag to track if shutdown was requested */
static volatile bool shutdown_requested = false;

/* Configure both buttons */
static void configure_buttons(void)
{
    ESP_LOGI(TAG, "Configuring BOOT button (power) on GPIO%d", BOOT_BUTTON_GPIO);
    ESP_LOGI(TAG, "Configuring MELODY button on GPIO%d", MELODY_BUTTON_GPIO);
    
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO) | (1ULL << MELODY_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,    /* Internal pull-up, buttons connect to GND */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE       /* We'll poll in the main loop */
    };
    gpio_config(&io_conf);
}

/* Legacy function name for compatibility */
static void configure_power_button(void)
{
    configure_buttons();
}

/* Check if power button (BOOT) is pressed (active low with pull-up) */
static bool is_power_button_pressed(void)
{
    return gpio_get_level(BOOT_BUTTON_GPIO) == 0;
}

/* Check if melody button is pressed (active low with pull-up) */
static bool is_melody_button_pressed(void)
{
    return gpio_get_level(MELODY_BUTTON_GPIO) == 0;
}

/* Standby mode: bright neon pink on ONBOARD LED only, waiting for button press
   NO signals sent to NeoPixel GPIO - safe to have nothing connected.
   Returns true when button is pressed (time to wake up) */
static bool run_standby_mode(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║     STANDBY MODE - Press button to start                 ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
    ESP_LOGI(TAG, ">>> NeoPixel GPIO is IDLE - safe to disconnect.");
    ESP_LOGI(TAG, ">>> Onboard LED: neon pink");
    ESP_LOGI(TAG, "");
    
    /* Set onboard LED to bright neon pink: R=255, G=20, B=147 (hot pink) */
    set_onboard_led_rgb_internal(255, 20, 147);
    
    while (1) {
        /* Check for button press */
        if (is_power_button_pressed()) {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            if (is_power_button_pressed()) {
                while (is_power_button_pressed()) {
                    vTaskDelay(50 / portTICK_PERIOD_MS);
                }
                vTaskDelay(100 / portTICK_PERIOD_MS);
                ESP_LOGI(TAG, ">>> Button pressed! Exiting standby...");
                buzzer_click();  /* Button feedback */
                return true;
            }
        }
        
        vTaskDelay(100 / portTICK_PERIOD_MS);  /* Slow poll - just waiting for button */
    }
}

/* Graceful fade-out for LED strip */
static void graceful_led_strip_shutdown(void)
{
    if (rgbw_strip == NULL) return;
    
    ESP_LOGI(TAG, ">>> Gracefully fading out LED strip...");
    
    /* Fade out over ~0.5 seconds (30 steps at 60fps) */
    for (int fade_step = 30; fade_step >= 0; fade_step--) {
        float fade = (float)fade_step / 30.0f;
        
        for (int i = 0; i < RGBW_LED_COUNT; i++) {
            /* We don't know current colors, so just fade to black uniformly */
            uint8_t dim = (uint8_t)(20 * fade);  /* Fade from dim white to black */
            led_strip_set_pixel_rgbw(rgbw_strip, i, dim, dim, dim, dim);
        }
        led_strip_refresh(rgbw_strip);
        vTaskDelay(16 / portTICK_PERIOD_MS);
    }
    
    /* Ensure all LEDs are fully off */
    for (int i = 0; i < RGBW_LED_COUNT; i++) {
        led_strip_set_pixel_rgbw(rgbw_strip, i, 0, 0, 0, 0);
    }
    led_strip_refresh(rgbw_strip);
    
    /* Small delay to ensure data line is stable (safe to disconnect) */
    vTaskDelay(50 / portTICK_PERIOD_MS);
    
    ESP_LOGI(TAG, ">>> LED strip is now safe to disconnect.");
}

/* Enter standby mode (called when power button pressed during operation) */
static void enter_standby_mode(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║     GRACEFUL SHUTDOWN → STANDBY                          ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
    
    /* Step 1: Gracefully fade out LED strip */
    graceful_led_strip_shutdown();
    
    /* Step 2: Disconnect MQTT gracefully */
    ESP_LOGI(TAG, ">>> Disconnecting MQTT...");
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    /* Step 3: Disconnect WiFi gracefully */
    ESP_LOGI(TAG, ">>> Disconnecting WiFi...");
    esp_wifi_disconnect();
    vTaskDelay(200 / portTICK_PERIOD_MS);
    esp_wifi_stop();
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    ESP_LOGI(TAG, ">>> All systems safely shut down.");
    ESP_LOGI(TAG, ">>> Safe to disconnect LEDs and peripherals.");
    ESP_LOGI(TAG, "");
    
    /* Run standby loop until button pressed */
    run_standby_mode();
    
    /* Button was pressed - restart the system to do full boot */
    ESP_LOGI(TAG, ">>> Restarting for full boot sequence...");
    esp_restart();
}

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

/* Draw brightness gauge on LED strip
   Shows current brightness as a bar graph with gradient */
static void draw_brightness_gauge(void)
{
    if (rgbw_strip == NULL) return;
    
    float brightness_pct = pot_brightness;  /* 0.05 to 1.0 */
    
    /* Normalize to 0-1 range for gauge calculation */
    float normalized = (brightness_pct - 0.05f) / 0.95f;
    if (normalized < 0) normalized = 0;
    if (normalized > 1) normalized = 1;
    
    /* Gauge fills like a gradient "pouring" into the strip:
       - At minimum: ~4 pixels lit with gradient
       - At maximum: all 45 pixels lit with gradient stretched across
       - The gradient always goes from bright (first pixel) to dim (last lit pixel)
    */
    
    const int MIN_FILL_PIXELS = 4;   /* Minimum pixels shown at lowest setting */
    const float HEAD_BRIGHTNESS = 0.30f;  /* First pixel brightness (30%) */
    const float TAIL_BRIGHTNESS = 0.02f;  /* Last lit pixel brightness (2% - barely visible) */
    
    /* Calculate how many pixels should be filled */
    /* Lerp from MIN_FILL_PIXELS to RGBW_LED_COUNT based on normalized */
    float fill_pixels_float = MIN_FILL_PIXELS + normalized * (RGBW_LED_COUNT - MIN_FILL_PIXELS);
    int fill_pixels = (int)(fill_pixels_float + 0.5f);  /* Round to nearest */
    if (fill_pixels < MIN_FILL_PIXELS) fill_pixels = MIN_FILL_PIXELS;
    if (fill_pixels > RGBW_LED_COUNT) fill_pixels = RGBW_LED_COUNT;
    
    for (int i = 0; i < RGBW_LED_COUNT; i++) {
        float pixel_brightness = 0.0f;
        
        if (i < fill_pixels) {
            /* This pixel is within the filled region */
            /* Calculate position within the gradient (0 = head, 1 = tail) */
            float gradient_pos = (fill_pixels > 1) ? (float)i / (float)(fill_pixels - 1) : 0.0f;
            
            /* Lerp from HEAD_BRIGHTNESS to TAIL_BRIGHTNESS */
            pixel_brightness = HEAD_BRIGHTNESS - gradient_pos * (HEAD_BRIGHTNESS - TAIL_BRIGHTNESS);
        }
        /* Pixels beyond fill_pixels remain at 0 */
        
        /* Set warm white only (W channel) */
        uint8_t w_value = (uint8_t)(pixel_brightness * 255.0f);
        set_pixel_rgbw(i, 0, 0, 0, w_value);
    }
    
    refresh_strip();
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

/* Master brightness is directly controlled by potentiometer
   - Pot at min (0V): 5% brightness
   - Pot at max (3.3V): 100% brightness
   pot_brightness ranges from 0.05 to 1.0 */
static float get_master_brightness(void)
{
    return pot_brightness;  /* Direct control, no cap */
}

/* Macro for all animations to use */
#define MASTER_BRIGHTNESS (get_master_brightness())

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

/* ============================================================================
   METEOR SHOWER ANIMATION
   ============================================================================
   Multiple meteors (4-5) moving in same direction with rainbow comet trails.
   Each meteor has varying brightness which determines tail length.
   ============================================================================ */

#define METEOR_SHOWER_COUNT 5   /* Number of simultaneous meteors */
#define METEOR_SHOWER_MIN_TAIL 3
#define METEOR_SHOWER_MAX_TAIL 15

static void draw_meteor_shower(bool reset)
{
    if (rgbw_strip == NULL) return;
    
    /* Static state for meteors */
    static struct {
        float position;        /* Current position (floating point for smooth movement) */
        float speed;           /* Movement speed (0.3 - 1.0) */
        float brightness;      /* Peak brightness (0.4 - 1.0), determines tail length */
        float hue;             /* Rainbow hue for this meteor (0-360) */
        int tail_length;       /* Tail length in pixels */
        bool active;           /* Is this meteor active */
    } meteors[METEOR_SHOWER_COUNT];
    
    static bool initialized = false;
    static uint32_t rand_seed = 98765;
    
    #define SHOWER_RAND() (rand_seed = rand_seed * 1103515245 + 12345, (rand_seed >> 16) & 0xFFFF)
    
    /* Initialize or reset meteors */
    if (reset || !initialized) {
        for (int i = 0; i < METEOR_SHOWER_COUNT; i++) {
            /* Spread meteors evenly across the strip initially */
            meteors[i].position = (float)(RGBW_LED_COUNT / METEOR_SHOWER_COUNT) * i;
            meteors[i].speed = 0.3f + (float)(SHOWER_RAND() % 70) / 100.0f;  /* 0.3 - 1.0 */
            meteors[i].brightness = 0.4f + (float)(SHOWER_RAND() % 60) / 100.0f;  /* 0.4 - 1.0 */
            meteors[i].hue = (float)(SHOWER_RAND() % 360);  /* Random starting hue */
            /* Tail length based on brightness */
            meteors[i].tail_length = METEOR_SHOWER_MIN_TAIL + 
                (int)((meteors[i].brightness - 0.4f) / 0.6f * (METEOR_SHOWER_MAX_TAIL - METEOR_SHOWER_MIN_TAIL));
            meteors[i].active = true;
        }
        initialized = true;
    }
    
    /* Clear strip first */
    for (int i = 0; i < RGBW_LED_COUNT; i++) {
        set_pixel_rgbw(i, 0, 0, 0, 0);
    }
    
    /* Pixel buffer to blend multiple meteors */
    static float pixel_r[RGBW_LED_COUNT];
    static float pixel_g[RGBW_LED_COUNT];
    static float pixel_b[RGBW_LED_COUNT];
    
    for (int i = 0; i < RGBW_LED_COUNT; i++) {
        pixel_r[i] = 0;
        pixel_g[i] = 0;
        pixel_b[i] = 0;
    }
    
    /* Draw each meteor */
    for (int m = 0; m < METEOR_SHOWER_COUNT; m++) {
        if (!meteors[m].active) continue;
        
        int head_pos = (int)meteors[m].position % RGBW_LED_COUNT;
        float brightness = meteors[m].brightness;
        int tail_len = meteors[m].tail_length;
        float base_hue = meteors[m].hue;
        
        /* Draw the meteor head and tail */
        for (int t = 0; t <= tail_len; t++) {
            int pixel_idx = (head_pos - t + RGBW_LED_COUNT) % RGBW_LED_COUNT;
            
            /* Calculate brightness falloff (exponential decay for tail) */
            float tail_factor;
            if (t == 0) {
                tail_factor = 1.0f;  /* Head is brightest */
            } else {
                /* Exponential decay for nice comet trail */
                tail_factor = powf(0.7f, (float)t);
            }
            
            float pixel_brightness = brightness * tail_factor;
            
            /* Rainbow gradient along the tail - hue shifts through the trail */
            float hue = fmodf(base_hue + (float)t * 15.0f, 360.0f);  /* Shift hue along tail */
            
            /* HSV to RGB conversion */
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
            
            /* Add to pixel buffer (additive blending) */
            pixel_r[pixel_idx] += r * pixel_brightness;
            pixel_g[pixel_idx] += g * pixel_brightness;
            pixel_b[pixel_idx] += b * pixel_brightness;
        }
        
        /* Move meteor forward */
        meteors[m].position += meteors[m].speed * animation_speed * 3.0f;
        
        /* Wrap around */
        if (meteors[m].position >= RGBW_LED_COUNT) {
            meteors[m].position -= RGBW_LED_COUNT;
            
            /* Randomize properties for next loop */
            meteors[m].speed = 0.3f + (float)(SHOWER_RAND() % 70) / 100.0f;
            meteors[m].brightness = 0.4f + (float)(SHOWER_RAND() % 60) / 100.0f;
            meteors[m].hue = (float)(SHOWER_RAND() % 360);
            meteors[m].tail_length = METEOR_SHOWER_MIN_TAIL + 
                (int)((meteors[m].brightness - 0.4f) / 0.6f * (METEOR_SHOWER_MAX_TAIL - METEOR_SHOWER_MIN_TAIL));
        }
        
        /* Slowly shift hue for rainbow effect */
        meteors[m].hue = fmodf(meteors[m].hue + 0.5f, 360.0f);
    }
    
    /* Apply pixel buffer to LEDs with clamping */
    for (int i = 0; i < RGBW_LED_COUNT; i++) {
        float r = pixel_r[i];
        float g = pixel_g[i];
        float b = pixel_b[i];
        
        /* Clamp to 1.0 */
        if (r > 1.0f) r = 1.0f;
        if (g > 1.0f) g = 1.0f;
        if (b > 1.0f) b = 1.0f;
        
        /* Apply gamma correction and master brightness */
        r = gamma_correct(r);
        g = gamma_correct(g);
        b = gamma_correct(b);
        
        set_pixel_rgbw(i, 
            (uint8_t)(r * 255 * MASTER_BRIGHTNESS),
            (uint8_t)(g * 255 * MASTER_BRIGHTNESS),
            (uint8_t)(b * 255 * MASTER_BRIGHTNESS),
            0);
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

/* Fusion mode - Two particles passing through each other
   
   Animation:
   - White particle starts at left (pixel 0)
   - Purple particle starts at right (pixel N-1)
   - They move toward each other with ease-in-out motion:
     * Start slow, accelerate toward the middle
     * Pass through each other at the center (fastest point)
     * Decelerate as they reach the opposite ends
   - Then they return the same way, creating a continuous loop
   
   Each particle is a single bright pixel (could add a small tail later)
*/
static void draw_fusion(float phase)
{
    if (rgbw_strip == NULL) return;
    
    /* Use sine wave for continuous spring-like motion
       - Never fully stops at the ends
       - Slows down at extremes but immediately springs back
       - Like bending knees before jumping: slow at bottom, springs back up
       
       sin(phase) oscillates -1 to 1, we map to 0 to 1 for position */
    float eased = 0.5f + 0.5f * sinf(phase);  /* Continuous 0 to 1 to 0 oscillation */
    
    /* Calculate particle positions (as float for sub-pixel positioning) */
    float max_pos = (float)(RGBW_LED_COUNT - 1);
    
    /* White particle: starts at 0, ends at max_pos */
    float white_pos = eased * max_pos;
    
    /* Purple particle: starts at max_pos, ends at 0 */
    float purple_pos = max_pos - eased * max_pos;
    
    /* Falloff rate - controls how quickly the trail fades
       Higher = sharper falloff, lower = longer trail */
    const float falloff_rate = 0.30f;  /* Doubled for faster falloff */
    
    /* For each pixel, calculate influence from both particles */
    for (int i = 0; i < RGBW_LED_COUNT; i++) {
        float pixel_pos = (float)i;
        
        /* Distance from each particle */
        float dist_from_white = fabsf(pixel_pos - white_pos);
        float dist_from_purple = fabsf(pixel_pos - purple_pos);
        
        /* Exponential falloff from each particle (1.0 at particle, fades with distance) */
        float white_influence = expf(-dist_from_white * falloff_rate);
        float purple_influence = expf(-dist_from_purple * falloff_rate);
        
        /* Combined brightness (both particles contribute) */
        float total_influence = white_influence + purple_influence;
        if (total_influence > 1.0f) total_influence = 1.0f;
        
        /* Calculate blend ratio between white and purple
           If white_influence dominates -> more white
           If purple_influence dominates -> more purple */
        float blend_sum = white_influence + purple_influence;
        float white_ratio = (blend_sum > 0.001f) ? (white_influence / blend_sum) : 0.5f;
        float purple_ratio = 1.0f - white_ratio;
        
        /* White color: pure warm white (W channel) */
        float w_val = 255.0f * white_influence;
        
        /* Purple color: R + B channels */
        float r_val = 120.0f * purple_influence;
        float b_val = 255.0f * purple_influence;
        
        /* Apply master brightness */
        uint8_t pr = (uint8_t)(r_val * MASTER_BRIGHTNESS);
        uint8_t pg = 0;
        uint8_t pb = (uint8_t)(b_val * MASTER_BRIGHTNESS);
        uint8_t pw = (uint8_t)(w_val * MASTER_BRIGHTNESS);
        
        set_pixel_rgbw(i, pr, pg, pb, pw);
    }
    
    refresh_strip();
}

/* Wave mode - blue pulse over dark blue ocean
   - Wave starts at center with gradual fade-in
   - Expands outward with smooth motion
   - Trail fades to a deep blue ocean floor
   - Ocean floor is always present (dark blue, value 15)
   - Wave ripples fade smoothly down to the floor
*/
static void draw_wave(float phase)
{
    if (rgbw_strip == NULL) return;
    
    /* Wave peak color (light blue) */
    const float peak_r = 30.0f;
    const float peak_g = 80.0f;
    const float peak_b = 255.0f;
    
    /* Ocean floor - deep dark blue that's always present */
    const float ocean_floor_b = 15.0f;  /* Deep blue ocean */
    
    /* Center of the strip */
    float center = (float)(RGBW_LED_COUNT - 1) / 2.0f;
    
    /* Wave width for soft gaussian-like falloff */
    float wave_width = 3.0f;
    
    /* Max radius extends beyond edge so wave fully exits before regenerating */
    float max_radius = center + wave_width * 3.0f;
    
    const float PI = 3.14159f;
    
    /* Normalize phase to 0-1 range */
    float t = phase / (2.0f * PI);
    if (t > 1.0f) t = 1.0f;
    if (t < 0.0f) t = 0.0f;
    
    /* Apply ease-in-out to wave position for smoother motion */
    float eased_t = t * t * (3.0f - 2.0f * t);
    float wave_pos = eased_t * max_radius;
    
    /* Wave intensity envelope: fade in at start, fade out at end */
    float fade_in_duration = 0.30f;
    float fade_out_start = 0.65f;
    
    float wave_intensity;
    if (t < fade_in_duration) {
        float fade_t = t / fade_in_duration;
        wave_intensity = fade_t * fade_t;
    } else if (t > fade_out_start) {
        float fade_t = (t - fade_out_start) / (1.0f - fade_out_start);
        wave_intensity = 1.0f - (fade_t * fade_t);
    } else {
        wave_intensity = 1.0f;
    }
    
    for (int i = 0; i < RGBW_LED_COUNT; i++) {
        /* Distance from center for this pixel */
        float dist_from_center = fabsf((float)i - center);
        
        /* Distance from the current wave position */
        float dist_from_wave = fabsf(dist_from_center - wave_pos);
        
        /* Gaussian falloff from wave position (0 to 1, 1 = at wave peak) */
        float proximity = expf(-dist_from_wave * dist_from_wave / (wave_width * wave_width));
        
        /* R and G fade FASTER than B (cube the proximity for sharper falloff) */
        float rg_factor = proximity * proximity * proximity;
        float b_factor = proximity;
        
        /* Apply wave intensity envelope */
        rg_factor *= wave_intensity;
        b_factor *= wave_intensity;
        
        /* Calculate wave colors (above the ocean floor)
           - At wave peak: full light blue (R, G, B)
           - In tail: mostly blue, R and G fade away faster */
        float r = peak_r * rg_factor;
        float g = peak_g * rg_factor;
        
        /* Blue: lerp between ocean floor and peak based on wave proximity
           Even when wave intensity is 0, the ocean floor remains */
        float wave_blue = peak_b * b_factor;
        float b = ocean_floor_b + wave_blue;  /* Floor + wave on top */
        if (b > 255.0f) b = 255.0f;
        
        /* Apply master brightness */
        uint8_t pr = (uint8_t)(r * MASTER_BRIGHTNESS);
        uint8_t pg = (uint8_t)(g * MASTER_BRIGHTNESS);
        uint8_t pb = (uint8_t)(b * MASTER_BRIGHTNESS);
        
        set_pixel_rgbw(i, pr, pg, pb, 0);
    }
    refresh_strip();
}

/* Tetris mode - random colored pixels falling, stacking, then draining
   - Pixels "fall" from one end (left) and stack on the other (right)
   - Each pixel has a random vibrant color
   - When full, drains: bottom pixel disappears, whole stack shifts down
   - Seamless conveyor belt effect out the bottom
*/
static void draw_tetris(float phase, bool reset)
{
    if (rgbw_strip == NULL) return;
    
    /* Static state - persists between calls */
    static uint8_t stacked_r[60];  /* Stacked pixel colors (index 0 = first landed = bottom) */
    static uint8_t stacked_g[60];
    static uint8_t stacked_b[60];
    static int stack_height = 0;   /* How many pixels are stacked */
    static int falling_pos = 0;    /* Current position of falling pixel */
    static uint8_t fall_r = 0, fall_g = 0, fall_b = 0;  /* Falling pixel color */
    static bool initialized = false;
    static bool draining = false;  /* false = filling, true = draining */
    static uint32_t random_seed = 12345;
    static int frame_count = 0;
    
    /* Simple pseudo-random number generator */
    #define TETRIS_RAND() (random_seed = random_seed * 1103515245 + 12345, (random_seed >> 16) & 0xFF)
    
    /* Generate a saturated color - one channel dominant, others low for contrast */
    #define NEW_COLOR() do { \
        uint8_t rnd = TETRIS_RAND(); \
        int color_type = rnd % 6;  /* 6 distinct color types */ \
        uint8_t high = 180 + (TETRIS_RAND() % 75);  /* Dominant: 180-255 */ \
        uint8_t mid = TETRIS_RAND() % 80;           /* Secondary: 0-80 */ \
        uint8_t low = TETRIS_RAND() % 30;           /* Tertiary: 0-30 */ \
        switch (color_type) { \
            case 0: fall_r = high; fall_g = low;  fall_b = mid;  break; /* Red-ish */ \
            case 1: fall_r = low;  fall_g = high; fall_b = mid;  break; /* Green-ish */ \
            case 2: fall_r = mid;  fall_g = low;  fall_b = high; break; /* Blue-ish */ \
            case 3: fall_r = high; fall_g = mid;  fall_b = low;  break; /* Orange/Yellow */ \
            case 4: fall_r = high; fall_g = low;  fall_b = high; break; /* Magenta */ \
            case 5: fall_r = low;  fall_g = high; fall_b = high; break; /* Cyan */ \
        } \
    } while(0)
    
    /* Reset state if requested or first run */
    if (reset || !initialized) {
        stack_height = 0;
        falling_pos = 0;
        draining = false;
        frame_count = 0;
        NEW_COLOR();
        initialized = true;
    }
    
    /* Animation speed - move every 2 frames at 60 FPS */
    int frames_per_step = 2;
    frame_count++;
    
    if (frame_count >= frames_per_step) {
        frame_count = 0;
        
        if (!draining) {
            /* FILLING MODE: pixels fall from left (pos 0), stack on right */
            falling_pos += 2;  /* Move 2 pixels at a time for faster animation */
            
            int land_position = RGBW_LED_COUNT - 1 - stack_height;
            if (falling_pos >= land_position) {
                /* Land the pixel - add to top of stack */
                if (stack_height < 60) {
                    stacked_r[stack_height] = fall_r;
                    stacked_g[stack_height] = fall_g;
                    stacked_b[stack_height] = fall_b;
                }
                stack_height++;
                
                /* Check if full */
                if (stack_height >= RGBW_LED_COUNT) {
                    draining = true;
                    /* No falling pixel during drain */
                } else {
                    falling_pos = 0;
                    NEW_COLOR();
                }
            }
        } else {
            /* DRAINING MODE: bottom pixels disappear, everything shifts down */
            /* Remove 2 pixels at a time to match faster filling speed */
            int pixels_to_drain = 2;
            for (int p = 0; p < pixels_to_drain && stack_height > 0; p++) {
                /* Shift everything down by 1 */
                for (int i = 0; i < stack_height - 1; i++) {
                    stacked_r[i] = stacked_r[i + 1];
                    stacked_g[i] = stacked_g[i + 1];
                    stacked_b[i] = stacked_b[i + 1];
                }
                stack_height--;
            }
            
            if (stack_height <= 0) {
                /* Fully drained, switch back to filling */
                draining = false;
                stack_height = 0;
                falling_pos = 0;
                NEW_COLOR();
            }
        }
    }
    
    /* Draw the scene */
    for (int i = 0; i < RGBW_LED_COUNT; i++) {
        uint8_t r = 0, g = 0, b = 0;
        
        /* Check if this pixel is part of the stack */
        /* Stack occupies positions from (RGBW_LED_COUNT - stack_height) to (RGBW_LED_COUNT - 1) */
        int stack_start = RGBW_LED_COUNT - stack_height;
        
        if (i >= stack_start && stack_height > 0) {
            /* This pixel is in the stack */
            /* Pixel at position (RGBW_LED_COUNT - 1) = stack index 0 (bottom/first landed) */
            /* Pixel at position stack_start = stack index (stack_height - 1) (top/last landed) */
            int stack_idx = RGBW_LED_COUNT - 1 - i;
            if (stack_idx >= 0 && stack_idx < stack_height && stack_idx < 60) {
                r = stacked_r[stack_idx];
                g = stacked_g[stack_idx];
                b = stacked_b[stack_idx];
            }
        } else if (!draining && i == falling_pos && falling_pos < stack_start) {
            /* This is the falling pixel (only during filling mode) */
            r = fall_r; g = fall_g; b = fall_b;
        }
        
        /* Apply master brightness */
        uint8_t pr = (uint8_t)(r * MASTER_BRIGHTNESS);
        uint8_t pg = (uint8_t)(g * MASTER_BRIGHTNESS);
        uint8_t pb = (uint8_t)(b * MASTER_BRIGHTNESS);
        
        set_pixel_rgbw(i, pr, pg, pb, 0);
    }
    refresh_strip();
    
    #undef TETRIS_RAND
    #undef NEW_COLOR
}

/* Stars twinkling animation - organic starry night
   - Random stars spawn and twinkle at random positions
   - Different star types with different rarities and brightness
   - All stars use some warm W channel for organic glow
   - Slow, gradual transitions for beautiful twinkling
*/
static void draw_stars(bool reset)
{
    if (rgbw_strip == NULL) return;
    
    /* Star types */
    #define STAR_NONE      0
    #define STAR_DIM       1   /* Common: subtle twinkle */
    #define STAR_BRIGHT    2   /* Uncommon: noticeable star */
    #define STAR_SUPERNOVA 3   /* Rare: dramatic bright star */
    
    #define MAX_STARS 12  /* Sparse stars - quality over quantity */
    
    /* Static state */
    static struct {
        int pos;           /* Position on strip (-1 = inactive) */
        int type;          /* Star type */
        float phase;       /* 0 = spawn, ~0.3 = peak, 1 = dead */
        float speed;       /* How fast this star evolves */
        float peak_point;  /* Where in the phase the star peaks (asymmetric) */
        float size_mult;   /* Size multiplier for variation (0.7-1.3) */
        bool has_blue;     /* 25% of stars get slight blue tint in trails */
    } stars[MAX_STARS];
    static bool initialized = false;
    static uint32_t rand_seed = 54321;
    static int frame_count = 0;
    static float twinkle_time = 0.0f;  /* Global time for twinkle oscillation */
    
    #define STAR_RAND() (rand_seed = rand_seed * 1103515245 + 12345, (rand_seed >> 16) & 0xFFFF)
    
    if (reset || !initialized) {
        for (int i = 0; i < MAX_STARS; i++) {
            stars[i].pos = -1;
            stars[i].type = STAR_NONE;
            stars[i].phase = 0;
        }
        initialized = true;
        frame_count = 0;
    }
    
    /* Spawn new stars occasionally */
    frame_count++;
    if (frame_count >= 6) {  /* Check every 6 frames (~10 times/sec at 60fps) */
        frame_count = 0;
        
        /* Find a free star slot */
        int free_slot = -1;
        for (int i = 0; i < MAX_STARS; i++) {
            if (stars[i].pos < 0) {
                free_slot = i;
                break;
            }
        }
        
        if (free_slot >= 0) {
            /* Random chance to spawn (1 in 6 checks for sparse, spread-out stars) */
            if ((STAR_RAND() % 6) == 0) {
                int new_pos = STAR_RAND() % RGBW_LED_COUNT;
                
                /* Make sure no star is already within 4 pixels (prevents overlap) */
                bool pos_taken = false;
                for (int i = 0; i < MAX_STARS; i++) {
                    if (stars[i].pos >= 0 && abs(stars[i].pos - new_pos) <= 4) {
                        pos_taken = true;
                        break;
                    }
                }
                
                if (!pos_taken) {
                    /* Determine star type by rarity:
                       - Mostly medium (BRIGHT) stars
                       - Some small (DIM), rare large (SUPERNOVA) */
                    int rarity_roll = STAR_RAND() % 100;
                    int type;
                    if (rarity_roll < 8) {
                        type = STAR_SUPERNOVA;  /* 8% chance - rare big flares */
                    } else if (rarity_roll < 65) {
                        type = STAR_BRIGHT;     /* 57% chance - most common */
                    } else {
                        type = STAR_DIM;        /* 35% chance - small twinkles */
                    }
                    
                    stars[free_slot].pos = new_pos;
                    stars[free_slot].type = type;
                    stars[free_slot].phase = 0.0f;
                    
                    /* Size variation for visual diversity (0.7 to 1.3) */
                    stars[free_slot].size_mult = 0.7f + (STAR_RAND() % 100) * 0.006f;
                    
                    /* 25% of stars get slight blue tint in trails */
                    stars[free_slot].has_blue = ((STAR_RAND() % 4) == 0);
                    
                    /* Smooth but faster transitions - ease in/out makes them feel natural
                       At 60fps: speed of 0.008 = 125 frames = ~2 seconds lifecycle */
                    float base_speed, speed_var;
                    if (type == STAR_SUPERNOVA) {
                        /* Supernovas: 3-4 seconds lifecycle */
                        base_speed = 0.005f;
                        speed_var = 0.002f;
                    } else if (type == STAR_BRIGHT) {
                        /* Bright stars: 2-3 seconds lifecycle */
                        base_speed = 0.007f;
                        speed_var = 0.003f;
                    } else {
                        /* Dim stars: 1-2 seconds lifecycle */
                        base_speed = 0.012f;
                        speed_var = 0.005f;
                    }
                    stars[free_slot].speed = base_speed + (STAR_RAND() % 100) * speed_var * 0.01f;
                    
                    /* Asymmetric peak: quick rise, slow fade (more natural) */
                    /* Peak between 0.2 and 0.4 of lifecycle */
                    stars[free_slot].peak_point = 0.2f + (STAR_RAND() % 100) * 0.002f;
                }
            }
        }
    }
    
    /* Initialize pixel arrays - pure black background (no floor!) */
    float pixel_r[60] = {0};
    float pixel_g[60] = {0};
    float pixel_b[60] = {0};
    float pixel_w[60] = {0};
    
    /* No blue floor - stars should emerge from pure darkness */
    
    /* Advance twinkle time (ultra slow for butter-smooth breathing) */
    twinkle_time += 0.02f;  /* Very slow progression for silky smooth effect */
    if (twinkle_time > 10000.0f) twinkle_time -= 10000.0f;  /* Prevent overflow */
    
    /* Update and draw each star */
    for (int s = 0; s < MAX_STARS; s++) {
        if (stars[s].pos < 0) continue;
        
        /* Advance phase */
        stars[s].phase += stars[s].speed;
        
        /* Check if star is dead */
        if (stars[s].phase >= 1.0f) {
            stars[s].pos = -1;
            stars[s].type = STAR_NONE;
            continue;
        }
        
        /* Calculate brightness with asymmetric curve:
           - Quick rise to peak (ease-out: starts fast, slows at peak)
           - Slow graceful fade (ease-in: starts slow, speeds up, then eases out at end) */
        float brightness;
        float peak = stars[s].peak_point;
        
        if (stars[s].phase < peak) {
            /* Rising phase: ease-out (fast start, slow at peak) */
            float t = stars[s].phase / peak;  /* 0 -> 1 */
            /* Quadratic ease-out */
            brightness = t * (2.0f - t);
        } else {
            /* Falling phase: ease-in-out (slow start, slow end) */
            float t = (stars[s].phase - peak) / (1.0f - peak);  /* 0 -> 1 */
            /* Smoothstep for graceful fade */
            float fade = t * t * (3.0f - 2.0f * t);
            brightness = 1.0f - fade;
        }
        
        /* Extra smoothing for very gradual changes */
        brightness = brightness * brightness * (3.0f - 2.0f * brightness);
        
        /* ✨ GENTLE TWINKLE EFFECT ✨
           Two layered sine waves for organic, slow twinkling:
           1. Major wave: slow breathing of stars (like stars gently pulsing)
           2. Perlin-like noise: subtle ambient shimmer (portal noise feel)
           Each star has unique phase offset for variety. */
        float star_offset = (float)(stars[s].pos * 17 + s * 31);  /* Unique per star */
        
        /* Layer 1: MAJOR slow breathing (very slow, gentle pulse)
           Period of ~4-6 seconds per cycle */
        float major_wave = sinf(twinkle_time * 0.08f + star_offset * 0.1f) * 0.12f;
        
        /* Layer 2: Subtle ambient shimmer (Perlin-noise-like, layered sines)
           Multiple slow frequencies that create organic drift */
        float noise1 = sinf(twinkle_time * 0.13f + star_offset * 0.23f) * 0.04f;
        float noise2 = sinf(twinkle_time * 0.19f + star_offset * 0.37f) * 0.03f;
        float noise3 = sinf(twinkle_time * 0.31f + star_offset * 0.41f) * 0.02f;
        float ambient_noise = noise1 + noise2 + noise3;
        
        /* Combined twinkle: major pulse + subtle ambient drift */
        float twinkle = 1.0f + major_wave + ambient_noise;
        
        /* Brighter stars have slightly more noticeable twinkle */
        if (stars[s].type == STAR_SUPERNOVA) {
            twinkle = 1.0f + (twinkle - 1.0f) * 1.3f;
        } else if (stars[s].type == STAR_BRIGHT) {
            twinkle = 1.0f + (twinkle - 1.0f) * 1.1f;
        }
        
        /* Apply twinkle to brightness (clamp to valid range) */
        brightness = brightness * twinkle;
        if (brightness < 0.0f) brightness = 0.0f;
        if (brightness > 1.0f) brightness = 1.0f;
        
        /* Star properties based on type:
           - Center: warm white (W channel only)
           - Trails: cold white (RGB equal) with halving falloff
           - Random pixels get slight blue tint for variety */
        float max_w;              /* W channel brightness for center */
        int halo_radius;          /* Trail length (pixels on each side) */
        float trail_intensity;    /* Starting trail brightness */
        
        /* Use stored size variation for this star */
        float size_variation = stars[s].size_mult;
        
        switch (stars[s].type) {
            case STAR_SUPERNOVA:
                /* Big flare: warm white core, 3-4 pixels each side */
                max_w = 255.0f * size_variation;
                halo_radius = 4;       /* 3-4 pixels on each side */
                trail_intensity = 120.0f;
                break;
            case STAR_BRIGHT:
                /* Medium star: warm core, 2-3 pixels each side */
                max_w = 180.0f * size_variation;
                halo_radius = 3;       /* 2-3 pixels on each side */
                trail_intensity = 80.0f;
                break;
            default: /* STAR_DIM */
                /* Small twinkle: subtle warm core, 1-2 pixels each side */
                max_w = 100.0f * size_variation;
                halo_radius = 2;       /* 1-2 pixels on each side */
                trail_intensity = 50.0f;
                break;
        }
        
        /* Draw the star center - WARM WHITE (W channel only) */
        int pos = stars[s].pos;
        pixel_w[pos] += brightness * max_w;
        
        /* Draw cold white trails with HALVING falloff (each pixel = half previous) */
        float current_intensity = trail_intensity * brightness;
        
        /* Blue tint: only 1 in 4 stars get +5 extra blue in trails */
        float blue_bonus = stars[s].has_blue ? 5.0f : 0.0f;
        
        for (int offset = 1; offset <= halo_radius; offset++) {
            /* Halving falloff - each step is half the previous */
            current_intensity *= 0.5f;
            
            /* Cold white trail (equal R, G, B) - most stars are pure white */
            float trail_val = current_intensity;
            
            /* Left neighbor */
            int left = pos - offset;
            if (left >= 0 && left < 60) {
                pixel_r[left] += trail_val;
                pixel_g[left] += trail_val;
                pixel_b[left] += trail_val + blue_bonus;  /* +5 blue for 25% of stars */
            }
            
            /* Right neighbor */
            int right = pos + offset;
            if (right >= 0 && right < RGBW_LED_COUNT && right < 60) {
                pixel_r[right] += trail_val;
                pixel_g[right] += trail_val;
                pixel_b[right] += trail_val + blue_bonus;  /* +5 blue for 25% of stars */
            }
        }
    }
    
    /* Output to strip with clamping and master brightness */
    for (int i = 0; i < RGBW_LED_COUNT; i++) {
        float r = pixel_r[i] * MASTER_BRIGHTNESS;
        float g = pixel_g[i] * MASTER_BRIGHTNESS;
        float b = pixel_b[i] * MASTER_BRIGHTNESS;
        float w = pixel_w[i] * MASTER_BRIGHTNESS;
        
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        if (w > 255) w = 255;
        
        set_pixel_rgbw(i, (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)w);
    }
    refresh_strip();
    
    #undef STAR_NONE
    #undef STAR_DIM
    #undef STAR_BRIGHT
    #undef STAR_SUPERNOVA
    #undef MAX_STARS
    #undef STAR_RAND
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

/* Set onboard LED to a specific RGB color (internal)
   Note: The onboard LED uses GRB order, so we swap R and G here */
static void set_onboard_led_rgb_internal(uint8_t r, uint8_t g, uint8_t b)
{
    if (led_strip) {
        /* Swap R and G for GRB LED order */
        led_strip_set_pixel(led_strip, 0, g, r, b);
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
    
    /* Step 1b: Configure the external RGBW NeoPixel (early, for startup test) */
    ESP_LOGI(TAG, ">>> STEP 1b: Configuring external RGBW NeoPixel for startup test...");
    configure_rgbw_led();
    
    /* Step 1c: Configure power button */
    ESP_LOGI(TAG, ">>> STEP 1c: Configuring power button...");
    configure_power_button();
    
    /* Step 1d: Initialize potentiometer for brightness control */
    ESP_LOGI(TAG, ">>> STEP 1d: Initializing potentiometer brightness control...");
    init_potentiometer();
    
    /* Step 1e: Initialize buzzer */
    ESP_LOGI(TAG, ">>> STEP 1e: Initializing passive buzzer...");
    init_buzzer();
    
    /* Step 1f: Initialize melody task for background playback */
    ESP_LOGI(TAG, ">>> STEP 1f: Starting melody background task...");
    init_melody_task();
    
    /* ========================================================================
       AUTO-BOOT ON STARTUP
       Device automatically boots and runs on power-up.
       Press power button (BOOT) during operation to enter standby.
       ======================================================================== */
    
    ESP_LOGI(TAG, ">>> Auto-boot enabled - starting immediately...");

    /* ========================================================================
       STARTUP SEQUENCE (only runs after button press from standby)
       1. Solid white for 1 second (onboard LED)
       2. Fade to black
       3. Breathing light blue (0 to 50%) while connecting WiFi
       4. Simultaneously: RGB pixel scan on LED strip (visual test)
       5. Fade to solid blue (success) or blinking red (failure)
       ======================================================================== */
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║     BOOTING UP - Starting full initialization            ║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
    
    /* Play startup melody */
    buzzer_startup();
    
    /* Step 1: Solid white for 1 second */
    ESP_LOGI(TAG, ">>> STARTUP: Solid white for 1 second...");
    jump_to_color(255, 255, 255);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    
    /* Step 2: Fade to black */
    ESP_LOGI(TAG, ">>> STARTUP: Fading to black...");
    fade_to_color(0, 0, 0, 500);

    /* ========================================================================
       WIFI CONNECTION + LED STRIP TEST
       - Onboard LED: Breathing light blue while connecting
       - LED Strip: RGB scan (R, then G, then B), then white breathing
       ======================================================================== */
    ESP_LOGI(TAG, ">>> STEP 2: Connecting to WiFi + Testing LED strip...");
    wifi_init_start();

    /* Light blue color for WiFi connecting (soft sky blue) - max 50% brightness = 127 */
    const uint8_t wifi_max_r = 50;
    const uint8_t wifi_max_g = 90;
    const uint8_t wifi_max_b = 127;
    
    /* Test brightness (50%) */
    const uint8_t test_brightness = 127;

    /* Breathing animation while waiting */
    float breath_t = 0.0f;
    const float breath_speed = 0.02f;
    int wifi_status = 0;
    bool breath_rising = true;
    
    /* LED strip test state */
    int test_pixel = 0;
    int test_frame_count = 0;
    const int frames_per_batch = 2;    /* How many frames per batch */
    const int batch_size = 3;          /* Sweep 3 LEDs at a time */
    int test_color_phase = 0;          /* 0=Red, 1=Green, 2=Blue, 3=Full white, 4=Complete */
    bool led_test_complete = false;
    
    /* Buzzer test state - clean sweep (4s up, 1s down, no hold) */
    bool buzzer_test_complete = false;
    int buzzer_frame_count = 0;
    int buzzer_phase = 0;              /* 0=sweep up, 1=sweep down */

    ESP_LOGI(TAG, "    Onboard: Breathing light blue while connecting to '%s'", WIFI_SSID);
    ESP_LOGI(TAG, "    Strip:   RGB scan (batches of %d) → 5s full white test", batch_size);
    ESP_LOGI(TAG, "    Buzzer:  Frequency sweep 200Hz → 4000Hz");
    ESP_LOGI(TAG, "    All THREE sequences must complete before entering main loop.");

    /* Loop until ALL THREE are complete: WiFi + LED test + Buzzer test */
    while (!led_test_complete || !buzzer_test_complete || wifi_status == 0) {
        /* === ONBOARD LED: Breathing light blue === */
        float eased = ease_in_out(breath_t);
        uint8_t r = (uint8_t)(wifi_max_r * eased);
        uint8_t g = (uint8_t)(wifi_max_g * eased);
        uint8_t b = (uint8_t)(wifi_max_b * eased);
        current_r = r; current_g = g; current_b = b;
        set_onboard_led_rgb_internal(r, g, b);
        
        /* Advance breathing phase */
        if (breath_rising) {
            breath_t += breath_speed;
            if (breath_t >= 1.0f) { breath_t = 1.0f; breath_rising = false; }
        } else {
            breath_t -= breath_speed;
            if (breath_t <= 0.0f) { breath_t = 0.0f; breath_rising = true; }
        }
        
        /* === LED STRIP TEST (runs independently of WiFi) === */
        if (rgbw_strip != NULL && !led_test_complete) {
            if (test_color_phase < 3) {
                /* Phase 0-2: RGB sweep in batches of 3 */
                /* Clear all pixels */
                for (int i = 0; i < RGBW_LED_COUNT; i++) {
                    set_pixel_rgbw(i, 0, 0, 0, 0);
                }
                
                /* Light up batch of pixels in current color */
                for (int b = 0; b < batch_size; b++) {
                    int pixel = test_pixel + b;
                    if (pixel < RGBW_LED_COUNT) {
                        if (test_color_phase == 0) {
                            set_pixel_rgbw(pixel, test_brightness, 0, 0, 0);
                        } else if (test_color_phase == 1) {
                            set_pixel_rgbw(pixel, 0, test_brightness, 0, 0);
                        } else {
                            set_pixel_rgbw(pixel, 0, 0, test_brightness, 0);
                        }
                    }
                }
                refresh_strip();
                
                /* Advance to next batch */
                test_frame_count++;
                if (test_frame_count >= frames_per_batch) {
                    test_frame_count = 0;
                    test_pixel += batch_size;
                    if (test_pixel >= RGBW_LED_COUNT) {
                        test_pixel = 0;
                        test_color_phase++;
                        if (test_color_phase == 1) {
                            ESP_LOGI(TAG, "    Strip:   Green scan...");
                        } else if (test_color_phase == 2) {
                            ESP_LOGI(TAG, "    Strip:   Blue scan...");
                        } else if (test_color_phase == 3) {
                            ESP_LOGI(TAG, "    Strip:   RGB complete! Starting 5s white test...");
                        }
                    }
                }
            } else if (test_color_phase == 3) {
                /* Phase 3a: Gradual ramp-up from 0 to full white (4 seconds) */
                ESP_LOGI(TAG, "    Strip:   Ramping up WHITE (W channel) 0%% -> 100%% over 4 seconds...");
                const int ramp_duration_ms = 4000;  /* 4 seconds ramp */
                const int ramp_steps = 200;         /* More steps for smoother 4s ramp */
                const int step_delay_ms = ramp_duration_ms / ramp_steps;
                
                for (int step = 0; step <= ramp_steps; step++) {
                    uint8_t brightness = (uint8_t)((step * 255) / ramp_steps);
                    for (int i = 0; i < RGBW_LED_COUNT; i++) {
                        set_pixel_rgbw(i, 0, 0, 0, brightness);
                    }
                    refresh_strip();
                    vTaskDelay(step_delay_ms / portTICK_PERIOD_MS);
                }
                
                /* Phase 3b: Hold at full white for 1 second */
                ESP_LOGI(TAG, "    Strip:   Holding 100%% WHITE for 1 second...");
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                
                /* Phase 3c: ACCELERATING STROBE TEST for 3.5 seconds */
                ESP_LOGI(TAG, "    Strip:   STROBE TEST - accelerating from 0.5s to max speed...");
                int64_t strobe_start = esp_timer_get_time();
                int64_t strobe_duration = 3500000;  /* 3.5 seconds in microseconds */
                int strobe_count = 0;
                bool strobe_on = true;
                
                /* Strobe timing: start at 500ms interval, end at ~1ms (as fast as possible) */
                const int start_delay_ms = 500;  /* Start: 0.5 second between toggles */
                const int end_delay_ms = 1;      /* End: as fast as possible */
                
                while ((esp_timer_get_time() - strobe_start) < strobe_duration) {
                    int64_t elapsed = esp_timer_get_time() - strobe_start;
                    float progress = (float)elapsed / (float)strobe_duration;  /* 0.0 to 1.0 */
                    
                    /* Use exponential curve for more dramatic acceleration at the end */
                    /* progress^3 makes it stay slow longer then ramp up quickly */
                    float curve = progress * progress * progress;
                    
                    /* Interpolate delay: start_delay -> end_delay */
                    int current_delay_ms = start_delay_ms - (int)(curve * (start_delay_ms - end_delay_ms));
                    if (current_delay_ms < end_delay_ms) current_delay_ms = end_delay_ms;
                    
                    if (strobe_on) {
                        /* All LEDs ON (full white) */
                        for (int i = 0; i < RGBW_LED_COUNT; i++) {
                            set_pixel_rgbw(i, 0, 0, 0, 255);
                        }
                    } else {
                        /* All LEDs OFF */
                        for (int i = 0; i < RGBW_LED_COUNT; i++) {
                            set_pixel_rgbw(i, 0, 0, 0, 0);
                        }
                    }
                    refresh_strip();
                    strobe_on = !strobe_on;
                    strobe_count++;
                    
                    /* Delay with current interval (gets shorter over time) */
                    vTaskDelay(current_delay_ms / portTICK_PERIOD_MS);
                }
                
                ESP_LOGI(TAG, "    Strip:   Strobe complete! %d toggles in 3.5 seconds", strobe_count);
                
                /* Drop to 10% white */
                ESP_LOGI(TAG, "    Strip:   LED test COMPLETE. Holding 10%% white.");
                uint8_t idle_white = 25;
                for (int i = 0; i < RGBW_LED_COUNT; i++) {
                    set_pixel_rgbw(i, 0, 0, 0, idle_white);
                }
                refresh_strip();
                
                test_color_phase = 4;
                led_test_complete = true;
                ESP_LOGI(TAG, "    >>> LED TEST FINISHED");
            }
        } else if (rgbw_strip == NULL) {
            led_test_complete = true;  /* No strip, skip test */
        }
        
        /* === BUZZER TEST: Clean sweep up then sweep down (no hold) === */
        if (!buzzer_test_complete && buzzer_initialized) {
            /* 
             * Clean sweep: 4 seconds UP (200Hz → 4000Hz), 1 second DOWN (4000Hz → 200Hz)
             * Using non-blocking buzzer_set_freq for smooth transitions
             */
            const int STARTUP_SWEEP_UP_FRAMES = 4 * 60;    /* 4 seconds at 60 FPS */
            const int STARTUP_SWEEP_DOWN_FRAMES = 1 * 60;  /* 1 second at 60 FPS */
            const int STARTUP_MIN_HZ = 200;
            const int STARTUP_MAX_HZ = 4000;
            
            if (buzzer_phase == 0) {
                /* Phase 0: Sweep UP (200Hz → 4000Hz) over 4 seconds */
                float progress = (float)buzzer_frame_count / (float)STARTUP_SWEEP_UP_FRAMES;
                int freq = STARTUP_MIN_HZ + (int)(progress * (STARTUP_MAX_HZ - STARTUP_MIN_HZ));
                buzzer_set_freq(freq);
                buzzer_frame_count++;
                
                if (buzzer_frame_count >= STARTUP_SWEEP_UP_FRAMES) {
                    buzzer_phase = 1;  /* Skip to sweep down (no hold) */
                    buzzer_frame_count = 0;
                    ESP_LOGI(TAG, "    Buzzer: Sweeping down...");
                }
            } else if (buzzer_phase == 1) {
                /* Phase 1: Sweep DOWN (4000Hz → 200Hz) over 1 second (4x faster) */
                float progress = (float)buzzer_frame_count / (float)STARTUP_SWEEP_DOWN_FRAMES;
                int freq = STARTUP_MAX_HZ - (int)(progress * (STARTUP_MAX_HZ - STARTUP_MIN_HZ));
                buzzer_set_freq(freq);
                buzzer_frame_count++;
                
                if (buzzer_frame_count >= STARTUP_SWEEP_DOWN_FRAMES) {
                    buzzer_stop();
                    buzzer_test_complete = true;
                    ESP_LOGI(TAG, "    >>> BUZZER TEST FINISHED");
                }
            }
        } else if (!buzzer_initialized) {
            buzzer_test_complete = true;  /* No buzzer, skip test */
        }
        
        /* Check WiFi status */
        if (wifi_status == 0) {
            wifi_status = wifi_check_status();
            if (wifi_status == 1) {
                ESP_LOGI(TAG, "    >>> WIFI CONNECTED");
            }
        }
        
        vTaskDelay(16 / portTICK_PERIOD_MS);  /* 60 FPS */
    }
    
    ESP_LOGI(TAG, ">>> All THREE sequences complete! Proceeding to main loop...");
    
    /* ========================================================================
       END OF STARTUP BOOT SEQUENCE
       ======================================================================== */
    
    /* Clear the LED strip after test */
    if (rgbw_strip != NULL) {
        for (int i = 0; i < RGBW_LED_COUNT; i++) {
            set_pixel_rgbw(i, 0, 0, 0, 0);
        }
        refresh_strip();
    }
    ESP_LOGI(TAG, ">>> STARTUP SEQUENCE COMPLETE");

    /* Show result with smooth fade */
    if (wifi_status == 1) {
        ESP_LOGI(TAG, ">>> WiFi CONNECTED! Fading to solid blue...");
        buzzer_chime_up();  /* Success chime! */
        fade_to_color(0, 0, 255, 800);  /* Fade to 100% blue */
        
        /* Start MQTT connection to Adafruit IO */
        ESP_LOGI(TAG, ">>> STEP 3: Starting MQTT connection...");
        mqtt_init();
        
        /* Start Zigbee coordinator for blind control */
        ESP_LOGI(TAG, ">>> STEP 4: Starting Zigbee Hub...");
        esp_err_t zb_err = zigbee_hub_init();
        if (zb_err == ESP_OK) {
            ESP_LOGI(TAG, ">>> Zigbee Hub started successfully!");
            
            /* ================================================================
               ZIGBEE FINDER MODE
               ================================================================
               Wait for Zigbee to either:
               1. Reconnect to previously paired devices, OR
               2. Complete finder mode (1 minute or until device pairs)
               
               During this time:
               - LED strip: stays at boot state (10% warm white)
               - Onboard LED: smooth sweeping green
               - Buzzer: 4s sweep up + 1s sweep down
               ================================================================ */
            
            ESP_LOGI(TAG, ">>> STEP 5: Waiting for Zigbee finder mode...");
            ESP_LOGI(TAG, "    (Searching for Zigbee devices for up to 60 seconds)");
            
            float finder_pulse_phase = 0.0f;
            int finder_wait_frames = 0;
            const int finder_max_frames = 60 * 60;  /* 60 seconds at 60 FPS */
            
            /* ================================================================
               BUZZER SWEEP DURING FINDER MODE
               ================================================================
               - 4 seconds: Slow sweep up from 200Hz to 4000Hz
               - 1 second: Fast sweep down from 4000Hz to 200Hz (4x faster)
               - Then silence
               Total: 5 seconds of buzzer, then quiet
               ================================================================ */
            const int SWEEP_UP_FRAMES = 4 * 60;     /* 4 seconds at 60 FPS = 240 frames */
            const int SWEEP_DOWN_FRAMES = 1 * 60;   /* 1 second at 60 FPS = 60 frames */
            const int SWEEP_MIN_HZ = 200;
            const int SWEEP_MAX_HZ = 4000;
            int buzzer_frame = 0;
            bool buzzer_done = false;
            
            while (!zigbee_is_finder_complete() && finder_wait_frames < finder_max_frames) {
                /* LED strip stays at boot state (10% warm white) - no changes needed */
                
                /* Onboard LED: Smooth sweeping green during finder mode */
                finder_pulse_phase += 0.05f;
                float brightness = (sinf(finder_pulse_phase) + 1.0f) * 0.5f;  /* 0 to 1 */
                uint8_t green_val = (uint8_t)(brightness * 255.0f);  /* Full brightness sweep */
                set_onboard_led_rgb(0, green_val, 0);
                
                /* Buzzer sweep logic (non-blocking) */
                if (!buzzer_done && buzzer_initialized) {
                    if (buzzer_frame < SWEEP_UP_FRAMES) {
                        /* Sweep UP: 200Hz → 4000Hz over 4 seconds */
                        float progress = (float)buzzer_frame / (float)SWEEP_UP_FRAMES;
                        int freq = SWEEP_MIN_HZ + (int)(progress * (SWEEP_MAX_HZ - SWEEP_MIN_HZ));
                        buzzer_set_freq(freq);  /* Non-blocking frequency update */
                    } else if (buzzer_frame < SWEEP_UP_FRAMES + SWEEP_DOWN_FRAMES) {
                        /* Sweep DOWN: 4000Hz → 200Hz over 1 second (4x faster) */
                        int down_frame = buzzer_frame - SWEEP_UP_FRAMES;
                        float progress = (float)down_frame / (float)SWEEP_DOWN_FRAMES;
                        int freq = SWEEP_MAX_HZ - (int)(progress * (SWEEP_MAX_HZ - SWEEP_MIN_HZ));
                        buzzer_set_freq(freq);  /* Non-blocking frequency update */
                    } else {
                        /* Done - stop buzzer */
                        buzzer_stop();
                        buzzer_done = true;
                        ESP_LOGI(TAG, "    Buzzer sweep complete");
                    }
                    buzzer_frame++;
                }
                
                finder_wait_frames++;
                vTaskDelay(16 / portTICK_PERIOD_MS);  /* 60 FPS */
            }
            
            /* Make sure buzzer is off when exiting finder mode */
            if (buzzer_initialized && !buzzer_done) {
                buzzer_stop();
            }
            
            /* Finder mode complete - show result */
            if (zigbee_get_device_count() > 0) {
                ESP_LOGI(TAG, ">>> Zigbee: %d device(s) found/connected!", zigbee_get_device_count());
                /* Flash green briefly to indicate success */
                fade_to_color(0, 255, 0, 300);
                vTaskDelay(500 / portTICK_PERIOD_MS);
            } else {
                ESP_LOGW(TAG, ">>> Zigbee: No devices found. Use 'blinds:pair' to pair later.");
            }
            
        } else {
            ESP_LOGE(TAG, ">>> Zigbee Hub failed to start: %s", esp_err_to_name(zb_err));
        }
    } else {
        ESP_LOGE(TAG, ">>> WiFi FAILED! Fading to blinking red...");
        ESP_LOGI(TAG, ">>> Press BOOT button to enter standby mode");
        buzzer_error();  /* Error beeps! */
        fade_to_color(255, 0, 0, 500);  /* Fade to 100% red */
        
        /* Blink red on/off - but check for power button to allow standby */
        while (1) {
            /* Check for power button press */
            if (is_power_button_pressed()) {
                vTaskDelay(50 / portTICK_PERIOD_MS);  /* Debounce */
                if (is_power_button_pressed()) {
                    ESP_LOGI(TAG, "Power button pressed, entering standby...");
                    buzzer_chime_down();  /* Shutdown melody */
                    while (is_power_button_pressed()) {
                        vTaskDelay(50 / portTICK_PERIOD_MS);
                    }
                    vTaskDelay(100 / portTICK_PERIOD_MS);  /* Debounce release */
                    enter_standby_mode();  /* Enter standby */
                }
            }
            
            fade_to_color(0, 0, 0, 400);
            vTaskDelay(200 / portTICK_PERIOD_MS);
            fade_to_color(255, 0, 0, 400);
            vTaskDelay(200 / portTICK_PERIOD_MS);
        }
    }

    /* ========================================================================
       MAIN ANIMATION LOOP
       ========================================================================
       This is the main runtime loop after WiFi is connected.
       Runs the animation framework at 30 FPS.
       Animations can be switched via MQTT voice commands.
       ======================================================================== */
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, ">>> ENTERING MAIN ANIMATION LOOP");
    
    /* Animation state */
    float head_position = 0.0f;      /* For meteor animation */
    float rainbow_phase = 0.0f;      /* For rainbow animation */
    float breathing_phase = 0.0f;    /* For breathing animation */
    float wave_phase = 0.0f;         /* For wave animation */
    float fusion_phase = 0.0f;       /* For fusion animation */
    float onboard_rainbow = 0.0f;    /* For slow rainbow on onboard LED */
    
    /* Global frame rate (default 60 FPS) */
    const int global_delay_ms = 16;  /* 16ms = ~60 FPS */
    
    /* Local FPS overrides per animation (0 = use global) */
    #define ANIM_FPS_GLOBAL     0     /* Use global 60 FPS */
    #define ANIM_FPS_45         22    /* 22ms = ~45 FPS */
    #define ANIM_FPS_30         33    /* 33ms = ~30 FPS */
    
    /* Get frame delay for current animation (local override or global default) */
    #define GET_ANIM_DELAY(mode) ( \
        (mode) == ANIM_STARS ? ANIM_FPS_45 : \
        global_delay_ms \
    )
    
    /* Cycle mode state (switches between fusion, wave, tetris every 20 seconds) */
    int cycle_timer_ms = 0;
    const int cycle_interval_ms = 20000;  /* 20 seconds */
    int cycle_anim_index = 0;             /* 0=fusion, 1=wave, 2=tetris */
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, ">>> STEP 5: Starting animation loop...");
    ESP_LOGI(TAG, "    - %d pixels in ring", RGBW_LED_COUNT);
    ESP_LOGI(TAG, "    - Brightness: controlled by potentiometer on GPIO%d (5%% to 100%%)", POT_GPIO);
    ESP_LOGI(TAG, "    - Lifetime rotations: %lu", (unsigned long)lifetime_rotations);
    ESP_LOGI(TAG, "    - MQTT: Listening for voice commands on '%s'", MQTT_TOPIC);
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "    Voice commands available:");
    ESP_LOGI(TAG, "      cycle, fusion, wave, tetris, stars, meteor, shower, rainbow, breathing, solid, off, on");
    ESP_LOGI(TAG, "      slow, medium, fast");
    ESP_LOGI(TAG, "      red, green, blue, purple, white, warm");
    ESP_LOGI(TAG, "      color:RRGGBB (hex)");
    ESP_LOGI(TAG, "");

    while (1) {
        /* Update brightness from potentiometer (smoothed reading) */
        read_potentiometer();
        
        /* Log system metrics every 30 seconds */
        log_system_metrics();
        
        /* Check if pot is being adjusted - show brightness gauge instead of animation */
        if (is_pot_adjusting()) {
            draw_brightness_gauge();
            
            /* Still update onboard LED rainbow */
            float hue = onboard_rainbow;
            float c = 1.0f;
            float x = 1.0f - fabsf(fmodf(hue / 60.0f, 2.0f) - 1.0f);
            float r1, g1, b1;
            if (hue < 60)       { r1 = c; g1 = x; b1 = 0; }
            else if (hue < 120) { r1 = x; g1 = c; b1 = 0; }
            else if (hue < 180) { r1 = 0; g1 = c; b1 = x; }
            else if (hue < 240) { r1 = 0; g1 = x; b1 = c; }
            else if (hue < 300) { r1 = x; g1 = 0; b1 = c; }
            else                { r1 = c; g1 = 0; b1 = x; }
            set_onboard_led_rgb_internal((uint8_t)(r1 * 80), (uint8_t)(g1 * 80), (uint8_t)(b1 * 80));
            onboard_rainbow += 0.4f;
            if (onboard_rainbow >= 360.0f) onboard_rainbow -= 360.0f;
            
            /* Check power button even during gauge display */
            if (is_power_button_pressed()) {
                vTaskDelay(50 / portTICK_PERIOD_MS);
                if (is_power_button_pressed()) {
                    ESP_LOGI(TAG, "Power button pressed, entering standby...");
                    buzzer_chime_down();
                    while (is_power_button_pressed()) {
                        vTaskDelay(50 / portTICK_PERIOD_MS);
                    }
                    vTaskDelay(100 / portTICK_PERIOD_MS);
                    enter_standby_mode();
                }
            }
            
            /* Check melody button during gauge display too (cancels and restarts if playing) */
            if (is_melody_button_pressed()) {
                vTaskDelay(50 / portTICK_PERIOD_MS);
                if (is_melody_button_pressed()) {
                    while (is_melody_button_pressed()) {
                        vTaskDelay(50 / portTICK_PERIOD_MS);
                    }
                    vTaskDelay(50 / portTICK_PERIOD_MS);
                    buzzer_play_random_song();
                }
            }
            
            vTaskDelay(global_delay_ms / portTICK_PERIOD_MS);
            continue;  /* Skip normal animation this frame */
        }
        
        /* Get current animation mode (may change from MQTT at any time) */
        animation_mode_t mode = current_animation;
        float speed = animation_speed;
        
        /* Get frame delay for current animation (may have local FPS override) */
        int frame_delay = GET_ANIM_DELAY(mode);
        
        switch (mode) {
            case ANIM_CYCLE:
                /* Auto-cycle between fusion, wave, tetris, stars every 20 seconds */
                cycle_timer_ms += frame_delay;
                if (cycle_timer_ms >= cycle_interval_ms) {
                    cycle_timer_ms = 0;
                    cycle_anim_index = (cycle_anim_index + 1) % 4;
                    const char* anim_names[] = {"FUSION", "WAVE", "TETRIS", "STARS"};
                    ESP_LOGI(TAG, "Cycle: switching to %s", anim_names[cycle_anim_index]);
                }
                
                /* Track previous index for reset logic */
                static int last_cycle_index = -1;
                bool anim_changed = (last_cycle_index != cycle_anim_index);
                
                if (cycle_anim_index == 0) {
                    draw_fusion(fusion_phase);
                    fusion_phase += speed * 0.12f;
                    if (fusion_phase >= 2 * 3.14159f) fusion_phase -= 2 * 3.14159f;
                } else if (cycle_anim_index == 1) {
                    draw_wave(wave_phase);
                    wave_phase += speed * 0.15f;
                    if (wave_phase >= 2 * 3.14159f) wave_phase -= 2 * 3.14159f;
                } else if (cycle_anim_index == 2) {
                    draw_tetris(0, anim_changed && last_cycle_index != 2);
                } else {
                    draw_stars(anim_changed && last_cycle_index != 3);
                }
                last_cycle_index = cycle_anim_index;
                break;
                
            case ANIM_FUSION:
                draw_fusion(fusion_phase);
                fusion_phase += speed * 0.12f;  /* Slower pulse for gradual animation */
                if (fusion_phase >= 2 * 3.14159f) fusion_phase -= 2 * 3.14159f;
                break;
            
            case ANIM_WAVE:
                draw_wave(wave_phase);
                wave_phase += speed * 0.15f;  /* Slower wave for longer fade-in */
                if (wave_phase >= 2 * 3.14159f) wave_phase -= 2 * 3.14159f;
                break;
            
            case ANIM_TETRIS:
                {
                    static bool tetris_first_frame = true;
                    draw_tetris(0, tetris_first_frame);
                    tetris_first_frame = false;
                }
                break;
            
            case ANIM_STARS:
                {
                    static bool stars_first_frame = true;
                    draw_stars(stars_first_frame);
                    stars_first_frame = false;
                }
                break;
                
            case ANIM_METEOR:
                draw_meteor_spinner(head_position);
                head_position += speed;
                if (head_position >= RGBW_LED_COUNT) {
                    head_position -= RGBW_LED_COUNT;
                    increment_rotation_count();
                }
                break;
            
            case ANIM_METEOR_SHOWER:
                {
                    static bool meteor_shower_first_frame = true;
                    draw_meteor_shower(meteor_shower_first_frame);
                    meteor_shower_first_frame = false;
                }
                break;
                
            case ANIM_RAINBOW:
                draw_rainbow(rainbow_phase);
                rainbow_phase += speed * 5.0f;  /* Faster for rainbow */
                if (rainbow_phase >= 360.0f) rainbow_phase -= 360.0f;
                break;
                
            case ANIM_BREATHING:
                draw_breathing(breathing_phase);
                breathing_phase += speed * 0.5f;  /* Breathing speed */
                if (breathing_phase >= 2 * 3.14159f) breathing_phase -= 2 * 3.14159f;
                break;
                
            case ANIM_SOLID:
                draw_solid();
                break;
                
            case ANIM_OFF:
                draw_off();
                break;
        }
        
        /* === ONBOARD LED: Slow rainbow cycle === */
        /* Convert hue (0-360) to RGB */
        float hue = onboard_rainbow;
        float c = 1.0f;
        float x = 1.0f - fabsf(fmodf(hue / 60.0f, 2.0f) - 1.0f);
        float r1, g1, b1;
        if (hue < 60)       { r1 = c; g1 = x; b1 = 0; }
        else if (hue < 120) { r1 = x; g1 = c; b1 = 0; }
        else if (hue < 180) { r1 = 0; g1 = c; b1 = x; }
        else if (hue < 240) { r1 = 0; g1 = x; b1 = c; }
        else if (hue < 300) { r1 = x; g1 = 0; b1 = c; }
        else                { r1 = c; g1 = 0; b1 = x; }
        
        /* Apply to onboard LED (at reduced brightness for subtlety) */
        uint8_t ob_r = (uint8_t)(r1 * 180);
        uint8_t ob_g = (uint8_t)(g1 * 180);
        uint8_t ob_b = (uint8_t)(b1 * 180);
        set_onboard_led_rgb_internal(ob_r, ob_g, ob_b);
        current_r = ob_r; current_g = ob_g; current_b = ob_b;
        
        /* Very slow rainbow cycle - full cycle in ~30 seconds */
        onboard_rainbow += 0.4f;  /* ~12 degrees/second at 30fps */
        if (onboard_rainbow >= 360.0f) onboard_rainbow -= 360.0f;
        
        /* === CHECK POWER BUTTON (BOOT button - GPIO9) === */
        if (is_power_button_pressed()) {
            /* Debounce: wait for release and confirm it was intentional */
            vTaskDelay(50 / portTICK_PERIOD_MS);
            if (is_power_button_pressed()) {
                /* Wait for button release */
                ESP_LOGI(TAG, "Power button pressed, entering standby...");
                buzzer_chime_down();  /* Shutdown melody */
                while (is_power_button_pressed()) {
                    vTaskDelay(50 / portTICK_PERIOD_MS);
                }
                vTaskDelay(100 / portTICK_PERIOD_MS);  /* Debounce release */
                
                /* Enter standby mode (this doesn't return - will restart) */
                enter_standby_mode();
            }
        }
        
        /* === CHECK MELODY BUTTON (External button - GPIO5) === */
        /* Now allows triggering even during playback (cancels and starts new song) */
        if (is_melody_button_pressed()) {
            /* Debounce */
            vTaskDelay(50 / portTICK_PERIOD_MS);
            if (is_melody_button_pressed()) {
                /* Wait for button release before playing */
                while (is_melody_button_pressed()) {
                    vTaskDelay(50 / portTICK_PERIOD_MS);
                }
                vTaskDelay(50 / portTICK_PERIOD_MS);  /* Debounce release */
                
                /* Play a random song (cancels current if playing) */
                buzzer_play_random_song();
            }
        }
        
        /* Wait before next frame (uses animation-specific FPS if set) */
        vTaskDelay(frame_delay / portTICK_PERIOD_MS);
    }
}
