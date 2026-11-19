// NomadMain.cpp - the main screen (see NomadMain.h).
#include "NomadMain.h"
#include "ui.h"
#include <string.h>
#include <stdio.h>

LV_FONT_DECLARE(nomad_font_label);
LV_FONT_DECLARE(nomad_font_body);
LV_FONT_DECLARE(nomad_font_title);
LV_FONT_DECLARE(nomad_font_count);
LV_FONT_DECLARE(nomad_font_small);
LV_IMG_DECLARE(nomad_icon_card_35);
LV_IMG_DECLARE(nomad_icon_wifi_35);
LV_IMG_DECLARE(nomad_icon_people_22);

#define C_TEXT 0xeaf6ff
#define C_DANGER 0xdc3545   // --danger
// fills the gap from the network pill (bottom y 84) to the status icons (top y ~243), centred in it
static const int QR_SIZE = 132;
static const int QR_TILE_Y = 4;   // from the screen centre: the gap's middle

static lv_obj_t *s_qrTile, *s_qr, *s_people, *s_count, *s_fullLabel;
static bool s_qrOn;
static char s_qrText[160];
static int s_n = -1;

static void tint(lv_obj_t *img) {
  lv_obj_set_style_img_recolor(img, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
}

extern "C" void mainInit(void) {
  if (!ui_Screen1 || s_qrTile) return;
  // same places and colours as the SquareLine page, smooth type
  lv_obj_set_style_text_font(ui_Label2, &nomad_font_label, 0);
  lv_obj_set_style_text_font(ui_Label1, &nomad_font_label, 0);
  lv_obj_set_size(ui_Panel1, 148, 32);
  lv_obj_set_style_radius(ui_Panel1, 16, 0);
  lv_obj_set_style_text_font(ui_ssidlabel, &nomad_font_body, 0);
  lv_obj_set_x(ui_ssidlabel, 0);
  lv_obj_set_size(ui_userlabel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_text_font(ui_userlabel, &nomad_font_count, 0);
  lv_obj_align(ui_userlabel, LV_ALIGN_CENTER, 0, 22);
  lv_img_set_src(ui_Image1, &nomad_icon_card_35);
  lv_img_set_src(ui_Image2, &nomad_icon_wifi_35);
  tint(ui_Image1);
  tint(ui_Image2);

  // the QR code in the tree's place, white tile so phones read it on the dark page
  s_qrTile = lv_obj_create(ui_Screen1);
  lv_obj_remove_style_all(s_qrTile);
  lv_obj_clear_flag(s_qrTile, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  // the white round the code is its quiet zone: scanners want 4 modules of it (the code draws at
  // whole pixels a module, 4 here, and centres itself in its square), so 9 px of tile plus its own
  lv_obj_set_size(s_qrTile, QR_SIZE + 18, QR_SIZE + 18);
  lv_obj_set_style_bg_color(s_qrTile, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(s_qrTile, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_qrTile, 8, 0);
  lv_obj_align(s_qrTile, LV_ALIGN_CENTER, 0, QR_TILE_Y);
  s_qr = lv_qrcode_create(s_qrTile, QR_SIZE, lv_color_black(), lv_color_white());
  lv_obj_center(s_qr);
  lv_obj_add_flag(s_qrTile, LV_OBJ_FLAG_HIDDEN);

  // with the code showing, the count between the two status icons
  s_people = lv_img_create(ui_Screen1);
  lv_img_set_src(s_people, &nomad_icon_people_22);
  tint(s_people);
  lv_obj_align(s_people, LV_ALIGN_CENTER, 0, 100);
  s_count = lv_label_create(ui_Screen1);
  lv_obj_set_style_text_font(s_count, &nomad_font_title, 0);
  lv_obj_set_style_text_color(s_count, lv_color_hex(C_TEXT), 0);
  lv_obj_align(s_count, LV_ALIGN_CENTER, 0, 128);
  lv_label_set_text(s_count, "0");
  lv_obj_add_flag(s_people, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_count, LV_OBJ_FLAG_HIDDEN);

  // "Card full" on the storage bar while uploads are refused
  s_fullLabel = lv_label_create(ui_Screen1);
  lv_obj_set_style_text_font(s_fullLabel, &nomad_font_small, 0);
  lv_obj_set_style_text_color(s_fullLabel, lv_color_white(), 0);
  lv_label_set_text(s_fullLabel, "Card full");
  lv_obj_align_to(s_fullLabel, ui_sdbar, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(s_fullLabel, LV_OBJ_FLAG_HIDDEN);
}

extern "C" void mainSetCardFull(int full) {
  if (!s_fullLabel) return;
  if (full) {
    lv_obj_set_style_bg_color(ui_sdbar, lv_color_hex(C_DANGER), LV_PART_INDICATOR);
    lv_obj_clear_flag(s_fullLabel, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_local_style_prop(ui_sdbar, LV_STYLE_BG_COLOR, LV_PART_INDICATOR);   // back to the theme's blue
    lv_obj_add_flag(s_fullLabel, LV_OBJ_FLAG_HIDDEN);
  }
}

extern "C" void mainSetCount(int n) {
  if (n == s_n) return;
  s_n = n;
  char b[8];
  snprintf(b, sizeof(b), "%d", n);
  lv_label_set_text(ui_userlabel, b);
  if (s_count) lv_label_set_text(s_count, b);
}

static void show(lv_obj_t *o, bool on) {
  if (!o) return;
  if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

extern "C" void mainSetQr(const char *payload) {
  if (!s_qrTile) return;
  const bool on = payload && payload[0];
  if (on && (!s_qrOn || strcmp(payload, s_qrText))) {
    strncpy(s_qrText, payload, sizeof(s_qrText) - 1);
    s_qrText[sizeof(s_qrText) - 1] = '\0';
    lv_qrcode_update(s_qr, s_qrText, strlen(s_qrText));
  }
  s_qrOn = on;
  show(s_qrTile, on);
  show(s_people, on);
  show(s_count, on);
  show(ui_Image3, !on);       // the tree
  show(ui_userlabel, !on);    // the count on it
  show(ui_Label1, !on);       // "Connected Users:": the icon at the bottom says it
}

extern "C" int mainWifiQr(char *out, unsigned outLen, const char *ssid, const char *password) {
  // WIFI: strings reserve \ ; , : and " in the name and password
  auto esc = [](char *dst, unsigned room, const char *src) -> int {
    unsigned o = 0;
    for (const char *p = src ? src : ""; *p; ++p) {
      const bool special = *p == '\\' || *p == ';' || *p == ',' || *p == ':' || *p == '"';
      if (o + (special ? 2 : 1) >= room) return -1;
      if (special) dst[o++] = '\\';
      dst[o++] = *p;
    }
    dst[o] = '\0';
    return (int)o;
  };
  char s[80], p[140];
  if (esc(s, sizeof(s), ssid) < 0 || esc(p, sizeof(p), password) < 0) return 0;
  const int n = (password && password[0])
      ? snprintf(out, outLen, "WIFI:T:WPA;S:%s;P:%s;;", s, p)
      : snprintf(out, outLen, "WIFI:T:nopass;S:%s;;", s);
  return n > 0 && (unsigned)n < outLen;
}
