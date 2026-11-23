// NomadScreen.h - the 1.47" screen on Nomad's board: a 172 x 320 ST7789 panel on SPI, its
// backlight, and LVGL drawing to it.
#pragma once
#include <Arduino.h>
#include <lvgl.h>

// Which board. The ESP32-S3-LCD-1.47 (USB-A) drives the backlight from GPIO 48, the 1.47B
// (USB-C) from GPIO 46, and nothing else differs. Set it here or build with -DBOARD_USB_C=1
// (tools/flash-nomad.sh --usbc). It has to be in this header: a #define in the sketch never
// reaches NomadScreen.cpp, and the wrong pin boots fine with a dark screen.
#ifndef BOARD_USB_C
#define BOARD_USB_C 0
#endif

constexpr int SCREEN_W = 172;
constexpr int SCREEN_H = 320;

// The panel, the backlight at full, then LVGL with the panel as its display.
void screenBegin();

// Turn the picture 180 degrees (for a Nomad plugged in upside down). Repaint afterwards. Only
// call it from the task that draws, never in the middle of a flush.
void screenFlip(bool upsideDown);

// Backlight, 0 to 100 percent. Anything above 100 is ignored.
void screenBacklight(uint8_t percent);

extern volatile uint32_t g_lcdFlushes;   // areas drawn since boot (bench: /bench/fault shows it)
