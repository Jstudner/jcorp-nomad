// ui_screen_mini.h - widgets specific to the 160x80 dongle layout.
// Included by nomad_ui.cpp so it can update the extra pages.

#pragma once
#include "board_config.h"

#if NOMAD_UI_LAYOUT == NOMAD_UI_LANDSCAPE_MINI

#include "lvgl.h"

#define NOMAD_MINI_PAGE_COUNT 3

// 160x80 vertical budget: 16 px title bar, 58 px page body, 6 px for the page
// indicator pips along the bottom edge.
#define NOMAD_MINI_BAR_H   16
#define NOMAD_MINI_PAGE_H  (LCD_HEIGHT - NOMAD_MINI_BAR_H - 6)

#ifdef __cplusplus
extern "C" {
#endif

extern lv_obj_t * ui_mini_page[NOMAD_MINI_PAGE_COUNT];
extern lv_obj_t * ui_iplabel;
extern lv_obj_t * ui_heaplabel;
extern lv_obj_t * ui_psramlabel;
extern lv_obj_t * ui_templabel;
extern lv_obj_t * ui_uptimelabel;
extern lv_obj_t * ui_storeUsed;
extern lv_obj_t * ui_storeTotal;
extern lv_obj_t * ui_storeFree;
extern lv_obj_t * ui_pagedot[NOMAD_MINI_PAGE_COUNT];

#ifdef __cplusplus
}
#endif

#endif  // NOMAD_UI_LAYOUT == NOMAD_UI_LANDSCAPE_MINI
