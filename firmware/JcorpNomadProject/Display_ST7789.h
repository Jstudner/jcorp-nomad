#pragma once
#include <Arduino.h>
#include <SPI.h>
#define LCD_WIDTH   172 //LCD width
#define LCD_HEIGHT  320 //LCD height

#define SPIFreq                        80000000
#define EXAMPLE_PIN_NUM_MISO           -1
#define EXAMPLE_PIN_NUM_MOSI           45
#define EXAMPLE_PIN_NUM_SCLK           40
#define EXAMPLE_PIN_NUM_LCD_CS         42
#define EXAMPLE_PIN_NUM_LCD_DC         41
#define EXAMPLE_PIN_NUM_LCD_RST        39
// backlight pin, and the only firmware-visible difference between the two boards:
// the 1.47 (USB-A) drives it from GPIO 48, the 1.47B (USB-C) from GPIO 46.
//   BOARD_USB_C 0 -> GPIO 48, USB-A (default)
//   BOARD_USB_C 1 -> GPIO 46, USB-C / 1.47B
// edit the 0 below, or build with -DBOARD_USB_C=1.
//
// it has to be set HERE, not in the .ino. the pin is used in Display_ST7789.cpp, which
// is its own translation unit, so a define in the sketch never reaches it and the board
// keeps GPIO 48 with no error and a dark screen. every file that needs the pin includes
// this header, which is why setting it here works.
#ifndef BOARD_USB_C
#define BOARD_USB_C 0
#endif

#if BOARD_USB_C
#define EXAMPLE_PIN_NUM_BK_LIGHT       46
#else
#define EXAMPLE_PIN_NUM_BK_LIGHT       48
#endif
#define Frequency       1000                    // PWM frequencyconst 
#define Resolution      10                      

#define VERTICAL   0
#define HORIZONTAL 1

#define Offset_X 34
#define Offset_Y 0


void LCD_SetCursor(uint16_t x1, uint16_t y1, uint16_t x2,uint16_t y2);

void LCD_Init(void);
void LCD_SetRotation180(bool flip);
void LCD_SetCursor(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t  Yend);
void LCD_addWindow(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend,uint16_t* color);

void Backlight_Init(void);
void Set_Backlight(uint8_t Light);
