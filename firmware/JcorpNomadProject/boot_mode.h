// boot_mode.h
#ifndef BOOT_MODE_H
#define BOOT_MODE_H

#include <Arduino.h>
#include <Preferences.h>

// Boot modes
#define MEDIA_MODE 0
#define USB_MODE   1

// NVS namespace/key
static constexpr const char* BOOT_NS  = "boot";
static constexpr const char* BOOT_KEY = "mode";

static inline void set_boot_mode(int mode) {
  Preferences prefs;
  if (prefs.begin(BOOT_NS, false)) {
    prefs.putUChar(BOOT_KEY, static_cast<uint8_t>(mode));
    prefs.end();
  }
}

static inline int get_boot_mode() {
  Preferences prefs;
  uint8_t mode = MEDIA_MODE;
  if (prefs.begin(BOOT_NS, true)) {
    mode = prefs.getUChar(BOOT_KEY, MEDIA_MODE);
    prefs.end();
  }
  return mode;
}

static inline void clear_boot_mode() {
  set_boot_mode(MEDIA_MODE);
}

// set on USB exit if the host wrote data, so the next boot knows the card changed
// outside our own upload/rename/delete and forces a rescan. kept in NVS not on the
// card - the FAT view is stale right after USB and writing then can corrupt the fs
static constexpr const char* REINDEX_KEY = "needs_reindex";

static inline void set_needs_reindex_flag() {
  Preferences prefs;
  if (prefs.begin(BOOT_NS, false)) {
    prefs.putBool(REINDEX_KEY, true);
    prefs.end();
  }
}

static inline bool get_needs_reindex_flag() {
  Preferences prefs;
  bool needsReindex = false;
  if (prefs.begin(BOOT_NS, true)) {
    needsReindex = prefs.getBool(REINDEX_KEY, false);
    prefs.end();
  }
  return needsReindex;
}

static inline void clear_needs_reindex_flag() {
  Preferences prefs;
  if (prefs.begin(BOOT_NS, false)) {
    prefs.putBool(REINDEX_KEY, false);
    prefs.end();
  }
}

#endif // BOOT_MODE_H
