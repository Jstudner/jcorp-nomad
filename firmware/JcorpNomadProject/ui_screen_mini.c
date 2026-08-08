// ui_screen_mini.c - 160x80 layout for the Pocket-Dongle-S3 class boards.
//
// Hand written rather than exported from SquareLine: at this size the original
// 172x320 design has nothing left to scale down to. It provides the same
// ui_Screen1_screen_init()/destroy() entry points and the same ui_* globals as
// the SquareLine layout, plus three swipeable-by-button pages:
//
//   page 0  connection    SSID, IP, connected users, SD usage bar
//   page 1  system        free heap, free PSRAM, die temperature, uptime
//   page 2  storage       used / total / free on the SD card
//
// Only one of ui_Screen1.c and ui_screen_mini.c is ever compiled - see
// NOMAD_UI_LAYOUT in board_config.h.

#include "board_config.h"

#if NOMAD_UI_LAYOUT == NOMAD_UI_LANDSCAPE_MINI

#include "ui.h"
#include "ui_screen_mini.h"

// --- SquareLine-compatible globals ---------------------------------------
lv_obj_t * ui_Screen1 = NULL;
lv_obj_t * ui_Spinner1 = NULL;
lv_obj_t * ui_wifi = NULL;        // LV_SYMBOL_WIFI label, tinted by state
lv_obj_t * ui_SDcard = NULL;      // LV_SYMBOL_SD_CARD label, tinted by state
lv_obj_t * ui_sdbar = NULL;
lv_obj_t * ui_Label1 = NULL;      // "Users" caption
lv_obj_t * ui_Label2 = NULL;      // SD percentage readout
lv_obj_t * ui_Panel1 = NULL;      // title bar
lv_obj_t * ui_ssidlabel = NULL;
lv_obj_t * ui_Image1 = NULL;      // unused at this size
lv_obj_t * ui_Image2 = NULL;      // unused at this size
lv_obj_t * ui_Image3 = NULL;      // unused at this size
lv_obj_t * ui_userlabel = NULL;
lv_obj_t * ui_MediaGen = NULL;    // full screen message overlay

// --- extra widgets owned by this layout ----------------------------------
lv_obj_t * ui_mini_page[NOMAD_MINI_PAGE_COUNT] = { NULL, NULL, NULL };
lv_obj_t * ui_iplabel = NULL;
lv_obj_t * ui_heaplabel = NULL;
lv_obj_t * ui_psramlabel = NULL;
lv_obj_t * ui_templabel = NULL;
lv_obj_t * ui_uptimelabel = NULL;
lv_obj_t * ui_storeUsed = NULL;
lv_obj_t * ui_storeTotal = NULL;
lv_obj_t * ui_storeFree = NULL;
lv_obj_t * ui_pagedot[NOMAD_MINI_PAGE_COUNT] = { NULL, NULL, NULL };

#define COL_BG      lv_color_hex(0x05070F)
#define COL_BAR     lv_color_hex(0x223E87)
#define COL_ACCENT  lv_color_hex(0xA5E0F9)
#define COL_TEXT    lv_color_hex(0xE8EEF7)
#define COL_MUTED   lv_color_hex(0x7C8AA5)
#define COL_OK      lv_color_hex(0x3DDC84)
#define COL_BAD     lv_color_hex(0x4A5468)

static void style_plain_container(lv_obj_t *o) {
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(o, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            lv_color_t color, lv_align_t align, lv_coord_t x, lv_coord_t y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_align(l, align);
    lv_obj_set_pos(l, x, y);
    return l;
}

// ---------------------------------------------------------------- page 0 ---
static void build_page_connection(lv_obj_t *parent) {
    // IP address is the one thing people actually need off this screen, so it
    // gets the biggest font and the middle of the panel.
    ui_iplabel = make_label(parent, "0.0.0.0", &lv_font_montserrat_14, COL_TEXT,
                            LV_ALIGN_TOP_MID, 0, 2);

    ui_Label1 = make_label(parent, "Users", &lv_font_montserrat_12, COL_MUTED,
                           LV_ALIGN_TOP_LEFT, 4, 24);
    ui_userlabel = make_label(parent, "0", &lv_font_montserrat_14, COL_ACCENT,
                              LV_ALIGN_TOP_LEFT, 46, 22);

    ui_Label2 = make_label(parent, "SD --%", &lv_font_montserrat_12, COL_MUTED,
                           LV_ALIGN_TOP_RIGHT, -4, 24);

    ui_sdbar = lv_bar_create(parent);
    lv_obj_set_size(ui_sdbar, 152, 8);
    lv_obj_set_align(ui_sdbar, LV_ALIGN_TOP_MID);
    lv_obj_set_pos(ui_sdbar, 0, 44);
    lv_bar_set_range(ui_sdbar, 0, 100);
    lv_bar_set_value(ui_sdbar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ui_sdbar, lv_color_hex(0x1B2437), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_sdbar, COL_ACCENT, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_sdbar, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_sdbar, 4, LV_PART_INDICATOR | LV_STATE_DEFAULT);
}

// ---------------------------------------------------------------- page 1 ---
static void build_page_system(lv_obj_t *parent) {
    make_label(parent, "RAM", &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 4, 0);
    ui_heaplabel = make_label(parent, "--", &lv_font_montserrat_12, COL_TEXT,
                              LV_ALIGN_TOP_LEFT, 52, 0);

    make_label(parent, "PSRAM", &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 4, 14);
    ui_psramlabel = make_label(parent, "--", &lv_font_montserrat_12, COL_TEXT,
                               LV_ALIGN_TOP_LEFT, 52, 14);

    make_label(parent, "Temp", &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 4, 28);
    ui_templabel = make_label(parent, "--", &lv_font_montserrat_12, COL_TEXT,
                              LV_ALIGN_TOP_LEFT, 52, 28);

    make_label(parent, "Up", &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 4, 42);
    ui_uptimelabel = make_label(parent, "--", &lv_font_montserrat_12, COL_TEXT,
                                LV_ALIGN_TOP_LEFT, 52, 42);
}

// ---------------------------------------------------------------- page 2 ---
static void build_page_storage(lv_obj_t *parent) {
    make_label(parent, "Used", &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 4, 3);
    ui_storeUsed = make_label(parent, "--", &lv_font_montserrat_12, COL_TEXT,
                              LV_ALIGN_TOP_LEFT, 52, 3);

    make_label(parent, "Free", &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 4, 20);
    ui_storeFree = make_label(parent, "--", &lv_font_montserrat_12, COL_OK,
                              LV_ALIGN_TOP_LEFT, 52, 20);

    make_label(parent, "Card", &lv_font_montserrat_12, COL_MUTED, LV_ALIGN_TOP_LEFT, 4, 37);
    ui_storeTotal = make_label(parent, "--", &lv_font_montserrat_12, COL_TEXT,
                               LV_ALIGN_TOP_LEFT, 52, 37);
}

// ------------------------------------------------------------------ build ---
void ui_Screen1_screen_init(void) {
    ui_Screen1 = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Screen1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Screen1, COL_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Screen1, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_Screen1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Screen1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // ---- title bar -------------------------------------------------------
    ui_Panel1 = lv_obj_create(ui_Screen1);
    lv_obj_set_size(ui_Panel1, LCD_WIDTH, NOMAD_MINI_BAR_H);
    lv_obj_set_align(ui_Panel1, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(ui_Panel1, 0, 0);
    lv_obj_clear_flag(ui_Panel1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Panel1, COL_BAR, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Panel1, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Panel1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Panel1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_Panel1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_ssidlabel = make_label(ui_Panel1, "Jcorp_Nomad", &lv_font_montserrat_12, COL_TEXT,
                              LV_ALIGN_LEFT_MID, 3, 0);
    lv_label_set_long_mode(ui_ssidlabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ui_ssidlabel, 108);

    ui_SDcard = make_label(ui_Panel1, LV_SYMBOL_SD_CARD, &lv_font_montserrat_12, COL_BAD,
                           LV_ALIGN_RIGHT_MID, -4, 0);
    ui_wifi = make_label(ui_Panel1, LV_SYMBOL_WIFI, &lv_font_montserrat_12, COL_BAD,
                         LV_ALIGN_RIGHT_MID, -20, 0);

    // ---- pages -----------------------------------------------------------
    for (int i = 0; i < NOMAD_MINI_PAGE_COUNT; ++i) {
        ui_mini_page[i] = lv_obj_create(ui_Screen1);
        lv_obj_set_size(ui_mini_page[i], LCD_WIDTH, NOMAD_MINI_PAGE_H);
        lv_obj_set_align(ui_mini_page[i], LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(ui_mini_page[i], 0, NOMAD_MINI_BAR_H);
        style_plain_container(ui_mini_page[i]);
        if (i != 0) lv_obj_add_flag(ui_mini_page[i], LV_OBJ_FLAG_HIDDEN);
    }

    build_page_connection(ui_mini_page[0]);
    build_page_system(ui_mini_page[1]);
    build_page_storage(ui_mini_page[2]);

    // ---- page indicator --------------------------------------------------
    // Three 6x3 pips along the bottom edge. A text label would need ~16 px of
    // line height, which this screen simply does not have to spare.
    for (int i = 0; i < NOMAD_MINI_PAGE_COUNT; ++i) {
        ui_pagedot[i] = lv_obj_create(ui_Screen1);
        lv_obj_set_size(ui_pagedot[i], 6, 3);
        lv_obj_set_align(ui_pagedot[i], LV_ALIGN_BOTTOM_MID);
        lv_obj_set_pos(ui_pagedot[i], (i - (NOMAD_MINI_PAGE_COUNT - 1) / 2) * 10, -1);
        lv_obj_clear_flag(ui_pagedot[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(ui_pagedot[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(ui_pagedot[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(ui_pagedot[i], 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(ui_pagedot[i], LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(ui_pagedot[i], i == 0 ? COL_ACCENT : COL_BAD,
                                  LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    // ui_Spinner1 is deliberately left NULL here: the SquareLine layout uses it
    // as decoration, but on 160x80 there is no room, and an always-running
    // spinner animation would burn CPU for something nobody can see.

    // ---- full screen message overlay ------------------------------------
    ui_MediaGen = lv_textarea_create(ui_Screen1);
    lv_obj_set_size(ui_MediaGen, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_align(ui_MediaGen, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(ui_MediaGen, 0, 0);
    lv_textarea_set_placeholder_text(ui_MediaGen, "Starting....");
    lv_obj_set_style_text_font(ui_MediaGen, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_MediaGen, COL_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_MediaGen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ui_MediaGen, COL_TEXT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_MediaGen, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_MediaGen, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_MediaGen, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_opa(ui_MediaGen, LV_OPA_TRANSP, LV_PART_CURSOR | LV_STATE_DEFAULT);
    lv_obj_add_flag(ui_MediaGen, LV_OBJ_FLAG_HIDDEN);
}

void ui_Screen1_screen_destroy(void) {
    if (ui_Screen1) lv_obj_del(ui_Screen1);

    ui_Screen1 = NULL;
    ui_Spinner1 = NULL;
    ui_wifi = NULL;
    ui_SDcard = NULL;
    ui_sdbar = NULL;
    ui_Label1 = NULL;
    ui_Label2 = NULL;
    ui_Panel1 = NULL;
    ui_ssidlabel = NULL;
    ui_Image1 = NULL;
    ui_Image2 = NULL;
    ui_Image3 = NULL;
    ui_userlabel = NULL;
    ui_MediaGen = NULL;

    for (int i = 0; i < NOMAD_MINI_PAGE_COUNT; ++i) ui_mini_page[i] = NULL;
    ui_iplabel = NULL;
    ui_heaplabel = NULL;
    ui_psramlabel = NULL;
    ui_templabel = NULL;
    ui_uptimelabel = NULL;
    ui_storeUsed = NULL;
    ui_storeTotal = NULL;
    ui_storeFree = NULL;
    for (int i = 0; i < NOMAD_MINI_PAGE_COUNT; ++i) ui_pagedot[i] = NULL;
}

#endif  // NOMAD_UI_LAYOUT == NOMAD_UI_LANDSCAPE_MINI
