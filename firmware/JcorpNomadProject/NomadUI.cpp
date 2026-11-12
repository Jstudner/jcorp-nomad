// NomadUI.cpp - the web interface served out of flash. See NomadUI.h for the why.

#include "NomadUI.h"
#include "nomad_board.h"
#include "NomadSD.h"   // outside the guard: the fallback path returns the card's FS too

#if NOMAD_HAS_FLASH_UI

#include <LittleFS.h>
#include <esp_heap_caps.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SD_MMC NomadSD

extern void webLog(const String &message, const String &type);
extern void webLogf(const String &type, const char *format, ...);

static bool s_mounted = false;
static volatile bool s_syncing = false;
// Whether flash is the authoritative interface. index.html is the marker, cached so it
// isn't stat-ed per request. See nomadUiHas() for why this gate exists.
static bool s_authoritative = false;

// The interface is everything the browser loads that isn't somebody's media. An explicit
// rule, so a card with films loose at the top level can't put a 4 GB file in the partition.
static bool hasExt(const String &lower, const char *ext) {
  return lower.endsWith(ext);
}

bool nomadUiIsInterfacePath(const String &path) {
  if (!path.startsWith("/")) return false;
  // The vendored browser libraries: pdf.worker, jszip, pako, the xz decoder.
  if (path.startsWith("/assets/")) return true;

  // Otherwise only root-level files, so /Movies/Foo/index.html is never mistaken for ours.
  if (path.indexOf('/', 1) != -1) return false;

  String lower = path;
  lower.toLowerCase();
  if (hasExt(lower, ".html") || hasExt(lower, ".css") || hasExt(lower, ".js")) return true;
  if (path == "/default-themes.json" || path == "/favicon.ico") return true;
  if (path == "/Logo.png" || path == "/placeholder.jpg") return true;
  // .system-ui.json and anything under /config are this device's own settings and belong
  // on the card, where the admin panel writes them.
  return false;
}

bool nomadUiBelongsInFlash(const String &path, uint64_t size) {
  if (!nomadUiIsInterfacePath(path)) return false;
  if (path.startsWith("/assets/maps/")) return false;
  // an Emscripten engine's memory file goes where its .js goes (build-p4.sh does the same)
  if (path.endsWith(".js.mem")) {
    const String js = path.substring(0, path.length() - 4);
    return SD_MMC.exists(js.c_str()) ? nomadUiBelongsInFlash(js, SD_MMC.fileSize64(js.c_str()))
                                     : size <= NOMAD_UI_FLASH_MAX_FILE;
  }
  return size <= NOMAD_UI_FLASH_MAX_FILE;
}

// Every path flash holds, as "\n/a\n/b\n" in PSRAM (a few KB for ~130 files): once as
// spelled and once lower-cased, so neither "does flash have it" nor "is this a case
// variant" touches LittleFS per request. LittleFS.exists() opens a newlib FILE, whose lock
// allocation aborts instead of failing when internal RAM is short, so with a few stuck
// page readers any request could reset the board.
static char *s_index = nullptr;
static char *s_lowerIndex = nullptr;
static portMUX_TYPE s_indexMux = portMUX_INITIALIZER_UNLOCKED;

static void indexDir(const char *dir, String &out, int depth) {
  if (depth > 6) return;
  File d = LittleFS.open(dir);
  if (!d || !d.isDirectory()) { if (d) d.close(); return; }
  File e;
  while ((e = d.openNextFile())) {
    String p = String(e.path());
    const bool isDir = e.isDirectory();
    e.close();
    if (isDir) { indexDir(p.c_str(), out, depth + 1); continue; }
    out += p;
    out += '\n';
  }
  d.close();
}

static char *indexCopy(const String &all) {
  char *buf = (char *)heap_caps_malloc(all.length() + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (char *)malloc(all.length() + 1);
  if (buf) memcpy(buf, all.c_str(), all.length() + 1);
  return buf;
}

static void rebuildLowerIndex() {
  String all = "\n";
  if (s_mounted) indexDir("/", all, 0);
  char *exact = indexCopy(all);
  all.toLowerCase();
  char *lower = indexCopy(all);
  portENTER_CRITICAL(&s_indexMux);
  char *oldExact = s_index, *oldLower = s_lowerIndex;
  s_index = exact;
  s_lowerIndex = lower;
  portEXIT_CRITICAL(&s_indexMux);
  free(oldExact);   // heap_caps memory frees with free()
  free(oldLower);
}

// Under the lock, so a rebuild can't free the buffer mid-search (a few KB: microseconds)
static bool indexHas(char *const &index, const String &key) {
  portENTER_CRITICAL(&s_indexMux);
  const bool hit = index && strstr(index, key.c_str()) != nullptr;
  portEXIT_CRITICAL(&s_indexMux);
  return hit;
}

static void refreshAuthoritative() {
  s_authoritative = s_mounted && LittleFS.exists("/index.html");
  rebuildLowerIndex();
}

bool nomadUiShadows(const String &path) {
  if (!s_mounted || !s_authoritative) return false;
  String key = "\n" + path + "\n";
  if (indexHas(s_index, key)) return false;          // flash has it as spelled: not a variant
  key.toLowerCase();
  return indexHas(s_lowerIndex, key);
}

bool nomadUiBegin() {
  if (s_mounted) return true;
  // formatOnFail: a fresh board has an empty partition, and formatting it is the only way
  // to get a usable filesystem. Nothing here can be lost, the card is the source.
  if (!LittleFS.begin(true, "/uiflash", 20, NOMAD_UI_PART_LABEL)) {
    log_e("NomadUI: could not mount or format the UI partition");
    return false;
  }
  s_mounted = true;
  refreshAuthoritative();
  log_i("NomadUI: partition mounted, %u of %u bytes used",
        (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  return true;
}

bool nomadUiActive() {
  return s_authoritative;
}

static char s_version[24] = {0};

const char *nomadUiVersion() {
  if (!s_mounted) return "";
  if (s_version[0]) return s_version;
  File f = LittleFS.open("/.uiversion", "r");
  if (!f) return "";
  size_t n = f.readBytes(s_version, sizeof(s_version) - 1);
  f.close();
  while (n && (s_version[n - 1] == '\n' || s_version[n - 1] == '\r')) n--;
  s_version[n] = 0;
  return s_version;
}

bool nomadUiHas(const String &path) {
  // Gated on the whole interface being authoritative, not just on this one file existing.
  // A half-copied flash (say a sync that ran out of room) leaves real files behind that
  // would keep winning over the card's complete set. Dropping index.html marks flash
  // non-authoritative, and this check enforces it.
  if (!s_mounted || !s_authoritative) return false;
  return indexHas(s_index, "\n" + path + "\n");
}

fs::FS &nomadUiFS(const String &path) {
  if (nomadUiHas(path)) return LittleFS;
  return SD_MMC;
}

uint64_t nomadUiTotalBytes() {
  return s_mounted ? LittleFS.totalBytes() : 0;
}

uint64_t nomadUiUsedBytes() {
  return s_mounted ? LittleFS.usedBytes() : 0;
}

static uint32_t countDir(const char *dir) {
  uint32_t n = 0;
  File d = LittleFS.open(dir);
  if (!d || !d.isDirectory()) {
    if (d) d.close();
    return 0;
  }
  File e;
  while ((e = d.openNextFile())) {
    if (e.isDirectory()) {
      String sub = String(e.path());
      e.close();
      n += countDir(sub.c_str());
    } else {
      n++;
      e.close();
    }
  }
  d.close();
  return n;
}

uint32_t nomadUiFileCount() {
  return s_mounted ? countDir("/") : 0;
}

fs::FS *nomadUiFlash() {
  return (s_mounted && s_authoritative) ? &LittleFS : nullptr;
}

uint64_t nomadUiFreeBytes() {
  if (!s_mounted) return 0;
  const uint64_t t = LittleFS.totalBytes(), u = LittleFS.usedBytes();
  return u < t ? t - u : 0;
}

void nomadUiNoteUpdated() {
  if (!s_mounted) return;
  String cur = nomadUiVersion();
  int n = 0;
  const int plus = cur.indexOf("+u");
  if (plus >= 0) {
    n = cur.substring(plus + 2).toInt();
    cur = cur.substring(0, plus);
  }
  if (!cur.length()) cur = "card";        // a flash copy synced off a card has no image id
  const String next = cur + "+u" + String(n + 1);
  File f = LittleFS.open("/.uiversion", "w");
  if (f) { f.print(next); f.print('\n'); f.close(); }
  s_version[0] = 0;
  refreshAuthoritative();
}

bool nomadUiSyncing() {
  return s_syncing;
}

// Sync. Copying the interface off the card is a fallback for a board flashed without
// the UI image (build-p4.sh makes it from SD_Card_Template/), and nothing calls it on its
// own. Don't run it automatically at boot: a card with an older frontend would take over
// the partition for good.

// Copy one file card -> flash, 4 KB at a time through a heap buffer. This runs while the
// server is up, so it must never hold a big allocation.
static bool copyOne(const String &path, uint32_t &bytesOut) {
  File src = SD_MMC.open(path, "r");
  if (!src || src.isDirectory()) {
    if (src) src.close();
    return false;
  }
  // LittleFS has no mkdir -p on open, so make the parent first.
  int slash = path.lastIndexOf('/');
  if (slash > 0) {
    String parent = path.substring(0, slash);
    if (!LittleFS.exists(parent)) LittleFS.mkdir(parent);
  }
  File dst = LittleFS.open(path, "w", true);
  if (!dst) {
    src.close();
    return false;
  }
  uint8_t *buf = (uint8_t *)malloc(4096);
  if (!buf) {
    src.close();
    dst.close();
    return false;
  }
  bool ok = true;
  uint32_t total = 0;
  while (src.available()) {
    size_t n = src.read(buf, 4096);
    if (n == 0) break;
    if (dst.write(buf, n) != n) {
      ok = false;
      break;
    }
    total += n;
    // Erasing a flash block stalls whoever is reading media, yield between chunks.
    vTaskDelay(1);
  }
  free(buf);
  dst.close();
  src.close();
  if (!ok) LittleFS.remove(path);
  bytesOut = total;
  return ok;
}

// Recursive card -> flash copy for a directory of interface files.
static void copyTree(const String &dir, int depth, uint32_t &files, uint32_t &failed, uint64_t &bytes) {
  // Both of these abandon a whole subtree. Counting them is what stops a sync that copied
  // nothing from reporting success and leaving flash authoritative over a complete card.
  if (depth > 4) {
    failed++;
    webLogf("warning", "[UIFLASH] %s is nested too deep to copy", dir.c_str());
    return;
  }
  File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) {
    if (d) d.close();
    failed++;
    webLogf("warning", "[UIFLASH] could not read %s", dir.c_str());
    return;
  }
  if (!LittleFS.exists(dir)) LittleFS.mkdir(dir);
  File e;
  while ((e = d.openNextFile())) {
    String p = String(e.path());
    if (!p.startsWith("/")) p = "/" + p;
    bool isDir = e.isDirectory();
    e.close();
    if (isDir) {
      if (p.startsWith("/assets/maps")) continue;     // stays on the card (NomadUI.h)
      copyTree(p, depth + 1, files, failed, bytes);
      continue;
    }
    if (!nomadUiBelongsInFlash(p, SD_MMC.fileSize64(p.c_str()))) continue;
    uint32_t n = 0;
    if (copyOne(p, n)) {
      files++;
      bytes += n;
    } else {
      failed++;
      webLogf("warning", "[UIFLASH] could not copy %s", p.c_str());
    }
  }
  d.close();
}

static void uiSyncTask(void *) {
  webLog("[UIFLASH] copying the interface off the card into flash", "info");
  uint32_t files = 0, failed = 0;
  uint64_t bytes = 0;

  // Root-level interface files.
  File root = SD_MMC.open("/");
  if (root && root.isDirectory()) {
    File e;
    while ((e = root.openNextFile())) {
      if (!e.isDirectory()) {
        String p = String(e.path());
        if (!p.startsWith("/")) p = "/" + p;
        e.close();
        if (nomadUiBelongsInFlash(p, SD_MMC.fileSize64(p.c_str()))) {
          uint32_t n = 0;
          if (copyOne(p, n)) {
            files++;
            bytes += n;
          } else {
            failed++;
            webLogf("warning", "[UIFLASH] could not copy %s", p.c_str());
          }
        }
      } else {
        e.close();
      }
    }
    root.close();
  }

  // The whole assets tree, subdirectories included. It isn't flat (leaflet ships as
  // assets/leaflet/ with its own images/).
  copyTree("/assets", 0, files, failed, bytes);

  if (failed || !LittleFS.exists("/index.html")) {
    webLogf("error", "[UIFLASH] sync incomplete: %u files copied, %u failed. The card keeps serving the interface.",
            (unsigned)files, (unsigned)failed);
    // A half-copied interface is worse than none, because the half that landed wins over
    // the card. index.html is the marker nomadUiActive() reads, so drop it and the flash
    // copy stops being authoritative even though the files are still there.
    if (failed) LittleFS.remove("/index.html");
    refreshAuthoritative();
  } else {
    refreshAuthoritative();
    webLogf("success", "[UIFLASH] %u files, %.1f MB now served from flash; the card's copies are ignored",
            (unsigned)files, (double)bytes / (1024.0 * 1024.0));
  }
  s_syncing = false;
  vTaskDelete(NULL);
}

bool nomadUiStartSync() {
  if (!s_mounted || s_syncing) return false;
  if (!SD_MMC.exists("/index.html")) return false;   // nothing to copy from
  s_syncing = true;
  // Own task, not one of the PSRAM-stack pool: it has to outlive a streaming shutdown, and
  // it's short-lived enough that the stack cost doesn't matter.
  if (xTaskCreatePinnedToCore(uiSyncTask, "UiSync", 8192, nullptr, 1, nullptr, 1) != pdPASS) {
    s_syncing = false;
    return false;
  }
  return true;
}

// Recursive, because copyTree creates nested directories (assets/leaflet/images), and
// remove() or rmdir() on a directory that isn't empty fails.
static void wipeTree(const String &dir, int depth) {
  if (depth > 4) return;
  File d = LittleFS.open(dir);
  if (!d || !d.isDirectory()) { if (d) d.close(); return; }
  String victims[64];
  bool dirs[64];
  int n = 0;
  File e;
  while ((e = d.openNextFile()) && n < 64) {
    victims[n] = String(e.path());
    dirs[n] = e.isDirectory();
    n++;
    e.close();
  }
  bool more = (n == 64);
  d.close();
  for (int i = 0; i < n; i++) {
    if (dirs[i]) { wipeTree(victims[i], depth + 1); LittleFS.rmdir(victims[i]); }
    else LittleFS.remove(victims[i]);
  }
  if (more) wipeTree(dir, depth);   // the directory held more than one batch
}

bool nomadUiClear() {
  if (!s_mounted || s_syncing) return false;
  // index.html first: if power goes at the wrong moment the interface is already
  // non-authoritative and the card takes over, rather than half of it winning.
  LittleFS.remove("/index.html");
  refreshAuthoritative();
  wipeTree("/", 0);
  refreshAuthoritative();
  webLog("[UIFLASH] flash copy cleared; the card serves the interface again", "info");
  return true;
}

#else  // no UI partition on this board

bool nomadUiBegin() { return false; }
bool nomadUiActive() { return false; }
bool nomadUiHas(const String &) { return false; }
fs::FS &nomadUiFS(const String &) { return NomadSD; }
bool nomadUiStartSync() { return false; }
bool nomadUiSyncing() { return false; }
bool nomadUiClear() { return false; }
const char *nomadUiVersion() { return ""; }
uint64_t nomadUiTotalBytes() { return 0; }
uint64_t nomadUiUsedBytes() { return 0; }
uint32_t nomadUiFileCount() { return 0; }
bool nomadUiIsInterfacePath(const String &) { return false; }
bool nomadUiBelongsInFlash(const String &, uint64_t) { return false; }
bool nomadUiShadows(const String &) { return false; }
fs::FS *nomadUiFlash() { return nullptr; }
uint64_t nomadUiFreeBytes() { return 0; }
void nomadUiNoteUpdated() {}

#endif  // NOMAD_HAS_FLASH_UI
