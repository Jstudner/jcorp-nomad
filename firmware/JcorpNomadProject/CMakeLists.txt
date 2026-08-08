# Board-independent sources plus whichever screen layout board_config.h selects.
# Both layout files are listed: each one #if's itself out for the other board.
SET(SOURCES ui.c
    ui_Screen1.c
    ui_screen_mini.c
    ui_comp_hook.c
    ui_helpers.c
    ui_img_1472434861.c
    ui_img_512784668.c
    ui_img_1859342711.c
    fonts/ui_font_Font1.c
    fonts/ui_font_Font2.c)

add_library(ui ${SOURCES})
