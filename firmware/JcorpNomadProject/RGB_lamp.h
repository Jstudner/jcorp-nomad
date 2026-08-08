// RGB_lamp.h - status LED, board independent.
//
// The Waveshare 1.47" board has a single WS2812 (one-wire). The Pocket-Dongle
// has a single APA102 (clock + data). Both are driven through the same tiny
// API; board_config.h picks the backend.

#pragma once
#include "Arduino.h"
#include "board_config.h"

// Kept for source compatibility with the original driver.
#define PIN_NEOPIXEL LED_PIN_DATA

#ifdef __cplusplus
extern "C" {
#endif

// Safe to call more than once. Called automatically by Set_Color().
void RGB_Init(void);

// Straight R, G, B (0..255 each). Channel ordering for the physical LED is
// handled inside the backend.
void Set_Color(uint8_t Red, uint8_t Green, uint8_t Blue);

// Smoothly cycles the hue. `Waiting` is the delay between hue steps in
// milliseconds - the loop is time based, so calling it more or less often no
// longer changes how fast the rainbow runs.
void RGB_Lamp_Loop(uint16_t Waiting);

#ifdef __cplusplus
}
#endif
