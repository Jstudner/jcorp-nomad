#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "nomad_board.h"
#define LCD_WIDTH   172 //LCD width
#define LCD_HEIGHT  320 //LCD height

#define SPIFreq                        80000000
#define EXAMPLE_PIN_NUM_MISO           -1
#define EXAMPLE_PIN_NUM_MOSI           NOMAD_LCD_MOSI
#define EXAMPLE_PIN_NUM_SCLK           NOMAD_LCD_SCLK
#define EXAMPLE_PIN_NUM_LCD_CS         NOMAD_LCD_CS
#define EXAMPLE_PIN_NUM_LCD_DC         NOMAD_LCD_DC
#define EXAMPLE_PIN_NUM_LCD_RST        NOMAD_LCD_RST
#define EXAMPLE_PIN_NUM_BK_LIGHT       NOMAD_LCD_BL
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
