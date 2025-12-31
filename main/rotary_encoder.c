/*
 * SPDX-FileCopyrightText: 2024 Halo Project
 * SPDX-License-Identifier: CC0-1.0
 *
 * Rotary Encoder Driver - Quadrature decoding with push button
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "rotary_encoder.h"

static const char *TAG = "encoder";

/* ============================================================================
   STATE VARIABLES
   ============================================================================ */

/* Rotation tracking */
static volatile int s_position = 0;         /* Accumulated position */
static volatile int s_delta = 0;            /* Delta since last read */
static volatile uint8_t s_last_state = 0;   /* Last AB state for decoding */

/* Button tracking */
static volatile bool s_button_pressed = false;
static volatile bool s_button_edge = false;         /* Press edge detected */
static volatile bool s_long_press_fired = false;    /* Long press event sent */
static volatile int64_t s_button_press_time = 0;    /* Time of button press (us) */

/* Debounce */
static volatile int64_t s_last_rotation_time = 0;
#define DEBOUNCE_US         2000    /* 2ms debounce for rotation */
#define BUTTON_DEBOUNCE_US  20000   /* 20ms debounce for button */
#define LONG_PRESS_US       1000000 /* 1 second for long press */

/* Event queue */
static QueueHandle_t s_event_queue = NULL;
#define EVENT_QUEUE_SIZE    16

/* ============================================================================
   QUADRATURE DECODING
   ============================================================================
   Standard quadrature decoding using state machine.
   
   State transition table:
   old_AB | new_AB | direction
   -------|--------|----------
     00   |   01   |   CW
     01   |   11   |   CW
     11   |   10   |   CW
     10   |   00   |   CW
     00   |   10   |   CCW
     10   |   11   |   CCW
     11   |   01   |   CCW
     01   |   00   |   CCW
   ============================================================================ */

/* State machine lookup table: [old_state][new_state] = direction (1=CW, -1=CCW, 0=invalid) */
static const int8_t ENCODER_STATE_TABLE[4][4] = {
    /* new:  00   01   10   11  */
    /* 00 */ { 0,  1, -1,  0 },
    /* 01 */ {-1,  0,  0,  1 },
    /* 10 */ { 1,  0,  0, -1 },
    /* 11 */ { 0, -1,  1,  0 },
};

/* ============================================================================
   INTERRUPT HANDLERS
   ============================================================================ */

static void IRAM_ATTR encoder_isr_handler(void *arg)
{
    int64_t now = esp_timer_get_time();
    
    /* Debounce */
    if ((now - s_last_rotation_time) < DEBOUNCE_US) {
        return;
    }
    s_last_rotation_time = now;
    
    /* Read current state */
    uint8_t a = gpio_get_level(ENCODER_GPIO_A);
    uint8_t b = gpio_get_level(ENCODER_GPIO_B);
    uint8_t new_state = (a << 1) | b;
    
    /* Decode direction using state table */
    int8_t direction = ENCODER_STATE_TABLE[s_last_state][new_state];
    
    if (direction != 0) {
        s_position += direction;
        s_delta += direction;
        
        /* Queue event */
        if (s_event_queue != NULL) {
            encoder_event_t event = (direction > 0) ? ENCODER_EVENT_CW : ENCODER_EVENT_CCW;
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xQueueSendFromISR(s_event_queue, &event, &xHigherPriorityTaskWoken);
            if (xHigherPriorityTaskWoken) {
                portYIELD_FROM_ISR();
            }
        }
    }
    
    s_last_state = new_state;
}

/* Double-tap detection threshold (microseconds) */
#define DOUBLE_TAP_WINDOW_US    400000  /* 400ms window for double-tap */

static void IRAM_ATTR button_isr_handler(void *arg)
{
    static int64_t last_button_time = 0;
    static int64_t last_press_time = 0;      /* Time of previous press (for double-tap) */
    static bool waiting_for_double = false;  /* Waiting to see if this is a double-tap */
    int64_t now = esp_timer_get_time();
    
    /* Debounce */
    if ((now - last_button_time) < BUTTON_DEBOUNCE_US) {
        return;
    }
    last_button_time = now;
    
    /* Button is active LOW (pulled up, connected to GND when pressed) */
    bool pressed = (gpio_get_level(ENCODER_GPIO_SW) == 0);
    
    if (pressed && !s_button_pressed) {
        /* Button just pressed */
        s_button_pressed = true;
        s_button_edge = true;
        s_button_press_time = now;
        s_long_press_fired = false;
        
        /* Check for double-tap: was there a recent press? */
        if (waiting_for_double && (now - last_press_time) < DOUBLE_TAP_WINDOW_US) {
            /* Double-tap detected! */
            waiting_for_double = false;
            if (s_event_queue != NULL) {
                encoder_event_t event = ENCODER_EVENT_DOUBLE_TAP;
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                xQueueSendFromISR(s_event_queue, &event, &xHigherPriorityTaskWoken);
                if (xHigherPriorityTaskWoken) {
                    portYIELD_FROM_ISR();
                }
            }
        } else {
            /* First tap - queue press event and wait for possible second tap */
            waiting_for_double = true;
            last_press_time = now;
            
            if (s_event_queue != NULL) {
                encoder_event_t event = ENCODER_EVENT_PRESS;
                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                xQueueSendFromISR(s_event_queue, &event, &xHigherPriorityTaskWoken);
                if (xHigherPriorityTaskWoken) {
                    portYIELD_FROM_ISR();
                }
            }
        }
    } else if (!pressed && s_button_pressed) {
        /* Button just released */
        s_button_pressed = false;
        
        /* Queue release event */
        if (s_event_queue != NULL) {
            encoder_event_t event = ENCODER_EVENT_RELEASE;
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xQueueSendFromISR(s_event_queue, &event, &xHigherPriorityTaskWoken);
            if (xHigherPriorityTaskWoken) {
                portYIELD_FROM_ISR();
            }
        }
    }
}

/* ============================================================================
   INITIALIZATION
   ============================================================================ */

esp_err_t encoder_init(void)
{
    ESP_LOGI(TAG, "Initializing rotary encoder...");
    ESP_LOGI(TAG, "  CLK (A): GPIO%d", ENCODER_GPIO_A);
    ESP_LOGI(TAG, "  DT (B):  GPIO%d", ENCODER_GPIO_B);
    ESP_LOGI(TAG, "  SW:      GPIO%d", ENCODER_GPIO_SW);
    
    /* Create event queue */
    s_event_queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(encoder_event_t));
    if (s_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }
    
    /* Configure encoder pins (A and B) with pull-up */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << ENCODER_GPIO_A) | (1ULL << ENCODER_GPIO_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,  /* Interrupt on any edge */
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure encoder GPIOs: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Configure button pin with pull-up */
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << ENCODER_GPIO_SW),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ret = gpio_config(&btn_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure button GPIO: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Read initial state */
    uint8_t a = gpio_get_level(ENCODER_GPIO_A);
    uint8_t b = gpio_get_level(ENCODER_GPIO_B);
    s_last_state = (a << 1) | b;
    
    /* Install GPIO ISR service */
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE means ISR service already installed, which is OK */
        ESP_LOGE(TAG, "Failed to install ISR service: %s", esp_err_to_name(ret));
        return ret;
    }
    
    /* Add ISR handlers */
    ret = gpio_isr_handler_add(ENCODER_GPIO_A, encoder_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR for GPIO A: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = gpio_isr_handler_add(ENCODER_GPIO_B, encoder_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR for GPIO B: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = gpio_isr_handler_add(ENCODER_GPIO_SW, button_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR for button: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Rotary encoder initialized successfully");
    return ESP_OK;
}

/* ============================================================================
   STATE QUERIES
   ============================================================================ */

int encoder_get_delta(void)
{
    int delta = s_delta;
    s_delta = 0;  /* Reset after reading */
    return delta;
}

int encoder_get_position(void)
{
    return s_position;
}

void encoder_reset_position(void)
{
    s_position = 0;
    s_delta = 0;
}

bool encoder_is_button_pressed(void)
{
    return s_button_pressed;
}

bool encoder_was_button_pressed(void)
{
    if (s_button_edge) {
        s_button_edge = false;
        return true;
    }
    return false;
}

bool encoder_was_long_press(void)
{
    /* Check if button is held and long press threshold exceeded */
    if (s_button_pressed && !s_long_press_fired) {
        int64_t now = esp_timer_get_time();
        if ((now - s_button_press_time) >= LONG_PRESS_US) {
            s_long_press_fired = true;
            
            /* Queue long press event */
            if (s_event_queue != NULL) {
                encoder_event_t event = ENCODER_EVENT_LONG_PRESS;
                xQueueSend(s_event_queue, &event, 0);
            }
            
            return true;
        }
    }
    return false;
}

/* ============================================================================
   EVENT POLLING
   ============================================================================ */

encoder_event_t encoder_poll_event(void)
{
    /* First check for long press (time-based, not interrupt-driven) */
    encoder_was_long_press();
    
    /* Then check queue for interrupt-driven events */
    encoder_event_t event = ENCODER_EVENT_NONE;
    if (s_event_queue != NULL) {
        if (xQueueReceive(s_event_queue, &event, 0) == pdTRUE) {
            return event;
        }
    }
    
    return ENCODER_EVENT_NONE;
}

