// Display_Driver.h - single SPI LCD driver covering every Nomad board.
//
// Replaces the old Display_ST7789.* pair. Which controller, which pins, which
// window offsets and which backlight polarity are all taken from
// board_config.h, so both the 1.47" ST7789 and the 0.96" ST7735 dongle panel
// are driven by the same code path.

#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "board_config.h"

// Kept for source compatibility with the original Waveshare demo naming.
#define SPIFreq                  LCD_SPI_FREQ
#define EXAMPLE_PIN_NUM_MISO     LCD_PIN_MISO
#define EXAMPLE_PIN_NUM_MOSI     LCD_PIN_MOSI
#define EXAMPLE_PIN_NUM_SCLK     LCD_PIN_SCLK
#define EXAMPLE_PIN_NUM_LCD_CS   LCD_PIN_CS
#define EXAMPLE_PIN_NUM_LCD_DC   LCD_PIN_DC
#define EXAMPLE_PIN_NUM_LCD_RST  LCD_PIN_RST
#define EXAMPLE_PIN_NUM_BK_LIGHT LCD_PIN_BL

#ifdef __cplusplus
extern "C" {
#endif

void LCD_Init(void);

// Rotate the panel output 180 degrees, for sticks mounted upside down.
// Both supported panels sit centred in their controller's RAM (172 of 240 with
// Offset_X 34 either side; 160 of 162 and 80 of 132 with offsets 1 and 26), so
// the window offsets stay valid in either orientation and only MADCTL changes.
// Call from the task that owns LVGL flushes and repaint afterwards - panel RAM
// shows mirrored content until it is redrawn.
void LCD_SetRotation180(bool flip);

void LCD_SetCursor(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend);
void LCD_addWindow(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend, uint16_t *color);

// Paint the whole panel one RGB565 colour. Handy for the boot blank and for
// the hardware self-test.
void LCD_FillColor(uint16_t color565);

void Backlight_Init(void);
void Set_Backlight(uint8_t Light);  // 0..100 %
uint8_t Get_Backlight(void);

#ifdef __cplusplus
}
#endif
