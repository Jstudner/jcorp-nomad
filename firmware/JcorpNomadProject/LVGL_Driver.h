#pragma once

#include <lvgl.h>
#include <lv_conf.h>
#include <esp_heap_caps.h>
#include "board_config.h"
#include "Display_Driver.h"

#define LVGL_WIDTH    LCD_WIDTH
#define LVGL_HEIGHT   LCD_HEIGHT
#define LVGL_BUF_LEN  (LVGL_WIDTH * LVGL_HEIGHT / LVGL_BUF_DIVISOR)

#define EXAMPLE_LVGL_TICK_PERIOD_MS  5

void Lvgl_print(const char * buf);
void Lvgl_Display_LCD( lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p ); // Displays LVGL content on the LCD.    This function implements associating LVGL data to the LCD screen
void Lvgl_Touchpad_Read( lv_indev_drv_t * indev_drv, lv_indev_data_t * data );                // Read the touchpad
void example_increase_lvgl_tick(void *arg);

void Lvgl_Init(void);
void Timer_Loop(void);

/* LVGL is not thread safe, and the firmware touches it from the streaming
   task, the UI task and loop(). Everything that renders or mutates widgets
   goes through this recursive lock. Returns false if the lock could not be
   taken within timeoutMs (in which case do not call Lvgl_Unlock). */
bool Lvgl_Lock(uint32_t timeoutMs);
void Lvgl_Unlock(void);
