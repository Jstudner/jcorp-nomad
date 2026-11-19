// NomadLed.h - the board's RGB LED, a WS2812 on GPIO 38.
#pragma once
#include <Arduino.h>

// Straight to rgbLedWrite(pin, first, second, third). Red and green come out swapped on
// this board's LED, so the sketch passes (green, red, blue).
void ledSet(uint8_t first, uint8_t second, uint8_t third);

// The rainbow: call it from a loop, and every `every` calls the colour moves one step around
// the wheel (192 steps, fading from one channel to the next).
void ledRainbowStep(uint16_t every);
