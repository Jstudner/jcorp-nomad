// Draws every status card the firmware shows, on a PC, to PPM files (build.sh turns them into
// PNGs and a sheet). Same LVGL version, lv_conf.h, fonts, icons and NomadStatus.cpp as the board.
#include <lvgl.h>
#include <stdio.h>
#include <string.h>
#include "NomadStatus.h"

static const int W = 172, H = 320;
static lv_color_t fb[W * H];
static lv_color_t buf[W * H];

static void flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
  for (int y = a->y1; y <= a->y2; ++y)
    for (int x = a->x1; x <= a->x2; ++x) fb[y * W + x] = *px++;
  lv_disp_flush_ready(d);
}

static void save(const char *name) {
  char path[256];
  snprintf(path, sizeof(path), "%s.ppm", name);
  FILE *f = fopen(path, "wb");
  fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (int i = 0; i < W * H; ++i) {
    uint16_t c = fb[i].full;
    uint8_t r = ((c >> 11) & 0x1f) * 255 / 31, g = ((c >> 5) & 0x3f) * 255 / 63, b = (c & 0x1f) * 255 / 31;
    fputc(r, f); fputc(g, f); fputc(b, f);
  }
  fclose(f);
}

static void run(int ms) {
  for (int t = 0; t < ms; t += 10) { lv_tick_inc(10); lv_timer_handler(); }
}

static void shot(const char *name, uint8_t icon, uint8_t tone, const char *title, const char *detail,
                 int progress, uint8_t flags, const char *foot) {
  StatusMsg m;
  statusMake(&m, icon, tone, title, detail, progress, flags, foot);
  statusShowNow(&m);
  run(400);
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(NULL);
  save(name);
}

int main(int argc, char **argv) {
  lv_init();
  static lv_disp_draw_buf_t db;
  lv_disp_draw_buf_init(&db, buf, NULL, W * H);
  static lv_disp_drv_t drv;
  lv_disp_drv_init(&drv);
  drv.hor_res = W; drv.ver_res = H; drv.flush_cb = flush; drv.draw_buf = &db; drv.full_refresh = 1;
  lv_disp_t *disp = lv_disp_drv_register(&drv);
  lv_theme_t *th = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED),
                                         true, LV_FONT_DEFAULT);
  lv_disp_set_theme(disp, th);
  statusInit();

  StatusMsg m;
  shot("01-starting", SI_LOGO, ST_WORK, "Starting", "", 35, 0, "");
  shot("02-first-setup", SI_FOLDER, ST_WORK, "First-time setup", "Building indexes", -1, SF_SPIN, "");
  {
    statusMake(&m, SI_FOLDER, ST_WORK, "Indexing", "Shows", 40, SF_KEEP_PLUGGED, "");
    strcpy(m.big, "12,408");
    statusShowNow(&m); run(400); lv_obj_invalidate(lv_scr_act()); lv_refr_now(NULL); save("03-indexing");
  }
  {
    statusMake(&m, SI_FOLDER, ST_WORK, "Indexing", "Avatar The Last Airbender S02E14 The Swamp", -1, SF_SPIN | SF_KEEP_PLUGGED, "");
    strcpy(m.big, "12,345,678");
    statusShowNow(&m); run(400); lv_obj_invalidate(lv_scr_act()); lv_refr_now(NULL); save("04-indexing-folder");
  }
  // the scan: three kinds found so far, then a card with many
  {
    statusMake(&m, SI_CARD, ST_WORK, "Scanning card", "", -1, 0, "");
    m.capMb = 953800; m.segN = 3;
    m.seg[0] = { SK_MOVIES, 209900 }; m.seg[1] = { SK_SHOWS, 120800 }; m.seg[2] = { SK_MUSIC, 31200 };
    statusShowNow(&m); run(400); lv_obj_invalidate(lv_scr_act()); lv_refr_now(NULL); save("05-scan-3");
    statusShow(&m); run(100);
    m.segN = 5; m.seg[3] = { SK_ARCHIVE, 98000 }; m.seg[4] = { SK_MAPS, 15500 };
    statusShow(&m); run(100);
    m.seg[2] = { SK_MUSIC, 55300 }; m.seg[3] = { SK_ARCHIVE, 98000 }; m.seg[4] = { SK_MAPS, 15500 };
    m.seg[5] = { SK_BOOKS, 4100 }; m.seg[6] = { SK_GAMES, 2600 }; m.seg[7] = { SK_GALLERY, 9900 };
    m.segN = 12; m.seg[8] = { SK_TRANSLATE, 3900 }; m.seg[9] = { SK_WORKSHOP, 2500 };
    m.seg[10] = { SK_FILES, 60 }; m.seg[11] = { SK_COOKBOOK, 6 };
    statusShowNow(&m); run(400); lv_obj_invalidate(lv_scr_act()); lv_refr_now(NULL); save("06-scan-8");
  }
  shot("07-no-card", SI_CARD, ST_WARN, "No SD card", "Put the card back in", -1, 0, "");
  shot("08-not-readable", SI_CARD, ST_BAD, "Card not readable", "Format it exFAT or FAT32", -1, 0, "");
  shot("09-card-found", SI_CARD, ST_GOOD, "Card found", "Restarting", -1, SF_SPIN, "");
  shot("10-usb", SI_USB, ST_WORK, "USB mode", "", -1, 0, "");
  shot("11-joining", SI_WIFI, ST_WORK, "Joining WiFi", "HomeWiFi", -1, SF_SPIN, "Hold for hotspot");
  shot("12-wifi-failed", SI_WIFI_OFF, ST_WARN, "WiFi failed", "Network not found", -1, 0, "Starting hotspot");
  shot("13-fw-update", SI_DOWNLOAD, ST_WORK, "Updating firmware", "", 62, SF_KEEP_PLUGGED, "");
  shot("14-flash-mode", SI_DOWNLOAD, ST_WORK, "Ready for update", "", -1, 0, "");
  shot("15-shutting-down", SI_POWER, ST_WORK, "Shutting down", "", -1, SF_SPIN, "");
  shot("16-off", SI_CHECK, ST_GOOD, "Safe to unplug", "", -1, 0, "");
  shot("17-checking", SI_CARD, ST_WORK, "Checking storage", "", -1, SF_SPIN, "");
  return 0;
}
