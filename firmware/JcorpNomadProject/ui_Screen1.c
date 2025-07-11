// This file was generated by SquareLine Studio
// SquareLine Studio version: SquareLine Studio 1.5.0
// LVGL version: 8.3.11
// Project name: SquareLine_Project

#include "ui.h"

void ui_Screen1_screen_init(void)
{
    ui_Screen1 = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Screen1, LV_OBJ_FLAG_SCROLLABLE);      /// Flags

    ui_Image2 = lv_img_create(ui_Screen1);
    lv_img_set_src(ui_Image2, &ui_img_nomadui_png);
    lv_obj_set_width(ui_Image2, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_Image2, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_align(ui_Image2, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Image2, LV_OBJ_FLAG_ADV_HITTEST);     /// Flags
    lv_obj_clear_flag(ui_Image2, LV_OBJ_FLAG_SCROLLABLE);      /// Flags

    ui_uiuserlabel = lv_label_create(ui_Screen1);
    lv_obj_set_width(ui_uiuserlabel, LV_SIZE_CONTENT);   /// 1
    lv_obj_set_height(ui_uiuserlabel, LV_SIZE_CONTENT);    /// 1
    lv_obj_set_x(ui_uiuserlabel, -3);
    lv_obj_set_y(ui_uiuserlabel, -22);
    lv_obj_set_align(ui_uiuserlabel, LV_ALIGN_CENTER);
    lv_label_set_text(ui_uiuserlabel, "0");
    lv_obj_set_style_text_font(ui_uiuserlabel, &ui_font_Font1, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Spinner3 = lv_spinner_create(ui_Screen1, 1000, 90);
    lv_obj_set_width(ui_Spinner3, 126);
    lv_obj_set_height(ui_Spinner3, 151);
    lv_obj_set_x(ui_Spinner3, 1);
    lv_obj_set_y(ui_Spinner3, 95);
    lv_obj_set_align(ui_Spinner3, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ui_Spinner3, LV_OBJ_FLAG_CLICKABLE);      /// Flags

    lv_obj_set_style_arc_color(ui_Spinner3, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(ui_Spinner3, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    ui_wifi = lv_switch_create(ui_Screen1);
    lv_obj_set_width(ui_wifi, 34);
    lv_obj_set_height(ui_wifi, 19);
    lv_obj_set_x(ui_wifi, -63);
    lv_obj_set_y(ui_wifi, 5);
    lv_obj_set_align(ui_wifi, LV_ALIGN_CENTER);


    ui_SDcard = lv_switch_create(ui_Screen1);
    lv_obj_set_width(ui_SDcard, 34);
    lv_obj_set_height(ui_SDcard, 19);
    lv_obj_set_x(ui_SDcard, 62);
    lv_obj_set_y(ui_SDcard, 7);
    lv_obj_set_align(ui_SDcard, LV_ALIGN_CENTER);


    uic_uiuserlabel = ui_uiuserlabel;
    uic_wifi = ui_wifi;
    uic_SDcard = ui_SDcard;

}
