// NomadStatus.h - the full-screen status pages on Nomad's screen: starting up, indexing, the card
// scan, USB mode, WiFi Mode, card trouble, updates, shutting down.
//
// Two layouts. Most states: a large icon, a title, a detail line, an optional big number, a
// full-width bar (filled to the progress, or a moving piece when the end isn't known) and a
// footer line. The card scan: a ring of what has been found so far, one coloured piece per kind
// of media, the amount in the middle, and a legend of the kinds under it.
//
// No Arduino or ESP-IDF here, only LVGL, so tools/screen/preview can draw the same pages on a PC.
// Every call has to come from the task that owns LVGL (or hold the sketch's LvglLock).
#pragma once
#include <stdint.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

enum StatusIcon : uint8_t {
  SI_NONE = 0, SI_LOGO, SI_CARD, SI_FOLDER, SI_REFRESH, SI_USB, SI_WIFI, SI_WIFI_OFF,
  SI_POWER, SI_DOWNLOAD, SI_WARNING, SI_CHECK
};
// colour of the icon and bar: the page theme's primary, success, warning, danger
enum StatusTone : uint8_t { ST_WORK = 0, ST_GOOD, ST_WARN, ST_BAD };
enum : uint8_t {
  SF_SPIN = 1,          // under way with no known end: the bar's piece moves
  SF_KEEP_PLUGGED = 2,  // footer "Don't unplug" in the warning colour (unless foot is set)
};
// kinds of media on the card, by top folder
enum StatusKind : uint8_t {
  SK_MOVIES = 0, SK_SHOWS, SK_MUSIC, SK_BOOKS, SK_GALLERY, SK_GAMES, SK_MAPS, SK_ARCHIVE,
  SK_COOKBOOK, SK_WORKSHOP, SK_TRANSLATE, SK_FILES, SK_OTHER, SK_COUNT
};
#define STATUS_SEGS 13   // one piece for every kind, so a big kind is never folded into Other

struct StatusSeg {
  uint8_t kind;         // StatusKind
  uint32_t mb;          // found so far, MiB
};

struct StatusMsg {
  uint8_t icon;         // StatusIcon
  uint8_t tone;         // StatusTone
  uint8_t flags;        // SF_*
  int8_t progress;      // 0 to 100 fills the bar, -1 none (or a moving piece with SF_SPIN)
  uint8_t segN;         // > 0: the card ring instead of the icon layout
  char title[28];
  char detail[64];      // one or two lines
  char big[16];         // the big number: digits , . % only
  char foot[32];        // overrides the SF_KEEP_PLUGGED footer
  uint32_t capMb;       // the ring: the card's size, MiB
  StatusSeg seg[STATUS_SEGS];   // the ring's pieces, in the order the walk found them
};

// Fill a message (no ring). NULL strings are empty.
void statusMake(StatusMsg *m, uint8_t icon, uint8_t tone, const char *title, const char *detail,
                int progress, uint8_t flags, const char *foot);

// "/Movies" -> SK_MOVIES. Anything not a Nomad folder is SK_OTHER.
uint8_t statusKindForFolder(const char *topFolder);
// Build the pages (hidden) on LVGL's top layer. Call once, after the display is up.
void statusInit(void);
// Show or update. Only what changed is redrawn. With transitions on, a page fades in, a different
// page cross-fades, and the same page updates in place.
void statusShow(const StatusMsg *m);
// Show at once, no fade: for setup(), USB mode and the moments right before a restart.
void statusShowNow(const StatusMsg *m);
// Transitions on once something calls lv_timer_handler often (the sketch's LVGL task).
void statusAnimate(int on);
void statusHide(void);
int statusVisible(void);
// The title of the page showing, "" when none.
const char *statusTitle(void);

// "1234567" -> "1,234,567"
void statusThousands(char *out, unsigned outLen, unsigned long n);

#ifdef __cplusplus
}
#endif
