/*
 * SPDX-FileCopyrightText: 2025 Halo Project
 * SPDX-License-Identifier: CC0-1.0
 *
 * Rotary Encoder Driver - Quadrature decoding with push button
 * 
 * Hardware:
 *   CLK (A)  → GPIO 19
 *   DT (B)   → GPIO 21
 *   SW       → GPIO 22
 *   +        → 3.3V
 *   GND      → GND
 */

#ifndef ROTARY_ENCODER_H
#define ROTARY_ENCODER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ============================================================================
   GPIO CONFIGURATION (from README.md - source of truth)
   ============================================================================ */

#define ENCODER_GPIO_A      19      /* CLK pin */
#define ENCODER_GPIO_B      21      /* DT pin */
#define ENCODER_GPIO_SW     22      /* Switch/button pin (GPIO18 used for MOSFET) */

/* ============================================================================
   ENCODER EVENTS
   ============================================================================ */

typedef enum {
    ENCODER_EVENT_NONE = 0,
    ENCODER_EVENT_CW,           /* Clockwise rotation (increment) */
    ENCODER_EVENT_CCW,          /* Counter-clockwise rotation (decrement) */
    ENCODER_EVENT_PRESS,        /* Button pressed (short press) */
    ENCODER_EVENT_DOUBLE_TAP,   /* Button double-tapped (two presses within 400ms) */
    ENCODER_EVENT_LONG_PRESS,   /* Button held > 1 second */
    ENCODER_EVENT_RELEASE,      /* Button released */
} encoder_event_t;

/* ============================================================================
   INITIALIZATION
   ============================================================================ */

/**
 * @brief Initialize the rotary encoder
 * 
 * Sets up GPIO interrupts for quadrature decoding and button input.
 * 
 * @return ESP_OK on success
 */
esp_err_t encoder_init(void);

/* ============================================================================
   STATE QUERIES
   ============================================================================ */

/**
 * @brief Get rotation delta since last call
 * 
 * Returns the number of detents (clicks) rotated since the last call.
 * Positive = clockwise, Negative = counter-clockwise.
 * Calling this function resets the delta counter.
 * 
 * @return Rotation delta (-N to +N)
 */
int encoder_get_delta(void);

/**
 * @brief Get accumulated position
 * 
 * Returns the total accumulated position (sum of all rotations).
 * This value persists across calls and can be positive or negative.
 * 
 * @return Current position
 */
int encoder_get_position(void);

/**
 * @brief Reset position to zero
 */
void encoder_reset_position(void);

/**
 * @brief Check if button is currently pressed
 * 
 * @return true if button is held down
 */
bool encoder_is_button_pressed(void);

/**
 * @brief Check if button was just pressed (edge detection)
 * 
 * Returns true once when button transitions from released to pressed.
 * Subsequent calls return false until button is released and pressed again.
 * 
 * @return true on press edge
 */
bool encoder_was_button_pressed(void);

/**
 * @brief Check for long press (>1 second hold)
 * 
 * Returns true once when button has been held for more than 1 second.
 * Resets when button is released.
 * 
 * @return true if long press detected
 */
bool encoder_was_long_press(void);

/* ============================================================================
   EVENT POLLING
   ============================================================================ */

/**
 * @brief Poll for encoder events
 * 
 * Returns the next pending event, or ENCODER_EVENT_NONE if no events.
 * Should be called regularly in the main loop.
 * 
 * @return Next encoder event
 */
encoder_event_t encoder_poll_event(void);

#endif /* ROTARY_ENCODER_H */

