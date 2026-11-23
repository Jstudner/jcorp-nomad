// NomadStatus.cpp - the status pages (see NomadStatus.h).
#include "NomadStatus.h"
#include <string.h>
#include <stdio.h>
#include <strings.h>

LV_FONT_DECLARE(nomad_font_big);
LV_FONT_DECLARE(nomad_font_title);
LV_FONT_DECLARE(nomad_font_head);
LV_FONT_DECLARE(nomad_font_body);
LV_FONT_DECLARE(nomad_font_small);
LV_IMG_DECLARE(nomad_icon_card);
LV_IMG_DECLARE(nomad_icon_folder);
LV_IMG_DECLARE(nomad_icon_folder_80);
LV_IMG_DECLARE(nomad_icon_refresh);
LV_IMG_DECLARE(nomad_icon_usb);
LV_IMG_DECLARE(nomad_icon_wifi);
LV_IMG_DECLARE(nomad_icon_wifi_off);
LV_IMG_DECLARE(nomad_icon_power);
LV_IMG_DECLARE(nomad_icon_download);
LV_IMG_DECLARE(nomad_icon_warning);
LV_IMG_DECLARE(nomad_icon_check);
LV_IMG_DECLARE(nomad_icon_k_movies);
LV_IMG_DECLARE(nomad_icon_k_shows);
LV_IMG_DECLARE(nomad_icon_k_music);
LV_IMG_DECLARE(nomad_icon_k_books);
LV_IMG_DECLARE(nomad_icon_k_gallery);
LV_IMG_DECLARE(nomad_icon_k_games);
LV_IMG_DECLARE(nomad_icon_k_maps);
LV_IMG_DECLARE(nomad_icon_k_archive);
LV_IMG_DECLARE(nomad_icon_k_cookbook);
LV_IMG_DECLARE(nomad_icon_k_workshop);
LV_IMG_DECLARE(nomad_icon_k_translate);
LV_IMG_DECLARE(nomad_icon_k_files);
LV_IMG_DECLARE(nomad_icon_k_other);
LV_IMG_DECLARE(ui_img_1859342711);   // Nomad's logo from the main screen

// The pages' dark theme (SD_Card_Template/master.css, body.dark and :root)
#define C_BG        0x071226   // --bg
#define C_TEXT      0xeaf6ff   // --text
#define C_MUTED     0x93a9c6   // --muted
#define C_PRIMARY   0x4aa3ff   // --primary
#define C_SUCCESS   0x28a745   // --success
#define C_WARNING   0xffc107   // --warning
#define C_DANGER    0xdc3545   // --danger
#define C_INFO      0x17a2b8   // --info
#define C_SECONDARY 0x6c757d   // --secondary

static const int SW = 172, SH = 320;
static const int ICON = 112;
static const int ICON_COMPACT = 80;        // pages with a big number
static const int LOGO_ZOOM = 358;            // 1.4x of the 83 x 110 logo
static const int LOGO_H = 154;
static const int BAR_X = 12, BAR_W = 148, BAR_H = 18, BAR_Y = 262;
static const int SLIDE_W = 44;
static const int TEXT_W = 164;
static const int RING_D = 156, RING_X = 8, RING_Y = 6, RING_T = 18;
static const int LEGEND_Y = 204, LEGEND_ROW = 27, LEGEND_ROWS = 4;
static const int ICON_TOP = 26;          // every icon page puts its icon here, so pages line up

struct KindInfo { const char *name; const lv_img_dsc_t *icon; };
static const KindInfo kKinds[SK_COUNT] = {
  { "Movies", &nomad_icon_k_movies },   { "Shows", &nomad_icon_k_shows },
  { "Music", &nomad_icon_k_music },     { "Books", &nomad_icon_k_books },
  { "Gallery", &nomad_icon_k_gallery }, { "Games", &nomad_icon_k_games },
  { "Maps", &nomad_icon_k_maps },       { "Archive", &nomad_icon_k_archive },
  { "Cookbook", &nomad_icon_k_cookbook }, { "Workshop", &nomad_icon_k_workshop },
  { "Translate", &nomad_icon_k_translate },
  { "Files", &nomad_icon_k_files },     { "Other", &nomad_icon_k_other },
};
// One colour per kind, the same on every card. Shows, Movies, Music and Books take the colours of
// the apps that fetch them (Sonarr, Radarr, Lidarr, Readarr: their logos), the rest one hue each.
static const uint32_t s_kindColor[SK_COUNT] = {
  0xffc230,   // Movies   Radarr yellow
  0x00ccff,   // Shows    Sonarr blue
  0x009252,   // Music    Lidarr green
  0x8e2222,   // Books    Readarr red
  0xe83e8c,   // Gallery  pink
  0x9b6dff,   // Games    purple
  0x20c997,   // Maps     teal
  0xfd7e14,   // Archive  orange
  0xc08457,   // Cookbook tan
  0xa9e34b,   // Workshop lime
  0x748ffc,   // Translate indigo
  C_MUTED,    // Files    slate
  C_SECONDARY // Other    grey
};

// icon layout
static lv_obj_t *s_root, *s_body, *s_curtain, *s_icon, *s_logo, *s_col, *s_title, *s_detail, *s_big, *s_foot;
static lv_obj_t *s_bar, *s_track, *s_slide;
// ring layout
static lv_obj_t *s_ring, *s_ringDial, *s_ringBig, *s_ringSub, *s_ringTitle;
// what the dial draws: piece colours and their end angles (0 at the top, clockwise)
static int s_dialN;
static uint32_t s_dialColor[STATUS_SEGS];
static int16_t s_dialEnd[STATUS_SEGS];
struct LegendCell { lv_obj_t *icon, *name, *val; };
static LegendCell s_leg[STATUS_SEGS];
static lv_obj_t *s_legBox, *s_legList;
static int s_legScroll;

static StatusMsg s_cur;
static bool s_shown;
static bool s_sliding;
// transitions: off until something calls lv_timer_handler often (setup() and USB mode don't)
static bool s_animate;
// No opacity on containers: LVGL 8.3 then draws each child through its own layer buffer (grey
// boxes on this panel, and more memory than is left). The page's background fades in over the
// main screen, and a cover in the page colour fades over the content to change it.
enum Phase : uint8_t { PH_IDLE, PH_SHOW_BG, PH_SHOW_IN, PH_CROSS_OUT, PH_CROSS_IN, PH_HIDE_OUT, PH_HIDE_BG };
static Phase s_phase = PH_IDLE;
static bool s_hasPending;
static StatusMsg s_pending;
static const uint32_t BG_MS = 120, COVER_MS = 120;

static lv_color_t toneColor(uint8_t tone) {
  switch (tone) {
    case ST_GOOD: return lv_color_hex(C_SUCCESS);
    case ST_WARN: return lv_color_hex(C_WARNING);
    case ST_BAD:  return lv_color_hex(C_DANGER);
    default:      return lv_color_hex(C_PRIMARY);
  }
}

static const lv_img_dsc_t *iconImage(uint8_t icon) {
  switch (icon) {
    case SI_CARD:     return &nomad_icon_card;
    case SI_FOLDER:   return &nomad_icon_folder;
    case SI_REFRESH:  return &nomad_icon_refresh;
    case SI_USB:      return &nomad_icon_usb;
    case SI_WIFI:     return &nomad_icon_wifi;
    case SI_WIFI_OFF: return &nomad_icon_wifi_off;
    case SI_POWER:    return &nomad_icon_power;
    case SI_DOWNLOAD: return &nomad_icon_download;
    case SI_WARNING:  return &nomad_icon_warning;
    case SI_CHECK:    return &nomad_icon_check;
    default:          return NULL;
  }
}

static void copyStr(char *dst, size_t n, const char *src) {
  if (!src) src = "";
  strncpy(dst, src, n - 1);
  dst[n - 1] = '\0';
}

extern "C" void statusMake(StatusMsg *m, uint8_t icon, uint8_t tone, const char *title, const char *detail,
                           int progress, uint8_t flags, const char *foot) {
  memset(m, 0, sizeof(*m));
  m->icon = icon;
  m->tone = tone;
  m->flags = flags;
  m->progress = (int8_t)(progress < 0 ? -1 : (progress > 100 ? 100 : progress));
  copyStr(m->title, sizeof(m->title), title);
  copyStr(m->detail, sizeof(m->detail), detail);
  copyStr(m->foot, sizeof(m->foot), foot);
}

extern "C" void statusThousands(char *out, unsigned outLen, unsigned long n) {
  char raw[24];
  snprintf(raw, sizeof(raw), "%lu", n);
  const int len = (int)strlen(raw);
  unsigned o = 0;
  for (int i = 0; i < len && o + 1 < outLen; ++i) {
    if (i && (len - i) % 3 == 0 && o + 2 < outLen) out[o++] = ',';
    out[o++] = raw[i];
  }
  out[o] = '\0';
}

extern "C" uint8_t statusKindForFolder(const char *top) {
  if (!top) return SK_OTHER;
  while (*top == '/') ++top;
  static const struct { const char *folder; uint8_t kind; } map[] = {
    { "Movies", SK_MOVIES }, { "Shows", SK_SHOWS }, { "Music", SK_MUSIC }, { "Books", SK_BOOKS },
    { "Gallery", SK_GALLERY }, { "Games", SK_GAMES }, { "EmulatorJS", SK_GAMES }, { "Maps", SK_MAPS },
    { "Archive", SK_ARCHIVE }, { "Cookbook", SK_COOKBOOK }, { "Workshop", SK_WORKSHOP }, { "Translate", SK_TRANSLATE }, { "Files", SK_FILES },
  };
  for (const auto &e : map)
    if (!strcasecmp(top, e.folder)) return e.kind;
  return SK_OTHER;
}

static lv_obj_t *plainObj(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return o;
}

static lv_obj_t *textLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int width) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_width(l, width);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, "");
  return l;
}

// The whole ring in one draw: the track, every piece, and the walk's place. One object, so any
// redraw paints all of it (separate arc objects leave the growing piece blank between updates).
static void dialDraw(lv_event_t *e) {
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  lv_obj_t *obj = lv_event_get_target(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  const lv_point_t c = { (lv_coord_t)(a.x1 + RING_D / 2), (lv_coord_t)(a.y1 + RING_D / 2) };
  const uint16_t r = RING_D / 2;
  const lv_opa_t fade = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);   // drawn by hand: apply the fade itself
  if (fade <= LV_OPA_MIN) return;
  lv_draw_arc_dsc_t d;
  lv_draw_arc_dsc_init(&d);
  d.width = RING_T;
  d.rounded = 0;
  d.color = lv_color_hex(C_PRIMARY);
  d.opa = (lv_opa_t)(LV_OPA_20 * fade / 255);
  lv_draw_arc(ctx, &d, &c, r, 0, 360);
  d.opa = fade;
  int start = 0;
  for (int i = 0; i < s_dialN; ++i) {
    const int end = s_dialEnd[i];
    if (end > start) {
      d.color = lv_color_hex(s_dialColor[i]);
      if (end - start >= 360) lv_draw_arc(ctx, &d, &c, r, 0, 360);
      else lv_draw_arc(ctx, &d, &c, r, (uint16_t)((start + 270) % 360), (uint16_t)((end + 270) % 360));
    }
    if (end > start) start = end;
  }
  if (start > 0) {   // where the walk is now
    const int e0 = start >= 3 ? start - 3 : 0;
    d.color = lv_color_hex(C_TEXT);
    lv_draw_arc(ctx, &d, &c, r, (uint16_t)((e0 + 270) % 360), (uint16_t)((e0 + 3 + 270) % 360));
  }
}

extern "C" void statusInit(void) {
  if (s_root) return;
  s_root = plainObj(lv_layer_top());
  lv_obj_set_size(s_root, SW, SH);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
  // everything on the page sits in the body, so a fade can hide it in one go
  s_body = plainObj(s_root);
  lv_obj_set_size(s_body, SW, SH);

  // icon layout
  s_icon = lv_img_create(s_body);
  lv_obj_set_style_img_recolor_opa(s_icon, LV_OPA_COVER, 0);
  s_logo = lv_img_create(s_body);
  lv_img_set_src(s_logo, &ui_img_1859342711);
  lv_img_set_zoom(s_logo, LOGO_ZOOM);
  lv_img_set_antialias(s_logo, true);

  s_col = plainObj(s_body);
  lv_obj_set_size(s_col, TEXT_W, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_col, 4, 0);
  s_title = textLabel(s_col, &nomad_font_title, C_TEXT, TEXT_W);
  s_detail = textLabel(s_col, &nomad_font_body, C_MUTED, TEXT_W);
  s_big = textLabel(s_col, &nomad_font_big, C_TEXT, TEXT_W);

  s_bar = lv_bar_create(s_body);
  lv_obj_remove_style_all(s_bar);
  lv_obj_set_size(s_bar, BAR_W, BAR_H);
  lv_obj_set_pos(s_bar, BAR_X, BAR_Y);
  lv_obj_set_style_radius(s_bar, BAR_H / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_20, LV_PART_MAIN);
  lv_obj_set_style_radius(s_bar, BAR_H / 2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_anim_time(s_bar, 300, 0);
  lv_bar_set_range(s_bar, 0, 100);

  // the bar when the end isn't known: a piece moving to and fro in the same track
  s_track = plainObj(s_body);
  lv_obj_set_size(s_track, BAR_W, BAR_H);
  lv_obj_set_pos(s_track, BAR_X, BAR_Y);
  lv_obj_set_style_radius(s_track, BAR_H / 2, 0);
  lv_obj_set_style_bg_opa(s_track, LV_OPA_20, 0);
  lv_obj_set_style_clip_corner(s_track, true, 0);
  s_slide = plainObj(s_track);
  lv_obj_set_size(s_slide, SLIDE_W, BAR_H);
  lv_obj_set_style_radius(s_slide, BAR_H / 2, 0);
  lv_obj_set_style_bg_opa(s_slide, LV_OPA_COVER, 0);

  s_foot = textLabel(s_body, &nomad_font_small, C_WARNING, TEXT_W);
  lv_obj_align(s_foot, LV_ALIGN_BOTTOM_MID, 0, -8);   // grows upward when it takes two lines

  // ring layout
  s_ring = plainObj(s_body);
  lv_obj_set_size(s_ring, SW, SH);
  s_ringDial = plainObj(s_ring);
  lv_obj_set_pos(s_ringDial, RING_X, RING_Y);
  lv_obj_set_size(s_ringDial, RING_D, RING_D);
  lv_obj_add_event_cb(s_ringDial, dialDraw, LV_EVENT_DRAW_MAIN, NULL);
  s_ringBig = textLabel(s_ring, &nomad_font_big, C_TEXT, 130);
  lv_obj_align(s_ringBig, LV_ALIGN_TOP_MID, 0, RING_Y + RING_D / 2 - 34);
  s_ringSub = textLabel(s_ring, &nomad_font_small, C_MUTED, 130);
  lv_obj_align(s_ringSub, LV_ALIGN_TOP_MID, 0, RING_Y + RING_D / 2 + 14);
  s_ringTitle = textLabel(s_ring, &nomad_font_head, C_TEXT, TEXT_W);
  lv_obj_align(s_ringTitle, LV_ALIGN_TOP_MID, 0, RING_Y + RING_D + 6);
  // legend: four rows show, the list behind them slides up as more kinds turn up
  s_legBox = plainObj(s_ring);
  lv_obj_set_pos(s_legBox, 0, LEGEND_Y);
  lv_obj_set_size(s_legBox, SW, LEGEND_ROWS * LEGEND_ROW);
  s_legList = plainObj(s_legBox);
  lv_obj_set_size(s_legList, SW, STATUS_SEGS * LEGEND_ROW);
  for (int i = 0; i < STATUS_SEGS; ++i) {
    const int y = i * LEGEND_ROW;
    s_leg[i].icon = lv_img_create(s_legList);
    lv_obj_set_style_img_recolor_opa(s_leg[i].icon, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_leg[i].icon, 14, y);
    s_leg[i].name = textLabel(s_legList, &nomad_font_body, C_TEXT, 112);
    lv_obj_set_style_text_align(s_leg[i].name, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_leg[i].name, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(s_leg[i].name, 46, y);
    s_leg[i].val = textLabel(s_legList, &nomad_font_body, C_MUTED, 60);
    lv_label_set_long_mode(s_leg[i].val, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_leg[i].val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_leg[i].val, SW - 12 - 60, y);
  }

  // the cover for fades, over everything on the page
  s_curtain = plainObj(s_root);
  lv_obj_set_size(s_curtain, SW, SH);
  lv_obj_set_style_bg_color(s_curtain, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s_curtain, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(s_curtain, LV_OBJ_FLAG_HIDDEN);

  lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
  memset(&s_cur, 0, sizeof(s_cur));
  s_shown = false;
}

static void setHidden(lv_obj_t *o, bool hidden) {
  if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void slideAnim(void *obj, int32_t x) { lv_obj_set_x((lv_obj_t *)obj, x); }
static void slideY(void *obj, int32_t y) { lv_obj_set_y((lv_obj_t *)obj, y); }

static void setSliding(bool on) {
  if (on == s_sliding) return;
  s_sliding = on;
  if (on) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_slide);
    lv_anim_set_exec_cb(&a, slideAnim);
    lv_anim_set_values(&a, 0, BAR_W - SLIDE_W);
    lv_anim_set_time(&a, 900);
    lv_anim_set_playback_time(&a, 900);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
  } else {
    lv_anim_del(s_slide, slideAnim);
  }
}

static void gbText(char *out, size_t n, uint32_t mb) {
  const float gb = mb / 1024.0f;
  if (gb < 10.0f) snprintf(out, n, "%.1f", gb);
  else snprintf(out, n, "%lu", (unsigned long)(gb + 0.5f));
}

// ---- the icon layout ----
static void showIconLayout(const StatusMsg *m, bool first) {
  const bool logo = m->icon == SI_LOGO;
  const lv_color_t color = toneColor(m->tone);
  const bool textChanged = first || s_cur.segN || m->icon != s_cur.icon || m->tone != s_cur.tone ||
                           strcmp(m->title, s_cur.title) || strcmp(m->detail, s_cur.detail) ||
                           strcmp(m->big, s_cur.big) || strcmp(m->foot, s_cur.foot) ||
                           m->flags != s_cur.flags || (m->progress < 0) != (s_cur.progress < 0);
  const bool spin = (m->flags & SF_SPIN) && m->progress < 0;
  const bool hasBar = m->progress >= 0 || spin;

  if (textChanged) {
    if (first || s_cur.segN || m->icon != s_cur.icon || m->tone != s_cur.tone) {
      const lv_img_dsc_t *img = iconImage(m->icon);
      setHidden(s_logo, !logo);
      setHidden(s_icon, logo || !img);
      if (img) lv_img_set_src(s_icon, img);
      lv_obj_set_style_img_recolor(s_icon, color, 0);
      lv_obj_set_style_bg_color(s_bar, color, LV_PART_MAIN);
      lv_obj_set_style_bg_color(s_bar, color, LV_PART_INDICATOR);
      lv_obj_set_style_bg_color(s_track, color, 0);
      lv_obj_set_style_bg_color(s_slide, color, 0);
    }
    setHidden(s_col, false);
    if (first || strcmp(m->title, s_cur.title)) lv_label_set_text(s_title, m->title);
    if (first || strcmp(m->detail, s_cur.detail)) lv_label_set_text(s_detail, m->detail);
    if (first || strcmp(m->big, s_cur.big)) {
      // a count too wide for the big digits drops to the title size: libraries get large
      lv_point_t sz;
      lv_txt_get_size(&sz, m->big, &nomad_font_big, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      lv_obj_set_style_text_font(s_big, sz.x > TEXT_W ? &nomad_font_title : &nomad_font_big, 0);
      lv_label_set_text(s_big, m->big);
    }
    setHidden(s_title, !m->title[0]);
    setHidden(s_detail, !m->detail[0]);
    setHidden(s_big, !m->big[0]);
    // with a big number (indexing) the detail is the item going in: bright, always two lines
    // tall so nothing moves while it counts, cut with dots past that
    lv_label_set_long_mode(s_detail, m->big[0] ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(m->big[0] ? C_TEXT : C_MUTED), 0);
    if (m->big[0]) lv_obj_set_height(s_detail, 2 * lv_font_get_line_height(&nomad_font_body) + 2);
    else lv_obj_set_height(s_detail, LV_SIZE_CONTENT);

    const char *foot = m->foot[0] ? m->foot : ((m->flags & SF_KEEP_PLUGGED) ? "Don't unplug" : "");
    lv_label_set_text(s_foot, foot);
    lv_obj_set_style_text_color(s_foot, lv_color_hex(m->foot[0] ? C_MUTED : C_WARNING), 0);
    setHidden(s_foot, !foot[0]);

    // footer on the bottom edge, the bar above it, icon and text centred in what is left
    lv_obj_update_layout(s_root);
    const int footTop = foot[0] ? SH - 8 - lv_obj_get_height(s_foot) : SH - 4;
    // the bar sits at one height on every page, footer or not, so pages swap cleanly
    const int barY = SH - 8 - lv_font_get_line_height(&nomad_font_small) - 10 - BAR_H;
    lv_obj_set_y(s_bar, barY);
    lv_obj_set_y(s_track, barY);
    const int bottom = hasBar ? barY - 8 : footTop - 8;
    const int colH = lv_obj_get_height(s_col);
    // pages with a big number take the smaller drawing of their icon, where there is one
    const bool compact = m->big[0] != 0 && m->icon == SI_FOLDER;
    if (iconImage(m->icon)) lv_img_set_src(s_icon, compact ? &nomad_icon_folder_80 : iconImage(m->icon));
    const int iconH = logo ? LOGO_H : (iconImage(m->icon) ? (compact ? ICON_COMPACT : ICON) : 0);
    const int gap = iconH ? 14 : 0;
    // the icon in the same place on every page; only a page with no icon centres its text
    int top = iconH ? (logo ? ICON_TOP - 12 : ICON_TOP) : (bottom - colH) / 2;
    if (top + iconH + gap + colH > bottom) top = bottom - (iconH + gap + colH);
    if (top < 6) top = 6;
    if (logo) lv_obj_set_pos(s_logo, (SW - 83) / 2, top + (LOGO_H - 110) / 2);
    else lv_obj_set_pos(s_icon, (SW - iconH) / 2, top);
    lv_obj_set_pos(s_col, (SW - TEXT_W) / 2, top + iconH + gap);
  }

  setSliding(spin);
  setHidden(s_track, !spin);
  if (m->progress < 0) {
    setHidden(s_bar, true);
  } else {
    const bool wasHidden = lv_obj_has_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    setHidden(s_bar, false);
    if (first || wasHidden || m->progress != s_cur.progress)
      lv_bar_set_value(s_bar, m->progress, (first || wasHidden) ? LV_ANIM_OFF : LV_ANIM_ON);
  }
}

// ---- the card ring ----
static void showRingLayout(const StatusMsg *m, bool first) {
  setSliding(false);
  const uint32_t cap = m->capMb ? m->capMb : 1;
  uint32_t found = 0;
  for (int i = 0; i < m->segN && i < STATUS_SEGS; ++i) found += m->seg[i].mb;

  // pieces in walk order, clockwise from the top; redrawn only when an angle or colour moves
  bool dialChanged = first || !s_cur.segN || m->segN != s_dialN;
  uint32_t run = 0;
  for (int i = 0; i < m->segN && i < STATUS_SEGS; ++i) {
    run += m->seg[i].mb;
    int end = (int)((uint64_t)run * 360 / cap);
    if (end > 360) end = 360;
    const uint32_t col = s_kindColor[m->seg[i].kind % SK_COUNT];
    if (end != s_dialEnd[i] || col != s_dialColor[i]) dialChanged = true;
    s_dialEnd[i] = (int16_t)end;
    s_dialColor[i] = col;
  }
  s_dialN = m->segN < STATUS_SEGS ? m->segN : STATUS_SEGS;
  if (dialChanged) lv_obj_invalidate(s_ringDial);

  char buf[24];
  gbText(buf, sizeof(buf), found);
  lv_label_set_text(s_ringBig, buf);
  char cap1[16];
  gbText(cap1, sizeof(cap1), m->capMb);
  snprintf(buf, sizeof(buf), "of %s GB", cap1);
  lv_label_set_text(s_ringSub, buf);
  if (first || strcmp(m->title, s_cur.title) || !s_cur.segN) lv_label_set_text(s_ringTitle, m->title);

  // legend: in the order the walk found them, the newest at the bottom of the four rows
  int n = 0;
  bool anySmall = false;   // one long name drops every name to the small size, so rows match
  for (int i = 0; i < m->segN && i < STATUS_SEGS; ++i) {
    if (!m->seg[i].mb) continue;
    LegendCell &c = s_leg[n++];
    const uint8_t k = m->seg[i].kind % SK_COUNT;
    lv_img_set_src(c.icon, kKinds[k].icon);
    lv_obj_set_style_img_recolor(c.icon, lv_color_hex(s_kindColor[k]), 0);
    gbText(buf, sizeof(buf), m->seg[i].mb);
    lv_label_set_text(c.val, buf);
    // a long name beside a number drops to the small size rather than run into it
    lv_point_t nsz, vsz;
    lv_txt_get_size(&nsz, kKinds[k].name, &nomad_font_body, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_txt_get_size(&vsz, buf, &nomad_font_body, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (nsz.x > SW - 12 - vsz.x - 6 - 46) anySmall = true;
    lv_label_set_text(c.name, kKinds[k].name);
    setHidden(c.icon, false); setHidden(c.name, false); setHidden(c.val, false);
  }
  for (int i = n; i < STATUS_SEGS; ++i) {
    setHidden(s_leg[i].icon, true); setHidden(s_leg[i].name, true); setHidden(s_leg[i].val, true);
  }
  for (int i = 0; i < n; ++i) {
    lv_obj_set_style_text_font(s_leg[i].name, anySmall ? &nomad_font_small : &nomad_font_body, 0);
    lv_obj_set_y(s_leg[i].name, i * LEGEND_ROW + (anySmall ? 3 : 0));
  }
  const int scroll = n > LEGEND_ROWS ? (n - LEGEND_ROWS) * LEGEND_ROW : 0;
  if (first || !s_cur.segN) {
    lv_anim_del(s_legList, slideY);
    lv_obj_set_y(s_legList, -scroll);
  } else if (scroll != s_legScroll) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_legList);
    lv_anim_set_exec_cb(&a, slideY);
    lv_anim_set_values(&a, -s_legScroll, -scroll);
    lv_anim_set_time(&a, 350);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
  }
  s_legScroll = scroll;
}

static void applyMsg(const StatusMsg *m) {
  const bool first = !s_shown;
  const bool ring = m->segN > 0;
  if (first || ring != (s_cur.segN > 0)) {
    setHidden(s_ring, !ring);
    lv_obj_t *const iconParts[] = { s_icon, s_logo, s_col, s_bar, s_track, s_foot };
    if (ring) for (lv_obj_t *o : iconParts) setHidden(o, true);
  }
  if (ring) showRingLayout(m, first);
  else showIconLayout(m, first);
  setHidden(s_root, false);
  s_cur = *m;
  s_shown = true;
}

static void coverAnim(void *, int32_t v) {
  lv_obj_set_style_bg_opa(s_curtain, (lv_opa_t)v, 0);
  if (v <= LV_OPA_MIN) lv_obj_add_flag(s_curtain, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_clear_flag(s_curtain, LV_OBJ_FLAG_HIDDEN);
}
static void bgAnim(void *, int32_t v) { lv_obj_set_style_bg_opa(s_root, (lv_opa_t)v, 0); }

static void run(lv_anim_exec_xcb_t cb, void *var, int from, int to, uint32_t ms, lv_anim_ready_cb_t done) {
  lv_anim_del(var, cb);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, var);
  lv_anim_set_exec_cb(&a, cb);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_time(&a, ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  if (done) lv_anim_set_ready_cb(&a, done);
  cb(var, from);
  lv_anim_start(&a);
}

static void stopFades() {
  lv_anim_del(s_curtain, coverAnim);
  lv_anim_del(s_root, bgAnim);
  coverAnim(s_curtain, LV_OPA_TRANSP);
  bgAnim(s_root, LV_OPA_COVER);
  lv_obj_clear_flag(s_body, LV_OBJ_FLAG_HIDDEN);
  s_phase = PH_IDLE;
  s_hasPending = false;
}

// a different page (layout, icon, colour or title): through the cover. The same page updates in place.
static bool pageChange(const StatusMsg *m) {
  if ((m->segN > 0) != (s_cur.segN > 0)) return true;
  if (m->segN) return false;
  return m->icon != s_cur.icon || m->tone != s_cur.tone || strcmp(m->title, s_cur.title) != 0;
}

static void inDone(lv_anim_t *) { s_phase = PH_IDLE; }

static void coverUp(lv_anim_t *) {          // content hidden: change it, uncover
  if (s_hasPending) { s_hasPending = false; applyMsg(&s_pending); }
  s_phase = PH_CROSS_IN;
  run(coverAnim, s_curtain, LV_OPA_COVER, LV_OPA_TRANSP, COVER_MS, inDone);
}

static void bgUp(lv_anim_t *) {             // page colour over the main screen: bring the content in
  lv_obj_clear_flag(s_body, LV_OBJ_FLAG_HIDDEN);
  s_phase = PH_SHOW_IN;
  run(coverAnim, s_curtain, LV_OPA_COVER, LV_OPA_TRANSP, COVER_MS, inDone);
}

static void bgDown(lv_anim_t *) {           // back to the main screen
  s_phase = PH_IDLE;
  setSliding(false);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
  bgAnim(s_root, LV_OPA_COVER);
  coverAnim(s_curtain, LV_OPA_TRANSP);
  lv_obj_clear_flag(s_body, LV_OBJ_FLAG_HIDDEN);
}

static void contentDown(lv_anim_t *) {      // content covered: fade the page colour away
  lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);
  coverAnim(s_curtain, LV_OPA_TRANSP);
  s_phase = PH_HIDE_BG;
  run(bgAnim, s_root, LV_OPA_COVER, LV_OPA_TRANSP, BG_MS, bgDown);
}

extern "C" void statusAnimate(int on) {
  s_animate = on != 0;
  if (!s_animate && s_root) stopFades();
}

extern "C" void statusShowNow(const StatusMsg *m) {
  if (!s_root || !m) return;
  if (s_phase != PH_IDLE) stopFades();
  applyMsg(m);
}

extern "C" void statusShow(const StatusMsg *m) {
  if (!s_root || !m) return;
  if (!s_animate) { statusShowNow(m); return; }
  if (s_phase == PH_HIDE_OUT || s_phase == PH_HIDE_BG) {   // asked back while going: straight back in
    stopFades();
    applyMsg(m);
    return;
  }
  if (!s_shown) {
    applyMsg(m);
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_HIDDEN);
    s_phase = PH_SHOW_BG;
    run(bgAnim, s_root, LV_OPA_TRANSP, LV_OPA_COVER, BG_MS, bgUp);
    return;
  }
  if (s_phase != PH_IDLE) { s_pending = *m; s_hasPending = true; return; }   // the newest wins
  if (pageChange(m)) {
    s_pending = *m;
    s_hasPending = true;
    s_phase = PH_CROSS_OUT;
    run(coverAnim, s_curtain, LV_OPA_TRANSP, LV_OPA_COVER, COVER_MS, coverUp);
    return;
  }
  applyMsg(m);
}

extern "C" void statusHide(void) {
  if (!s_root || !s_shown) return;
  s_shown = false;
  if (!s_animate) {
    stopFades();
    setSliding(false);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  s_hasPending = false;
  const int from = lv_obj_get_style_bg_opa(s_curtain, 0);
  lv_anim_del(s_root, bgAnim);
  bgAnim(s_root, LV_OPA_COVER);
  lv_obj_clear_flag(s_body, LV_OBJ_FLAG_HIDDEN);
  s_phase = PH_HIDE_OUT;
  run(coverAnim, s_curtain, from, LV_OPA_COVER, COVER_MS, contentDown);
}

extern "C" int statusVisible(void) { return s_shown ? 1 : 0; }
extern "C" const char *statusTitle(void) { return s_shown ? s_cur.title : ""; }
