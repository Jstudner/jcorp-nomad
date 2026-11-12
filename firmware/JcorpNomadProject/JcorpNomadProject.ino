// Jcorp Nomad Backend - exFAT edition
//<!-- Version 4.8 -->
//
// Board variant: set it in Display_ST7789.h, NOT here.
//
//  USB-A  Waveshare ESP32-S3-LCD-1.47   backlight GPIO 48   BOARD_USB_C 0  (default)
//  USB-C  Waveshare ESP32-S3-LCD-1.47B  backlight GPIO 46   BOARD_USB_C 1
//
// The backlight pin is the only difference between the two. A #define here never reaches
// Display_ST7789.cpp, so a USB-C board would get GPIO 48 and a dark screen. Edit the line
// in Display_ST7789.h or build with -DBOARD_USB_C=1 (tools/flash-nomad.sh --usbc).
#include <Arduino.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_heap_caps.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
// NomadSD replaces SD_MMC: same pins, SdFat underneath, so exFAT, FAT32 and FAT16 all
// mount. Included early because StreamHandle below holds a NomadFile64. NomadSD.h hides
// SdFat's headers, its global `typedef FsFile File` would collide with fs::File.
#include "NomadSD.h"
#include "NomadDLNA.h"
#define SD_MMC NomadSD
#ifndef SD
#define SD NomadSD
#endif
#include "NomadDNS.h"
#include "NomadGuardWeb.h" // path rules for every request: /config, the interface, guest writes
#include "NomadReadAhead.h" // aligned card reads for media fills (off unless switched on)
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <map>
#include <time.h>
#include "Display_ST7789.h"
#include "LVGL_Driver.h"
#include "ui.h" 
#include "RGB_lamp.h"
#include <SPIFFS.h>
#include <Preferences.h>
#include "esp_wifi.h"
#include "esp_netif.h"      // esp_netif_dhcps_*: restart softAP DHCP if it stays down (issue #126)
#include "esp_heap_caps.h"  // heap_caps_get_*: DHCP-start heap logging
#if defined(ARDUINO_ARCH_ESP32)
  #include "soc/soc.h"
  #include "soc/rtc_cntl_reg.h"
  #include "esp_system.h"
  #include "esp_core_dump.h"
#endif
#include "esp32-hal-tinyusb.h"  // usb_persist_restart, for getting into ROM download mode
#include "usb_mode.h"
#include "boot_mode.h" // library for firmware switching
#include "freertos/idf_additions.h"   // xTaskCreatePinnedToCoreWithCaps / vTaskDeleteWithCaps


struct BreakdownStats {
  uint64_t bytes = 0;
  uint32_t files = 0;
  uint32_t dirs = 0;
};

void handleRangeRequest(AsyncWebServerRequest *request);
#ifndef NOMAD_AP_HT20
#define NOMAD_AP_HT20 0          // 1 = hotspot on 20 MHz channels
#endif
#ifndef NOMAD_ROUTES_PSRAM
#define NOMAD_ROUTES_PSRAM 0     // 1 = route tables in PSRAM, frees about 23 KB of DMA memory on an S3
#endif
#ifndef NOMAD_MEDIA_SNDBUF
#define NOMAD_MEDIA_SNDBUF 0     // max media socket send buffer in bytes, 0 leaves the core's 5,744
#endif
#if NOMAD_BENCH
static bool benchApplySndBuf(AsyncWebServerRequest *request);   // NomadBench.h
static void benchApplyBootRadio();                               // NomadBench.h
#endif
String urlDecode(const String& str);
void enqueueIndexUpdateForPath(const String& path);
void RGB_SetMode(uint8_t mode);
void applyRGBSettings();
String sanitizeToken(const String &s);
bool saveSettings();
String humanSize(size_t bytes);
extern void usb_setup();
extern void usb_loop();
#define BOOT_BUTTON_PIN 0
#include <vector>
#include <algorithm>
#include <atomic>
#ifndef GLOBAL_INDEX_BUF
#define GLOBAL_INDEX_BUF 1024
#endif
enum { HALF_INDEX_BUF = GLOBAL_INDEX_BUF / 2 };

static char g_lineBuf[GLOBAL_INDEX_BUF];
struct LvglMsg {
  char text[80];
  bool show;
};
static QueueHandle_t lvglQueue = NULL;

// LVGL isn't thread-safe and three tasks touch it: StreamingTask pumps lv_timer_handler(),
// UiTask updates labels and the SD bar, loop() paints the flash-mode notice. Two inside it
// at once can panic the allocator, so every lv_* call after setup() takes this lock.
// Created in setup() before any task starts.
static SemaphoreHandle_t lvglMutex = NULL;
struct LvglLock {
  bool held;
  LvglLock() : held(lvglMutex && xSemaphoreTakeRecursive(lvglMutex, portMAX_DELAY) == pdTRUE) {}
  ~LvglLock() { if (held) xSemaphoreGiveRecursive(lvglMutex); }
};

// Background tasks with stacks in PSRAM, to save internal RAM (about 190 KB of heap for
// everything). The indexer, scans, boot coordinator and config writer don't touch DMA or
// write flash, so PSRAM stacks are fine for them (SDMMC uses its own bounce buffer).
// Tasks that drive SPI/LVGL or write NVS keep internal stacks: StreamingTask, UiTask,
// loop, async_tcp.
//
// A task made here must exit through nomadTaskExit(), never vTaskDelete(NULL). A WithCaps
// task has to be deleted with vTaskDeleteWithCaps from another task, so it parks itself on
// the reap queue and suspends, and loop() deletes it. Falls back to an internal stack if
// PSRAM allocation fails.
static QueueHandle_t g_taskReapQueue = NULL;

static BaseType_t nomadCreateTask(TaskFunction_t fn, const char *name, uint32_t stackBytes,
                                  void *arg, UBaseType_t prio, TaskHandle_t *handle, BaseType_t core) {
  BaseType_t r = xTaskCreatePinnedToCoreWithCaps(fn, name, stackBytes, arg, prio, handle, core,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (r == pdPASS) return r;
  Serial.printf("[Task] %s: no PSRAM for its stack, using internal RAM\n", name);
  return xTaskCreatePinnedToCoreWithCaps(fn, name, stackBytes, arg, prio, handle, core,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void nomadTaskExit() {
  TaskHandle_t me = xTaskGetCurrentTaskHandle();
  if (g_taskReapQueue && xQueueSend(g_taskReapQueue, &me, pdMS_TO_TICKS(200)) == pdTRUE) {
    vTaskSuspend(NULL);          // loop() reaps us from here
  }
  // no queue (or it stayed full): the discouraged but supported self-delete
  vTaskDeleteWithCaps(NULL);
  for (;;) vTaskDelay(portMAX_DELAY);
}

// called from loop(): free finished background tasks. A task that has not reached
// its suspend yet goes back on the queue for the next pass.
static void nomadReapTasks() {
  if (!g_taskReapQueue) return;
  TaskHandle_t h = NULL;
  int n = uxQueueMessagesWaiting(g_taskReapQueue);
  while (n-- > 0 && xQueueReceive(g_taskReapQueue, &h, 0) == pdTRUE && h) {
    if (eTaskGetState(h) == eSuspended) vTaskDeleteWithCaps(h);
    else xQueueSend(g_taskReapQueue, &h, 0);
  }
}

static void lvglSendMsg(const char *text, bool show) {
  if (!lvglQueue) return;
  LvglMsg m;
  m.show = show;
  if (text) {
    strncpy(m.text, text, sizeof(m.text) - 1);
    m.text[sizeof(m.text) - 1] = '\0';
  } else {
    m.text[0] = '\0';
  }
  xQueueSend(lvglQueue, &m, pdMS_TO_TICKS(50));
}

static void lvglDrainQueue() {
  if (!lvglQueue) return;
  LvglLock lock;
  LvglMsg msg;
  bool drainedAny = false;
  bool endedHidden = false;
  while (xQueueReceive(lvglQueue, &msg, 0) == pdTRUE) {
    if (msg.show) {
      lv_obj_clear_flag(ui_MediaGen, LV_OBJ_FLAG_HIDDEN);
      lv_textarea_set_text(ui_MediaGen, msg.text);
      endedHidden = false;
    } else {
      lv_textarea_set_text(ui_MediaGen, "");
      lv_obj_add_flag(ui_MediaGen, LV_OBJ_FLAG_HIDDEN);
      endedHidden = true;
    }
    drainedAny = true;
  }
  //only redraws the screen after all updates are done, removed the spinner (reloading constantly.. whoops lol)
  if (drainedAny) {
    // if we just hid the overlay, redraw twice so no remnant is left behind
    if (endedHidden) {
      lv_obj_invalidate(lv_scr_act());
      lv_timer_handler();
    }
    lv_timer_handler();
  }
}

static volatile bool bootButtonPressed = false;
// set once StreamingTask (the LVGL pump) is running, loop() only pumps as a fallback
static volatile bool g_lvglPumpTaskUp = false;
// /flash-mode sets this, loop() does the actual switch (LVGL + restart must
// not run on the async_tcp task)
static volatile bool g_enterFlashMode = false;
#ifndef INDEX_MIN_HEAP
#define INDEX_MIN_HEAP 15000UL
#endif
static inline bool enoughHeapForIndex(size_t estNeededBytes = 20000) {
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < (INDEX_MIN_HEAP + estNeededBytes)) {
    Serial.printf("[Index] Not enough heap for index work (free=%u, need~%u). Deferring.\n",
                  (unsigned)freeHeap, (unsigned)(INDEX_MIN_HEAP + estNeededBytes));
    return false;
  }
  return true;
}
static size_t jsonEscapeToBuf(const String &in, char *dst, size_t dstLen) {
  if (!dst || dstLen == 0) return 0;
  size_t pos = 0;
  for (size_t i = 0; i < in.length() && pos + 1 < dstLen; ++i) {
    char c = in.charAt(i);
    if (c == '\\' || c == '\"') {
      if (pos + 2 >= dstLen) break;
      dst[pos++] = '\\';
      dst[pos++] = c;
    } else if (c == '\n') {
      if (pos + 2 >= dstLen) break;
      dst[pos++] = '\\';
      dst[pos++] = 'n';
    } else if (c == '\r') {
      if (pos + 2 >= dstLen) break;
      dst[pos++] = '\\';
      dst[pos++] = 'r';
    } else {
      dst[pos++] = c;
    }
  }
  dst[pos] = '\0';
  return pos;
}

void handleConnector(AsyncWebServerRequest *request);
float currentTempC = 0.0;
volatile bool mediaStreamingActive = false; // Flag to indicate active media streaming
volatile bool g_fwPausingMedia = false;      // the Nomad is fetching new firmware: media waits (NomadFirmware.h)
volatile unsigned long lastStreamIoMs = 0;  // last time a streaming request/fill touched the SD
SemaphoreHandle_t sdMutex = NULL;
#include <map>
static uint32_t nextStreamId = 1;
// NomadFile64 so streams can seek and size past 4 GB on exFAT. Move-only, like the
// SdFat handle.
struct StreamHandle {
  NomadFile64 file;
  String path;
  unsigned long lastActivity;
  uint64_t lastEndByte;
  // True while a response is still filling from this handle.
  // Two live responses sharing one handle re-seek on every 16 KB fill, and on FAT32 a
  // backward seek walks the cluster chain from the start, so the cost grows with the
  // offset into the file. Big films slow way down. exFAT seeks don't have this problem.
  bool active = false;
  // Replies on this handle whose connection hasn't closed yet. Counted up when a reply is
  // built and down in its onDisconnect (every reply carries Connection: close). This one
  // is exact, active is a guess, so this decides whether a rename or delete must wait.
  uint8_t inflight = 0;
};
static std::map<uint32_t, StreamHandle> streamingFiles;
static std::map<String, uint32_t> streamPathIndex;
static SemaphoreHandle_t streamingFilesMutex = NULL;
// A scrub or resume can want a second handle for one path while the first response
// drains. About 300 bytes each.
static const int MAX_CONCURRENT_STREAMS = 16;

// true when this request is a viewer coming back for the next piece of the film it is
// playing: a handle for the path whose last response ended at (or, if the connection broke,
// up to 8 MB after) where this one starts, active in the last 30 s
static bool continuingStream(const String &path, AsyncWebServerRequest *request) {
  if (!request->hasHeader("Range")) return false;
  const String r = request->header("Range");
  if (!r.startsWith("bytes=") || r.length() < 7 || r.charAt(6) == '-') return false;
  const uint64_t start = strtoull(r.c_str() + 6, nullptr, 10);
  if (start == 0 || !streamingFilesMutex) return false;
  if (xSemaphoreTake(streamingFilesMutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
  bool yes = false;
  auto pi = streamPathIndex.find(path);
  if (pi != streamPathIndex.end()) {
    auto it = streamingFiles.find(pi->second);
    if (it != streamingFiles.end()) {
      const uint64_t next = it->second.lastEndByte + 1;
      yes = start <= next && start + (8ULL << 20) >= next && millis() - it->second.lastActivity < 30000UL;
    }
  }
  xSemaphoreGive(streamingFilesMutex);
  return yes;
}

// A handle whose response was abandoned mid-flight is spotted by going quiet. A filling
// response touches lastActivity many times a second.
static const unsigned long STREAM_BUSY_QUIET_MS = 2000;

// DMA-capable memory runs out here while free heap still looks fine. When the sdmmc
// driver can't get its DMA bounce buffer it returns ESP_ERR_NO_MEM, which reads as a
// missing file, so /media would 404 a film that's on the card. On the P4 the ESP-Hosted
// link to the C6 radio panics instead (transport_drv_ap_tx asserts on a failed copy_buff).
// The floor only stops the 404s. It's low enough that a page full of cover art still loads.
// The S3 doesn't have the ESP-Hosted panic and MEDIA_SLOTS bounds the worst case, so it
// can go lower. A higher floor there stopped a third film from starting.
#if CONFIG_IDF_TARGET_ESP32P4
static const size_t STREAM_DMA_FLOOR = 48 * 1024;
#else
static const size_t STREAM_DMA_FLOOR = 24 * 1024;
#endif
static const size_t SMALL_FILE_DMA_FLOOR = 24 * 1024;   // covers and page assets through /media
static const size_t CONTINUE_DMA_FLOOR = 12 * 1024;     // the next piece of a film already playing
static inline size_t streamDmaFree() {
  return heap_caps_get_free_size(MALLOC_CAP_DMA);
}

// Media slots: a cap on live media responses, taken before the memory floor check.
// Every open response holds network buffers until its client is done, and a paused or
// stalled player keeps holding them. Too many at once crash the board, so count them.
// Cover art and page files aren't counted, they finish in milliseconds.
//
// The floor is checked at the top of the handler but the response is built much later,
// so several requests arriving together could all pass it before any of them allocates.
// Taking a slot first closes that gap.
//
// A slot older than MEDIA_SLOT_STALE_MS is reclaimed. That frees the count, not the
// socket, so the worst case is letting one extra in for a while.
//
// 8 matches how many films play cleanly at once (see MAX_CLIENTS).
static const int MEDIA_SLOTS = 8;
static const uint32_t MEDIA_SLOT_STALE_MS = 10UL * 60UL * 1000UL;
static portMUX_TYPE mediaSlotMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t mediaSlotStartMs[MEDIA_SLOTS] = {0};
static volatile int inFlightMediaResponses = 0;

// Returns a slot index, or -1 when every slot is busy. *token gets the slot's start time,
// pass it back to mediaSlotGive(). That way a late disconnect from a reclaimed slot can't
// free whoever took it next.
static int mediaSlotTake(uint32_t *token = nullptr) {
  const uint32_t now = millis();
  int idx = -1;
  portENTER_CRITICAL(&mediaSlotMux);
  for (int i = 0; i < MEDIA_SLOTS; i++) {
    if (mediaSlotStartMs[i] != 0 &&
        (uint32_t)(now - mediaSlotStartMs[i]) > MEDIA_SLOT_STALE_MS) {
      mediaSlotStartMs[i] = 0;
      if (inFlightMediaResponses > 0) inFlightMediaResponses--;
    }
  }
  for (int i = 0; i < MEDIA_SLOTS; i++) {
    if (mediaSlotStartMs[i] == 0) {
      mediaSlotStartMs[i] = now ? now : 1;   // 0 is the free marker, never a timestamp
      if (token) *token = mediaSlotStartMs[i];
      inFlightMediaResponses++;
      idx = i;
      break;
    }
  }
  portEXIT_CRITICAL(&mediaSlotMux);
  return idx;
}

static void mediaSlotGive(int idx, uint32_t token) {
  if (idx < 0 || idx >= MEDIA_SLOTS) return;
  portENTER_CRITICAL(&mediaSlotMux);
  // only the holder that took this slot may free it: a reaped slot must not decrement
  // twice, and a reaped-and-retaken slot belongs to someone else now
  if (mediaSlotStartMs[idx] != 0 && mediaSlotStartMs[idx] == token) {
    mediaSlotStartMs[idx] = 0;
    if (inFlightMediaResponses > 0) inFlightMediaResponses--;
  }
  portEXIT_CRITICAL(&mediaSlotMux);
}

// Frees the slot on any early return between admission and send. On success, release()
// hands it to onDisconnect.
struct MediaSlotGuard {
  int idx = -1;
  uint32_t token = 0;
  bool take() { idx = mediaSlotTake(&token); return idx >= 0; }
  int release() { int h = idx; idx = -1; return h; }
  ~MediaSlotGuard() { if (idx >= 0) mediaSlotGive(idx, token); }
};

// Remove the path -> id entry only when it still names THIS handle. Two handles can
// share a path (the newest is the published one), so closing the older one mustn't
// unpublish the live one.
static inline void streamUnpublish(const String &path, uint32_t id) {
  auto pi = streamPathIndex.find(path);
  if (pi != streamPathIndex.end() && pi->second == id) streamPathIndex.erase(pi);
}

// close a handle we opened but never sent, so it doesn't leak until the LRU gets it
static void closeStreamById(uint32_t streamId) {
  if (!streamId) return;
  bool locked = (!streamingFilesMutex) ||
                (xSemaphoreTake(streamingFilesMutex, pdMS_TO_TICKS(300)) == pdTRUE);
  if (!locked) return;
  auto it = streamingFiles.find(streamId);
  if (it != streamingFiles.end()) {
    streamPathIndex.erase(it->second.path);
    it->second.file.close();
    streamingFiles.erase(it);
  }
  if (streamingFilesMutex) xSemaphoreGive(streamingFilesMutex);
}

// Before a rename, delete or save: is a reply still open on this file (or anything inside
// this folder)? Idle handles get closed here, so a file the editor just read can be changed
// right away. A handle with a reply still open is left alone.
// Returns 1 for in use, 0 for clear, -1 when the lock wasn't available.
static int releaseStreamsFor(const String &path) {
  if (!streamingFilesMutex) return 0;
  if (xSemaphoreTake(streamingFilesMutex, pdMS_TO_TICKS(300)) != pdTRUE) return -1;
  const String under = path.endsWith("/") ? path : path + "/";
  bool busy = false;
  for (auto it = streamingFiles.begin(); it != streamingFiles.end(); ) {
    const String &p = it->second.path;
    if (p != path && !p.startsWith(under)) { ++it; continue; }
    if (it->second.inflight) { busy = true; ++it; continue; }
    streamUnpublish(it->second.path, it->first);
    it->second.file.close();
    it = streamingFiles.erase(it);
  }
  xSemaphoreGive(streamingFilesMutex);
  return busy ? 1 : 0;
}

// a reply on this handle has closed (its onDisconnect)
static void streamReplyDone(uint32_t streamId) {
  if (!streamId || !streamingFilesMutex) return;
  if (xSemaphoreTake(streamingFilesMutex, pdMS_TO_TICKS(300)) != pdTRUE) return;
  auto it = streamingFiles.find(streamId);
  if (it != streamingFiles.end() && it->second.inflight) it->second.inflight--;
  xSemaphoreGive(streamingFilesMutex);
}

static uint64_t cachedTotalBytes = 0;
static uint64_t cachedUsedBytes = 0;
static bool g_sdStatTrusted = false;
const char* SD_USAGE_FILE = "/.system-index/sd_usage.json";
static unsigned long lastScanTime = 0;
volatile bool sdbarDirty = false;
volatile int activeStreams = 0;
struct IndexBuildArgs {
  String dir;   // directory path to build index for (ex: "/Music/Album")
  String out;   // output index filename
};
static QueueHandle_t indexQueue = nullptr;

TaskHandle_t indexWorkerTaskHandle = nullptr;
TaskHandle_t storageMonitorTaskHandle = nullptr;
volatile bool shutdownBackgroundTasks = false;
volatile bool indexingTasksActive = false;
String currentIndexingPath = "";
// Progress tracker for active indexing task
int g_currentBucketNum = 0;
int g_totalBucketsForProgress = 0;
int g_indexProgressPercent = 0;
SemaphoreHandle_t indexingPathMutex = NULL;

// Multiplayer room store (HTTP polling, no WebSockets).
// Fixed-size char buffers only, no Arduino String and no SD I/O. Strings shared across
// tasks corrupted the heap before. Everything here is guarded by gameMutex.
#define MP_MAX_ROOMS 8
#define MP_CODE_LEN 5      // 4-char room code + NUL
#define MP_GAME_LEN 16     // e.g. "tictactoe", "chess"
#define MP_TOKEN_LEN 9     // 8 hex chars + NUL
#define MP_STATE_LEN 512   // opaque client-owned state blob (chess FEN ~90B, TTT ~32B)
#define MP_ROOM_IDLE_MS (2UL * 60UL * 1000UL)  // idle rooms are reclaimed after ~2 min

struct MpRoom {
  bool active;
  char code[MP_CODE_LEN];
  char game[MP_GAME_LEN];
  char token[2][MP_TOKEN_LEN];  // seat 0/1 tokens, empty string = seat open
  char state[MP_STATE_LEN];
  uint32_t seq;
  uint32_t games;               // finished games, used to alternate who starts a rematch
  unsigned long lastMs;
};
static MpRoom mpRooms[MP_MAX_ROOMS];
SemaphoreHandle_t gameMutex = NULL;

// Function declarations for task management
void shutdownBackgroundTasksForStreaming();
void startBackgroundTasksIfNeeded();
void ensureBackgroundTasks();
void checkStreamingTimeout();
void immediateEnqueueTopLevelTask(void *param);
void triggerIndexingIfNeeded(const String& filePath);
void updateSDBAR() {
  sdbarDirty = true;
}
void updateSDBAR_UI_ThreadOnly() {
  if (cachedTotalBytes == 0) return;
  LvglLock lock;
  
  int usage = (int)((cachedUsedBytes * 100) / cachedTotalBytes);
  if (usage > 100) usage = 100;
  if (usage < 0) usage = 0;
  
  lv_bar_set_value(ui_sdbar, usage, LV_ANIM_OFF);
  sdbarDirty = false;
}
#include <string> // used by std::map key
static std::map<std::string, unsigned long> lastIndexRequestMs;
const unsigned long INDEX_REQUEUE_COALESCE_MS = 2000UL; // 2 seconds coalescing
const size_t INDEX_REQUEST_MAP_EVICT_THRESHOLD = 200;       // prune once the map grows this large
const unsigned long INDEX_REQUEST_MAP_MAX_AGE_MS = 60000UL; // entries older than this are stale
static bool shouldCoalesceIndexRequest(const String &path) {
  std::string k(path.c_str());
  unsigned long now = millis();
  auto it = lastIndexRequestMs.find(k);
  if (it != lastIndexRequestMs.end()) {
    if (now - it->second < INDEX_REQUEUE_COALESCE_MS) {
      return false;
    }
  }
  lastIndexRequestMs[k] = now;

  if (lastIndexRequestMs.size() > INDEX_REQUEST_MAP_EVICT_THRESHOLD) {
    for (auto mit = lastIndexRequestMs.begin(); mit != lastIndexRequestMs.end(); ) {
      if (now - mit->second > INDEX_REQUEST_MAP_MAX_AGE_MS) {
        mit = lastIndexRequestMs.erase(mit);
      } else {
        ++mit;
      }
    }
  }
  return true;
}

// Helper: get parent directory for a path ("/Music/song.mp3" -> "/Music")
static String parentDirFromPath(const String &path) {
  if (path.length() == 0) return "/";
  int last = path.lastIndexOf('/');
  if (last <= 0) return "/"; // root or malformed -> treat as root
  String p = path.substring(0, last);
  if (p.length() == 0) return "/";
  return p;
}

// Paths that should never be indexed as media content. (generic SD card stuff that was causing some issues.)
static bool isFilesystemNoisePath(const String &path) {
  if (path.startsWith("/.system-index")) return true;      // our own index storage
  if (path.startsWith("/.")) return true;                  // hidden folders
  if (path.startsWith("/config")) return true;              // our own settings storage
  if (path.startsWith("/System Volume Information")) return true;
  if (path.startsWith("/$")) return true;                   // Windows/FAT system artifacts
  if (path.equalsIgnoreCase("/lost.dir")) return true;       // FAT filesystem recovery folder (Android/Linux)
  if (path.startsWith("/FOUND.")) return true;               // FAT32 chkdsk recovery folders (FOUND.000, FOUND.001, ...)
  if (path.equalsIgnoreCase("/RECYCLER")) return true;       // Windows recycle bin (older FAT)
  return false;
}

static bool shouldSkipIndexingPath(const String &path) {
  if (isFilesystemNoisePath(path)) return true;
  if (path.startsWith("/Archive")) return true;             // large ZIM archives, not media
  return false;
}

// Max number of devices that can connect at once. The driver clamps anything higher
// to 10 (it says so in the boot log).
// Ten is also about where media runs out. Total SD throughput is around 1 MB/s however
// many readers share it, so about 8 films at 125 KB/s or 10 at 100 KB/s play cleanly.
// This caps devices, one phone can pull several streams. The bottleneck is the card.
#define MAX_CLIENTS 10
String encodeIndexName(const String &path);

struct AdminSettings {
  String rgbMode = "off";
  String rgbColor = "#ff0000";
  String adminPassword = "";
  String wifiSSID = "Jcorp_Nomad";
  String wifiPassword = "password";
  int brightness = 100;            // percent 0-100 (Set_Backlight range)
  bool autoGenerateMedia = true;   // check for new files on boot (default on)
  bool flipScreen = false;         // rotate LCD 180 deg (USB port upside down, e.g. car mounts)
  bool dlnaEnabled = true;         // TV support: DLNA server + SSDP discovery
  // WiFi Mode (station): join an existing network instead of running the hotspot.
  // staPersist=true -> try every boot (with hotspot fallback on failure).
  // staPersist=false -> only when the sta_once NVS flag is armed, a power cycle clears it.
  String staSSID = "";
  String staPassword = "";
  bool staPersist = false;
  // old wildcard captive DNS (answer every lookup with Nomad's address).
  // off = only answer for Nomad, so phones keep using mobile data for the internet
  bool dnsCatchAll = false;
};

AdminSettings settings;
// Set by the /settings handler, picked up by the task that owns LVGL flushes.
// MADCTL must never be written mid-flush from another task.
volatile bool lcdRotatePending = false;
const char* SETTINGS_PATH = "/config/settings.json";

// --- Admin session auth ---
// The password, sessions and the /auth/* routes are in NomadAuth.h (included after
// saveSettings, which it uses). Declared here because handlers above it call them.
bool isAdminAuthRequired();
bool checkAdminAuth(AsyncWebServerRequest *request);
void nomadAuthLoad();

// isConfigPath() lives in NomadGuard.h with the rest of the path rules. Don't compare
// the raw string here: "/CONFIG/", "//config/" and "/config./" all reach the same file.

// Web Console Logging System
#define MAX_LOG_ENTRIES 50
struct LogEntry {
  String message;
  String type;
  unsigned long timestamp;
};

LogEntry webLogs[MAX_LOG_ENTRIES];
int logIndex = 0;
int logCount = 0;
// guards webLogs[], lots of tasks write it and /console-logs reads it. unsynced
// String read/writes race the heap and panic. made in setup() before the server,
// so early boot logs (NULL mutex) skip locking safely
SemaphoreHandle_t webLogMutex = NULL;

// Function to add log entry for web console
void webLog(const String& message, const String& type = "info") {
  bool locked = (webLogMutex && xSemaphoreTake(webLogMutex, pdMS_TO_TICKS(50)) == pdTRUE);

  // if the reader has the lock, just go to serial instead of writing unsynced
  if (locked || !webLogMutex) {
    webLogs[logIndex].message = message;
    webLogs[logIndex].type = type;
    webLogs[logIndex].timestamp = millis();

    logIndex = (logIndex + 1) % MAX_LOG_ENTRIES;
    if (logCount < MAX_LOG_ENTRIES) {
      logCount++;
    }
  }

  if (locked) xSemaphoreGive(webLogMutex);

  // Also send to serial for debugging (always, lock or not)
  Serial.println(message);
}

// Function for formatted web logging
void webLogf(const String& type, const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  webLog(String(buffer), type);
}
#define SD_CLK_PIN 14
#define SD_CMD_PIN 15
#define SD_D0_PIN 16
#define SD_D1_PIN 18
#define SD_D2_PIN 17
#define SD_D3_PIN 21
const char *INDEX_DIR = "/.system-index";   // on-SD folder for index files
// Normalize path: ensure leading '/', remove trailing '/'
String normalizePath(const String &p_in){
  if (p_in.length() == 0) return "/";
  String p = p_in;
  if (!p.startsWith("/")) p = "/" + p;
  // Collapse runs of '/'. The index builder joins path + "/" + entry, so entries under
  // the card root come out as "//Books" and string compares break. Same rule as
  // canonicalPath() in the config guard.
  if (p.indexOf("//") >= 0) {
    String out;
    out.reserve(p.length());
    for (unsigned int i = 0; i < p.length(); ++i) {
      char c = p.charAt(i);
      if (c == '/' && out.length() && out.charAt(out.length() - 1) == '/') continue;
      out += c;
    }
    p = out;
  }
  while (p.length() > 1 && p.endsWith("/")) p = p.substring(0, p.length()-1);
  return p;
}
// One /api/index-nested response, streamed: the prefix, then the index body with its
// header line dropped and newlines turned into commas, then "]}". Every read takes
// sdMutex briefly. A busy card means "ask again", never a short body.
struct NestedIndexStream {
  NomadFile64 f;
  String prefix;
  uint64_t pos = 0;
  uint8_t phase = 0;     // 0 prefix, 1 skipping the header line, 2 entries, 3 closing, 4 done
  bool sep = false, any = false;
  ~NestedIndexStream() {
    if (!f.isOpen()) return;
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE) { f.close(); xSemaphoreGive(sdMutex); }
    else f.close();
  }
  size_t fill(uint8_t *buf, size_t maxLen) {
    size_t n = 0;
    if (phase == 0) {
      if (prefix.length() > maxLen) return RESPONSE_TRY_AGAIN;
      memcpy(buf, prefix.c_str(), prefix.length());
      n = prefix.length();
      phase = 1;
    }
    uint8_t tmp[512];
    while ((phase == 1 || phase == 2) && maxLen - n >= 8) {
      size_t want = (maxLen - n - 2) / 2;        // each byte read can become two (",{")
      if (want > sizeof(tmp)) want = sizeof(tmp);
      if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(50)) != pdTRUE) return n ? n : RESPONSE_TRY_AGAIN;
      size_t got = f.readAt(pos, tmp, want);
      if (got < want) f.close();                 // end of file: close while we hold the card
      if (sdMutex) xSemaphoreGive(sdMutex);
      pos += got;
      for (size_t i = 0; i < got; i++) {
        const uint8_t c = tmp[i];
        if (phase == 1) { if (c == '\n') phase = 2; continue; }
        if (c == '\n' || c == '\r') { if (any) sep = true; continue; }
        if (sep) { buf[n++] = ','; sep = false; }
        buf[n++] = c;
        any = true;
      }
      if (got < want) phase = 3;
    }
    if (phase == 3 && maxLen - n >= 2) { buf[n++] = ']'; buf[n++] = '}'; phase = 4; }
    return n;                                    // 0 only once phase 4 has been sent
  }
};

// ETag for an index file: the signature from its header line plus the file size.
// "" when it can't be read.
String indexEtag(const String &fullPath) {
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) != pdTRUE) return "";
  String etag;
  NomadFile64 f;
  if (f.open(fullPath.c_str())) {
    char head[192];
    size_t n = f.readAt(0, (uint8_t *)head, sizeof(head) - 1);
    head[n] = 0;
    const char *sig = strstr(head, "\"sig\":\"");
    if (sig) {
      sig += 7;
      const char *end = strchr(sig, '"');
      if (end && end - sig < 40) etag = "\"" + String(sig).substring(0, end - sig) + "-" + String((unsigned long long)f.size()) + "\"";
    }
    f.close();
  }
  if (sdMutex) xSemaphoreGive(sdMutex);
  return etag;
}

String encodeIndexName(const String &path_in) {
  String p = path_in;
  if (p.length() == 0) return String("root");

  // normalize leading/trailing slashes
  if (p.startsWith("/")) p = p.substring(1);
  while (p.length() > 1 && p.endsWith("/")) p = p.substring(0, p.length()-1);
  if (p.length() == 0) return String("root");

  // split on slashes and sanitize each segment
  std::vector<String> parts;
  String cur;
  for (size_t i = 0; i < p.length(); ++i) {
    char c = p.charAt(i);
    if (c == '/') {
      if (cur.length()) parts.push_back(cur);
      cur = "";
    } else {
      cur += c;
    }
  }
  if (cur.length()) parts.push_back(cur);

  // build encoded name joined by "__"
  String out;
  for (size_t i = 0; i < parts.size(); ++i) {
    String tok = sanitizeToken(parts[i]);
    if (tok.length() == 0) tok = "_";
    if (i) out += "__";
    out += tok;
  }
  if (out.length() == 0) out = "root";
  return out;
}
// Sanitize a directory name into a filename token (keeps alnum, - and _, else underscore)
String sanitizeToken(const String &s){
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s.charAt(i);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out += c;
    else if (c == '-' || c == '_') out += c;
    else out += '_';
  }
  return out;
}

// Minimal JSON escape used for NDJSON fields
String jsonEscape(const String &in){
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in.charAt(i);
    if (c == '\\' || c == '\"') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

// Build NDJSON header line: {"_type":"dir","path":"/X","sig":"hex","count":N}
String buildIndexHeader(const String &path, const String &sigHex, uint32_t count){
  String h;
  h.reserve(128);
  h += "{\"_type\":\"dir\",\"path\":\"";
  h += jsonEscape(path);
  h += "\",\"sig\":\"";
  h += sigHex;
  h += "\",\"count\":";
  h += String(count);
  h += "}\n";
  return h;
}

bool readIndexHeaderSig(const String &indexPath, String &outSig, uint32_t &outCount) {
  outSig = "";
  outCount = 0;
  if (!SD_MMC.exists(indexPath)) return false;
  File f = SD_MMC.open(indexPath, FILE_READ);
  if (!f) return false;
  String header = f.readStringUntil('\n');
  f.close();
  if (header.length() == 0) return false;

  // Parse small JSON header
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, header);
  if (err) return false;
  const char* sig = doc["sig"];
  if (sig) outSig = String(sig);
  outCount = (uint32_t)(doc["count"] | doc["count"]); // header key is "count"
  return true;
}
// Media file detector + safe recursive counter
static bool isMediaFile(const String &lowerName) {
  int dot = lowerName.lastIndexOf('.');
  if (dot < 0) return false;
  String ext = lowerName.substring(dot); // includes the dot, e.g. ".mp4"

  // Video (playback generally reliable: mp4, mov, mkv, webm, m4v)
  if (ext == ".mp4"  || ext == ".mov"  || ext == ".mkv"  || ext == ".webm" || ext == ".m4v"
   || ext == ".ts"   || ext == ".m2ts") return true;

  // Audio (mp3/flac/wav plus common containers)
  if (ext == ".mp3"  || ext == ".flac" || ext == ".wav"  || ext == ".aac"  || ext == ".m4a"
   || ext == ".ogg"  || ext == ".opus") return true;

  // Images
  if (ext == ".jpg"  || ext == ".jpeg" || ext == ".png"  || ext == ".webp" || ext == ".avif"
   || ext == ".gif"  || ext == ".bmp"  || ext == ".tiff" || ext == ".tif"  || ext == ".heic") return true;

  // Books / Documents / Archives (PDF and EPUB primary,CBZ works, CBR will not)
  if (ext == ".pdf"  || ext == ".epub" || ext == ".txt"  || ext == ".html" || ext == ".htm"
   || ext == ".cbz"  || ext == ".cbr"  || ext == ".azw3" || ext == ".mobi") return true;

  // Other video containers we count (wont work, dont use these dummy): .avi, .flv, .rmvb
  if (ext == ".avi" || ext == ".flv" || ext == ".rmvb") return true;

  return false;
}

// Audio or video a player pulls with Range requests over minutes. isMediaFile() also
// covers images and books, and the grids fetch every cover through /media, so using it
// would mark the box as streaming on every scroll. Keep the two lists next to each other.
static bool isTimedMediaExtension(const String &lowerPath) {
  int dot = lowerPath.lastIndexOf('.');
  if (dot < 0) return false;
  String ext = lowerPath.substring(dot);   // includes the dot, e.g. ".mkv"

  if (ext == ".mp4"  || ext == ".m4v"  || ext == ".mov"  || ext == ".mkv"
   || ext == ".webm" || ext == ".ts"   || ext == ".m2ts" || ext == ".avi") return true;

  if (ext == ".mp3"  || ext == ".m4a"  || ext == ".flac" || ext == ".wav"
   || ext == ".ogg"  || ext == ".opus" || ext == ".aac") return true;

  return false;
}
static bool isComicFolder(const String &dirPath) {
  File d = SD_MMC.open(dirPath);
  if (!d || !d.isDirectory()) {
    if (d) d.close();
    return false;
  }
  
  bool hasImages = false;
  bool hasBookFiles = false;
  int imageCount = 0;
  
  d.rewindDirectory();
  File e;
  while ((e = d.openNextFile())) {
    if (e.isDirectory()) {
      e.close();
      continue;
    }
    
    String name = String(e.name());
    int lastSlash = name.lastIndexOf('/');
    if (lastSlash >= 0) name = name.substring(lastSlash + 1);
    name.toLowerCase();
    
    if (name.endsWith(".pdf") || name.endsWith(".epub") || 
        name.endsWith(".azw3") || name.endsWith(".mobi") ||
        name.endsWith(".mp3") || name.endsWith(".m4a") || 
        name.endsWith(".m4b") || name.endsWith(".flac")) {
      hasBookFiles = true;
      e.close();
      break;
    }
    
    if (name.endsWith(".png") || name.endsWith(".jpg") || 
        name.endsWith(".jpeg") || name.endsWith(".webp")) {
      hasImages = true;
      imageCount++;
    }
    
    e.close();
  }
  
  d.close();
  
  return hasImages && !hasBookFiles && imageCount >= 3;
}

static bool isBookFormat(const String &name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".pdf") || lower.endsWith(".epub") || 
         lower.endsWith(".azw3") || lower.endsWith(".mobi");
}
// Ensure INDEX_DIR exists (creates it if missing.. usualy)
void ensureIndexDir(){
  if (!SD_MMC.exists(INDEX_DIR)) {
    if (!SD_MMC.mkdir(INDEX_DIR)) {
      Serial.printf("[Index] Failed to create index dir %s\n", INDEX_DIR);
    } else {
      Serial.printf("[Index] Created index dir %s\n", INDEX_DIR);
    }
  }
}

// FNV-1a 64-bit hash incremental update (used to compute signature)
uint64_t fnv1a64_update(uint64_t h, const String &s){
  const uint64_t FNV_PRIME = 0x100000001b3ULL;
  uint64_t hash = h;
  for (size_t i = 0; i < s.length(); ++i) {
    hash ^= (uint8_t)s[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// Rename or copy fallback: try rename first, if that fails try copy & remove.
bool renameOrCopy(const String &src, const String &dst) {
  if (SD_MMC.exists(dst)) SD_MMC.remove(dst);
  if (SD_MMC.rename(src, dst)) return true;

  File fsrc = SD_MMC.open(src, FILE_READ);
  if (!fsrc) return false;
  File fdst = SD_MMC.open(dst, FILE_WRITE);
  if (!fdst) { fsrc.close(); return false; }

  uint8_t buf[512];
  while (fsrc.available()) {
    size_t r = fsrc.read(buf, sizeof(buf));
    if (r > 0) fdst.write(buf, r);
  }
  fsrc.close();
  fdst.close();
  SD_MMC.remove(src);
  return true;
}

bool writeNDIndexForDir(const String &dirPath, const String &outFilename) {
  // Acquire SD card mutex to prevent concurrent access
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.printf("[Index] Mutex timeout for writeNDIndexForDir('%s')\n", dirPath.c_str());
    return false;
  }
  // Tracks whether this task currently holds sdMutex. 
  bool mutexHeld = true;

  // ensure index folder exists
  if (!SD_MMC.exists(INDEX_DIR)) SD_MMC.mkdir(INDEX_DIR);

  if (!enoughHeapForIndex()) {
    Serial.printf("[Index] Skipping index for '%s' due to low memory (free=%u)\n",
    dirPath.c_str(), (unsigned)ESP.getFreeHeap());
    if (sdMutex && mutexHeld) xSemaphoreGive(sdMutex);
    return false;
  }

// Normalize target dir
  String normPath = dirPath;
  if (!normPath.startsWith("/")) normPath = "/" + normPath;
  normPath = normalizePath(normPath);

  Serial.printf("[Index] Building index for '%s' (free heap=%u)\n", normPath.c_str(), (unsigned)ESP.getFreeHeap());
  webLogf("indexing_progress", "Starting indexing for '%s'", normPath.c_str());
  if (!SD_MMC.exists(normPath)) {
    Serial.printf("[Index] Path does not exist: %s\n", normPath.c_str());
    if (sdMutex && mutexHeld) xSemaphoreGive(sdMutex);
    return false;
  }

  // Open once to verify directory
  File root = SD_MMC.open(normPath);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    Serial.printf("[Index] Not a directory: %s\n", normPath.c_str());
    if (sdMutex && mutexHeld) xSemaphoreGive(sdMutex);
    return false;
  }
  root.close();

   // Determine recursion strategy based on path
  bool isRoot = (normPath == "/");
  bool isShows = (normPath == "/Shows");
  bool isShowSubfolder = normPath.startsWith("/Shows/") && normPath.indexOf('/', 7) < 0;
  bool isShowSeasonFolder = normPath.startsWith("/Shows/") && normPath.indexOf('/', 7) > 0;
  bool isMusicSubfolder = normPath.startsWith("/Music/");
  bool isBooks = (normPath == "/Books");
  bool isBooksSubfolder = normPath.startsWith("/Books/") && normPath.indexOf('/', 7) < 0;
  bool isBooksComicFolder = normPath.startsWith("/Books/") && normPath.indexOf('/', 7) > 0;
  int maxDepth = 10;

  if (isRoot) {
    maxDepth = 0;
  } else if (isBooks) {
    maxDepth = 0;
  } else if (isBooksSubfolder) {
    maxDepth = 0;
  } else if (isBooksComicFolder) {
    maxDepth = 0;
  }

  Serial.printf("[Index] Recursion depth for '%s': %d\n", normPath.c_str(), maxDepth);
  // Prepass: compute signature and total count
  uint64_t sig = 0xcbf29ce484222325ULL;
  unsigned long count = 0;
  unsigned long writeCount = 0; // mirrors 'count' but tracked separately during writepass
  bool abortIndex = false;

  // soft caps so a huge/slow dir aborts instead of running forever
  const unsigned long MAX_INDEX_ITEMS = 20000;
  const unsigned long MAX_INDEX_BUILD_MS = 120000; // 2 minutes
  unsigned long buildStartMs = millis();

  std::function<void(const String&, int)> prepass = [&](const String &path, int depth) {
    vTaskDelay(pdMS_TO_TICKS(1));
    // Stop recursion if we've hit max depth or if abort flag is set
    if (abortIndex || depth > maxDepth) return;

    // Skip system/hidden folders
    if (shouldSkipIndexingPath(path)) {
    Serial.printf("[Index] Skipping folder: %s\n", path.c_str());
    return;
    }

    if (path.startsWith("/Books/") && isComicFolder(path)) {
    Serial.printf("[Index] Detected comic folder, skipping contents: %s\n", path.c_str());
    return;
    }

    File d = SD_MMC.open(path);
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }
    vTaskDelay(pdMS_TO_TICKS(1));
    d.rewindDirectory();

    int itemCount = 0;
    while (true) {
    if (abortIndex) break;

    if (count > MAX_INDEX_ITEMS || (millis() - buildStartMs) > MAX_INDEX_BUILD_MS) {
      Serial.printf("[Index] Aborting prepass for '%s': safety limit reached (count=%lu, elapsed=%lums)\n",
                    normPath.c_str(), count, (unsigned long)(millis() - buildStartMs));
      webLogf("warning", "Indexing '%s' aborted - directory too large or taking too long", normPath.c_str());
      abortIndex = true;
      break;
    }

    File e = d.openNextFile();
    if (!e) break;

    String full = String(e.name());
    // Normalize full path for consistency
    if (!full.startsWith("/")) full = normalizePath(path + "/" + full);
    else full = normalizePath(full);

    int ls = full.lastIndexOf('/');
    String tail = (ls >= 0) ? full.substring(ls + 1) : full;

    // Skip hidden files/folders
    if (tail.startsWith(".")) {
    e.close();
    continue;
    }

    if (e.isDirectory()) {
    sig = fnv1a64_update(sig, full + "|" + tail);
    ++count;
    e.close();
    // Recurse with depth tracking
    prepass(full, depth + 1);
    } else {
    uint64_t fsz = (uint64_t)e.size();
    uint64_t fmt = 0;
    sig = fnv1a64_update(sig, full + "|" + String(fsz) + "|" + String(fmt));
    ++count;
    e.close();
    }

    // Yield more frequently to prevent WDT and keep device responsive
    if (++itemCount % 5 == 0) {
    vTaskDelay(pdMS_TO_TICKS(3));
    if (sdMutex) {
      xSemaphoreGive(sdMutex);
      mutexHeld = false;
      vTaskDelay(pdMS_TO_TICKS(10));
      if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        Serial.printf("[Index] Lost mutex during prepass, aborting\n");
        abortIndex = true;
        d.close();
        return;
      }
      mutexHeld = true;
    }
    }
    }

    d.close();
  };

  prepass(normPath, 0);  // Start at depth 0

  if (abortIndex) {
    Serial.printf("[Index] Aborted index for '%s' during prepass (mutex loss or safety limit, count so far=%lu, free heap=%u)\n",
                  normPath.c_str(), count, (unsigned)ESP.getFreeHeap());
    if (sdMutex && mutexHeld) xSemaphoreGive(sdMutex);
    return false;
  }

  Serial.printf("[Index] Prepass complete for '%s': %lu items, free heap=%u\n",
                normPath.c_str(), count, (unsigned)ESP.getFreeHeap());

  // Prepare files
  String tmpPath   = String(INDEX_DIR) + "/" + outFilename + ".tmp";
  String finalPath = String(INDEX_DIR) + "/" + outFilename;

  char sigHex[17];
  snprintf(sigHex, sizeof(sigHex), "%016llx", (unsigned long long)sig);

  // if nothing changed since last build, skip the rewrite. reading the header sig
  // is basically free vs rewriting the whole file. (yay)
  {
    String oldSig;
    uint32_t oldCount = 0;
    if (readIndexHeaderSig(finalPath, oldSig, oldCount) &&
        oldSig.equalsIgnoreCase(sigHex) && oldCount == (uint32_t)count) {
      Serial.printf("[Index] Unchanged, skipping rewrite for '%s' (count=%lu, sig=%s)\n",
                    normPath.c_str(), count, sigHex);
      if (sdMutex && mutexHeld) xSemaphoreGive(sdMutex);
      return true;
    }
  }

  File fout = SD_MMC.open(tmpPath, FILE_WRITE);
  if (!fout) {
    Serial.printf("[Index] FAILED to open tmp for write: %s\n", tmpPath.c_str());
    if (sdMutex && mutexHeld) xSemaphoreGive(sdMutex);
    return false;
  }

  // Write header line
  String header = buildIndexHeader(normPath, String(sigHex), count);
  fout.write((const uint8_t*)header.c_str(), header.length());

  // second pass: write entries. everyone funnels through here so the throttled
  // screen progress below covers full scans, queue items and manual reindexes
  unsigned long lastIndexScreenUpdateMs = 0;
  std::function<void(const String&, int)> writepass = [&](const String &path, int depth) {
    vTaskDelay(pdMS_TO_TICKS(1));
    if (abortIndex || depth > maxDepth) return;

    if (shouldSkipIndexingPath(path)) return;

    if (path.startsWith("/Books/") && isComicFolder(path)) return;

    File d = SD_MMC.open(path);
    if (!d || !d.isDirectory()) { if (d) d.close(); return; }
    vTaskDelay(pdMS_TO_TICKS(1));
    d.rewindDirectory();

    int itemCount = 0;
    while (true) {
    if (abortIndex) break;

    if (writeCount > MAX_INDEX_ITEMS || (millis() - buildStartMs) > MAX_INDEX_BUILD_MS) {
      Serial.printf("[Index] Aborting writepass for '%s': safety limit reached (count=%lu, elapsed=%lums)\n",
                    normPath.c_str(), writeCount, (unsigned long)(millis() - buildStartMs));
      webLogf("warning", "Indexing '%s' aborted - directory too large or taking too long", normPath.c_str());
      abortIndex = true;
      fout.close();
      SD_MMC.remove(tmpPath);
      break;
    }

    File e = d.openNextFile();
    if (!e) break;
    ++writeCount;

    // throttled to 1500ms (not per-item) so it cant flood the small lvglQueue
    unsigned long nowMsForScreen = millis();
    if (nowMsForScreen - lastIndexScreenUpdateMs > 1500) {
      lastIndexScreenUpdateMs = nowMsForScreen;
      char screenBuf[80];
      snprintf(screenBuf, sizeof(screenBuf), "Indexing: %s\n%lu items\n\nDo not unplug!",
               normPath.c_str(), writeCount);
      lvglSendMsg(screenBuf, true);
    }

    String full = String(e.name());
    if (!full.startsWith("/")) full = normalizePath(path + "/" + full);
    else full = normalizePath(full);

    int ls = full.lastIndexOf('/');
    String tail = (ls >= 0) ? full.substring(ls + 1) : full;

    if (tail.startsWith(".")) {
    e.close();
    continue;
    }

    char entryType = e.isDirectory() ? 'd' : 'f';

    char escName[HALF_INDEX_BUF];
    char escPath[HALF_INDEX_BUF];
    jsonEscapeToBuf(tail, escName, HALF_INDEX_BUF);
    jsonEscapeToBuf(full, escPath, HALF_INDEX_BUF);

    if (entryType == 'f') {
    // size() sticks at SIZE_MAX past 4 GB. Only then do the 64-bit lookup, so normal
    // files cost nothing extra
    uint64_t fsz = (uint64_t)e.size();
    if (fsz == (uint64_t)SIZE_MAX) fsz = SD_MMC.fileSize64(full.c_str());
    uint64_t fmt = 0;
    int pos = snprintf(g_lineBuf, GLOBAL_INDEX_BUF,
    "{\"t\":\"f\",\"n\":\"%s\",\"p\":\"%s\",\"sz\":%llu,\"mt\":%llu}\n",
    escName, escPath, (unsigned long long)fsz, (unsigned long long)fmt);
    if (pos < 0) pos = 0;
    size_t wlen = strlen(g_lineBuf);
    if (wlen && fout.write((const uint8_t*)g_lineBuf, wlen) != wlen) {
      // A refused write only shows up in this return value. Ignoring it would publish a
      // truncated index, so abandon the build and keep the previous one.
      Serial.printf("[Index] Short write building '%s', abandoning\n", normPath.c_str());
      webLogf("error", "Indexing '%s' abandoned - the card refused a write", normPath.c_str());
      abortIndex = true;
      fout.close();
      SD_MMC.remove(tmpPath);
      e.close();
      break;
    }
    } else {
    bool isComic = full.startsWith("/Books/") && isComicFolder(full);
    int pos = snprintf(g_lineBuf, GLOBAL_INDEX_BUF,
    "{\"t\":\"d\",\"n\":\"%s\",\"p\":\"%s\"%s}\n",
    escName, escPath, isComic ? ",\"comic\":true" : "");
    if (pos < 0) pos = 0;
    size_t wlen = strlen(g_lineBuf);
    if (wlen && fout.write((const uint8_t*)g_lineBuf, wlen) != wlen) {
      // same as the file branch above: abandon on a refused write
      Serial.printf("[Index] Short write building '%s', abandoning\n", normPath.c_str());
      webLogf("error", "Indexing '%s' abandoned - the card refused a write", normPath.c_str());
      abortIndex = true;
      fout.close();
      SD_MMC.remove(tmpPath);
      e.close();
      break;
    }
    }

    if (entryType == 'd') {
    e.close();
    writepass(full, depth + 1);
    } else {
    e.close();
    }

    if (++itemCount % 5 == 0) {
    vTaskDelay(pdMS_TO_TICKS(3));
    if (sdMutex) {
      xSemaphoreGive(sdMutex);
      mutexHeld = false;
      vTaskDelay(pdMS_TO_TICKS(10));
      if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        Serial.printf("[Index] Lost mutex during writepass, aborting\n");
        abortIndex = true;
        d.close();
        fout.close();
        SD_MMC.remove(tmpPath);
        return;
      }
      mutexHeld = true;
    }
    }
    }

    d.close();
  };

  writepass(normPath, 0);

  if (abortIndex) {
    Serial.printf("[Index] Aborted writepass for '%s' (mutex loss or safety limit, written so far=%lu, free heap=%u)\n",
                  normPath.c_str(), writeCount, (unsigned)ESP.getFreeHeap());
    if (sdMutex && mutexHeld) xSemaphoreGive(sdMutex);
    return false;
  }

  fout.flush();
  fout.close();

  String newFinal = finalPath + ".new";
  if (SD_MMC.exists(newFinal)) SD_MMC.remove(newFinal);

  bool moved = SD_MMC.rename(tmpPath, newFinal);
  if (!moved) {
    File fsrc = SD_MMC.open(tmpPath, FILE_READ);
    if (fsrc) {
    File fdst = SD_MMC.open(newFinal, FILE_WRITE);
    if (fdst) {
    uint8_t buf[512];
    while (fsrc.available()) {
    size_t r = fsrc.read(buf, sizeof(buf));
    if (r > 0) fdst.write(buf, r);
    }
    fsrc.close();
    fdst.close();
    SD_MMC.remove(tmpPath);
    moved = true;
    } else {
    fsrc.close();
    }
    }
  }

  if (!moved) {
    Serial.printf("[Index] FAILED staging -> %s from tmp %s\n", newFinal.c_str(), tmpPath.c_str());
    SD_MMC.remove(tmpPath);
    if (sdMutex && mutexHeld) xSemaphoreGive(sdMutex);
    return false;
  }

  if (SD_MMC.exists(finalPath)) SD_MMC.remove(finalPath);

  if (!SD_MMC.rename(newFinal, finalPath)) {
    if (!renameOrCopy(newFinal, finalPath)) {
    Serial.printf("[Index] FAILED atomic replace %s -> %s\n", newFinal.c_str(), finalPath.c_str());
    SD_MMC.remove(newFinal);
    webLogf("error", "Failed atomic replace %s -> %s", newFinal.c_str(), finalPath.c_str());
    if (sdMutex && mutexHeld) xSemaphoreGive(sdMutex);
    return false;
    } else {
    SD_MMC.remove(newFinal);
    }
  }

  Serial.printf("[Index] Built index %s for %s (count=%lu, sig=%s, maxDepth=%d)\n",
    outFilename.c_str(), normPath.c_str(), count, sigHex, maxDepth);
  webLogf("completed_index_logging", "Completed indexing '%s' - %lu items processed", normPath.c_str(), count);

    String metaFilename = outFilename;
    if (metaFilename.endsWith(".ndjson")) {
    metaFilename = metaFilename.substring(0, metaFilename.length() - 7); // remove ".ndjson"
    }
    metaFilename += ".meta";
    String metaPath = String(INDEX_DIR) + "/" + metaFilename;

    File metaFile = SD_MMC.open(metaPath, FILE_WRITE);
    if (metaFile) {
    // Write JSON meta with path, count, and signature
    metaFile.print("{\"path\":\"");
    metaFile.print(jsonEscape(normPath));
    metaFile.print("\",\"count\":");
    metaFile.print(count);
    metaFile.print(",\"sig\":\"");
    metaFile.print(sigHex);
    metaFile.println("\"}");
    metaFile.close();
    Serial.printf("[Index] Wrote meta file %s\n", metaFilename.c_str());
    } else {
    Serial.printf("[Index] WARNING: Failed to write meta file %s\n", metaFilename.c_str());
    }

    if (sdMutex && mutexHeld) xSemaphoreGive(sdMutex);
    return true;
}

// write bucket-level index
// write top-level bucket index (ex: /Music -> Music.index.ndjson)
bool writeBucketIndex(const String &bucketPath) {
  String name = bucketPath;
  if (name.startsWith("/")) name = name.substring(1);
  int slash = name.indexOf('/');
  if (slash >= 0) name = name.substring(0, slash);
  if (!name.length()) name = "root";
  String outFile = String(name) + ".index.ndjson";
  return writeNDIndexForDir(bucketPath, outFile);
}

// Build bucket index convenience wrapper (returns true on success)
bool buildBucketIndex(const String &bucketPath) {
  String bucket = bucketPath;
  if (bucket.startsWith("/")) bucket = bucket.substring(1);
  if (bucket.endsWith("/")) bucket = bucket.substring(0, bucket.length()-1);
  if (bucket.length() == 0) bucket = "root";
  String outFilename = bucket + ".index.ndjson";

  bool ok = writeNDIndexForDir(bucketPath, outFilename);
  if (ok) {
    Serial.printf("[Index] Built bucket index %s for %s\n", outFilename.c_str(), bucketPath.c_str());
    webLogf("completed_index_logging", "Completed indexing bucket '%s' - wrote %s", bucketPath.c_str(), outFilename.c_str());
    return true;
  }
  Serial.printf("[Index] Failed building bucket index for %s\n", bucketPath.c_str());
  return false;
}
String urlencode(String str) {
  String encoded = "";
  char c;
  char code0, code1;
  char code[] = "0123456789ABCDEF";

  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (isalnum(c)) {
      encoded += c;
    } else {
      code0 = code[(c >> 4) & 0xF];
      code1 = code[c & 0xF];
      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }
  return encoded;
}
// Settings Setup:
bool loadSettings() {
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
    Serial.println("[Settings] Mutex timeout in loadSettings");
    return false;
  }

  if (!SD_MMC.exists(SETTINGS_PATH)) {
    Serial.println("Settings file not found. Generating default.");
    if (sdMutex) xSemaphoreGive(sdMutex);
    return saveSettings();  // Save defaults
  }

  File file = SD_MMC.open(SETTINGS_PATH);
  if (!file || file.isDirectory()) {
    Serial.println("Failed to open settings file.");
    if (sdMutex) xSemaphoreGive(sdMutex);
    return false;
  }

  StaticJsonDocument<768> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (sdMutex) xSemaphoreGive(sdMutex);

  if (error) {
    Serial.println("Failed to parse settings JSON.");
    return false;
  }

  settings.rgbMode = doc["rgbMode"] | "off";
  settings.rgbColor = doc["rgbColor"] | "#ff0000";
  settings.adminPassword = doc["adminPassword"] | "";
  settings.wifiSSID = doc["wifiSSID"] | "Jcorp_Nomad";
  settings.wifiPassword = doc["wifiPassword"] | "password";
  settings.brightness = doc["brightness"] | 100;
  // brightness is 0-100 (Set_Backlight ignores >100). old builds defaulted to 230, clamp it
  settings.brightness = constrain(settings.brightness, 0, 100);
  settings.autoGenerateMedia = doc["autoGenerateMedia"] | true;   // default on if unset
  settings.flipScreen = doc["flipScreen"] | false;
  settings.dlnaEnabled = doc["dlnaEnabled"] | true;
  settings.staSSID = doc["staSSID"] | "";
  settings.staPassword = doc["staPassword"] | "";
  settings.staPersist = doc["staPersist"] | false;
  settings.dnsCatchAll = doc["dnsCatchAll"] | false;

  return true;
}
// Deferred config writes.
// Writing a file on the async_tcp task stalls the web server while the filesystem
// hunts for free clusters, and on a nearly full card that trips the task watchdog.
// Small config writes go through this queue and a background task does the SD work.
struct ConfigWriteJob {
  String path;
  String content;
};
QueueHandle_t cfgWriteQueue = NULL;

// atomic temp+rename write, only ever runs on the writer task
static bool writeFileAtomicBlocking(const String &path, const String &content) {
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(15000)) != pdTRUE) {
    Serial.printf("[CFGW] SD busy too long, write of %s dropped\n", path.c_str());
    return false;
  }
  int slash = path.lastIndexOf('/');
  if (slash > 0) SD_MMC.mkdir(path.substring(0, slash));
  String tempPath = path + ".tmp";
  bool ok = false;
  File f = SD_MMC.open(tempPath, FILE_WRITE);
  if (f) {
    size_t n = f.write((const uint8_t *)content.c_str(), content.length());
    f.close();
    if (n == content.length()) {
      if (SD_MMC.exists(path)) SD_MMC.remove(path);
      ok = SD_MMC.rename(tempPath, path);
    }
    if (!ok) SD_MMC.remove(tempPath);
  }
  if (sdMutex) xSemaphoreGive(sdMutex);
  if (ok) Serial.printf("[CFGW] wrote %s (%u bytes)\n", path.c_str(), (unsigned)content.length());
  else webLogf("error", "Config write failed: %s", path.c_str());
  return ok;
}

void configWriterTask(void *param) {
  ConfigWriteJob *job = NULL;
  for (;;) {
    if (xQueueReceive(cfgWriteQueue, &job, portMAX_DELAY) == pdTRUE && job) {
      bool ok = writeFileAtomicBlocking(job->path, job->content);
      // /save writes land here, so a file saved into a content folder (the cookbook
      // editor) needs that folder's index refreshed or it won't show up until the next
      // full scan. Queued after the write so the rebuild can't run before the file exists.
      // Non-bucket paths (settings, .system-ui.json) are ignored by the callee.
      if (ok) triggerIndexingIfNeeded(job->path);
      delete job;
      job = NULL;
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

// Queue a write. False means the queue is full or not created yet. Boot code can
// just write inline, it isn't on async_tcp.
bool queueConfigWrite(const String &path, const String &content) {
  if (!cfgWriteQueue) return false;
  ConfigWriteJob *job = new ConfigWriteJob{ path, content };
  if (xQueueSend(cfgWriteQueue, &job, pdMS_TO_TICKS(50)) != pdTRUE) {
    delete job;
    return false;
  }
  return true;
}

// UI config (menu pages, downloads, uploads, motd).
// Held and served from RAM, saved to /.system-ui.json through the config writer, and
// only changed through the admin POST /api/ui-config. Pages can still read the file.
const char *UI_CONFIG_PATH = "/.system-ui.json";
String g_uiConfigJson = "{}";
SemaphoreHandle_t uiCfgMutex = NULL;

// The admin's upload and download switches, read whenever the UI config changes so the
// firmware enforces them itself. Each has a master switch and a per-page list (ulOff,
// dlOff: page keys, the folder names in lower case). A page's bit is its place in kMenuPages.
static const char *const kMenuPages[] = { "movies", "shows", "music", "books", "gallery", "files", "games",
                                          "maps", "archive", "translate", "cookbook", "workshop", "chat" };
volatile bool g_guestWritesOff = false;
volatile bool g_downloadsOff = false;
volatile uint32_t g_ulOffMask = 0, g_dlOffMask = 0;
// pages taken off the menu: their page files answer with a "turned off" page instead, so
// typing the address doesn't get round it
volatile uint32_t g_hiddenMask = 0;
static const char *const kMenuPageNames[] = { "Movies", "Shows", "Music", "Books", "Gallery", "Files", "Games",
                                              "Maps", "Archive", "Translate", "Cookbook", "Workshop", "Chat" };
static uint32_t pageBit(const char *key) {
  for (size_t i = 0; i < sizeof(kMenuPages) / sizeof(kMenuPages[0]); ++i) if (!strcasecmp(key, kMenuPages[i])) return 1u << i;
  return 0;
}
// the page a path belongs to: its first folder ("/Gallery/x.jpg" is gallery)
static uint32_t pageBitForPath(const String &path) {
  const int a = path.startsWith("/") ? 1 : 0;
  int b = path.indexOf('/', a);
  if (b < 0) b = path.length();
  char key[16];
  const String first = path.substring(a, b);
  if (first.length() >= sizeof(key)) return 0;
  strcpy(key, first.c_str());
  return pageBit(key);
}
bool guestWritesOffFor(const String &path) { return g_guestWritesOff || (g_ulOffMask & pageBitForPath(path)); }
bool downloadsOffFor(const String &path) { return g_downloadsOff || (g_dlOffMask & pageBitForPath(path)); }
void refreshUiFlags(const String &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  g_guestWritesOff = doc["uploadsDisabled"] | false;
  g_downloadsOff = doc["downloadsDisabled"] | false;
  uint32_t ul = 0, dl = 0;
  for (JsonVariant v : doc["ulOff"].as<JsonArray>()) ul |= pageBit(v | "");
  for (JsonVariant v : doc["dlOff"].as<JsonArray>()) dl |= pageBit(v | "");
  g_ulOffMask = ul;
  g_dlOffMask = dl;
  uint32_t hid = 0;
  for (JsonVariant v : doc["hiddenPages"].as<JsonArray>()) hid |= pageBit(v | "");
  g_hiddenMask = hid;
}

// The page a page file belongs to, for the hidden-pages switch: its own file, the music
// page's tabs, and the built-in games. Readers (epub, pdf, comics) and Watch together are
// not pages of their own on the menu and stay reachable.
static int pageIndexForFile(const String &path) {
  String f = path;
  f.toLowerCase();
  static const char *const files[][2] = {
    { "/movies.html", "movies" }, { "/shows.html", "shows" }, { "/music.html", "music" }, { "/songs.html", "music" },
    { "/playlist.html", "music" }, { "/queue.html", "music" }, { "/books.html", "books" }, { "/gallery.html", "gallery" },
    { "/files.html", "files" }, { "/games.html", "games" }, { "/maps.html", "maps" }, { "/maps-basic.html", "maps" },
    { "/archive.html", "archive" }, { "/translate.html", "translate" }, { "/cookbook.html", "cookbook" },
    { "/workshop.html", "workshop" }, { "/chat.html", "chat" } };
  const char *key = nullptr;
  if (f.startsWith("/games/") && f.endsWith(".html") && f != "/games/watch together.html") key = "games";
  for (auto &m : files) if (!key && f == m[0]) key = m[1];
  if (!key) return -1;
  for (size_t i = 0; i < sizeof(kMenuPages) / sizeof(kMenuPages[0]); ++i) if (!strcmp(key, kMenuPages[i])) return i;
  return -1;
}

// what a hidden page answers with: plain, themed like the rest, and a way back
static void sendPageOff(AsyncWebServerRequest *request, int idx) {
  const char *name = kMenuPageNames[idx];
  String html = F("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Nomad &bull; ");
  html += name;
  html += F(" is turned off</title><link rel=\"stylesheet\" href=\"/master.css\"><script src=\"/theme-boot.js?v=5\"></script><script src=\"/theme-manager.js?v=5\"></script>"
    "<style>body{min-height:100vh;display:grid;place-items:center;background:var(--bg);color:var(--text);margin:0;padding:20px;box-sizing:border-box}"
    ".off{max-width:420px;text-align:center;background:var(--card-bg,var(--card));border:1px solid var(--card-border);border-radius:16px;padding:28px 22px}"
    ".off h1{font-size:1.25rem;margin:0 0 8px;color:var(--text)}.off p{color:var(--muted);margin:0 0 18px;line-height:1.5}"
    ".off a.go{display:inline-block;padding:10px 18px;border-radius:11px;background:var(--primary);color:#fff;text-decoration:none;font-weight:700}"
    ".off small{display:block;margin-top:16px}.off small a{color:var(--muted);font-weight:600}</style></head><body><div class=\"off\"><h1>");
  html += name;
  html += F(" is turned off</h1><p>Everything else on the menu still works.</p>"
    "<a class=\"go\" href=\"/menu\">Back to the menu</a>"
    "<small><a href=\"/admin.html#pages\">Turn it back on (admin)</a></small></div></body></html>");
  AsyncWebServerResponse *r = request->beginResponse(403, "text/html", html);
  r->addHeader("Cache-Control", "no-store");
  request->send(r);
}

void loadUiConfigFromSD() {
  if (!SD_MMC.exists(UI_CONFIG_PATH)) return;
  File f = SD_MMC.open(UI_CONFIG_PATH, FILE_READ);
  if (!f) return;
  String s = f.readString();
  f.close();
  s.trim();
  if (s.length() > 0 && s.length() < 4096) {
    g_uiConfigJson = s;
    refreshUiFlags(s);
  }
}

bool saveSettings() {
  StaticJsonDocument<768> sdoc;
  sdoc["rgbMode"] = settings.rgbMode;
  sdoc["rgbColor"] = settings.rgbColor;
  sdoc["adminPassword"] = settings.adminPassword;
  sdoc["wifiSSID"] = settings.wifiSSID;
  sdoc["wifiPassword"] = settings.wifiPassword;
  sdoc["brightness"] = settings.brightness;
  sdoc["autoGenerateMedia"] = settings.autoGenerateMedia;
  sdoc["flipScreen"] = settings.flipScreen;
  sdoc["dlnaEnabled"] = settings.dlnaEnabled;
  sdoc["staSSID"] = settings.staSSID;
  sdoc["staPassword"] = settings.staPassword;
  sdoc["staPersist"] = settings.staPersist;
  sdoc["dnsCatchAll"] = settings.dnsCatchAll;
  String json;
  serializeJson(sdoc, json);

  // normal path: hand the write to the config writer so the caller (usually
  // a web handler on async_tcp) never blocks on the card
  if (queueConfigWrite(SETTINGS_PATH, json)) return true;

  // boot path, writer not running yet: write inline, this is not async_tcp
  SD_MMC.mkdir("/config"); // Ensure directory exists

  File file = SD_MMC.open(SETTINGS_PATH, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open settings file for writing.");
    return false;
  }

  StaticJsonDocument<768> doc;
  doc["rgbMode"] = settings.rgbMode;
  doc["rgbColor"] = settings.rgbColor;
  doc["adminPassword"] = settings.adminPassword;
  doc["wifiSSID"] = settings.wifiSSID;
  doc["wifiPassword"] = settings.wifiPassword;
  doc["brightness"] = settings.brightness;
  doc["autoGenerateMedia"] = settings.autoGenerateMedia;
  doc["flipScreen"] = settings.flipScreen;
  doc["dlnaEnabled"] = settings.dlnaEnabled;
  doc["staSSID"] = settings.staSSID;
  doc["staPassword"] = settings.staPassword;
  doc["staPersist"] = settings.staPersist;
  doc["dnsCatchAll"] = settings.dnsCatchAll;

  bool success = serializeJson(doc, file) > 0;
  file.close();
  return success;
}

#include "NomadAuth.h"   // the admin password, challenge sign-in, sessions (/auth/*)
#include "NomadActivity.h"   // what is playing, for the admin Activity section, and stopping it
#include "NomadUsage.h"      // what got used: busiest moment, data sent, plays, pages (admin Overview)

// delete recursive
bool deleteRecursive(String path) {
  // Never delete the card root or /config, however the path is spelled. Deleting "/"
  // would walk into //config too.
  if (canonicalPath(path) == "/" || isConfigPath(path)) return false;
  File entry = SD_MMC.open(path);
  if (!entry) return false;

  if (!entry.isDirectory()) {
    entry.close();
    return SD_MMC.remove(path);
  }

  File child;
  int n = 0;
  while ((child = entry.openNextFile())) {
    String childPath = String(path) + "/" + child.name();
    deleteRecursive(childPath);
    child.close();
    // deleting a season folder is hundreds of FAT chain frees in a row. without
    // a breather this hits the task watchdog on the async_tcp task.
    if (++n % 16 == 0) vTaskDelay(pdMS_TO_TICKS(1));
  }

  entry.close();
  return SD_MMC.rmdir(path);
}


// SD recovery globals
volatile bool sdErrorFlag            = false;
unsigned long sdErrorCooldownUntil   = 0;

// Mount as fast as the board will reliably go, stepping down on failure. The last
// step is 1-bit at 20 MHz, for a board or card that can't do 4-bit.
static const char *g_sdModeDesc = "not mounted";

bool mountSDCard() {
  struct { bool oneBit; int khz; const char *desc; } modes[] = {
    { false, SDMMC_FREQ_HIGHSPEED, "4-bit @ 40MHz" },
    { false, SDMMC_FREQ_DEFAULT,   "4-bit @ 20MHz" },
    { true,  SDMMC_FREQ_DEFAULT,   "1-bit @ 20MHz" },
  };
  for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
    if (SD_MMC.begin("/sdcard", modes[i].oneBit, false, modes[i].khz, 12)) {
      g_sdModeDesc = modes[i].desc;
      Serial.printf("[SD] Mounted %s (%s)\n", modes[i].desc, NomadSD.fsTypeName());
      return true;
    }
    Serial.printf("[SD] Mount failed at %s, stepping down\n", modes[i].desc);
    SD_MMC.end();
    delay(200);
  }
  g_sdModeDesc = "not mounted";
  return false;
}

// No card at boot: say so on the little screen instead of sitting on "Booting..."
// forever, and keep probing. inserting the card restarts into a normal boot
static void sdBootHalt(const char *msg) {
  lv_obj_clear_flag(ui_MediaGen, LV_OBJ_FLAG_HIDDEN);
  lv_textarea_set_text(ui_MediaGen, msg);
  for (;;) {
    for (int i = 0; i < 100; ++i) { lv_timer_handler(); delay(30); }  // ~3s
    if (mountSDCard()) esp_restart();
  }
}

bool tryRecoverSDCard() {
    Serial.println("[SD] Attempting recovery…");

    // close streaming handles before unmounting, else an async fill on a dead
    // File is a use-after-free that reboots the device. lock order: streamingFilesMutex -> sdMutex
    if (streamingFilesMutex && xSemaphoreTake(streamingFilesMutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
        bool sdHeld = (!sdMutex) || (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) == pdTRUE);
        for (auto &kv : streamingFiles) kv.second.file.close();
        streamingFiles.clear();
        streamPathIndex.clear();
        activeStreams = 0;

        SD_MMC.end();          // unmount
        delay(1000);           // give hardware a breather
        bool ok = mountSDCard();

        if (sdHeld && sdMutex) xSemaphoreGive(sdMutex);
        xSemaphoreGive(streamingFilesMutex);
        Serial.println(ok ? "[SD] Recovery OK." : "[SD] Recovery failed.");
        return ok;
    }

    // couldnt get the stream map (something wedged holding it), remount under sdMutex alone.
    // give back only what we took, releasing a mutex we don't hold frees it under its owner
    bool sdTaken = (!sdMutex) || (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) == pdTRUE);
    if (!sdTaken) Serial.println("[SD] Recovery: sdMutex timeout, remounting without it.");
    SD_MMC.end();
    delay(1000);
    bool ok = mountSDCard();
    if (sdMutex) xSemaphoreGive(sdMutex);
    Serial.println(ok ? "[SD] Recovery OK." : "[SD] Recovery failed.");
    return ok;
}

String rfc3339Now() {
  // no NTP/RTC offline, so fake it: fixed epoch + uptime so OPDS times at least move
  const time_t REFERENCE_EPOCH = 1752321600; // 2025-07-12T12:00:00Z
  time_t now = REFERENCE_EPOCH + (time_t)(millis() / 1000UL);
  struct tm t;
  gmtime_r(&now, &t);
  char buf[21];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
  return String(buf);
}

// Captive DNS lives in NomadDNS.cpp
AsyncWebServer server(80); // Web server on port 80
std::map<AsyncWebServerRequest*, File> activeUploads;
int connectedClients = 0;
// LED Mode and Color Helper Wrappers
uint8_t currentLEDMode = 0;  // 0=off, 1=rainbow, 2=solid color
uint8_t solidR = 0, solidG = 0, solidB = 0;
void RGB_SetColor(uint8_t r, uint8_t g, uint8_t b) {
    solidR = r;
    solidG = g;
    solidB = b;
    currentLEDMode = 2; 
    Set_Color(g, r, b);
}
extern lv_obj_t *ui_wifi;
extern lv_obj_t *ui_SDcard;
bool lastWifiStatus = false;
bool lastSDStatus = false;
//Globals for SD scan
unsigned long lastUpdateTime = 0;
volatile bool requestIndexing = false;     // set by admin endpoint, consumed by index worker
// true only when the boot change check found changes and queued a reindex.
// a boot with no changes indexes nothing.
volatile bool bootReindexQueued = false;
volatile bool indexingInProgress = false;  // guard so we never run multiple index runs concurrently
volatile bool settingsReady = false;       // set to true after loadSettings() runs

// --- WiFi Mode (station) state ---
bool g_staMode = false;             // true = boot joined an existing network instead of running the hotspot
bool g_safeMode = false;            // NomadSupport.h: crashed SAFE_AFTER times in a row, heavy parts skipped this start
String g_staFailReason = "";        // why the last join attempt failed, written in setup() only, read by /api/wifi-mode
volatile uint32_t g_wifiModeRestartAt = 0;  // millis deadline for a deferred restart after a wifi-mode change, 0 = none

// every generated URL (OPDS, m3u, DLNA) uses this: the assigned STA address
// when joined to a home network, the AP address otherwise
IPAddress nomadLocalIP() {
  return g_staMode ? WiFi.localIP() : WiFi.softAPIP();
}

int maskToCidr(IPAddress mask) {
  uint32_t m = (uint32_t)mask;
  int bits = 0;
  while (m) { bits += m & 1; m >>= 1; }
  return bits;
}

// New scan/index coordination flags
volatile bool sdScanInProgress = false;    // true while SD scan is performing its initial pass
volatile bool sdScanCompleted = false;     // set to true after the initial SD scan completes


// background indexing control

// Update the UI with the number of connected users
void updateUI(int userCount) {
    char buffer[10];
    snprintf(buffer, sizeof(buffer), "%d", userCount);
    LvglLock lock;
    lv_label_set_text(ui_userlabel, buffer);
}
void updateToggleStatus() {
    // hotspot: up = AP iface has its address. station: up = link to the home
    // network still held (the core auto-reconnects on drops)
    bool currentWifiStatus = g_staMode ? (WiFi.status() == WL_CONNECTED)
                                       : (bool)(uint32_t)WiFi.softAPIP();
    if (currentWifiStatus != lastWifiStatus) {
        LvglLock lock;
        if (currentWifiStatus) {
            lv_obj_add_state(ui_wifi, LV_STATE_CHECKED);
            if (!lastWifiStatus) {
                webLog(g_staMode ? "[SYSTEM] WiFi network link verified" : "[SYSTEM] WiFi AP verified successfully", "success");
            }
        } else {
            lv_obj_clear_state(ui_wifi, LV_STATE_CHECKED);
            webLog(g_staMode ? "[SYSTEM] WiFi network link lost - auto-reconnect running" : "[SYSTEM] WiFi AP failure detected - attempting recovery", "error");
        }
        lastWifiStatus = currentWifiStatus;
    }
}

void updateSDStatus() {
    bool currentSDStatus = SD_MMC.cardType() != CARD_NONE;
    if (currentSDStatus != lastSDStatus) {
        LvglLock lock;
        if (currentSDStatus) {
            lv_obj_add_state(ui_SDcard, LV_STATE_CHECKED);
            if (!lastSDStatus) {
                webLog("[SYSTEM] SD card verified successfully", "success");
            }
        } else {
            lv_obj_clear_state(ui_SDcard, LV_STATE_CHECKED);
            webLog("[SYSTEM] SD card failure detected - attempting recovery", "error");
        }
        lastSDStatus = currentSDStatus;
    }
}

// Stream a chunk of text to save RAM
void opdsWrite(AsyncResponseStream *s, const String &chunk) {
    s->print(chunk);
}
//(OPDS thing, MUST FIX THIS ITS SO BROKEN (STILL SO BROKEN, will update the paths to handle new library format... later...))
String xmlEscape(const String &in) {   
  String out;
  for (char c : in) {
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      default:   out += c;        break;
    }
  }
  return out;
}

String slugify(const String &in) {        
  String out;
  for (char c : in) {
    if (isalnum(c))       out += (char)tolower(c);
    else if (c==' ' || c=='_' || c=='-') out += '-';
    // every other char is dropped
  }
  return out;
}

// Get a count of currently connected WiFi clients
void updateClientCount() {
    wifi_sta_list_t wifi_sta_list;
    if (esp_wifi_ap_get_sta_list(&wifi_sta_list) == ESP_OK) {
        connectedClients = wifi_sta_list.num;
        updateUI(connectedClients);
    }
}

String absURL(const String &path) {
    return "http://" + nomadLocalIP().toString() + path;
}


void handleOPDSRoot(AsyncWebServerRequest *request) {
    AsyncResponseStream *res = request->beginResponseStream(
        "application/atom+xml;profile=opds-catalog;kind=navigation");

    opdsWrite(res, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                   "<feed xmlns=\"http://www.w3.org/2005/Atom\" "
                   "xmlns:opds=\"http://opds-spec.org/2010/catalog\">\n");

    opdsWrite(res, "  <id>urn:uuid:nomad-opds-root</id>\n"
                   "  <title>Nomad OPDS Catalog</title>\n"
                   "  <updated>2025-07-12T12:00:00Z</updated>\n"
                   "  <author><name>Nomad Server</name></author>\n");

    // Add required navigation links
    opdsWrite(res, "  <link rel=\"self\" href=\"" + absURL("/opds/root.xml") + "\" "
                   "type=\"application/atom+xml;profile=opds-catalog;kind=navigation\"/>\n");
    opdsWrite(res, "  <link rel=\"start\" href=\"" + absURL("/opds/root.xml") + "\" "
                   "type=\"application/atom+xml;profile=opds-catalog;kind=navigation\"/>\n");

    opdsWrite(res, "  <entry>\n"
                   "    <title>All Books</title>\n"
                   "    <id>urn:uuid:nomad-opds-books</id>\n"
                   "    <updated>2025-07-12T12:00:00Z</updated>\n"
                   "    <link rel=\"http://opds-spec.org/catalog\" "
                   "type=\"application/atom+xml;profile=opds-catalog;kind=acquisition\" "
                   "href=\"" + absURL("/opds/books.xml") + "\"/>\n"
                   "  </entry>\n");

    opdsWrite(res, "</feed>");
    request->send(res);
}

void handleOPDSBooks(AsyncWebServerRequest *request) {
    Serial.println("[OPDS] === handleOPDSBooks() called ===");
    // full recursive walk of /Books, so wait for the card like every other reader
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
      request->send(503, "text/plain", "SD busy");
      return;
    }
    AsyncResponseStream *res =
        request->beginResponseStream(
            "application/atom+xml;profile=opds-catalog;kind=acquisition");

    opdsWrite(res,"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                  "<feed xmlns=\"http://www.w3.org/2005/Atom\" "
                  "xmlns:opds=\"http://opds-spec.org/2010/catalog\">\n");
    opdsWrite(res,
      "  <id>urn:uuid:nomad-opds-books</id>\n"
      "  <title>All Books</title>\n"
      "  <updated>"+rfc3339Now()+"</updated>\n"
      "  <link rel=\"self\"  href=\""+absURL("/opds/books.xml")+"\" "
      "type=\"application/atom+xml;profile=opds-catalog;kind=acquisition\"/>\n"
      "  <link rel=\"start\" href=\""+absURL("/opds/root.xml")+"\" "
      "type=\"application/atom+xml;profile=opds-catalog;kind=navigation\"/>\n");

    std::function<void(const String&)> processDir = [&](const String& dirPath) {
      File dir = SD_MMC.open(dirPath);
      if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
      }
      
      dir.rewindDirectory();
      while (true) {
        File file = dir.openNextFile();
        if (!file) break;

        // name() returns the bare entry name, so build the real path from the directory
        // being walked
        String fileName = String(file.name());
        int lastSlash = fileName.lastIndexOf('/');
        if (lastSlash >= 0) fileName = fileName.substring(lastSlash + 1);
        String fullPath = dirPath + (dirPath.endsWith("/") ? "" : "/") + fileName;
        
        if (file.isDirectory()) {
          if (isComicFolder(fullPath)) {
            String base = fileName;
            String safeTitle = xmlEscape(base);
            String safeId = "urn:uuid:nomad-comic-" + slugify(base);
            
            String coverPath = "placeholder.jpg";
            File comicDir = SD_MMC.open(fullPath);
            if (comicDir && comicDir.isDirectory()) {
              comicDir.rewindDirectory();
              File firstImg;
              while ((firstImg = comicDir.openNextFile())) {
                if (!firstImg.isDirectory()) {
                  String imgName = String(firstImg.name());
                  imgName.toLowerCase();
                  if (imgName.endsWith(".jpg") || imgName.endsWith(".png") || imgName.endsWith(".jpeg")) {
                    coverPath = fullPath.substring(1) + "/" + String(firstImg.name()).substring(String(firstImg.name()).lastIndexOf('/') + 1);
                    firstImg.close();
                    break;
                  }
                }
                firstImg.close();
              }
              comicDir.close();
            }
            
            opdsWrite(res,"  <entry>\n"
                          "    <title>"+safeTitle+" [Comic]</title>\n"
                          "    <id>"+safeId+"</id>\n"
                          "    <updated>"+rfc3339Now()+"</updated>\n"
                          "    <link rel=\"http://opds-spec.org/image/thumbnail\" "
                          "type=\"image/jpeg\" href=\""+absURL("/"+urlencode(coverPath))+"\"/>\n"
                          "    <link rel=\"alternate\" "
                          "type=\"text/html\" "
                          "href=\"" + absURL("/comicreader.html?path=" + urlencode(fullPath)) + "\"/>\n"
                          "  </entry>\n");
          } else {
            processDir(fullPath);
          }
          file.close();
          continue;
        }
        
        String fnLower = fileName;
        fnLower.toLowerCase();
        
        if (!(fnLower.endsWith(".epub") || fnLower.endsWith(".pdf"))) {
          file.close();
          continue;
        }
        
        String base = fileName.substring(0, fileName.lastIndexOf('.'));
        String safeTitle = xmlEscape(base);
        String safeId = "urn:uuid:nomad-book-" + slugify(base);
        String mime = fnLower.endsWith(".epub") ? "application/epub+zip" : "application/pdf";
        
        String parentPath = fullPath.substring(0, fullPath.lastIndexOf('/'));
        String coverPath = parentPath + "/" + base + ".jpg";
        if (!SD_MMC.exists(coverPath)) {
          coverPath = "placeholder.jpg";
        } else {
          coverPath = coverPath.substring(1);
        }
        
        String ext = fnLower.endsWith(".pdf") ? ".pdf" : ".epub";
        String dlPath = fullPath;
        
        opdsWrite(res,"  <entry>\n"
                      "    <title>"+safeTitle+"</title>\n"
                      "    <id>"+safeId+"</id>\n"
                      "    <updated>"+rfc3339Now()+"</updated>\n"
                      "    <link rel=\"http://opds-spec.org/image/thumbnail\" "
                      "type=\"image/jpeg\" href=\""+absURL("/"+urlencode(coverPath))+"\"/>\n"
                      "    <link rel=\"http://opds-spec.org/acquisition\" "
                      "type=\""+mime+"\" "
                      "href=\"" + absURL(dlPath) + "\"/>\n"
                      "  </entry>\n");
        
        file.close();
      }
      
      dir.close();
    };
    
    processDir("/Books");

    if (sdMutex) xSemaphoreGive(sdMutex);
    opdsWrite(res,"</feed>");
    request->send(res);
}
String urlDecode(const String& str) {
    String decoded = "";
    char temp[] = "0x00";
    for (unsigned int i = 0; i < str.length(); i++) {
        if (str[i] == '%') {
            if (i + 2 < str.length()) {
                temp[2] = str[i+1];
                temp[3] = str[i+2];
                decoded += (char) strtol(temp, NULL, 16);
                i += 2;
            }
        } else if (str[i] == '+') {
            decoded += ' ';
        } else {
            decoded += str[i];
        }
    }
    return decoded;
}

#include <map>
#include <set>
#include <utility> // for std::pair

// 1 = refuse playback while an index pass runs. Off because the indexer gives up
// sdMutex every five entries, so streaming can share the card with it.
#ifndef NOMAD_BLOCK_MEDIA_WHILE_INDEXING
#define NOMAD_BLOCK_MEDIA_WHILE_INDEXING 0
#endif

// Same ETag for a file whichever route sends it. A download started as a plain GET
// (sendSdFile) resumes with a Range request here, and the browser only resumes if the
// two agree. Low 32 bits of the size, same as sendSdFile.
static String nomadEtag(time_t mtime, uint64_t size) {
  char etag[40];
  snprintf(etag, sizeof(etag), "\"%lx-%lx\"", (unsigned long)mtime, (unsigned long)size);
  return String(etag);
}

// Content-Disposition for a file the page asked to download: the plain filename= for old
// browsers (ASCII, no quotes) and filename*= with the real name.
static String nomadAttachment(const String &path) {
  const String name = path.substring(path.lastIndexOf('/') + 1);
  String ascii;
  for (size_t i = 0; i < name.length(); ++i) {
    const char c = name.charAt(i);
    ascii += ((uint8_t)c < 0x20 || (uint8_t)c > 0x7e || c == '"' || c == '\\') ? '_' : c;
  }
  return "attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + urlencode(name);
}

void handleRangeRequest(AsyncWebServerRequest *request) {
#if NOMAD_BLOCK_MEDIA_WHILE_INDEXING
  if (indexingTasksActive) {
    Serial.println("[RangeHandler] Blocked: indexing in progress");
    request->send(503, "text/plain", "Indexing in progress. Please wait...");
    return;
  }
#endif

  String filePath;
  if (request->hasParam("file")) {
    filePath = request->getParam("file")->value();
  } else {
    // url() is already percent-decoded by the server, decoding it again breaks a
    // literal % in a filename
    filePath = request->url();
  }

  if (!filePath.startsWith("/")) filePath = "/" + filePath;
  filePath = normalizePath(filePath);
  if (isConfigPath(filePath)) {
    request->send(404, "text/plain", "File not found");
    return;
  }

  // Admission control, in two parts: a media slot first (see MEDIA_SLOTS), then the
  // memory floor. Only audio and video take a slot. A HEAD never takes one.
  MediaSlotGuard slot;
  bool admitTimed = false;
  {
    String admitLower = filePath;
    admitLower.toLowerCase();
    admitTimed = isTimedMediaExtension(admitLower);
    if (request->method() != HTTP_HEAD && admitTimed) {
      if (!slot.take()) {
        Serial.printf("[RangeHandler] %d media responses in flight, refusing: %s\n",
                      (int)inFlightMediaResponses, filePath.c_str());
        AsyncWebServerResponse *full = request->beginResponse(503, "text/plain", "Server busy - retry shortly");
        full->addHeader("Retry-After", "1");
        request->send(full);
        return;
      }
    }
  }

  // Every response in flight costs DMA memory, and the card stops reading before the
  // heap is actually gone. Turning one client away with a retry beats all of them failing.
  // Cover art and page files get a lower floor, with a film playing the full floor
  // turns most covers on a grid away.
  // A film already playing gets the lowest floor. Open-ended replies are capped at 8 MB,
  // so every viewer comes back about once a minute for the next piece, right after its
  // last response gave its memory back. At the full floor those viewers stall.
  const bool continuing = admitTimed && continuingStream(filePath, request);
  const size_t admitFloor = continuing ? CONTINUE_DMA_FLOOR : admitTimed ? STREAM_DMA_FLOOR : SMALL_FILE_DMA_FLOOR;
  if (streamDmaFree() < admitFloor) {
    Serial.printf("[RangeHandler] DMA heap %u below floor, refusing: %s\n",
                  (unsigned)streamDmaFree(), filePath.c_str());
    AsyncWebServerResponse *busy = request->beginResponse(503, "text/plain", "Server busy - retry shortly");
    busy->addHeader("Retry-After", "1");
    request->send(busy);
    return;
  }

  // 1 s wait for the card
  if (sdMutex) {
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
      Serial.printf("[RangeHandler] SD mutex timeout for: %s\n", filePath.c_str());
      request->send(503, "text/plain", "SD busy, retrying shortly");
      return;
    }
  }

  auto releaseSd = [&](){
    if (sdMutex) xSemaphoreGive(sdMutex);
  };

  bool fileExists = SD_MMC.exists(filePath);
  if (!fileExists) {
    // exists() also answers false when the lookup couldn't get a DMA bounce buffer,
    // which happens under load. Ask once more before believing it, a failed allocation
    // usually works on the next try.
    fileExists = SD_MMC.exists(filePath);
    if (fileExists) {
      Serial.printf("[RangeHandler] Lookup failed once then succeeded: %s\n", filePath.c_str());
    }
  }

  if (!fileExists) {
    releaseSd();
    // Below the floor a failed lookup is probably memory, not a missing file. A 404 would
    // tell the client to stop asking for something that's still there.
    if (streamDmaFree() < admitFloor) {
      Serial.printf("[RangeHandler] Lookup failed under memory pressure (%u free): %s\n",
                    (unsigned)streamDmaFree(), filePath.c_str());
      AsyncWebServerResponse *busy = request->beginResponse(503, "text/plain", "Server busy - retry shortly");
      busy->addHeader("Retry-After", "1");
      request->send(busy);
      return;
    }
    Serial.printf("[RangeHandler] File not found: %s\n", filePath.c_str());
    request->send(404, "text/plain", "File not found");
    return;
  }

  // 64-bit stat: fs::File::size() wraps at 4 GB, which would reject every range past it.
  uint64_t fileSize = 0;
  time_t fileMtime = 0;
  SD_MMC.fileStat64(filePath.c_str(), fileSize, fileMtime);   // 0 for a folder or a missing file
  const String etag = nomadEtag(fileMtime, fileSize);

  if (fileSize == 0) {
    // fileSize64 also answers 0 for a directory or an empty file. Only an open failure
    // is an SD error. Flagging the others would remount the card and kill every stream.
    File probe = SD_MMC.open(filePath, FILE_READ);
    bool opened = (bool)probe;
    bool isDir = opened && probe.isDirectory();
    if (opened) probe.close();
    releaseSd();
    if (isDir) {
      request->send(404, "text/plain", "Not a file");
      return;
    }
    if (opened) {
      request->send(200, "application/octet-stream", "");
      return;
    }
    Serial.printf("[SD] open failed for existing path: '%s'\n", filePath.c_str());
    sdErrorFlag = true;
    sdErrorCooldownUntil = millis() + 5000;
    request->send(503, "text/plain", "SD error");
    return;
  }
  releaseSd();

  if (request->method() == HTTP_HEAD) {
    AsyncWebServerResponse *headResponse = request->beginResponse(200, "application/octet-stream", "");
    headResponse->addHeader("Accept-Ranges", "bytes");
    headResponse->addHeader("ETag", etag);
    headResponse->addHeader("Content-Length", String(fileSize));
    headResponse->addHeader("Cache-Control", "public, max-age=3600");
    headResponse->addHeader("Pragma", "no-cache");
    // DLNA players often probe with HEAD before playing
    headResponse->addHeader("transferMode.dlna.org", "Streaming");
    headResponse->addHeader("contentFeatures.dlna.org", "DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000");
    request->send(headResponse);
    return;
  }

  String rangeHeader = "";
  if (request->hasHeader("Range")) rangeHeader = request->header("Range");
  // A browser resuming a download sends If-Range with the ETag it was first given. Same
  // file: carry on from where it stopped. Changed: send the whole new file, a range would
  // splice two files together (RFC 9110 13.1.5).
  bool resumingDownload = false;
  if (rangeHeader.length() && request->hasHeader("If-Range")) {
    if (request->header("If-Range") == etag) resumingDownload = true;
    else rangeHeader = "";
  }
  // A download wants the rest of the file in one reply. Only a player's open-ended range
  // is cut short (below): it seeks soon anyway, and a long reply holds the stream handle.
  const bool download = request->hasParam("download");
  // the admin turned downloads off for this page (or for all): its download links stop,
  // playing and reading in the page still work. a signed-in admin can still download
  if (download && downloadsOffFor(filePath) && !(isAdminAuthRequired() && checkAdminAuth(request))) {
    request->send(403, "application/json", "{\"error\":\"Downloads are turned off for this page\"}");
    return;
  }
  const bool wholeRest = rangeHeader.length() == 0 || resumingDownload || download;
  // fetching new firmware takes the memory replies use, players retry shortly
  if (g_fwPausingMedia) {
    AsyncWebServerResponse *busy = request->beginResponse(503, "text/plain", "Nomad is updating its firmware");
    if (busy) { busy->addHeader("Retry-After", "30"); request->send(busy); } else request->send(503);
    return;
  }
  // the admin stopped this device on this file (Activity, Stop): refused for a while, so a
  // player that reconnects at once doesn't simply carry on
  if (nomadActBlocked(request, filePath)) {
    request->send(403, "application/json", "{\"error\":\"Stopped by the admin\"}");
    return;
  }

  uint64_t startByte = 0;
  uint64_t endByte = fileSize - 1;
  bool openEndedRange = false;

  if (rangeHeader.length() && rangeHeader.startsWith("bytes=")) {
    int dashIndex = rangeHeader.indexOf('-');
    // "bytes=-N" asks for the last N bytes
    bool suffixRange = (dashIndex == 6);
    // strtoull: offsets into a >4 GB exFAT file need 64 bits
    if (dashIndex > 6) {
      startByte = (uint64_t)strtoull(rangeHeader.substring(6, dashIndex).c_str(), nullptr, 10);
    }
    if (dashIndex + 1 < rangeHeader.length()) {
      String endStr = rangeHeader.substring(dashIndex + 1);
      if (endStr.length() > 0) {
        uint64_t v = (uint64_t)strtoull(endStr.c_str(), nullptr, 10);
        if (suffixRange) {
          startByte = (v >= fileSize) ? 0 : (fileSize - v);
          endByte = fileSize - 1;
        } else {
          endByte = v;
        }
      } else {
        openEndedRange = true;
      }
    } else {
      openEndedRange = true;
    }
  }

  if (openEndedRange && !wholeRest && (endByte - startByte) > (8ULL * 1024 * 1024)) {
    endByte = startByte + (8ULL * 1024 * 1024) - 1;
  }

  if (startByte >= fileSize) {
    // RFC 9110: a start beyond EOF gets a 416
    AsyncWebServerResponse *r416 = request->beginResponse(416, "text/plain", "Range not satisfiable");
    r416->addHeader("Content-Range", "bytes */" + String(fileSize));
    request->send(r416);
    return;
  }
  if (endByte >= fileSize) endByte = fileSize - 1;
  if (startByte > endByte) startByte = endByte;

  // A response body must fit size_t, so cap any single reply. Fewer bytes than asked is
  // fine as long as Content-Range says so. Downloads get nearly the full 4 GB, a smaller
  // cap gets saved by some download managers as the whole file. Player ranges stay at 256 MB.
  const uint64_t MAX_RESPONSE_BYTES = wholeRest ? 0xFFFFF000ULL : 256ULL * 1024 * 1024;
  if (endByte - startByte + 1 > MAX_RESPONSE_BYTES) {
    endByte = startByte + MAX_RESPONSE_BYTES - 1;
  }
  size_t contentLength = (size_t)(endByte - startByte + 1);
  // a plain GET of a file too big for one body degrades to partial content
  bool forcedPartial = (rangeHeader.length() == 0) && (endByte + 1 < fileSize);

  String mimeType = "application/octet-stream";
  String pLower = filePath;
  pLower.toLowerCase();

  // Go by the extension for whether this is a stream. The Content-Type chain below
  // doesn't know .mkv, .mov, .ts or .m2ts, and those still need the DLNA headers.
  bool isMediaStream = isTimedMediaExtension(pLower);
  if (pLower.endsWith(".epub")) mimeType = "application/epub+zip";
  else if (pLower.endsWith(".pdf")) mimeType = "application/pdf";
  else if (pLower.endsWith(".mp3")) { mimeType = "audio/mpeg"; isMediaStream = true; }
  else if (pLower.endsWith(".flac")) { mimeType = "audio/flac"; isMediaStream = true; }
  else if (pLower.endsWith(".wav")) { mimeType = "audio/wav"; isMediaStream = true; }
  else if (pLower.endsWith(".ogg")) { mimeType = "audio/ogg"; isMediaStream = true; }
  else if (pLower.endsWith(".aac")) { mimeType = "audio/aac"; isMediaStream = true; }
  else if (pLower.endsWith(".m4a")) { mimeType = "audio/mp4"; isMediaStream = true; }
  else if (pLower.endsWith(".mp4")) { mimeType = "video/mp4"; isMediaStream = true; }
  else if (pLower.endsWith(".webm")) { mimeType = "video/webm"; isMediaStream = true; }
  else if (pLower.endsWith(".m4v")) { mimeType = "video/x-m4v"; isMediaStream = true; }
  else if (pLower.endsWith(".jpg") || pLower.endsWith(".jpeg")) mimeType = "image/jpeg";
  else if (pLower.endsWith(".png")) mimeType = "image/png";
  else if (pLower.endsWith(".webp")) mimeType = "image/webp";
  else if (pLower.endsWith(".gif")) mimeType = "image/gif";
  else if (pLower.endsWith(".bmp")) mimeType = "image/bmp";
  else if (pLower.endsWith(".avif")) mimeType = "image/avif";
  else if (pLower.endsWith(".cbz")) mimeType = "application/vnd.comicbook+zip";
  else if (pLower.endsWith(".cbr")) mimeType = "application/vnd.comicbook-rar";

  if (isMediaStream && startByte < 65536) {
    String fileName = filePath.substring(filePath.lastIndexOf('/') + 1);
    webLog("Playing: " + fileName + " (" + humanSize(fileSize) + ") [Media]", "media");
  }

  // Mark a stream as live. This defers the start of a full scan and slows the storage
  // monitor. Don't call shutdownBackgroundTasksForStreaming() here, it stalls the request
  // and drops queued index work.
  if (isMediaStream && contentLength > 10000) {
    mediaStreamingActive = true;
    lastStreamIoMs = millis();
  }

  if (pLower.indexOf("/archive/") >= 0) {
    mediaStreamingActive = true;
    lastStreamIoMs = millis();
  }

  uint32_t streamId = 0;
  if (streamingFilesMutex && xSemaphoreTake(streamingFilesMutex, pdMS_TO_TICKS(300)) == pdTRUE) {

    bool sdHeld = (!sdMutex) || (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) == pdTRUE);
    if (!sdHeld) {
      xSemaphoreGive(streamingFilesMutex);
      request->send(503, "text/plain", "SD busy");
      return;
    }
    auto pidx = streamPathIndex.find(filePath);
    if (pidx != streamPathIndex.end()) {
      uint32_t existingId = pidx->second;
      auto eit = streamingFiles.find(existingId);

      // a file replaced since this handle opened it (a save, a copy over it) is a new file,
      // the old handle would serve the old contents
      if (eit != streamingFiles.end() && eit->second.file.isOpen() && eit->second.file.size() != fileSize &&
          !eit->second.inflight) {
        streamUnpublish(eit->second.path, eit->first);
        eit->second.file.close();
        streamingFiles.erase(eit);
        eit = streamingFiles.end();
      }
      if (eit != streamingFiles.end() && eit->second.file.isOpen()) {
        // Join this handle only when nothing else is reading it.
        // Sequential playback stays on one handle, the previous chunk is done by the time
        // the next range arrives. A resume or scrub while the first response is still
        // filling gets its own handle, sharing makes both re-seek on every fill.
        // No seek here: readAt() is the only positioned reader of a stream handle. If a
        // plain file.read() is ever added to the fill closure, put the seek back.
        const bool busy = eit->second.active &&
                          (millis() - eit->second.lastActivity) < STREAM_BUSY_QUIET_MS;
        if (!busy) {
          eit->second.lastActivity = millis();
          eit->second.lastEndByte = endByte;
          eit->second.active = true;
          eit->second.inflight++;
          streamId = existingId;
          Serial.printf("[Stream] Reuse #%u at %llu: %s (heap=%u)\n",
                        streamId, (unsigned long long)startByte, filePath.c_str(),
                        (unsigned)ESP.getFreeHeap());
          if (sdMutex) xSemaphoreGive(sdMutex);
          xSemaphoreGive(streamingFilesMutex);
          goto stream_ready;
        }
        // Busy: open a second handle for this path. The existing one is left for its own
        // response and goes on eviction or the 120 s idle reap. The new one becomes the
        // published handle, so this reader's next chunk finds it.
        Serial.printf("[Stream] #%u busy, private handle at %llu: %s\n",
                      existingId, (unsigned long long)startByte, filePath.c_str());
      } else {
        streamPathIndex.erase(pidx);
        if (eit != streamingFiles.end()) streamingFiles.erase(eit);
      }
    }

    bool isArchive = (filePath.indexOf("/Archive/") >= 0) || (filePath.indexOf("/archive/") >= 0);
    if (isArchive) {
      for (auto ait = streamingFiles.begin(); ait != streamingFiles.end(); ) {
        const String &ap = ait->second.path;
        if (ap.indexOf("/Archive/") >= 0 || ap.indexOf("/archive/") >= 0) {
          Serial.printf("[Stream] Archive single-handle: closing #%u (%s)\n", ait->first, ap.c_str());
          streamUnpublish(ait->second.path, ait->first);
          ait->second.file.close();
          ait = streamingFiles.erase(ait);
        } else {
          ++ait;
        }
      }
    }

    if ((int)streamingFiles.size() >= MAX_CONCURRENT_STREAMS) {
      // Evicting a handle that's still filling cuts that response short. Take the oldest
      // idle handle first and only fall back to a live one if every slot is busy.
      uint32_t oldestId = 0,        oldestAnyId = 0;
      unsigned long oldestTime = 0xFFFFFFFFUL, oldestAnyTime = 0xFFFFFFFFUL;
      const unsigned long evictNow = millis();
      for (auto &kv : streamingFiles) {
        // unsigned subtraction, so millis() rollover is fine
        const bool kvBusy = kv.second.active &&
                            (evictNow - kv.second.lastActivity) < STREAM_BUSY_QUIET_MS;
        if (kv.second.lastActivity < oldestAnyTime) {
          oldestAnyTime = kv.second.lastActivity;
          oldestAnyId = kv.first;
        }
        if (!kvBusy && kv.second.lastActivity < oldestTime) {
          oldestTime = kv.second.lastActivity;
          oldestId = kv.first;
        }
      }
      if (!oldestId) {   // every slot is busy: fall back to the oldest of them
        oldestId = oldestAnyId;
        oldestTime = oldestAnyTime;
      }
      if (oldestId) {
        auto it = streamingFiles.find(oldestId);
        if (it != streamingFiles.end()) {
          Serial.printf("[Stream] Evicting #%u (%s)\n", oldestId, it->second.path.c_str());
          streamUnpublish(it->second.path, it->first);
          it->second.file.close();
          streamingFiles.erase(it);
        }
      }
    }

    StreamHandle sh;
    if (!sh.file.open(filePath.c_str())) {
      Serial.printf("[Stream] Failed to open: %s\n", filePath.c_str());
      if (sdMutex) xSemaphoreGive(sdMutex);
      xSemaphoreGive(streamingFilesMutex);
      request->send(503, "text/plain", "Failed to open file");
      return;
    }
    sh.file.seek(startByte);
    streamId = nextStreamId++;
    if (nextStreamId == 0) nextStreamId = 1;
    sh.path = filePath;
    sh.lastActivity = millis();
    sh.lastEndByte = endByte;
    sh.active = true;
    sh.inflight = 1;
    streamingFiles[streamId] = std::move(sh);  // NomadFile64 is move-only
    streamPathIndex[filePath] = streamId;
    activeStreams = streamingFiles.size();
    Serial.printf("[Stream] New #%u at %llu: %s (active=%d heap=%u)\n",
                  streamId, (unsigned long long)startByte, filePath.c_str(), activeStreams,
                  (unsigned)ESP.getFreeHeap());
    if (sdMutex) xSemaphoreGive(sdMutex);
    xSemaphoreGive(streamingFilesMutex);
  } else {
    Serial.printf("[Stream] Mutex timeout for: %s\n", filePath.c_str());
    request->send(503, "text/plain", "Server busy");
    return;
  }

  stream_ready:

  // Read position for this response. Two clients can share one handle, so each response
  // keeps its own offset.
  auto streamPos = std::make_shared<uint64_t>(startByte);
  // Per-response read-ahead (NomadReadAhead.h). Empty until the mode is on and a fill
  // needs it, freed with the response.
  auto raCache = std::make_shared<NomadReadAheadCache>();
  // The last byte this response promised. A Content-Length response can't stop short,
  // see the filler.
  const uint64_t lastByte = endByte;
  const String streamFilePath = filePath;

  AsyncWebServerResponse *response = request->beginResponse(
    mimeType,
    contentLength,
    [streamId, streamPos, lastByte, streamFilePath, raCache](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      // busy mutex isnt end-of-data: returning 0 truncates the response, TRY_AGAIN retries later
      if (streamingFilesMutex && xSemaphoreTake(streamingFilesMutex, pdMS_TO_TICKS(150)) != pdTRUE) {
        return RESPONSE_TRY_AGAIN;
      }

      auto it = streamingFiles.find(streamId);
      if (it == streamingFiles.end()) {
        // The handle was evicted, or reaped while a paused player stopped reading, with
        // bytes still owed. Returning 0 doesn't end a Content-Length body, the response
        // just re-polls forever holding the socket. Closing the client from here deletes
        // the request we were called from. So re-open the file under the same id and
        // carry on from streamPos.
        StreamHandle sh;
        bool reopened = false;
        if (!sdMutex) {
          reopened = sh.file.open(streamFilePath.c_str());
        } else if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(150)) == pdTRUE) {
          reopened = sh.file.open(streamFilePath.c_str());
          xSemaphoreGive(sdMutex);
        }
        if (!reopened) {
          xSemaphoreGive(streamingFilesMutex);
          return RESPONSE_TRY_AGAIN;   // busy or out of memory: ask again, never 0
        }
        sh.path = streamFilePath;
        sh.lastActivity = millis();
        sh.lastEndByte = lastByte;
        sh.active = true;
        sh.inflight = 1;   // this reply, still open, its onDisconnect counts it down
        streamingFiles[streamId] = std::move(sh);
        it = streamingFiles.find(streamId);
        Serial.printf("[Stream] Re-opened #%u at %llu: %s\n",
                      streamId, (unsigned long long)*streamPos, streamFilePath.c_str());
      }

      NomadFile64 &file = it->second.file;
      it->second.lastActivity = millis();
      lastStreamIoMs = it->second.lastActivity;

      size_t toRead = maxLen;
      if (toRead > 16384) toRead = 16384;

      size_t bytesRead = 0;
      bool sdBusy = false;
      const uint64_t fillPos = *streamPos;
      const int64_t fillT0 = esp_timer_get_time();
      if (g_nomadAlignedReads && g_nomadRaDma) {
        // aligned multi-sector reads into the DMA buffer, served from this response's cache
        bytesRead = nomadReadAheadFill(*raCache, file, *streamPos, buffer, toRead, sdMutex, sdBusy);
        *streamPos += bytesRead;
      } else if (!sdMutex) {
        bytesRead = file.readAt(*streamPos, buffer, toRead);
        *streamPos += bytesRead;
      } else if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1500)) == pdTRUE) {
        // readAt keeps the seek and the read in one lock acquisition, so another
        // reader of the same file cannot move the handle in between
        bytesRead = file.readAt(*streamPos, buffer, toRead);
        *streamPos += bytesRead;
        xSemaphoreGive(sdMutex);
      } else {
        sdBusy = true;  // transient: retry this fill later, don't truncate
      }
      // Hand the handle back once this response has sent its range, so the next chunk
      // rejoins it instead of opening a second one. A response that dies early never gets
      // here, its handle is freed by going quiet (STREAM_BUSY_QUIET_MS).
      if (!sdBusy && *streamPos > it->second.lastEndByte) {
        it->second.active = false;
      }
      xSemaphoreGive(streamingFilesMutex);
      nomadFillNote(maxLen, buffer, fillPos, bytesRead,
                    (uint32_t)(esp_timer_get_time() - fillT0), sdBusy);

      if (sdBusy) return RESPONSE_TRY_AGAIN;
      // An empty read with bytes still owed means the sdmmc bounce buffer didn't
      // allocate (the range was clamped to the file size). Returning 0 would wedge the
      // response, so ask again.
      if (bytesRead == 0 && *streamPos <= lastByte) return RESPONSE_TRY_AGAIN;
      return bytesRead;
    }
  );

  // beginResponse returns NULL on low heap, and using it would panic async_tcp and take
  // the whole server down. closeStreamById frees the handle the response would have owned.
  if (!response) {
    Serial.printf("[Stream] beginResponse OOM for %s (heap=%u)\n",
                  filePath.c_str(), (unsigned)ESP.getFreeHeap());
    closeStreamById(streamId);
    AsyncWebServerResponse *oom = request->beginResponse(503, "text/plain", "Server low on memory - retry");
    if (oom) { oom->addHeader("Retry-After", "1"); request->send(oom); }
    else request->send(503, "text/plain", "Server low on memory - retry");
    return;
  }

  if (rangeHeader.length() > 0 || forcedPartial) {
    // forcedPartial: no Range was sent but the file is too big for one body, so
    // answer 206 with a real Content-Range instead of a truncated 200
    response->setCode(206);
    response->addHeader("Content-Range", "bytes " + String(startByte) + "-" + String(endByte) + "/" + String(fileSize));
  } else {
    response->setCode(200);
  }

  response->addHeader("Access-Control-Allow-Origin", "*");
  response->addHeader("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS");
  response->addHeader("Access-Control-Allow-Headers", "Range, Content-Type, Accept");
  response->addHeader("Accept-Ranges", "bytes");
  response->addHeader("ETag", etag);
  if (download) response->addHeader("Content-Disposition", nomadAttachment(filePath));
  response->addHeader("Content-Length", String(contentLength));
  response->addHeader("Cache-Control", "public, max-age=3600");
  response->addHeader("Connection", "close");
  if (isMediaStream) {
    // DLNA players need these on the stream itself. Samsung refuses to play without
    // transferMode + contentFeatures, LG will not seek without realTimeInfo.
    response->addHeader("transferMode.dlna.org", "Streaming");
    response->addHeader("contentFeatures.dlna.org", "DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000");
    response->addHeader("realTimeInfo.dlna.org", "DLNA.ORG_TLAG=*");
  }
  // Hand the media slot to onDisconnect. With Connection: close one disconnect is one
  // response. After release() the guard's destructor does nothing.
  // films, songs and downloads show in the admin Activity section, with this reply's read
  // position as their progress
  const uint32_t actId = (isMediaStream || download)
      ? nomadActNote(request, filePath, fileSize, endByte, download, streamPos) : 0;
  {
    const uint32_t token = slot.token;
    const int held = slot.release();   // -1 with no slot, which mediaSlotGive ignores
    request->onDisconnect([held, token, actId, request, streamId]() {
      mediaSlotGive(held, token);
      nomadActGone(actId, request);
      streamReplyDone(streamId);
    });
  }
  // A bigger send window makes one viewer a lot faster, but every byte of it is internal
  // DMA memory. Films split about 40 KB evenly (capped at NOMAD_MEDIA_SNDBUF each), and
  // past that fall back to the core's 5,744 B, so the card reads keep the memory they need
#if NOMAD_BENCH
  if (!benchApplySndBuf(request))
#endif
  {
    const int films = inFlightMediaResponses > 0 ? inFlightMediaResponses : 1;
    uint32_t share = (40u * 1024u) / (uint32_t)films;
    if (share > NOMAD_MEDIA_SNDBUF) share = NOMAD_MEDIA_SNDBUF;
    nomadRaiseSndBuf(request, share > 5744 ? share : 0);
  }
  request->send(response);
}
void handleListFiles(AsyncWebServerRequest *request) {
    if (!request->hasParam("dir")) {
        request->send(400, "application/json", "{\"error\":\"Missing 'dir' parameter\"}");
        return;
    }
    String dir = request->getParam("dir")->value();

    // chat and the live whiteboard poll this every couple of seconds, so it has
    // to fail fast rather than wait on the SdFat lock with no timeout
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        request->send(503, "application/json", "{\"error\":\"SD busy\"}");
        return;
    }
    auto listDone = [&]() { if (sdMutex) xSemaphoreGive(sdMutex); };

    if (!SD_MMC.exists(dir)) {
        listDone();
        request->send(404, "application/json", "{\"error\":\"Directory not found\"}");
        return;
    }

    File directory = SD_MMC.open(dir);
    if (!directory || !directory.isDirectory()) {
        if (directory) directory.close();
        listDone();
        request->send(404, "application/json", "{\"error\":\"Not a directory\"}");
        return;
    }

    // ArduinoJson 7 grows a document on demand, the size passed to DynamicJsonDocument
    // doesn't limit anything and overflowed() only trips on a real allocation failure.
    // The whole response also has to fit in one String, so cap it ourselves.
    static const size_t LIST_MAX_BYTES = 49152;   // ~800 entries, far past any real folder
    DynamicJsonDocument doc(LIST_MAX_BYTES);
    JsonArray arr = doc.to<JsonArray>();
    bool truncated = false;
    size_t approxBytes = 2;                       // the enclosing []

    File file = directory.openNextFile();
    while (file) {
        JsonObject f = arr.createNestedObject();

        String filename = String(file.name());
        // Strip dir prefix
        if (filename.startsWith(dir)) {
            filename = filename.substring(dir.length());
            if (filename.startsWith("/")) filename = filename.substring(1);
        }

        if (file.isDirectory()) {
            // Append slash to indicate directory
            f["name"] = filename + "/";
            f["size"] = 0;
            f["isDir"] = true;
        } else {
            f["name"] = filename;
            uint64_t fsz = (uint64_t)file.size();
            if (fsz == (uint64_t)SIZE_MAX) fsz = SD_MMC.fileSize64(file.path());
            f["size"] = fsz;
            f["isDir"] = false;
        }
        // {"name":"...","size":N,"isDir":false} plus the separator, near enough
        approxBytes += filename.length() + 48;

        file = directory.openNextFile();
        if (approxBytes > LIST_MAX_BYTES) { truncated = true; break; }
        if (doc.overflowed()) { truncated = true; break; }   // heap actually ran out
        yield();
    }
    if (file) file.close();   // breaking out of the loop leaves the handle open

    // A marker entry, since the response is a bare array. Callers that read it know to
    // go to the NDJSON index for the full listing.
    if (truncated) {
        JsonObject t = arr.createNestedObject();
        t["name"] = "";
        t["truncated"] = true;
        Serial.printf("[ListFiles] '%s' truncated at %u entries\n", dir.c_str(), (unsigned)(arr.size() - 1));
    }

    // Serialize and send
    String response;
    serializeJson(arr, response);
    directory.close();
    listDone();
    request->send(200, "application/json", response);
}

// Archive manifest
String escapeJsonString(const String &in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in.charAt(i);
    if (c == '\\' || c == '\"') {
      out += '\\';
      out += c;
    } else if (c == '\b') out += "\\b";
    else if (c == '\f') out += "\\f";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

static String archiveTypeForExt(const String &lowerName) {
  if (lowerName.endsWith(".zim")) return "zim";
  // .zimaa .. .zimzz split parts
  int len = lowerName.length();
  if (len > 6 && lowerName.lastIndexOf(".zim") == len - 6) {
    char a = lowerName.charAt(len - 2), b = lowerName.charAt(len - 1);
    if (a >= 'a' && a <= 'z' && b >= 'a' && b <= 'z') return "zim";
  }
  if (lowerName.endsWith(".mbtiles")) return "maps";     // future: map tiles
  if (lowerName.endsWith(".nes") || lowerName.endsWith(".sfc") || lowerName.endsWith(".smc") ||
      lowerName.endsWith(".gb")  || lowerName.endsWith(".gbc") || lowerName.endsWith(".gba") ||
      lowerName.endsWith(".md")  || lowerName.endsWith(".z64")) return "rom"; // future: EmulatorJS
  return "file";
}

void handleArchiveList(AsyncWebServerRequest *request) {
  struct ArchivePart { String path; uint64_t size; };
  struct ArchiveGroup { String id; String type; bool split; std::vector<ArchivePart> parts; };
  std::vector<ArchiveGroup> groups;

  bool sdLocked = false;
  if (sdMutex) {
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
      request->send(503, "application/json", "{\"error\":\"SD busy\"}");
      return;
    }
    sdLocked = true;
  }

  File root = SD_MMC.open("/Archive");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    if (sdLocked) xSemaphoreGive(sdMutex);
    // No /Archive folder is a normal setup, not an error: empty manifest.
    AsyncWebServerResponse *r = request->beginResponse(200, "application/json", "{\"archives\":[]}");
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
    return;
  }

  int fileCount = 0;
  File file = root.openNextFile();
  while (file && fileCount < 128) {
    if (!file.isDirectory()) {
      fileCount++;
      String name = String(file.name());
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (!name.startsWith(".")) {
        String lower = name;
        lower.toLowerCase();
        String type = archiveTypeForExt(lower);

        // id = filename minus extension, split parts (.zimaa) drop the suffix too so they share one id
        String id = name;
        bool isSplitPart = false;
        int len = lower.length();
        if (type == "zim" && len > 6 && lower.lastIndexOf(".zim") == len - 6 && !lower.endsWith(".zim")) {
          isSplitPart = true;
          id = name.substring(0, len - 6);
        } else {
          int dot = id.lastIndexOf('.');
          if (dot > 0) id = id.substring(0, dot);
        }

        ArchiveGroup *g = nullptr;
        for (auto &existing : groups) {
          if (existing.id == id && existing.type == type) { g = &existing; break; }
        }
        if (!g) {
          groups.push_back(ArchiveGroup{ id, type, false, {} });
          g = &groups.back();
        }
        if (isSplitPart) g->split = true;
        // size() sticks at SIZE_MAX past 4 GB, then ask fileSize64. big ZIMs are why the
        // exFAT build exists
        uint64_t psz = (uint64_t)file.size();
        if (psz == (uint64_t)SIZE_MAX) psz = SD_MMC.fileSize64(file.path());
        g->parts.push_back(ArchivePart{ String("/Archive/") + name, psz });
      }
    }
    file.close();
    file = root.openNextFile();
    yield();
  }
  root.close();
  if (sdLocked) xSemaphoreGive(sdMutex);

  String resp; resp.reserve(12 * 1024);   // ~128 parts at ~90B each, no realloc churn
  resp = "{\"archives\":[";
  bool firstGroup = true;
  for (auto &g : groups) {
    // Alphabetical part order == byte order for zimsplit suffixes.
    std::sort(g.parts.begin(), g.parts.end(),
              [](const ArchivePart &a, const ArchivePart &b) { return a.path < b.path; });
    uint64_t total = 0;
    for (auto &p : g.parts) total += p.size;

    if (!firstGroup) resp += ",";
    firstGroup = false;
    resp += "{\"id\":\"" + escapeJsonString(g.id) + "\",\"name\":\"" + escapeJsonString(g.id) + "\"";
    resp += ",\"type\":\"" + g.type + "\",\"split\":" + (g.split ? "true" : "false");
    resp += ",\"totalSize\":" + String((unsigned long long)total) + ",\"parts\":[";
    bool firstPart = true;
    for (auto &p : g.parts) {
      if (!firstPart) resp += ",";
      firstPart = false;
      resp += "{\"path\":\"" + escapeJsonString(p.path) + "\",\"size\":" + String((unsigned long long)p.size) + "}";
    }
    resp += "]}";
  }
  resp += "]}";

  AsyncWebServerResponse *r = request->beginResponse(200, "application/json", resp);
  r->addHeader("Access-Control-Allow-Origin", "*");
  request->send(r);
  Serial.printf("[ARCHIVE] /api/archive-list: %d group(s), %u bytes\n", (int)groups.size(), (unsigned)resp.length());
}

// Cheap directory scan so menu.html can reveal the Games tile only when /Games has
// content. same pattern as handleArchiveList, reads no file content.
void handleGamesList(AsyncWebServerRequest *request) {
  bool sdLocked = false;
  if (sdMutex) {
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
      request->send(503, "application/json", "{\"error\":\"SD busy\"}");
      return;
    }
    sdLocked = true;
  }

  File root = SD_MMC.open("/Games");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    if (sdLocked) xSemaphoreGive(sdMutex);
    AsyncWebServerResponse *r = request->beginResponse(200, "application/json", "{\"games\":[]}");
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
    return;
  }

  String resp; resp.reserve(24 * 1024);   // ~256 entries at ~90B each, no realloc churn
  resp = "{\"games\":[";
  bool first = true;
  int fileCount = 0;
  File file = root.openNextFile();
  while (file && fileCount < 256) {
    if (!file.isDirectory()) {
      String name = String(file.name());
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      // Skip dotfiles and cover art, only ROM files count toward "has content".
      String lower = name;
      lower.toLowerCase();
      if (!name.startsWith(".") && !lower.endsWith(".png") && !lower.endsWith(".jpg") && !lower.endsWith(".jpeg")) {
        fileCount++;
        if (!first) resp += ",";
        first = false;
        resp += "{\"name\":\"" + escapeJsonString(name) + "\",\"path\":\"/Games/" + escapeJsonString(name) + "\"";
        resp += ",\"size\":" + String((unsigned long long)file.size()) + "}";
      }
    }
    file.close();
    file = root.openNextFile();
    yield();
  }
  root.close();
  if (sdLocked) xSemaphoreGive(sdMutex);

  resp += "]}";
  AsyncWebServerResponse *r = request->beginResponse(200, "application/json", resp);
  r->addHeader("Access-Control-Allow-Origin", "*");
  request->send(r);
  Serial.printf("[GAMES] /api/games-list: %d file(s), %u bytes\n", fileCount, (unsigned)resp.length());
}

// Cheap directory scan for menu.html's Maps tile: region subfolders under /Maps that
// have a manifest.json. The client fetches each region's details itself.
// {"regions":[{"id","manifest"}...]}. *status is 503 if the card stayed busy.
String mapsListJson(int *status) {
  *status = 200;
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    *status = 503;
    return "{\"error\":\"SD busy\"}";
  }
  File root = SD_MMC.open("/Maps");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    if (sdMutex) xSemaphoreGive(sdMutex);
    return "{\"regions\":[]}";
  }
  // Every US state is its own pack, plus the base packs and image maps: up to 160.
  // ~70 bytes a region, a full card grows past the 6 KB reserve.
  String resp; resp.reserve(6 * 1024);
  resp = "{\"regions\":[";
  bool first = true;
  int regionCount = 0;
  File entry = root.openNextFile();
  while (entry && regionCount < 160) {
    if (entry.isDirectory()) {
      String name = String(entry.name());
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (!name.startsWith(".")) {
        String manifestPath = "/Maps/" + name + "/manifest.json";
        if (SD_MMC.exists(manifestPath)) {
          regionCount++;
          if (!first) resp += ",";
          first = false;
          resp += "{\"id\":\"" + escapeJsonString(name) + "\",\"manifest\":\"" + escapeJsonString(manifestPath) + "\"}";
        }
      }
    }
    entry.close();
    entry = root.openNextFile();
    yield();
  }
  root.close();
  if (sdMutex) xSemaphoreGive(sdMutex);
  resp += "]}";
  Serial.printf("[MAPS] /api/maps-list: %d region(s), %u bytes\n", regionCount, (unsigned)resp.length());
  return resp;
}

// The menu's optional tiles: whether each optional page has anything to show, in one
// answer. Each check stops at the first thing it finds, and the answer is kept for 20 s.
// (ints, not an enum, the prototypes Arduino generates go above any type declared here)
#define MENU_ANY 0
#define MENU_ZIM 1
#define MENU_MAP 2
static bool menuDirHas(const char *dir, int want) {
  File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) { if (d) d.close(); return false; }
  bool found = false;
  for (File f = d.openNextFile(); f && !found; f = d.openNextFile()) {
    String n = String(f.name());
    n = n.substring(n.lastIndexOf('/') + 1);
    const bool isDir = f.isDirectory();
    f.close();
    if (n.startsWith(".")) continue;
    String low = n;
    low.toLowerCase();
    if (want == MENU_ANY) found = true;
    else if (want == MENU_ZIM) found = !isDir && archiveTypeForExt(low) == "zim";
    else found = isDir && SD_MMC.exists(String(dir) + "/" + n + "/manifest.json");
  }
  d.close();
  return found;
}

static bool menuTranslatePacks() {
  File f = SD_MMC.open("/Translate/manifest.json", FILE_READ);
  if (!f) return false;
  JsonDocument doc;
  const bool ok = !deserializeJson(doc, f);
  f.close();
  if (!ok) return false;
  JsonArray list = doc["packs"].is<JsonArray>() ? doc["packs"].as<JsonArray>() : doc["pairs"].as<JsonArray>();
  return !list.isNull() && list.size() > 0;
}

static String g_menuJson;
static uint32_t g_menuAt = 0;
static SemaphoreHandle_t g_menuLock = nullptr;

// Work the answer out again. Runs from loop() every 30 s, so a phone asking gets the kept
// one (a fresh look can take over a second, depending on what else the card is doing).
static bool menuFeaturesRefresh(uint32_t waitMs) {
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(waitMs)) != pdTRUE) return false;
  // Games shows whenever this firmware answers: Play Together works with no ROMs
  const String j = String("{\"archive\":") + (menuDirHas("/Archive", MENU_ZIM) ? "true" : "false") +
                   ",\"games\":true" +
                   ",\"maps\":" + (menuDirHas("/Maps", MENU_MAP) ? "true" : "false") +
                   ",\"translate\":" + (menuTranslatePacks() ? "true" : "false") +
                   ",\"cookbook\":" + (menuDirHas("/Cookbook", MENU_ANY) ? "true" : "false") +
                   ",\"workshop\":" + (menuDirHas("/Workshop", MENU_ANY) ? "true" : "false") + "}";
  if (sdMutex) xSemaphoreGive(sdMutex);
  xSemaphoreTake(g_menuLock, portMAX_DELAY);
  g_menuJson = j;
  g_menuAt = millis();
  xSemaphoreGive(g_menuLock);
  return true;
}

void nomadMenuTick() {
  if (g_menuAt && millis() - g_menuAt < 30000) return;
  static uint32_t tried = 0;
  if (millis() - tried < 5000) return;          // card busy: not every loop
  tried = millis();
  menuFeaturesRefresh(200);
}

void handleMenuFeatures(AsyncWebServerRequest *request) {
  if (!g_menuAt && !menuFeaturesRefresh(1500)) {   // only before loop() has had a go
    AsyncWebServerResponse *busy = request->beginResponse(503, "application/json", "{\"error\":\"SD busy\"}");
    busy->addHeader("Retry-After", "1");
    request->send(busy);
    return;
  }
  xSemaphoreTake(g_menuLock, portMAX_DELAY);
  const String cached = g_menuJson;
  xSemaphoreGive(g_menuLock);
  AsyncWebServerResponse *r = request->beginResponse(200, "application/json", cached);
  r->addHeader("Cache-Control", "no-store");
  request->send(r);
}

void handleMapsList(AsyncWebServerRequest *request) {
  int status = 200;
  const String resp = mapsListJson(&status);
  AsyncWebServerResponse *r = request->beginResponse(status, "application/json", resp);
  r->addHeader("Access-Control-Allow-Origin", "*");
  request->send(r);
}

// Multiplayer (HTTP polling) helpers. All assume gameMutex is already held.

static void mpGenerateCode(char *out) {  // out must be MP_CODE_LEN bytes
  static const char kAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";  // no 0/O/1/I
  for (int i = 0; i < MP_CODE_LEN - 1; i++) out[i] = kAlphabet[esp_random() % (sizeof(kAlphabet) - 1)];
  out[MP_CODE_LEN - 1] = '\0';
}

static void mpGenerateToken(char *out) {  // out must be MP_TOKEN_LEN bytes
  static const char kHex[] = "0123456789abcdef";
  for (int i = 0; i < MP_TOKEN_LEN - 1; i++) out[i] = kHex[esp_random() % 16];
  out[MP_TOKEN_LEN - 1] = '\0';
}

// Reclaims any room idle past MP_ROOM_IDLE_MS. Call before allocating/looking up a room.
static void mpReapIdleRooms() {
  unsigned long now = millis();
  for (int i = 0; i < MP_MAX_ROOMS; i++) {
    if (mpRooms[i].active && (now - mpRooms[i].lastMs) > MP_ROOM_IDLE_MS) {
      Serial.printf("[MP] Reclaiming idle room '%s'\n", mpRooms[i].code);
      mpRooms[i].active = false;
    }
  }
}

static int mpFindRoomByCode(const char *code) {
  for (int i = 0; i < MP_MAX_ROOMS; i++) {
    if (mpRooms[i].active && strncmp(mpRooms[i].code, code, MP_CODE_LEN) == 0) return i;
  }
  return -1;
}

// Takes a slot index rather than a MpRoom&. Arduino's auto-generated prototypes go
// at the top of the translation unit, before the MpRoom struct, so a custom-type
// parameter here would fail to compile.
static int mpFindSeatByToken(int slot, const char *token) {
  MpRoom &room = mpRooms[slot];
  for (int seat = 0; seat < 2; seat++) {
    if (room.token[seat][0] != '\0' && strncmp(room.token[seat], token, MP_TOKEN_LEN) == 0) return seat;
  }
  return -1;
}

// POST /api/mp/create {game} -> {code, token, seat, seq}. Creator always takes seat 0.
void handleMpCreate(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, data, len)) { request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return; }
  const char *game = doc["game"] | "";
  if (!game[0] || strlen(game) >= MP_GAME_LEN) {
    request->send(400, "application/json", "{\"error\":\"Invalid game\"}");
    return;
  }

  if (!gameMutex || xSemaphoreTake(gameMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    request->send(503, "application/json", "{\"error\":\"busy\"}");
    return;
  }

  mpReapIdleRooms();
  int slot = -1;
  for (int i = 0; i < MP_MAX_ROOMS; i++) { if (!mpRooms[i].active) { slot = i; break; } }
  if (slot < 0) {
    xSemaphoreGive(gameMutex);
    request->send(503, "application/json", "{\"error\":\"No rooms available\"}");
    return;
  }

  MpRoom &room = mpRooms[slot];
  memset(&room, 0, sizeof(room));
  char code[MP_CODE_LEN];
  int guard = 0;
  do { mpGenerateCode(code); } while (mpFindRoomByCode(code) >= 0 && ++guard < 20);
  memcpy(room.code, code, MP_CODE_LEN);
  strncpy(room.game, game, MP_GAME_LEN - 1);
  mpGenerateToken(room.token[0]);
  room.seq = 0;  // 0 = empty/waiting state, turn = seq % 2, so seat 0 moves first
  room.lastMs = millis();
  room.active = true;

  char codeOut[MP_CODE_LEN], tokenOut[MP_TOKEN_LEN];
  memcpy(codeOut, room.code, MP_CODE_LEN);
  memcpy(tokenOut, room.token[0], MP_TOKEN_LEN);
  xSemaphoreGive(gameMutex);

  String resp = String("{\"code\":\"") + codeOut + "\",\"token\":\"" + tokenOut + "\",\"seat\":0,\"seq\":0}";
  AsyncWebServerResponse *r = request->beginResponse(200, "application/json", resp);
  r->addHeader("Access-Control-Allow-Origin", "*");
  request->send(r);
  Serial.printf("[MP] Created room '%s' game=%s\n", codeOut, game);
}

// POST /api/mp/join {code} -> {token, seat}. Only seat 1 can be joined (seat 0 = creator).
void handleMpJoin(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, data, len)) { request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return; }
  const char *code = doc["code"] | "";
  if (!code[0]) { request->send(400, "application/json", "{\"error\":\"Missing code\"}"); return; }

  if (!gameMutex || xSemaphoreTake(gameMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    request->send(503, "application/json", "{\"error\":\"busy\"}");
    return;
  }

  mpReapIdleRooms();
  int slot = mpFindRoomByCode(code);
  if (slot < 0) {
    xSemaphoreGive(gameMutex);
    request->send(404, "application/json", "{\"error\":\"Room not found\"}");
    return;
  }
  MpRoom &room = mpRooms[slot];
  if (room.token[1][0] != '\0') {
    xSemaphoreGive(gameMutex);
    request->send(409, "application/json", "{\"error\":\"Room full\"}");
    return;
  }
  mpGenerateToken(room.token[1]);
  room.lastMs = millis();
  char tokenOut[MP_TOKEN_LEN];
  memcpy(tokenOut, room.token[1], MP_TOKEN_LEN);
  char gameOut[MP_GAME_LEN];
  memcpy(gameOut, room.game, MP_GAME_LEN);
  xSemaphoreGive(gameMutex);

  // Say which game the room is, so a client can refuse a Chess code typed into the
  // Tic-Tac-Toe box instead of joining and desyncing.
  String resp = String("{\"token\":\"") + tokenOut + "\",\"seat\":1,\"game\":\"" +
                escapeJsonString(String(gameOut)) + "\"}";
  AsyncWebServerResponse *r = request->beginResponse(200, "application/json", resp);
  r->addHeader("Access-Control-Allow-Origin", "*");
  request->send(r);
  Serial.printf("[MP] Seat 1 joined room '%s'\n", code);
}

// POST /api/mp/move {code, token, move, seq} -> {seq}. `move` is an opaque blob the
// client computed, legality is validated client side (chess.js). the server only
// enforces turn ownership (seat == seq%2) and that `seq` matches the room's current
// seq, then relays and bumps seq.
void handleMpMove(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  StaticJsonDocument<896> doc;  // MP_STATE_LEN (512) + code/token/seq overhead
  if (deserializeJson(doc, data, len)) { request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return; }
  const char *code = doc["code"] | "";
  const char *token = doc["token"] | "";
  const char *move = doc["move"] | "";
  uint32_t clientSeq = doc["seq"] | 0xFFFFFFFFu;
  if (!code[0] || !token[0]) { request->send(400, "application/json", "{\"error\":\"Missing code/token\"}"); return; }
  if (strlen(move) >= MP_STATE_LEN) { request->send(400, "application/json", "{\"error\":\"State too large\"}"); return; }

  if (!gameMutex || xSemaphoreTake(gameMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    request->send(503, "application/json", "{\"error\":\"busy\"}");
    return;
  }

  mpReapIdleRooms();
  int slot = mpFindRoomByCode(code);
  if (slot < 0) {
    xSemaphoreGive(gameMutex);
    request->send(404, "application/json", "{\"error\":\"Room not found\"}");
    return;
  }
  MpRoom &room = mpRooms[slot];
  int seat = mpFindSeatByToken(slot, token);
  if (seat < 0) {
    xSemaphoreGive(gameMutex);
    request->send(403, "application/json", "{\"error\":\"Invalid token\"}");
    return;
  }
  bool reset = doc["reset"] | false;
  if (reset) {
    // Play again: either seated player may restart with a fresh state. the turn and
    // stale-seq checks dont apply since a finished game's parity is arbitrary. rematches
    // alternate who moves first, games where rules fix it (chess) pass "first" to pin it.
    room.games++;
    int firstSeat = doc["first"] | -1;
    uint32_t wantParity = (firstSeat == 0 || firstSeat == 1) ? (uint32_t)firstSeat : (room.games % 2);
    uint32_t next = room.seq + 1;
    if (next % 2 != wantParity) next++;
    room.seq = next;
    strncpy(room.state, move, MP_STATE_LEN - 1);
    room.state[MP_STATE_LEN - 1] = '\0';
    room.lastMs = millis();
    uint32_t seqOut = room.seq;
    xSemaphoreGive(gameMutex);
    String resp = String("{\"seq\":") + seqOut + "}";
    AsyncWebServerResponse *r = request->beginResponse(200, "application/json", resp);
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
    return;
  }

  uint32_t turnSeat = room.seq % 2;
  if ((uint32_t)seat != turnSeat) {
    xSemaphoreGive(gameMutex);
    request->send(409, "application/json", "{\"error\":\"Not your turn\"}");
    return;
  }
  if (clientSeq != room.seq) {
    xSemaphoreGive(gameMutex);
    request->send(409, "application/json", "{\"error\":\"Stale seq\"}");
    return;
  }

  strncpy(room.state, move, MP_STATE_LEN - 1);
  room.state[MP_STATE_LEN - 1] = '\0';
  room.seq++;
  room.lastMs = millis();
  uint32_t seqOut = room.seq;
  xSemaphoreGive(gameMutex);

  String resp = String("{\"seq\":") + seqOut + "}";
  AsyncWebServerResponse *r = request->beginResponse(200, "application/json", resp);
  r->addHeader("Access-Control-Allow-Origin", "*");
  request->send(r);
}

// GET /api/mp/state?code=&since= -> {seq, changed, joined, state}. Short-poll target (~1s).
void handleMpState(AsyncWebServerRequest *request) {
  if (!request->hasParam("code")) { request->send(400, "application/json", "{\"error\":\"Missing code\"}"); return; }
  String codeStr = request->getParam("code")->value();
  uint32_t since = 0;
  if (request->hasParam("since")) since = (uint32_t) request->getParam("since")->value().toInt();

  if (!gameMutex || xSemaphoreTake(gameMutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    request->send(503, "application/json", "{\"error\":\"busy\"}");
    return;
  }

  int slot = mpFindRoomByCode(codeStr.c_str());
  if (slot < 0) {
    xSemaphoreGive(gameMutex);
    request->send(404, "application/json", "{\"error\":\"Room not found\"}");
    return;
  }
  MpRoom &room = mpRooms[slot];
  // Polling counts as activity, otherwise a pause longer than MP_ROOM_IDLE_MS between
  // moves gets the room reclaimed mid-game.
  room.lastMs = millis();
  bool changed = room.seq > since;
  bool joined = room.token[1][0] != '\0';
  uint32_t seqOut = room.seq;
  char stateOut[MP_STATE_LEN];
  memcpy(stateOut, room.state, MP_STATE_LEN);
  xSemaphoreGive(gameMutex);

  String resp = String("{\"seq\":") + seqOut + ",\"changed\":" + (changed ? "true" : "false") +
                ",\"joined\":" + (joined ? "true" : "false") +
                ",\"state\":\"" + escapeJsonString(String(stateOut)) + "\"}";
  AsyncWebServerResponse *r = request->beginResponse(200, "application/json", resp);
  r->addHeader("Access-Control-Allow-Origin", "*");
  request->send(r);
}

void handleRename(AsyncWebServerRequest *request) {
    Serial.println("[RENAME] Request received");

    if (!request->hasParam("oldname", true) || !request->hasParam("newname", true)) {
        Serial.println("[RENAME] Missing parameters");
        request->send(400, "application/json", "{\"error\":\"Missing parameters\"}");
        return;
    }

    String oldName = request->getParam("oldname", true)->value();
    String newName = request->getParam("newname", true)->value();
    if ((isConfigPath(oldName) || isConfigPath(newName)) && !checkAdminAuth(request)) {
        request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }

    Serial.printf("[RENAME] Attempting to rename: '%s' -> '%s'\n", oldName.c_str(), newName.c_str());

    switch (releaseStreamsFor(oldName)) {
        case 1:
            Serial.printf("[RENAME] File is currently streaming: %s\n", oldName.c_str());
            request->send(409, "application/json", "{\"error\":\"File is currently in use\"}");
            return;
        case -1:
            request->send(503, "application/json", "{\"error\":\"Busy, try again\"}");
            return;
    }

    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        Serial.printf("[RENAME] Mutex timeout\n");
        request->send(503, "application/json", "{\"error\":\"SD card busy\"}");
        return;
    }

    if (!SD_MMC.exists(oldName)) {
        Serial.printf("[RENAME] Source file not found: %s\n", oldName.c_str());
        if (sdMutex) xSemaphoreGive(sdMutex);
        request->send(404, "application/json", "{\"error\":\"Original file not found\"}");
        return;
    }

    if (SD_MMC.exists(newName)) {
        Serial.printf("[RENAME] Target already exists: %s\n", newName.c_str());
        if (sdMutex) xSemaphoreGive(sdMutex);
        request->send(409, "application/json", "{\"error\":\"Target file already exists\"}");
        return;
    }

    bool wasDirectory = false;
    File sourceFile = SD_MMC.open(oldName);
    if (!sourceFile) {
        Serial.printf("[RENAME] Cannot open source file: %s\n", oldName.c_str());
        if (sdMutex) xSemaphoreGive(sdMutex);
        request->send(500, "application/json", "{\"error\":\"Cannot access source file\"}");
        return;
    }

    if (sourceFile.isDirectory()) {
        wasDirectory = true;
        Serial.printf("[RENAME] Source is directory: %s\n", oldName.c_str());
    }
    sourceFile.close();

    // no settle delays in this handler. SdFat writes synchronously, a delay just
    // stalls async_tcp for every other client.
    bool renameSuccess = SD_MMC.rename(oldName, newName);

    if (!renameSuccess) {
        Serial.printf("[RENAME] SD_MMC.rename() failed: %s -> %s\n", oldName.c_str(), newName.c_str());
        webLogf("error", "Rename failed: %s → %s", oldName.c_str(), newName.c_str());
        if (sdMutex) xSemaphoreGive(sdMutex);
        request->send(500, "application/json", "{\"error\":\"Rename operation failed\"}");
        return;
    }

    if (SD_MMC.exists(oldName)) {
        Serial.printf("[RENAME] ERROR: Source still exists after rename: %s\n", oldName.c_str());
        if (sdMutex) xSemaphoreGive(sdMutex);
        request->send(500, "application/json", "{\"error\":\"Rename verification failed - source still exists\"}");
        return;
    }

    if (!SD_MMC.exists(newName)) {
        Serial.printf("[RENAME] ERROR: Target doesn't exist after rename: %s\n", newName.c_str());
        if (sdMutex) xSemaphoreGive(sdMutex);
        request->send(500, "application/json", "{\"error\":\"Rename verification failed - target missing\"}");
        return;
    }

    Serial.printf("[RENAME] Verification passed: '%s' -> '%s'\n", oldName.c_str(), newName.c_str());

    if (wasDirectory) {
        String oldNestedName = encodeIndexName(oldName) + ".nested.ndjson";
        String oldNestedPath = String(INDEX_DIR) + "/" + oldNestedName;

        webLogf("info", "%s renamed: %s → %s", wasDirectory ? "Directory" : "File", oldName.c_str(), newName.c_str());
        Serial.printf("[RENAME] Directory renamed successfully: '%s' -> '%s'\n", oldName.c_str(), newName.c_str());

        if (SD_MMC.exists(oldNestedPath)) {
            if (SD_MMC.remove(oldNestedPath)) {
                Serial.printf("[RENAME] Removed old nested index: %s\n", oldNestedPath.c_str());
            } else {
                Serial.printf("[RENAME] Warning: Failed to remove old nested index: %s\n", oldNestedPath.c_str());
            }
        }
    } else {
        Serial.printf("[RENAME] File renamed successfully: '%s' -> '%s'\n", oldName.c_str(), newName.c_str());
    }

    if (sdMutex) xSemaphoreGive(sdMutex);

    String parentOld = parentDirFromPath(oldName);
    String parentNew = parentDirFromPath(newName);

    Serial.printf("[RENAME] Triggering index updates: parentOld='%s', parentNew='%s'\n", parentOld.c_str(), parentNew.c_str());

    triggerIndexingIfNeeded(parentOld);
    if (parentNew != parentOld) {
        triggerIndexingIfNeeded(parentNew);
    }

    String bucketOld = "/";
    int firstSlashOld = oldName.indexOf('/', 1);
    if (firstSlashOld > 0) {
        bucketOld = oldName.substring(0, firstSlashOld);
        if (bucketOld != parentOld) {
            Serial.printf("[RENAME] Enqueuing bucket update for old: %s\n", bucketOld.c_str());
            enqueueIndexUpdateForPath(bucketOld);
        }
    }
    
    String bucketNew = "/";
    int firstSlashNew = newName.indexOf('/', 1);
    if (firstSlashNew > 0) {
        bucketNew = newName.substring(0, firstSlashNew);
        if (bucketNew != parentNew && bucketNew != bucketOld) {
            Serial.printf("[RENAME] Enqueuing bucket update for new: %s\n", bucketNew.c_str());
            enqueueIndexUpdateForPath(bucketNew);
        }
    }

    // Final success response
    Serial.printf("[RENAME] Operation completed successfully: '%s' -> '%s'\n", oldName.c_str(), newName.c_str());
    request->send(200, "application/json", "{\"status\":\"Rename successful\"}");
}

void handleDelete(AsyncWebServerRequest *request) {
    if (!request->hasParam("filename", true)) {
    request->send(400, "application/json", "{\"error\":\"Missing 'filename' parameter\"}");
    return;
    }

    String filename = request->getParam("filename", true)->value();
    if (isConfigPath(filename) && !checkAdminAuth(request)) {
        request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
    }

    // for a folder this checks everything inside it too
    switch (releaseStreamsFor(filename)) {
        case 1:
            Serial.printf("[DELETE] File is currently streaming: %s\n", filename.c_str());
            request->send(409, "application/json", "{\"error\":\"File is currently in use\"}");
            return;
        case -1:
            request->send(503, "application/json", "{\"error\":\"Busy, try again\"}");
            return;
    }

    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        Serial.printf("[DELETE] Mutex timeout\n");
        request->send(503, "application/json", "{\"error\":\"SD card busy\"}");
        return;
    }

    if (!SD_MMC.exists(filename)) {
    if (sdMutex) xSemaphoreGive(sdMutex);
    request->send(404, "application/json", "{\"error\":\"File not found\"}");
    return;
    }

    bool success = false;
    bool wasDirectory = false;
    File f = SD_MMC.open(filename);
    if (f && f.isDirectory()) {
        wasDirectory = true;
    }
    if (f) f.close();

    webLogf("info", "Deleting %s: '%s'", wasDirectory ? "folder" : "file", filename.c_str());
    Serial.printf("Deleting %s: %s\n", wasDirectory ? "directory" : "file", filename.c_str());

    if (wasDirectory) {
        success = deleteRecursive(filename);
    } else {
        success = SD_MMC.remove(filename);
    }

    if (sdMutex) xSemaphoreGive(sdMutex);

    if (success) {
        if (wasDirectory) {
            String nestedIndexName = encodeIndexName(filename) + ".nested.ndjson";
            String nestedIndexPath = String(INDEX_DIR) + "/" + nestedIndexName;
            if (SD_MMC.exists(nestedIndexPath)) {
                SD_MMC.remove(nestedIndexPath);
                Serial.printf("[Delete] Removed stale nested index: %s\n", nestedIndexPath.c_str());
            }
        }
        
        // Enqueue index refresh for parent folder AND bucket root
        String parent = parentDirFromPath(filename);
        triggerIndexingIfNeeded(parent);
        
        // Also update bucket root
        String bucketRoot = "/";
        int firstSlash = filename.indexOf('/', 1);
        if (firstSlash > 0) {
            bucketRoot = filename.substring(0, firstSlash);
            if (bucketRoot != parent) {
                enqueueIndexUpdateForPath(bucketRoot);
            }
        }
        
        request->send(200, "application/json", "{\"status\":\"Delete successful\"}");
    } else {
        webLogf("error", "Delete failed: %s", filename.c_str());
        request->send(500, "application/json", "{\"error\":\"Delete failed\"}");
    }
}
// SD usage persistence helpers
// File used to persist the last known usage %
// Save usage atomically
void saveSdUsageToFile(uint64_t totalBytes, uint64_t usedBytes, unsigned long tsMillis = 0) {
  DynamicJsonDocument doc(256);
  doc["total"] = totalBytes;
  doc["used"]  = usedBytes;
  doc["ts"]    = tsMillis ? tsMillis : millis();
  doc["statTrusted"] = g_sdStatTrusted;

  String tmp = String(SD_USAGE_FILE) + ".tmp";
  File f = SD_MMC.open(tmp, FILE_WRITE);
  if (!f) {
    static unsigned long lastFailLog = 0;
    if (millis() - lastFailLog > 10000) {
      Serial.println("[SDBAR] Warning: couldn't open temp sd usage file for write");
      lastFailLog = millis();
    }
    return;
  }
  serializeJson(doc, f);
  f.close();

  if (SD_MMC.exists(SD_USAGE_FILE)) SD_MMC.remove(SD_USAGE_FILE);
  if (!SD_MMC.rename(tmp, SD_USAGE_FILE)) {
    File ft = SD_MMC.open(tmp, FILE_READ);
    File ff = SD_MMC.open(SD_USAGE_FILE, FILE_WRITE);
    if (ft && ff) {
      while (ft.available()) ff.write(ft.read());
      ft.close();
      ff.close();
      SD_MMC.remove(tmp);
    } else {
      if (ft) ft.close();
      if (ff) ff.close();
      static unsigned long lastRenameFail = 0;
      if (millis() - lastRenameFail > 20000) {
        Serial.println("[SDBAR] Warning: rename fallback failed for sd usage file");
        lastRenameFail = millis();
      }
    }
  }
}

bool loadSdUsageFromFile() {
  if (!SD_MMC.exists(SD_USAGE_FILE)) return false;
  File f = SD_MMC.open(SD_USAGE_FILE, FILE_READ);
  if (!f) return false;
  String s = f.readString();
  f.close();

  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, s);
  if (err) {
    Serial.println("[SDBAR] Failed parsing sd usage snapshot");
    return false;
  }
  uint64_t t = (uint64_t)(doc["total"] | 0ULL);
  uint64_t u = (uint64_t)(doc["used"]  | 0ULL);

  if (t == 0) return false;
  cachedTotalBytes = t;
  cachedUsedBytes  = u;
  g_sdStatTrusted  = doc["statTrusted"] | false;
  lastScanTime     = 0;
  Serial.printf("[SDBAR] Loaded saved usage: total=%llu used=%llu (trusted=%s)\n",
                (unsigned long long)cachedTotalBytes, (unsigned long long)cachedUsedBytes,
                g_sdStatTrusted ? "yes" : "no");
  return true;
}

bool refreshCachedTotalsFromStat(uint64_t &outStatUsedBytes, uint32_t mutexTimeoutMs = 400) {
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(mutexTimeoutMs)) != pdTRUE) {
    Serial.println("[SDBAR] refreshCachedTotalsFromStat: SD busy, skipped");
    return false;
  }
  uint64_t total = SD_MMC.totalBytes();

  // usedBytes() is cheap on FAT32, but on exFAT it walks the whole allocation bitmap and
  // holds the SdFat lock for seconds, long enough to trip the async_tcp watchdog.
  // Recompute rarely and reuse the last value in between.
  static uint64_t lastUsedBytes = 0;
  static unsigned long lastUsedMs = 0;
  static bool haveUsedBytes = false;
  const unsigned long USED_BYTES_TTL_MS = 5UL * 60UL * 1000UL;
  uint64_t used;
  bool expensive = SD_MMC.isExFat();
  if (!expensive || !haveUsedBytes || (millis() - lastUsedMs) >= USED_BYTES_TTL_MS) {
    used = SD_MMC.usedBytes();
    lastUsedBytes = used;
    lastUsedMs = millis();
    haveUsedBytes = true;
  } else {
    used = lastUsedBytes;
  }
  if (sdMutex) xSemaphoreGive(sdMutex);

  if (total == 0) {
    Serial.println("[SDBAR] refreshCachedTotalsFromStat: SD_MMC reported 0 total bytes, skipping update");
    return false;
  }

  outStatUsedBytes = used;
  cachedTotalBytes = total;
  if (g_sdStatTrusted) {
    cachedUsedBytes = used;
  }
  sdbarDirty = true;
  return true;
}

// overload for callers that just want to refresh the cache and dont need the raw reading
bool refreshCachedTotalsFromStat(uint32_t mutexTimeoutMs = 400) {
  uint64_t unused;
  return refreshCachedTotalsFromStat(unused, mutexTimeoutMs);
}

// checks a real walk's usage against SD_MMC.usedBytes() and trusts the fast stat path
// only if they agree. this is the only place g_sdStatTrusted is set true.
void reconcileStatTrust(uint64_t walkedUsedBytes) {
  uint64_t statUsed = SD_MMC.usedBytes();
  if (walkedUsedBytes == 0 || statUsed == 0) {
    Serial.println("[SDBAR] reconcileStatTrust: skipped (zero reading)");
    return;
  }

  uint64_t diff = (statUsed > walkedUsedBytes) ? (statUsed - walkedUsedBytes) : (walkedUsedBytes - statUsed);
  double diffPct = (double)diff / (double)walkedUsedBytes * 100.0;
  bool agrees = diffPct <= 2.0; // within 2% of the real walk counts as trustworthy

  Serial.printf("[SDBAR] Stat reconciliation: walked=%llu stat=%llu diff=%.2f%% -> %s\n",
                (unsigned long long)walkedUsedBytes, (unsigned long long)statUsed, diffPct,
                agrees ? "TRUSTED" : "NOT TRUSTED");
  if (!agrees) {
    webLogf("warning", "SD_MMC.usedBytes() disagrees with a real scan by %.1f%% - fast totals will be marked unverified", diffPct);
  }

  g_sdStatTrusted = agrees;
}

// Disk usage

static std::map<String, BreakdownStats> g_lastBreakdown;

const char* SD_BREAKDOWN_FILE = "/.system-index/sd_breakdown.json";
static const size_t SD_BREAKDOWN_MAX_ENTRIES = 48; // bounds JSON doc size and heap use

std::vector<std::pair<String, BreakdownStats>> sortedBreakdownEntries(const std::map<String, BreakdownStats> &breakdown) {
  std::vector<std::pair<String, BreakdownStats>> entries(breakdown.begin(), breakdown.end());
  std::sort(entries.begin(), entries.end(), [](const std::pair<String, BreakdownStats> &a,
                                                const std::pair<String, BreakdownStats> &b) {
    return a.second.bytes > b.second.bytes;
  });
  if (entries.size() > SD_BREAKDOWN_MAX_ENTRIES) {
    entries.resize(SD_BREAKDOWN_MAX_ENTRIES);
  }
  return entries;
}

void saveSdBreakdownToFile(const std::map<String, BreakdownStats> &breakdown) {
  std::vector<std::pair<String, BreakdownStats>> entries = sortedBreakdownEntries(breakdown);

  // Sized off the actual entry count
  size_t docSize = JSON_ARRAY_SIZE(entries.size()) + entries.size() * JSON_OBJECT_SIZE(4) + entries.size() * 48 + 128;
  DynamicJsonDocument doc(docSize);
  doc["ts"] = millis();
  JsonArray arr = doc.createNestedArray("breakdown");
  for (auto &e : entries) {
    JsonObject o = arr.createNestedObject();
    o["dir"] = e.first;
    o["bytes"] = e.second.bytes;
    o["files"] = e.second.files;
    o["dirs"] = e.second.dirs;
  }

  String tmp = String(SD_BREAKDOWN_FILE) + ".tmp";
  File f = SD_MMC.open(tmp, FILE_WRITE);
  if (!f) {
    Serial.println("[SDBreakdown] Warning: couldn't open temp breakdown file for write");
    return;
  }
  serializeJson(doc, f);
  f.close();

  if (SD_MMC.exists(SD_BREAKDOWN_FILE)) SD_MMC.remove(SD_BREAKDOWN_FILE);
  if (!SD_MMC.rename(tmp, SD_BREAKDOWN_FILE)) {
    File ft = SD_MMC.open(tmp, FILE_READ);
    File ff = SD_MMC.open(SD_BREAKDOWN_FILE, FILE_WRITE);
    if (ft && ff) {
      while (ft.available()) ff.write(ft.read());
      ft.close();
      ff.close();
      SD_MMC.remove(tmp);
    } else {
      if (ft) ft.close();
      if (ff) ff.close();
      Serial.println("[SDBreakdown] Warning: rename fallback failed for breakdown file");
    }
  }
}

bool loadSdBreakdownFromFile() {
  if (!SD_MMC.exists(SD_BREAKDOWN_FILE)) return false;
  File f = SD_MMC.open(SD_BREAKDOWN_FILE, FILE_READ);
  if (!f) return false;
  String s = f.readString();
  f.close();

  DynamicJsonDocument doc(s.length() + 512);
  DeserializationError err = deserializeJson(doc, s);
  if (err) {
    Serial.println("[SDBreakdown] Failed parsing breakdown snapshot");
    return false;
  }

  g_lastBreakdown.clear();
  JsonArray arr = doc["breakdown"].as<JsonArray>();
  for (JsonObject o : arr) {
    String dir = o["dir"] | "";
    if (dir.length() == 0) continue;
    BreakdownStats stats;
    stats.bytes = (uint64_t)(o["bytes"] | 0ULL);
    stats.files = (uint32_t)(o["files"] | 0UL);
    stats.dirs  = (uint32_t)(o["dirs"]  | 0UL);
    g_lastBreakdown[dir] = stats;
  }
  Serial.printf("[SDBreakdown] Loaded %u cached breakdown entries\n", (unsigned)g_lastBreakdown.size());
  return true;
}

// Full walk of the card into cachedUsedBytes and g_lastBreakdown. Takes sdMutex itself
// and hands it back every ~50 ms, so streams and page loads keep working during the
// 10-20 minutes a big card takes. Totals are only saved at the end, so an abandoned walk
// leaves the previous numbers in place.
// Returns false if the walk couldn't start or lost the card part way.
bool scanSDCardUsage() {
  const unsigned long minScanIntervalMs = 15UL * 60UL * 1000UL;
  if (lastScanTime != 0 && (millis() - lastScanTime) < minScanIntervalMs) {
    Serial.printf("[SDBAR] scan skipped: last scan %lu ms ago (min %lu ms)\n", millis() - lastScanTime, minScanIntervalMs);
    return false;
  }
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
    Serial.println("[SDScan] Failed to acquire mutex for SD scan");
    webLog("[SD Scan] Failed to acquire SD lock - will retry later", "warning");
    return false;
  }

  uint64_t reportedTotal = SD_MMC.cardSize();
  if (reportedTotal == 0) {
    reportedTotal = cachedTotalBytes > 0 ? cachedTotalBytes : (64ULL * 1024ULL * 1024ULL * 1024ULL);
  }
  cachedTotalBytes = reportedTotal;

  uint32_t startMs = millis();
  uint64_t walked = 0;
  uint64_t lastSavedBytes = 0;
  uint64_t lastAnnounced = 0;
  const uint32_t tickBudgetMs = 50;
  uint32_t lastYield = millis();

  std::map<String, BreakdownStats> breakdown;   // committed to g_lastBreakdown at the end
  auto topLevelBucketFor = [](const String &path) -> String {
    int slash = path.indexOf('/', 1);
    return (slash < 0) ? String("/") : path.substring(0, slash);
  };

  std::vector<String> dirStack;
  dirStack.push_back("/");

  while (!dirStack.empty()) {
    String currentPath = dirStack.back();
    dirStack.pop_back();

    File dir = SD_MMC.open(currentPath);
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      continue;
    }

    while (true) {
      File entry = dir.openNextFile();
      if (!entry) break;

      String entryPath = String(entry.path());

      if (entry.isDirectory()) {
        dirStack.push_back(entryPath);
        if (entryPath != "/") breakdown[topLevelBucketFor(entryPath)].dirs++;
        entry.close();
      } else {
        uint64_t fileSize = entry.size();
        entry.close();
        // saturated size means the file is past 4 GB, get the real one so the
        // usage total is not short by 4 GB for every such file
        if (fileSize == (uint64_t)SIZE_MAX) fileSize = SD_MMC.fileSize64(entryPath.c_str());

        walked += fileSize;
        BreakdownStats &bucket = breakdown[topLevelBucketFor(entryPath)];
        bucket.bytes += fileSize;
        bucket.files++;

        if ((walked - lastAnnounced) > (2ULL * 1024ULL * 1024ULL * 1024ULL)) {
          lastAnnounced = walked;
          float usedGB = (float)walked / (1024.0 * 1024.0 * 1024.0);
          float totalGB = (float)cachedTotalBytes / (1024.0 * 1024.0 * 1024.0);
          webLogf("info", "SD scan progress: %.1f GB / %.1f GB scanned", usedGB, totalGB);

          // Same checkpoint (every 2GB) throttles the screen progress, so it can't
          // flood the small lvglQueue.
          int pct = (totalGB > 0.0f) ? (int)((usedGB / totalGB) * 100.0f) : 0;
          char screenBuf[80];
          snprintf(screenBuf, sizeof(screenBuf), "Scanning SD card...\n%.1f / %.1f GB (%d%%)\n\nDo not unplug!",
                    usedGB, totalGB, pct);
          lvglSendMsg(screenBuf, true);
        }

        if ((walked - lastSavedBytes) > (32ULL * 1024ULL * 1024ULL)) {
          saveSdUsageToFile(cachedTotalBytes, walked, millis());
          lastSavedBytes = walked;
        }
      }

      if ((millis() - lastYield) > tickBudgetMs) {
        lastYield = millis();
        // hand the card to whoever is waiting (a stream fill, a page load) before
        // carrying on. The open directory handle survives the gap, SdFat keeps its
        // own position.
        if (sdMutex) {
          xSemaphoreGive(sdMutex);
          vTaskDelay(pdMS_TO_TICKS(10));
          if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
            dir.close();
            Serial.println("[SDBAR] scan abandoned: card stayed busy for 10 s, keeping previous totals");
            webLog("[SD Scan] Stopped early - the card stayed busy. Previous totals kept.", "warning");
            return false;
          }
        } else {
          vTaskDelay(pdMS_TO_TICKS(1));
        }
      }
    }
    dir.close();
  }

  cachedUsedBytes = walked;
  g_lastBreakdown.swap(breakdown);
  reconcileStatTrust(cachedUsedBytes);

  saveSdUsageToFile(cachedTotalBytes, cachedUsedBytes, millis());
  saveSdBreakdownToFile(g_lastBreakdown);

  lastScanTime = millis();
  sdbarDirty = true;
  if (sdMutex) xSemaphoreGive(sdMutex);

  Serial.printf("[SDBAR] SD card scan complete: used=%llu bytes, total=%llu bytes, duration=%u ms\n",
    (unsigned long long)cachedUsedBytes, (unsigned long long)cachedTotalBytes, (unsigned) (millis() - startMs));

  float usedGB = (float)cachedUsedBytes / (1024.0 * 1024.0 * 1024.0);
  float totalGB = (float)cachedTotalBytes / (1024.0 * 1024.0 * 1024.0);
  float percent = (totalGB > 0.0f) ? (usedGB / totalGB) * 100.0f : 0.0f;

  webLogf("success", "SD card scan complete: %.1f GB used of %.1f GB total (%.1f%% full)",
    usedGB, totalGB, percent);
  return true;
}
void handleSDInfo(AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(256);

    uint64_t totalBytes = cachedTotalBytes > 0 ? cachedTotalBytes : (64ULL * 1024 * 1024 * 1024);
    uint64_t usedBytes = cachedUsedBytes;

    doc["total"] = totalBytes;
    doc["used"] = usedBytes;
    doc["statTrusted"] = g_sdStatTrusted;

    String output;
    serializeJson(doc, output);

    request->send(200, "application/json", output);
}

extern uint64_t cachedTotalBytes; // set by scanSDCardUsage()
extern uint64_t cachedUsedBytes;  // set by scanSDCardUsage()

static inline int calcUsagePct(uint64_t used, uint64_t total) {
  if (total == 0) return 0;
  uint64_t pct = (used * 100ULL) / total;
  return (int)(pct > 100 ? 100 : pct);
}

void handleConnector(AsyncWebServerRequest *request) {
  String dir = "/";
  if (request->hasParam("dir", true)) {
    dir = request->getParam("dir", true)->value();
  }
  if (!dir.endsWith("/")) dir += "/";

  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
    request->send(503, "text/plain", "SD card busy");
    return;
  }

  File root = SD_MMC.open(dir);
  if (!root || !root.isDirectory()) {
    if (sdMutex) xSemaphoreGive(sdMutex);
    request->send(400, "text/plain", "Invalid directory");
    return;
  }

  String html = "<ul class=\"jqueryFileTree\" style=\"display: none;\">";
  html.reserve(2048);
  root.rewindDirectory();
  File entry;
  while ((entry = root.openNextFile())) {
    String name = entry.name();
    // the file browser parses this with innerHTML, so names are escaped like any
    // other text going into a page
    const String rel = nomadHtmlEscape(dir + name);
    const String label = nomadHtmlEscape(name);
    if (entry.isDirectory()) {
      html += "<li class=\"directory collapsed\">"
           "<a href=\"#\" rel=\"" + rel + "/\">" + label + "</a>"
           "</li>";
    } else {
      int dot = name.lastIndexOf('.');
      String ext = dot > 0 ? nomadHtmlEscape(name.substring(dot+1)) : "";
      html += "<li class=\"file ext_" + ext + "\">"
           "<a href=\"#\" rel=\"" + rel + "\">" + label + "</a>"
           "</li>";
    }
    entry.close();
    yield();
  }
  html += "</ul>";
  root.close();

  // release on this path too. leaving it held wedges every later SD request
  // with "SD busy" until reboot
  if (sdMutex) xSemaphoreGive(sdMutex);

  request->send(200, "text/html", html);
}
void handleMkdir(AsyncWebServerRequest *request) {
    if (!request->hasParam("dir", true)) {
        request->send(400, "application/json", "{\"error\":\"Missing 'dir' parameter\"}");
        return;
    }

    String dirPath = request->getParam("dir", true)->value();

    if (dirPath == "/" || dirPath == "") {
        request->send(400, "application/json", "{\"error\":\"Invalid directory path\"}");
        return;
    }

    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        request->send(503, "application/json", "{\"error\":\"SD busy\"}");
        return;
    }
    bool ok = SD_MMC.mkdir(dirPath);
    if (sdMutex) xSemaphoreGive(sdMutex);

    if (ok) {
        request->send(200, "application/json", "{\"success\":\"Directory created\"}");
    } else {
        request->send(500, "application/json", "{\"error\":\"Failed to create directory\"}");
    }
}
void applyRGBSettings() {
  if (settings.rgbMode == "off") {
    RGB_SetMode(0);  // Off
  } else if (settings.rgbMode == "solid") {
    if (settings.rgbColor.length() == 7 && settings.rgbColor.charAt(0) == '#') {
      // Parse "#RRGGBB"
      char rs[3] = { settings.rgbColor.charAt(1), settings.rgbColor.charAt(2), 0 };
      char gs[3] = { settings.rgbColor.charAt(3), settings.rgbColor.charAt(4), 0 };
      char bs[3] = { settings.rgbColor.charAt(5), settings.rgbColor.charAt(6), 0 };

      solidR = strtol(rs, NULL, 16);
      solidG = strtol(gs, NULL, 16);
      solidB = strtol(bs, NULL, 16);

      RGB_SetMode(2);  // Solid color mode
    } else {
      Serial.println("[RGB] Invalid color format in settings.rgbColor");
    }
  } else if (settings.rgbMode == "rainbow") {
    RGB_SetMode(1);  // Rainbow mode
  }
}
// mDNS responder: makes http://nomad.local/ work alongside 192.168.4.1. .local only
// resolves via multicast, so our captive DNS doesn't cover it. Must be restarted
// whenever the AP interface is torn down.
const char* MDNS_HOSTNAME = "nomad";
// nomad.local only works where multicast does (Android has no system mDNS resolver).
// nomad.lan is a normal name that our DNS responder on :53 answers. Advertise both.
const char* UNICAST_HOSTNAME = "nomad.lan";

// MDNS.end() has no "is it running" check, so calling it when the responder never
// started, or after its netif is gone, panics. Track state ourselves and always stop
// before the interface goes away.
static bool g_mdnsRunning = false;
static bool g_dnsOffered = false;
static bool g_captivePortalUri = false;

void stopNomadMDNS() {
  if (!g_mdnsRunning) return;
  MDNS.end();
  g_mdnsRunning = false;
}

void startNomadMDNS() {
  stopNomadMDNS();
  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    g_mdnsRunning = true;
    Serial.printf("[mDNS] Responder started: http://%s.local/\n", MDNS_HOSTNAME);
  } else {
    Serial.println("[mDNS] Failed to start responder (nomad.local unavailable; IP access unaffected)");
  }
}

// dhcps usually starts inside WiFi.softAP(), but after an AP restart it can silently
// stay down. clients then associate but get no lease and self-assign 169.254.x.x.
// Restart it if it isnt running, heap is logged in case low internal DRAM is what
// stopped it claiming its pool. (issue #126)
void ensureApDhcpServer(const char *phase) {
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap) {
    Serial.printf("[DHCP] (%s) AP netif handle unavailable\n", phase);
    return;
  }
  esp_netif_dhcp_status_t st = ESP_NETIF_DHCP_INIT;
  if (esp_netif_dhcps_get_status(ap, &st) != ESP_OK) {
    Serial.printf("[DHCP] (%s) could not query dhcps status\n", phase);
    return;
  }
  Serial.printf("[DHCP] (%s) status=%s  internalHeap=%u largestBlock=%u apIP=%s\n",
                phase,
                st == ESP_NETIF_DHCP_STARTED ? "STARTED" : "NOT-RUNNING",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                WiFi.softAPIP().toString().c_str());
  if (st != ESP_NETIF_DHCP_STARTED) {
    // A bare dhcps_start on a netif left half-initialised by an AP restart can report
    // STARTED yet never serve leases. Do a clean rebuild: stop, re-assert the AP IP, start.
    esp_netif_dhcps_stop(ap);
    esp_netif_ip_info_t ipInfo;
    esp_netif_str_to_ip4("192.168.4.1",   &ipInfo.ip);
    esp_netif_str_to_ip4("192.168.4.1",   &ipInfo.gw);
    esp_netif_str_to_ip4("255.255.255.0", &ipInfo.netmask);
    esp_netif_set_ip_info(ap, &ipInfo);
    esp_err_t e = esp_netif_dhcps_start(ap);
    st = ESP_NETIF_DHCP_INIT;
    esp_netif_dhcps_get_status(ap, &st);
    Serial.printf("[DHCP] (%s) dhcps down -> rebuild %s, now %s\n",
                  phase, (e == ESP_OK ? "ok" : "err"),
                  st == ESP_NETIF_DHCP_STARTED ? "STARTED" : "STILL-DOWN");
  }
}

// By default the softAP's DHCP hands out an address, netmask and router but no DNS
// server. dhcps only sends option 6 when OFFER_DNS is set, and the Arduino core only
// sets it inside WiFi.softAPConfig(). Without it nomad.lan never resolves, our :53
// responder never gets a query and the sign-in popup never appears.
// Call after the AP is up. dhcps has to be stopped to take an option, which is fine at
// boot since no client has connected yet.
void apAdvertiseDns() {
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap) {
    Serial.println("[DNS] AP netif unavailable - cannot advertise a DNS server");
    return;
  }

  esp_netif_dns_info_t dns = {};
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4.addr = (uint32_t)WiFi.softAPIP();
  esp_err_t eSet = esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);

  esp_netif_dhcps_stop(ap);
  // esp_netif reads this as a boolean and sets OFFER_DNS itself, and the GET reports a
  // plain 1 back. So 0x02 in and 0x01 out both mean "enabled".
  uint8_t offer = 0x02;  // OFFER_DNS from dhcpserver.h, inlined to avoid an lwIP include
  esp_err_t eOpt = esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET,
                                          ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
  esp_netif_dhcps_start(ap);

  // Read back after the restart. A GET while dhcps is stopped fails without writing
  // the buffer, which looks like the option didn't stick.
  uint8_t back = 0;
  esp_err_t eGet = esp_netif_dhcps_option(ap, ESP_NETIF_OP_GET,
                                          ESP_NETIF_DOMAIN_NAME_SERVER, &back, sizeof(back));

  g_dnsOffered = (eOpt == ESP_OK) && (eGet == ESP_OK) && (back != 0);
  Serial.printf("[DNS] DHCP option 6 (DNS server) -> %s : set=%s opt=%s readback=%s/0x%02X\n",
                WiFi.softAPIP().toString().c_str(),
                eSet == ESP_OK ? "ok" : esp_err_to_name(eSet),
                eOpt == ESP_OK ? "ok" : esp_err_to_name(eOpt),
                eGet == ESP_OK ? "ok" : esp_err_to_name(eGet), back);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
  // RFC 8910: hand the portal URL over DHCP as well (option 114). iOS 14+, Android 11+
  // and Windows use it to open the sign-in sheet straight at us.
  g_captivePortalUri = WiFi.AP.enableDhcpCaptivePortal();
  Serial.printf("[DNS] DHCP option 114 (captive portal URI) %s\n",
                g_captivePortalUri ? "advertised" : "not set");
#endif
}

// WiFi Mode (join an existing network).
// Try to join settings.staSSID. Runs in setup() before any background task, so
// pumping LVGL directly here is safe. Returns false on timeout/auth failure and
// leaves the radio OFF so the caller can bring up the hotspot cleanly.
bool startStationMode() {
  String msg = "Joining WiFi:\n" + settings.staSSID + "\n\nHold side button\nfor hotspot mode";
  lv_textarea_set_text(ui_MediaGen, msg.c_str());
  lv_timer_handler();
  webLogf("info", "WiFi Mode: joining network '%s'%s", settings.staSSID.c_str(),
          settings.staPersist ? " (auto-reconnect)" : " (until power cycle)");

  WiFi.persistent(false);      // our copy lives in settings.json, keep the core out of NVS
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);        // modem sleep adds latency to every request
  WiFi.setAutoReconnect(true); // ride out router reboots without user action
  WiFi.begin(settings.staSSID.c_str(), settings.staPassword.length() ? settings.staPassword.c_str() : NULL);

  const unsigned long JOIN_TIMEOUT_MS = 20000;
  unsigned long start = millis();
  bool cancelled = false;
  while (WiFi.status() != WL_CONNECTED && millis() - start < JOIN_TIMEOUT_MS) {
    // escape hatch: side button during the wait forces hotspot mode and turns
    // auto-reconnect off. safe to poll here, the USB-mode interrupt on this
    // pin isnt attached until the end of setup()
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
      cancelled = true;
      break;
    }
    delay(100);
    lv_timer_handler();  // keep the boot screen alive during the wait
  }

  if (cancelled) {
    g_staFailReason = "cancelled by button";
    settings.staPersist = false;
    clear_sta_once_flag();
    saveSettings();
    webLog("[WiFiMode] Join cancelled by button - auto-reconnect disabled, starting hotspot", "warning");
  } else if (WiFi.status() == WL_CONNECTED) {
    g_staFailReason = "";
    webLogf("success", "WiFi Mode: joined '%s' - IP %s/%d", settings.staSSID.c_str(),
            WiFi.localIP().toString().c_str(), maskToCidr(WiFi.subnetMask()));
    return true;
  } else {
    switch (WiFi.status()) {
      case WL_NO_SSID_AVAIL:  g_staFailReason = "network not found"; break;
      case WL_CONNECT_FAILED: g_staFailReason = "join rejected (wrong password?)"; break;
      default:                g_staFailReason = "no connection within 20s"; break;
    }
    webLogf("error", "WiFi Mode: could not join '%s' (%s) - falling back to hotspot",
            settings.staSSID.c_str(), g_staFailReason.c_str());
    String failMsg = "WiFi join failed:\n" + g_staFailReason + "\n\nStarting hotspot...";
    lv_textarea_set_text(ui_MediaGen, failMsg.c_str());
    lv_timer_handler();
    delay(1500);  // long enough to read why before the boot screen moves on
  }

  // full teardown so the hotspot path starts from the same state as a normal boot
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  return false;
}

// Single network bring-up decision, called once from setup() after loadSettings().
// Ends with either the STA link or the hotspot up, plus mDNS + DLNA bound to
// whichever interface won.
void startNetwork() {
  bool staOnce = get_sta_once_flag();
  esp_reset_reason_t rr = esp_reset_reason();

  // unplug/replug (or brownout) cancels a one-shot join: power cycle -> hotspot
  // is back. soft restarts and crashes keep it, so a mid-session panic doesnt
  // pull the server off the home network.
  if (staOnce && (rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT)) {
    clear_sta_once_flag();
    staOnce = false;
    Serial.println("[WiFiMode] Power-on reset cleared one-shot join request -> hotspot mode");
  }

  bool wantSta = settings.staSSID.length() > 0 && (settings.staPersist || staOnce);

  if (wantSta && startStationMode()) {
    g_staMode = true;
    // no captive DNS in station mode: the router owns DNS on this network
    startNomadMDNS();
  } else {
    if (wantSta && !settings.staPersist) {
      // a failed one-shot must not retry on the next soft reboot
      clear_sta_once_flag();
    }
    g_staMode = false;
    webLogf("info", "Starting WiFi Access Point with SSID: '%s'", settings.wifiSSID.c_str());
    WiFi.mode(WIFI_AP);
    WiFi.softAP(settings.wifiSSID.c_str(), settings.wifiPassword.c_str(), 1, 0, MAX_CLIENTS);
    {
      // Read back what the driver actually accepted rather than trusting the request.
      wifi_config_t apConf;
      if (esp_wifi_get_config(WIFI_IF_AP, &apConf) == ESP_OK) {
        Serial.printf("[AP] max_connection requested=%d accepted=%d\n",
                      MAX_CLIENTS, (int)apConf.ap.max_connection);
        webLogf("info", "Hotspot allows %d devices", (int)apConf.ap.max_connection);
      }
    }
    ensureApDhcpServer("boot-initial");  // make sure clients get a lease, not APIPA (issue #126)
#if NOMAD_AP_HT20
    // The softAP defaults to 40 MHz, which on channel 1 also covers 5-6 where most
    // neighbours sit, and phones mostly refuse 40 MHz on 2.4 GHz anyway.
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
#endif
#if NOMAD_BENCH
    benchApplyBootRadio();               // a radio state stored by /bench/radio?persist=1
#endif
    webLogf("success", "WiFi Access Point started successfully - IP: %s", WiFi.softAPIP().toString().c_str());
    apAdvertiseDns();                    // without this no client ever asks us to resolve anything
    nomadDnsSetCatchAll(settings.dnsCatchAll);
    nomadDnsStart(WiFi.softAPIP());
    startNomadMDNS();
  }
  webLogf("info", "Device also reachable at http://%s/ and, where multicast works, http://%s.local/",
          UNICAST_HOSTNAME, MDNS_HOSTNAME);

  // DLNA discovery. The HTTP routes are registered with the rest of the server.
  // This binds the SSDP multicast listener on whichever netif just came up.
  dlnaSetEnabled(settings.dlnaEnabled && !g_safeMode);   // safe mode: TV support waits for a normal start
  dlnaRestart();
  webLogf("info", "DLNA TV support %s", settings.dlnaEnabled && !g_safeMode ? "enabled" : "disabled");

  // hand the boot screen back to the rest of setup()
  lv_textarea_set_text(ui_MediaGen, "Booting...");
  lv_timer_handler();
}
// Return number of connected stations on the softAP
int getConnectedUserCount() {
  // WiFi.softAPgetStationNum() returns uint8_t number of clients
  return WiFi.softAPgetStationNum();
}
// full walk of the whole card (also buckets into g_lastBreakdown). per-folder scope
// lives on the indexing side, see /api/reindex
void sdScanTask(void* pvParameters) {
  webLog("[SD Scan] Starting full SD card scan - this may take 10-20 minutes on large cards", "warning");
  lvglSendMsg("Scanning SD card...\nStarting up...\n\nDo not unplug!", true);

  sdScanInProgress = true;
  sdScanCompleted = false;

  // takes and periodically yields sdMutex itself, so playback and page loads keep
  // working while it walks, pushes its own throttled screen progress
  scanSDCardUsage();

  sdScanInProgress = false;
  sdScanCompleted = true;
  lvglSendMsg("", false);
  Serial.println("[SDScan] SD scan complete; sdScanCompleted = true");
  // the real stack headroom, to check the stack size on the device
  Serial.printf("[SDScan] Task stack high-water mark: %u bytes free (minimum seen)\n",
                (unsigned)uxTaskGetStackHighWaterMark(NULL));

  nomadTaskExit();
}

// How busy the server is, turned into small work budgets and pauses.
static inline int getServerLoadScore() {
  // 0 = idle … higher = busier
  // Count stations + uploads, cheap approximation but good enough to throttle.
  extern std::map<AsyncWebServerRequest*, File> activeUploads;
  int users   = getConnectedUserCount();
  int uploads = (int)activeUploads.size();
  return users * 3 + uploads * 5;
}

// Returns a pair: { entriesToProcessBeforePause, pauseMsAfterBatch }
static inline std::pair<int,int> chooseWorkBudget() {
  if (mediaStreamingActive) {
    return { 10, 100 }; // Process fewer items with longer pauses
  }

  int score = getServerLoadScore();
  if (score <= 0) return { 300, 1 };     // idle: move fast but yield
  if (score <= 3) return { 150, 2 };     // light load
  if (score <= 6) return { 80,  5 };     // a bit busy
  if (score <= 10) return { 40, 10 };    // busy
  return { 20, 20 };                     // very busy: crawl
}

// Trigger indexing for known media directories when it's safe to do so.
void triggerIndexingIfNeeded(const String& filePath) {
  // Only trigger indexing for media files in known directories
  if (filePath.startsWith("/Shows/") || filePath.startsWith("/Music/") ||
      filePath.startsWith("/Movies/") || filePath.startsWith("/Books/") ||
      filePath.startsWith("/Gallery/") || filePath.startsWith("/Files/") ||
      filePath.startsWith("/Games/") || filePath.startsWith("/Cookbook/") ||
      filePath.startsWith("/Workshop/")) {

    // Extract the parent directory for indexing
    String parentDir = parentDirFromPath(filePath);

    // Enqueue even while streaming, or a delete or rename during playback never gets
    // reindexed. The worker drains the queue (max 16) during playback.
    enqueueIndexUpdateForPath(parentDir);
  }
}

void indexWorkerTask(void *param) {
  const char *buckets[] = { "/Shows", "/Music", "/Movies", "/Books", "/Gallery", "/Files", "/Games", "/Cookbook", "/Workshop", "/", NULL };

  bool queuedIndexingMsgActive = false;
  unsigned long lastQueuedLvglMsg = 0;

  while (!settingsReady) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  for (;;) {
    if (shutdownBackgroundTasks && !indexingInProgress) {
      Serial.println("[IndexWorker] Shutting down to prioritize media streaming");
      indexWorkerTaskHandle = nullptr;
      indexingTasksActive = false;
      nomadTaskExit();
      return;
    }

    // A live stream only defers the start of a full scan. No `continue`, the queue drain
    // below still has to run or upload/rename/delete reindexes pile up past 16 and get
    // dropped. requestIndexing is cleared inside the branch, so the scan runs once the
    // stream ends.
    if (requestIndexing && !indexingInProgress && !mediaStreamingActive) {
      Serial.printf("[IndexWorker] Pre-index heap check: free=%u\n", ESP.getFreeHeap());

      vTaskDelay(pdMS_TO_TICKS(200));

      if (!enoughHeapForIndex(20000)) {
        Serial.println("[IndexWorker] Insufficient heap for indexing, clearing caches and retrying");

        lastIndexRequestMs.clear();

        vTaskDelay(pdMS_TO_TICKS(3000));

        if (!enoughHeapForIndex(20000)) {
          if (ESP.getFreeHeap() < 27000) {
            Serial.printf("[IndexWorker] Critically low heap (free=%u), cannot safely index. Deferring.\n", ESP.getFreeHeap());
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
          }
          Serial.printf("[IndexWorker] Low heap mode: proceeding cautiously (free=%u)\n", ESP.getFreeHeap());
        }
      }
      
      Serial.printf("[IndexWorker] Starting index with heap: free=%u\n", ESP.getFreeHeap());
      webLog("[IndexWorker] Index request detected - starting full media scan", "info");
      indexingInProgress = true;
      indexingTasksActive = true;
      requestIndexing = false;

      // Count total buckets for progress display
      int totalBuckets = 0;
      for (int i = 0; buckets[i]; ++i) {
        totalBuckets++;
      }

      for (int i = 0; buckets[i]; ++i) {
        if (shutdownBackgroundTasks) {
          indexingInProgress = false;
          indexingTasksActive = false;
          if (indexingPathMutex && xSemaphoreTake(indexingPathMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            currentIndexingPath = "";
            g_currentBucketNum = 0;
            g_totalBucketsForProgress = 0;
            g_indexProgressPercent = 0;
            xSemaphoreGive(indexingPathMutex);
          }

          lvglSendMsg("", false);

          webLog("[IndexWorker] Indexing interrupted for media streaming priority", "info");
          break;
        }

        const String bucket = String(buckets[i]);

        // Calculate progress percentage
        int currentBucketNum = i + 1;
        int progressPercent = (currentBucketNum * 100) / totalBuckets;

        if (indexingPathMutex && xSemaphoreTake(indexingPathMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          currentIndexingPath = bucket;
          g_currentBucketNum = currentBucketNum;
          g_totalBucketsForProgress = totalBuckets;
          g_indexProgressPercent = progressPercent;
          xSemaphoreGive(indexingPathMutex);
        }

        // Show current bucket on screen with progress via thread-safe queue
        char screenBuf[80];
        snprintf(screenBuf, sizeof(screenBuf), "Indexing: %s\n(%d/%d) %d%%\n\nDo not unplug!", bucket.c_str(), currentBucketNum, totalBuckets, progressPercent);
        lvglSendMsg(screenBuf, true);
        vTaskDelay(pdMS_TO_TICKS(300));

        auto [batch, pauseMs] = chooseWorkBudget();

        Serial.printf("[IndexWorker] Processing bucket=%s (batch=%d pause=%dms, heap=%u)\n",
                      bucket.c_str(), batch, pauseMs, ESP.getFreeHeap());
        webLogf("indexing_progress", "Indexing: %s (%d/%d) %d%%", bucket.c_str(), currentBucketNum, totalBuckets, progressPercent);

        buildBucketIndex(bucket);

        // Extended yield between buckets to prevent watchdog and allow background tasks
        Serial.printf("[IndexWorker] Bucket complete, yielding (heap=%u)\n", ESP.getFreeHeap());
        vTaskDelay(pdMS_TO_TICKS(500));

        // Process queued items after each bucket
        IndexBuildArgs *msg = nullptr;
        int processedCount = 0;
        // test processedCount before the dequeue, or the last pass takes an item and
        // leaks it
        while (indexQueue && processedCount < 5 && xQueueReceive(indexQueue, &msg, 0) == pdTRUE && msg) {
          if (shutdownBackgroundTasks) {
            delete msg;
            break;
          }

          Serial.printf("[IndexWorker] Processing queued item (heap=%u)\n", ESP.getFreeHeap());
          writeNDIndexForDir(msg->dir, msg->out);
          delete msg;
          processedCount++;

          vTaskDelay(pdMS_TO_TICKS(pauseMs));
        }
      }

      if (!shutdownBackgroundTasks) {
        File f = SD_MMC.open("/boot_done.flag", FILE_WRITE);
        if (f) { f.print("1"); f.close(); }

        // Show completion and trigger a storage scan (covers both first-time and
        // regular full-index runs, there's no case where a reboot is needed here).
        lvglSendMsg("Filesystem update complete!\nUpdating storage info...", true);
        Serial.println("[IndexWorker] Index complete, triggering storage scan");

        // Extended delay before spawning storage scan to stabilize system
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (refreshCachedTotalsFromStat(2000)) {
          sdScanCompleted = true;
          saveSdUsageToFile(cachedTotalBytes, cachedUsedBytes);
          Serial.println("[IndexWorker] Storage totals refreshed after index");
        }

        webLog("[IndexWorker] Full media scan completed successfully", "success");

        // Hide indexing screen after completion message shown briefly
        vTaskDelay(pdMS_TO_TICKS(2000));
        lvglSendMsg("", false);
      }
      indexingInProgress = false;
      indexingTasksActive = false;
      if (indexingPathMutex && xSemaphoreTake(indexingPathMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        currentIndexingPath = "";
        g_currentBucketNum = 0;
        g_totalBucketsForProgress = 0;
        g_indexProgressPercent = 0;
        xSemaphoreGive(indexingPathMutex);
      }

      lastIndexRequestMs.clear();

      continue;
    }

    // Process queued index requests (live updates)
    IndexBuildArgs *msg = nullptr;
    if (indexQueue && xQueueReceive(indexQueue, &msg, pdMS_TO_TICKS(100)) == pdTRUE && msg) {
      if (shutdownBackgroundTasks) {
        delete msg;
        Serial.println("[IndexWorker] Shutting down, discarding queued index request");
        indexWorkerTaskHandle = nullptr;
        indexingTasksActive = false;
        nomadTaskExit();
        return;
      }

      // Check heap before processing 
      if (!enoughHeapForIndex(15000)) {  // 30KB total with 15K min heap
        Serial.printf("[IndexWorker] Low heap (%u bytes), re-queuing %s\n", 
                     ESP.getFreeHeap(), msg->dir.c_str());
        
        vTaskDelay(pdMS_TO_TICKS(1000));
        
        // Try to re-queue for later
        if (xQueueSend(indexQueue, &msg, 0) != pdTRUE) {
          // Queue full, drop the request
          delete msg;
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
        continue;
      }

      indexingTasksActive = true;
      Serial.printf("[IndexWorker] Processing queued index request for '%s' -> %s (heap=%u)\n",
                   msg->dir.c_str(), msg->out.c_str(), ESP.getFreeHeap());

      String bucketName = msg->dir;
      if (bucketName == "/") bucketName = "Root Directory";
      else if (bucketName.startsWith("/")) bucketName = bucketName.substring(1);

      if (indexingPathMutex && xSemaphoreTake(indexingPathMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        currentIndexingPath = bucketName;
        xSemaphoreGive(indexingPathMutex);
      }

      unsigned long nowMs = millis();
      if (nowMs - lastQueuedLvglMsg > 1500) {
        char screenBuf[80];
        snprintf(screenBuf, sizeof(screenBuf), "Updating index:\n%s", bucketName.c_str());
        lvglSendMsg(screenBuf, true);
        lastQueuedLvglMsg = nowMs;
        queuedIndexingMsgActive = true;
      }

      webLogf("info", "Processing index request for %s", bucketName.c_str());
      writeNDIndexForDir(msg->dir, msg->out);
      delete msg;

      indexingTasksActive = false;
    } else {
      // Queue is empty, if we were showing an incremental-update message, clear it
      // and reset currentIndexingPath now that there's nothing left to process.
      if (queuedIndexingMsgActive) {
        lvglSendMsg("", false);
        queuedIndexingMsgActive = false;
        if (indexingPathMutex && xSemaphoreTake(indexingPathMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          currentIndexingPath = "";
          xSemaphoreGive(indexingPathMutex);
        }
      }
      int sleepMs = mediaStreamingActive ? 2000 : 500;
      vTaskDelay(pdMS_TO_TICKS(sleepMs));
    }
  }
}
// Helper: is this request for a top-level bucket root? ("/", "/Shows", "/Music", "/Movies", "/Books", "/Gallery", "/Files")
static inline bool isBucketRootPath(const String &p) {
  if (p == "/") return true;
  if (!p.startsWith("/")) return false;
  // Only one leading slash allowed, and NO second slash => bucket root
  int nextSlash = p.indexOf('/', 1);
  return (nextSlash < 0);
}

// The path enqueueIndexUpdateForPath will actually build for `path`.
// /Shows/* and /Music/* fold onto their bucket root, those indexes are recursive and
// carry every nested entry. The /api/index handlers use this so they and the queue agree.
String indexTargetForPath(const String &path) {
  if (path.startsWith("/Music/")) return String("/Music");
  if (path.startsWith("/Shows/")) return String("/Shows");
  return path;
}

void enqueueIndexUpdateForPath(const String &path) {
  // Final safety net: never queue work for our own internal/system folders, no
  // matter which caller reaches this function.
  if (shouldSkipIndexingPath(path)) {
    Serial.printf("[Index] Refusing to enqueue system/internal folder '%s'\n", path.c_str());
    return;
  }

  String targetPath = indexTargetForPath(path);
  if (targetPath != path) {
    Serial.printf("[Index] Redirecting '%s' -> '%s' (bucket index is recursive)\n",
                  path.c_str(), targetPath.c_str());
  }

  // Coalesce quickly repeated requests
  if (!shouldCoalesceIndexRequest(targetPath)) {
    Serial.printf("[Index] Coalesced duplicate index request for '%s'\n", targetPath.c_str());
    return;
  }

  // Decide target index file from path
  String out;
  if (isBucketRootPath(targetPath)) {
    String bucket = targetPath == "/" ? "root" : targetPath.substring(1);
    out = bucket + ".index.ndjson";
  } else {
    out = encodeIndexName(targetPath) + ".nested.ndjson";
  }

  IndexBuildArgs *msg = new IndexBuildArgs{ targetPath, out };

  if (indexQueue && xQueueSend(indexQueue, &msg, pdMS_TO_TICKS(10)) == pdTRUE) {
    Serial.printf("[Index] Enqueued index update for '%s' -> %s\n", targetPath.c_str(), out.c_str());
    return;
  }

  // Queue full, don't try inline indexing (causes stack overflow)
  Serial.printf("[Index] Queue full; index request for '%s' dropped (will retry on next access)\n", targetPath.c_str());
  delete msg;
}

void storageMonitorTask(void *param) {
    webLog("[StorageMonitor] Task started", "info");
    while (true) {
        if (shutdownBackgroundTasks) {
            webLog("[StorageMonitor] Shutting down to prioritize media streaming", "warning");
            storageMonitorTaskHandle = nullptr;
            nomadTaskExit();
            return;
        }

        if (indexingInProgress) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        refreshCachedTotalsFromStat(1000);

        int delayMs = mediaStreamingActive ? 30000 : 10000;
        vTaskDelay(pdMS_TO_TICKS(delayMs));
    }
}

void immediateEnqueueTopLevelTask(void *param) {
  Serial.println("[Index] immediateEnqueueTopLevelTask: starting one-shot boot scan of top-level dirs");
  File root = SD_MMC.open("/");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    Serial.println("[Index] immediateEnqueueTopLevelTask: root not available, exiting task");
    nomadTaskExit();
    return;
  }

  // Known bucket roots that the indexWorker loop's full-scan path already handles
  // directly, skip them here to avoid redundantly queuing a duplicate rebuild.
  const char* buckets[] = { "Shows", "Music", "Movies", "Books", "Gallery", "Files", "Games", "Cookbook", "Workshop", NULL };

  File entry;
  while ((entry = root.openNextFile())) {
    if (entry.isDirectory()) {
      String path = String(entry.path());
      if (!path.startsWith("/")) path = "/" + path;

      if (shouldSkipIndexingPath(path)) {
        Serial.printf("[Index] Skipping system/internal folder '%s' (not media)\n", path.c_str());
        entry.close();
        continue;
      }

      bool isBucket = false;
      for (int i = 0; buckets[i]; i++) {
        if (path == String("/") + buckets[i]) {
          Serial.printf("[Index] Skipping bucket root '%s' (handled by main loop)\n", path.c_str());
          isBucket = true;
          break;
        }
      }

      if (!isBucket) {
        Serial.printf("[Index] Enqueuing top-level folder for indexing: '%s'\n", path.c_str());
        enqueueIndexUpdateForPath(path);
      }
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    entry.close();
  }
  root.close();

  Serial.println("[Index] immediateEnqueueTopLevelTask: enqueue complete, exiting");
  nomadTaskExit();
}

void shutdownBackgroundTasksForStreaming() {
  if (indexingInProgress || indexingTasksActive) {
    Serial.println("[TaskMgr] Skipping shutdown - indexing is active");
    return;
  }

  if (!shutdownBackgroundTasks) {
    Serial.println("[TaskMgr] Shutting down background tasks to prioritize media streaming");
    webLog("[PERFORMANCE] Background tasks shut down for media streaming", "info");
    shutdownBackgroundTasks = true;

    // Give tasks time to shut down gracefully
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// Recreate either background task whose handle is null. Leaves shutdownBackgroundTasks
// alone, healing a dead task mustn't cancel a shutdown the admin asked for. Also covers
// a task that failed to start at boot.
void ensureBackgroundTasks() {
  {
    // Restart indexWorkerTask if it was shut down
    if (indexWorkerTaskHandle == nullptr) {
      // Small delay to prevent immediate resource contention
      vTaskDelay(pdMS_TO_TICKS(50));
      BaseType_t r = nomadCreateTask(indexWorkerTask, "IndexWorker", 16 * 1024, NULL, 2, &indexWorkerTaskHandle, 0);
      if (r != pdPASS) {
        Serial.println("[TaskMgr] Failed to restart IndexWorker task");
        webLog("[ERROR] Failed to restart IndexWorker task", "error");
      } else {
        webLog("[PERFORMANCE] IndexWorker task restarted", "success");
      }
    }

    // Restart storageMonitorTask if it was shut down
    if (storageMonitorTaskHandle == nullptr) {
      // Small delay to prevent immediate resource contention
      vTaskDelay(pdMS_TO_TICKS(50));
      BaseType_t r = nomadCreateTask(storageMonitorTask, "StorageMonitor", 4096, NULL, 1, &storageMonitorTaskHandle, 0);
      if (r != pdPASS) {
        Serial.println("[TaskMgr] Failed to restart StorageMonitor task");
        webLog("[ERROR] Failed to restart StorageMonitor task", "error");
      } else {
        webLog("[PERFORMANCE] StorageMonitor task restarted", "success");
      }
    }
  }
}

// The operator's "restart background tasks" path: lift the kill switch, then heal. Only
// POST /api/tasks?action=restart reaches this. Streaming must never call it, or finishing
// a video would silently undo POST /api/tasks?action=shutdown.
void startBackgroundTasksIfNeeded() {
  if (shutdownBackgroundTasks) {
    Serial.println("[TaskMgr] Restarting background tasks");
    webLog("[PERFORMANCE] Background tasks restarted", "success");
    shutdownBackgroundTasks = false;
  }
  ensureBackgroundTasks();
}

// Simple streaming timeout check 
void checkStreamingTimeout() {
  const unsigned long STREAMING_IDLE_TIMEOUT = 15000; // 15 seconds

  // Restart background tasks once streaming has really been idle for a while.
  if (mediaStreamingActive && (millis() - lastStreamIoMs > STREAMING_IDLE_TIMEOUT)) {
    Serial.println("[TaskMgr] Streaming idle - clearing the streaming flag");
    mediaStreamingActive = false;
    // ensure, not startBackgroundTasksIfNeeded: the end of a playback should heal a task
    // that died, but it must not lift an operator's shutdown.
    ensureBackgroundTasks();
  }

  static unsigned long lastCleanup = 0;
  if (millis() - lastCleanup > 15000) {
    lastCleanup = millis();
    if (streamingFilesMutex && xSemaphoreTake(streamingFilesMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (!streamingFiles.empty()) {
        unsigned long now = millis();
        int closed = 0;
        for (auto it = streamingFiles.begin(); it != streamingFiles.end(); ) {
          bool stale = !it->second.file || (now - it->second.lastActivity > 120000UL);
          if (stale) {
            Serial.printf("[StreamCleanup] Idle #%u (%s, %lus)\n",
                          it->first, it->second.path.c_str(),
                          (unsigned long)((now - it->second.lastActivity) / 1000));
            streamUnpublish(it->second.path, it->first);
            it->second.file.close();
            it = streamingFiles.erase(it);
            closed++;
          } else {
            ++it;
          }
        }
        if (closed > 0) {
          activeStreams = streamingFiles.size();
          Serial.printf("[StreamCleanup] Closed %d idle, %d active\n", closed, activeStreams);
        }
      }
      xSemaphoreGive(streamingFilesMutex);
    }
  }
}

void bootCoordinatorTask(void *pv) {
  Serial.println("[BootCoord] bootCoordinatorTask starting; delaying so UI can come up...");

  vTaskDelay(pdMS_TO_TICKS(3000));

  int settingsWaitLoops = 0;
  while (!settingsReady && settingsWaitLoops++ < 100) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  // safe mode: no index work at all this start, the indexes on the card are used as they are
  if (g_safeMode) {
    sdScanCompleted = true;
    sdScanInProgress = false;
    Serial.println("[BootCoord] safe mode -> no index work this start");
    nomadTaskExit();
    return;
  }

  // old firmware used one /media.json instead of per-bucket NDJSON. if its still here, nuke it and rebuild
  bool legacyUpgradeNeeded = false;
  if (SD_MMC.exists("/media.json")) {
    Serial.println("[BootCoord] Found legacy media.json - upgrading to NDJSON index system");
    lvglSendMsg("Updating to new index system...", true);
    if (SD_MMC.remove("/media.json")) {
      Serial.println("[BootCoord] Removed legacy media.json file");
    } else {
      Serial.println("[BootCoord] WARNING: failed to remove legacy media.json file");
    }
    legacyUpgradeNeeded = true;
  }

  // "Has this device ever completed an index?" 
  String rootIndexFile = String(INDEX_DIR) + "/root.index.ndjson";
  if (legacyUpgradeNeeded || !SD_MMC.exists(INDEX_DIR) || !SD_MMC.exists(rootIndexFile)) {
    Serial.println("[BootCoord] Index missing or incomplete - triggering normal index build");

    lvglSendMsg("First-time setup detected\nBuilding media indexes...\nThis will take a few minutes", true);
    Serial.println("[BootCoord] LVGL screen updated with first-time setup message");

    sdScanCompleted = true;
    sdScanInProgress = false;

    requestIndexing = true;
    Serial.println("[BootCoord] First-time index build queued via normal scan path");
    Serial.println("[BootCoord] coordination done; exiting task");
    nomadTaskExit();
    return;
  }

  // index exists. only enqueue top-level folders if the boot change-detector queued a
  // reindex (bootReindexQueued). nothing changed = no indexing
  if (bootReindexQueued) {
    Serial.println("[BootCoord] Boot reindex active -> enqueuing top-level folders");

    sdScanCompleted = false;
    sdScanInProgress = false;

    BaseType_t ir = nomadCreateTask(immediateEnqueueTopLevelTask, "IncScanBoot", 12 * 1024, NULL, 1, NULL, 1);
    if (ir == pdPASS) {
      Serial.println("[BootCoord] Top-level enqueue task started to populate index queue");
    }

    // requestIndexing was already set by setup() and may already be processing
    Serial.println("[BootCoord] Reindex queued by setup(), directories being enqueued");
  } else {
    Serial.println("[BootCoord] No boot reindex -> indexes exist, storage loads from cache/on-demand");

    sdScanCompleted = true;
    sdScanInProgress = false;
  }

  vTaskDelay(pdMS_TO_TICKS(100));
  Serial.println("[BootCoord] boot coordination done; exiting task");
  nomadTaskExit();
}

// human-readable byte formatting
String humanSize(size_t bytes) {
  double b = (double)bytes;
  const char *units[] = { "B", "KB", "MB", "GB", "TB" };
  int u = 0;
  while (b >= 1024.0 && u < 4) { b /= 1024.0; ++u; }
  char buf[32];
  // one decimal point for readability (e.g. 52.3MB)
  snprintf(buf, sizeof(buf), "%.1f%s", b, units[u]);
  return String(buf);
}
String mimeForPath(const String &path) {
  String p = path;
  p.toLowerCase();
  if (p.endsWith(".html")) return "text/html";
  if (p.endsWith(".css"))  return "text/css";
  if (p.endsWith(".js"))   return "application/javascript";
  if (p.endsWith(".mjs"))  return "text/javascript";      
  if (p.endsWith(".json")) return "application/json";
  if (p.endsWith(".wasm")) return "application/wasm";
  if (p.endsWith(".svg"))  return "image/svg+xml";
  if (p.endsWith(".png"))  return "image/png";
  if (p.endsWith(".jpg") || p.endsWith(".jpeg")) return "image/jpeg";
  if (p.endsWith(".gif")) return "image/gif";
  if (p.endsWith(".webp")) return "image/webp";
  if (p.endsWith(".avif")) return "image/avif";
  if (p.endsWith(".bmp"))  return "image/bmp";
  if (p.endsWith(".ico"))  return "image/x-icon";
  if (p.endsWith(".map")) return "application/octet-stream";
  if (p.endsWith(".woff2")) return "font/woff2";
  if (p.endsWith(".woff"))  return "font/woff";
  if (p.endsWith(".ttf"))   return "font/ttf";
  // fallback:
  return "application/octet-stream";
}

// Helper: explicit mapping for file types we care about
String getMimeType(const String &path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".js"))   return "application/javascript";
  if (path.endsWith(".mjs"))  return "application/javascript";
  if (path.endsWith(".wasm")) return "application/wasm";
  if (path.endsWith(".css"))  return "text/css";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".map"))  return "application/json";
  if (path.endsWith(".svg"))  return "image/svg+xml";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".gif"))  return "image/gif";
  if (path.endsWith(".webp")) return "image/webp";
  if (path.endsWith(".ico"))  return "image/x-icon";
  if (path.endsWith(".ndjson")) return "application/x-ndjson";
  return "application/octet-stream"; // fallback safe binary
}
// Serve one file off the card with an ETag of size+mtime. Pages are no-cache, so the
// ETag saves re-reading big files like the menu and master.css on every visit. A match
// is one lookup and a 304. Files the device writes carry the fake uptime clock, so the
// test is equality. The body is read by NomadPageBody.h.
// gzSibling: send filePath + ".gz" as filePath, with Content-Encoding: gzip.
#include "NomadPageBody.h"
static void sendSdFile(AsyncWebServerRequest *request, const String &filePath, const String &mime,
                       const char *cacheControl, bool cors = false, bool gzSibling = false) {
    if (mime == "text/html") {
        const int page = pageIndexForFile(filePath);
        if (page >= 0 && (g_hiddenMask & (1u << page))) { sendPageOff(request, page); return; }
        usageNotePage(page);
    }
    const String cardPath = gzSibling ? filePath + ".gz" : filePath;
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        request->send(503, "text/plain", "SD busy");
        return;
    }
    uint64_t size = 0;
    time_t mtime = 0;
    const bool ok = SD_MMC.fileStat64(cardPath.c_str(), size, mtime);   // false for a folder too
    if (sdMutex) xSemaphoreGive(sdMutex);
    if (!ok) {
        request->send(404, "text/plain", "File not found");
        return;
    }

    const String etag = nomadEtag(mtime, size);

    if (request->hasHeader("If-None-Match") && request->getHeader("If-None-Match")->value() == etag) {
        AsyncWebServerResponse *r = request->beginResponse(304);
        r->addHeader("ETag", etag);
        r->addHeader("Cache-Control", cacheControl);
        if (cors) r->addHeader("Access-Control-Allow-Origin", "*");
        request->send(r);
        return;
    }
    // a whole-file reply's length is a size_t: a 4 GB file in /Files goes the way films do
    if (size >= 0xFFFFFFFFull && !gzSibling) {
        request->redirect("/media?file=" + nomadQueryEncode(filePath));
        return;
    }

    AsyncWebServerResponse *response = nomadPageResponse(request, cardPath, mime, size);
    // Same OOM guard as the streaming path
    if (!response) {
        Serial.printf("[Static] no response for %s (heap=%u)\n",
                      filePath.c_str(), (unsigned)ESP.getFreeHeap());
        AsyncWebServerResponse *busy = request->beginResponse(503, "text/plain", "Server busy - retry");
        if (busy) { busy->addHeader("Retry-After", "1"); request->send(busy); }
        return;
    }
    response->addHeader("ETag", etag);
    response->addHeader("Cache-Control", cacheControl);
    // no Accept-Ranges here, the library already sends "none" and this reply is always
    // the whole file
    if (gzSibling) response->addHeader("Content-Encoding", "gzip");
    usageNoteSent(size);
    if (cors) response->addHeader("Access-Control-Allow-Origin", "*");
    request->send(response);
}

// Nomad-Tools' Speed up games keeps a ROM as Game.gba.gz (a GBA ROM is about 40% of its
// size gzipped) and deletes Game.gba. The page still asks for Game.gba, so the emulator
// gets the same URL, bytes and save-file name.
static bool nomadOnlyGzSibling(const String &path) {
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  const bool gz = !SD_MMC.exists(path) && SD_MMC.exists(path + ".gz");
  if (sdMutex) xSemaphoreGive(sdMutex);
  return gz;
}

void serveProtectedFile(AsyncWebServerRequest *request, const String& filePath) {
    String mime = getMimeType(filePath);
    // pages are never cached so an update shows right away. JS/CSS stay cached and use
    // ?v= tags. Both revalidate by ETag, so it's a 304 instead of a re-download.
    sendSdFile(request, filePath, mime, mime == "text/html" ? "no-cache" : "public, max-age=600");
}
// Main setup
// -DNOMAD_BENCH=1 adds the /bench/* measurement endpoints. Never set in a shipped build.
#ifndef NOMAD_BENCH
#define NOMAD_BENCH 0
#endif
#if NOMAD_BENCH
#include "NomadBench.h"
#endif
#include "NomadUpdate.h"   // interface updates over the internet, from the admin panel
#include "NomadHealth.h"   // Check videos: headers only, results for admin and the file browser
#include "NomadStorageHealth.h"   // the card: who made it, read errors since boot, a quick read check
#include "NomadFirmware.h"   // new firmware over the air or from a file, checked, with a way back
#include "NomadSupport.h"    // safe mode after repeated crashes, and the support file's build/crash part
#include "NomadParty.h"      // watch together: a host, a code, everyone at the same moment, and voting
#include "NomadZimServe.h"  // archive pages served from the ZIM by the Nomad, for phones without a service worker
#include "NomadChat.h"      // the chat page's messages: numbered as they arrive, kept in PSRAM, logged to /.chat

// Uploads.
// A file is written into a hidden part file next to where it goes and renamed into place
// once all of it is there, so a dropped connection never leaves a broken file under the
// real name. With total= the page sends the file in slices and can pick up where it
// stopped after a dropped connection or a reload:
//   GET  /upload-status?path=/Gallery/a.jpg&total=N  {"size":-1|n,"part":-1|n,"free":-1|n}
//   POST /upload?dir=/Gallery&total=N&offset=K       multipart, one slice of the file
//   POST /upload-cancel  path=/Gallery/a.jpg&total=N  drops the part
// Without total= (the theme and menu files) a request is the whole file, and a part it
// leaves unfinished is removed.
struct NomadUpload {
  File f;
  String part, finalPath;
  uint64_t total = 0, have = 0;
  int failCode = 0;              // set once a write fails: later chunks are dropped
  const char *failMsg = "";
};
static std::map<AsyncWebServerRequest *, NomadUpload> g_uploads;

// .<name>.<total>.nomad-part: hidden, so no index or listing shows it, and tied to the
// size, so a different file under the same name never resumes onto it
static String nomadPartPath(const String &finalPath, uint64_t total) {
  const int slash = finalPath.lastIndexOf('/');
  String p = finalPath.substring(0, slash + 1) + "." + finalPath.substring(slash + 1);
  if (total) {
    char n[24];
    snprintf(n, sizeof(n), ".%llu", (unsigned long long)total);
    p += n;
  }
  return p + ".nomad-part";
}

// free space as last measured (-1 unknown). usedBytes() can hold the card for seconds
// on exFAT, so this uses the cached figure. A failed write is the backstop.
static int64_t nomadFreeBytes() {
  if (cachedTotalBytes == 0) return -1;
  return cachedUsedBytes >= cachedTotalBytes ? 0 : (int64_t)(cachedTotalBytes - cachedUsedBytes);
}

static uint64_t nomadU64Param(AsyncWebServerRequest *request, const char *name) {
  const String v = nomadParamValue(request, name);
  return v.length() ? (uint64_t)strtoull(v.c_str(), nullptr, 10) : 0;
}

static void nomadUploadJson(AsyncWebServerRequest *request, int code, const String &body) {
  AsyncWebServerResponse *r = request->beginResponse(code, "application/json", body);
  if (!r) { request->send(code); return; }
  r->addHeader("Cache-Control", "no-store");
  if (code == 503) r->addHeader("Retry-After", "2");
  request->send(r);
}

// New root folders are admin only, but the guard only sees the file's path, and /upload
// and /save create the file's folder: /Anything/x.bin would make /Anything. Same rule as
// /mkdir. The caller holds sdMutex.
static bool nomadMakesRootFolder(const String &fullPath) {
  const int top = fullPath.indexOf('/', 1);
  if (top <= 0) return false;
  const String root = fullPath.substring(0, top);
  return !isCommunityPath(root) && !SD_MMC.exists(root);
}

// the upload's full path from dir= (form field or query) and the file's name, or "" with
// the refusal already sent
static String nomadUploadTarget(AsyncWebServerRequest *request, const String &filename) {
  String dir = nomadParamValue(request, "dir");
  if (!dir.length()) dir = "/";
  if (!dir.startsWith("/")) dir = "/" + dir;
  if (dir.endsWith("/")) dir.remove(dir.length() - 1);
  const String fullPath = dir + "/" + filename;
  // same traversal guard as /save and /mkdir. dot names are fine (the menu keeps
  // /.search-index.json), only a name that looks like an upload part is refused
  if (fullPath.indexOf("..") >= 0 || filename.indexOf('/') >= 0 || filename.endsWith(".nomad-part")) {
    nomadUploadJson(request, 400, "{\"error\":\"Invalid path\"}");
    return String();
  }
  // Chunks arrive before the server's middleware runs, so the write rules have to be
  // asked here: /config, the interface files and the uploads switch.
  if (!nomadUploadAllowed(request, fullPath)) return String();
  if (!checkAdminAuth(request) && nomadMakesRootFolder(fullPath)) {
    nomadUploadJson(request, 401, "{\"error\":\"Admin login required to make a new folder there\"}");
    return String();
  }
  if (isConfigPath(fullPath) && !checkAdminAuth(request)) {
    nomadUploadJson(request, 401, "{\"error\":\"Unauthorized\"}");
    return String();
  }
  return fullPath;
}

void setup() {
    Serial.begin(115200);
    delay(100);
    nomadFirmwareBoot();   // new firmware proving itself, or the result of going back
    nomadSafeBoot();       // count crash restarts, safe mode at three in a row, read the stored crash
    Serial.println("=== Booting Nomad (debug) ===");
    // Crash Diagnostic, checks what went wrong, and helps me when yall send me logs.
    {
      esp_reset_reason_t rr = esp_reset_reason();
      const char* n = "?";
      switch (rr) {
        case ESP_RST_POWERON: n = "POWERON"; break;
        case ESP_RST_SW: n = "SW/restart"; break;
        case ESP_RST_PANIC: n = "PANIC/exception"; break;
        case ESP_RST_INT_WDT: n = "INT_WDT"; break;
        case ESP_RST_TASK_WDT: n = "TASK_WDT"; break;
        case ESP_RST_WDT: n = "OTHER_WDT"; break;
        case ESP_RST_BROWNOUT: n = "BROWNOUT"; break;
        default: break;
      }
      Serial.printf("[BOOT] reset_reason=%d (%s)  freeHeap=%u  minEverHeap=%u\n",
                    (int)rr, n, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    }
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

    if (get_boot_mode() == USB_MODE) {
      clear_boot_mode();    // next boot will go back to MEDIA
      // disk first, display second: the host starts mounting the moment the
      // MSC LUN is ready, so every millisecond before usb_setup() is wait time
      btStop(); //Stops bluetooth (dont need)
      usb_setup();
      LCD_Init();
      Lvgl_Init();
      ui_init();
      lv_scr_load(ui_Screen1);
      lv_obj_clear_flag(ui_MediaGen, LV_OBJ_FLAG_HIDDEN);
      lv_textarea_set_text(ui_MediaGen, "USB Mass-Storage Mode");
      lv_timer_handler();

      for (;;) {
        usb_loop();
      }
      return;
    }


    LCD_Init();
    Lvgl_Init();
    ui_init();
    lv_obj_clear_flag(ui_MediaGen, LV_OBJ_FLAG_HIDDEN);
    lv_textarea_set_text(ui_MediaGen, "Booting...");
    lv_timer_handler();
    delay(200);


    webLog("[SYSTEM] Nomad System Captive Portal & SDMMC Online", "info");

    // WiFi (hotspot OR joining a home network) starts in startNetwork() after
    // loadSettings(): the WiFi Mode choice lives in settings.json on the card,
    // so the card has to come up first.

// Initialize SD card with full diagnostics
Serial.println("Initializing SD Card...");

// Test basic pin setup first
Serial.println("Setting up MMC pins...");
if (!SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN)) {
    Serial.println("ERROR: SDMMC Pin configuration failed!");
    sdBootHalt("SD card error");
}

if (!mountSDCard()) {
    Serial.println("ERROR: SDMMC Card initialization failed.");
    sdBootHalt("No SD card\nInsert a card to boot");
}

Serial.println("SD Card initialized successfully!");

    webLogf("success", "SD Card initialized successfully - %s, %s, Size: %.1f GB", NomadSD.fsTypeName(), g_sdModeDesc, (float)SD_MMC.cardSize() / (1024.0 * 1024.0 * 1024.0));

    refreshCachedTotalsFromStat();

    // --- create queue + SD mutex BEFORE starting index worker (single creation) ---
    // Update SD card switch indicator
    updateSDStatus();

    // queue holds pointers to IndexBuildArgs*
    if (!indexQueue) {
      indexQueue = xQueueCreate(16, sizeof(IndexBuildArgs*));
      if (!indexQueue) {
        Serial.println("[ERROR] indexQueue creation failed; enqueue will fallback to inline builds");
      } else {
        Serial.println("[IndexQueue] indexQueue created (16 entries)");
      }
    }

    if (!lvglQueue) {
      lvglQueue = xQueueCreate(4, sizeof(LvglMsg));
      if (!lvglQueue) {
        Serial.println("[WARN] lvglQueue creation failed; UI updates from background tasks may be unsafe");
      } else {
        Serial.println("[LVGL] lvglQueue created");
      }
    }

    if (!lvglMutex) lvglMutex = xSemaphoreCreateRecursiveMutex();
    if (!g_taskReapQueue) g_taskReapQueue = xQueueCreate(8, sizeof(TaskHandle_t));

    // make the log mutex before the web server comes up so log writers cant race the reader
    if (!webLogMutex) {
      webLogMutex = xSemaphoreCreateMutex();
      Serial.println(webLogMutex ? "[Log] webLogMutex created"
                                 : "[WARN] webLogMutex creation failed; console logging left unsynchronized");
    }

    // create a mutex to serialize SD card access (protect heavy reads/writes)
    if (!sdMutex) {
      sdMutex = xSemaphoreCreateMutex();
      if (!sdMutex) {
        Serial.println("[WARN] sdMutex creation failed; concurrent SD I/O may be unsafe");
      } else {
        Serial.println("[Index] sdMutex created");
      }
    }



    if (!streamingFilesMutex) {
      streamingFilesMutex = xSemaphoreCreateMutex();
      if (!streamingFilesMutex) {
        Serial.println("[WARN] streamingFilesMutex creation failed");
      } else {
        Serial.println("[Stream] streamingFilesMutex created");
      }
    }

    if (!indexingPathMutex) {
      indexingPathMutex = xSemaphoreCreateMutex();
      if (!indexingPathMutex) {
        Serial.println("[WARN] indexingPathMutex creation failed");
      } else {
        Serial.println("[Index] indexingPathMutex created");
      }
    }

    if (!gameMutex) {
      gameMutex = xSemaphoreCreateMutex();
      if (!gameMutex) {
        Serial.println("[WARN] gameMutex creation failed");
      } else {
        Serial.println("[MP] gameMutex created");
      }
    }

    // config writer: keeps settings and admin config writes off async_tcp
    if (!uiCfgMutex) uiCfgMutex = xSemaphoreCreateMutex();
    if (!cfgWriteQueue) {
      cfgWriteQueue = xQueueCreate(8, sizeof(ConfigWriteJob *));
      if (cfgWriteQueue) {
        nomadCreateTask(configWriterTask, "CfgWriter", 6 * 1024, NULL, 1, NULL, 1);
        Serial.println("[CFGW] config writer task started");
      } else {
        Serial.println("[WARN] cfgWriteQueue creation failed, config writes stay inline");
      }
    }
    loadUiConfigFromSD();
    nomadUsageLoad();

    Serial.println("Loading Settings...");
    loadSettings();
#ifdef NOMAD_RESET_ADMIN_PASSWORD
    // a forgotten admin password: build with -DNOMAD_RESET_ADMIN_PASSWORD=1, flash, let it
    // boot once (the password and every kept sign-in are cleared), then flash a normal build
    if (settings.adminPassword.length()) {
      settings.adminPassword = "";
      saveSettings();
      SD_MMC.remove("/config/sessions.json");
      Serial.println("[Auth] admin password cleared by NOMAD_RESET_ADMIN_PASSWORD");
    }
#endif
    nomadAuthLoad();      // devices kept signed in, from before the restart
    settingsReady = true; // signal background tasks the settings are loaded
    Serial.printf("[SETTINGS] autoGenerateMedia = %s\n", settings.autoGenerateMedia ? "true" : "false");
    startNetwork();  // hotspot or WiFi Mode (station), decided by settings + the one-shot flag
    applyRGBSettings();
    if (settings.flipScreen) {
      // setup() is still single-threaded here (LVGL tasks not started yet) so writing
      // MADCTL directly is safe. repaint so the boot screen isnt left mirrored.
      LCD_SetRotation180(true);
      lv_obj_invalidate(lv_scr_act());
      lv_timer_handler();
    }
    // hotspot: "Connect to:" the SSID. station mode: same headline, but what the
    // user needs is the address the router handed us, so show ip/prefix instead.
    if (g_staMode) {
      String staAddr = WiFi.localIP().toString() + "/" + String(maskToCidr(WiFi.subnetMask()));
      lv_label_set_text(ui_ssidlabel, staAddr.c_str());
    } else {
      lv_label_set_text(ui_ssidlabel, settings.wifiSSID.c_str());
    }
    Serial.print("settings.brightness = ");
    Serial.println(settings.brightness);
    // legacy one-time flag + did USB mode write data last session. the indexer only sees
    // web-UI changes, so a USB write is otherwise invisible to it
    bool generateOnce = SD_MMC.exists("/generate_once.flag");
    bool needsReindexAfterUsb = get_needs_reindex_flag();

    // Log state
    Serial.print("[BOOT] autoGenerateMedia (from settings) = ");
    Serial.println(settings.autoGenerateMedia ? "true" : "false");
    Serial.print("[BOOT] generate_once.flag = ");
    Serial.println(generateOnce ? "true" : "false");
    Serial.print("[BOOT] needs_reindex (post-USB) = ");
    Serial.println(needsReindexAfterUsb ? "true" : "false");

    bool oneTimeReindexRequested = generateOnce || needsReindexAfterUsb;

    // "check for new files on boot" (autoGenerateMedia) turns the change check on or off.
    // ON = reindex only if something changed, OFF = boot normally and clear pending flags.
    // a never-indexed card gets built earlier either way.

    if (g_safeMode) {
      // flags stay where they are, so a normal start still picks the changes up
      Serial.println("[BOOT] Safe mode -> skipping the boot check for new files");
    } else if (settings.autoGenerateMedia && oneTimeReindexRequested) {
      requestIndexing = true;
      bootReindexQueued = true;
      Serial.println("[BOOT] Boot check ON + filesystem changes detected -> queued reindex");

      if (generateOnce && SD_MMC.exists("/generate_once.flag")) {
        SD_MMC.remove("/generate_once.flag");
        Serial.println("[BOOT] Cleared /generate_once.flag");
      }
      if (needsReindexAfterUsb) {
        clear_needs_reindex_flag();
        Serial.println("[BOOT] Cleared post-USB reindex flag");
      }
    } else if (settings.autoGenerateMedia) {
      // toggle on but nothing changed, so no reindex
      Serial.println("[BOOT] Boot check ON but no filesystem changes detected -> booting normally");
    } else {
      Serial.println("[BOOT] Boot check OFF -> booting normally (manual index only)");

      // Clear any pending one-time signals so a stale flag can't surprise-index later.
      if (generateOnce && SD_MMC.exists("/generate_once.flag")) {
        SD_MMC.remove("/generate_once.flag");
        Serial.println("[BOOT] Cleared /generate_once.flag (boot check off)");
      }
      if (needsReindexAfterUsb) {
        clear_needs_reindex_flag();
        Serial.println("[BOOT] Files changed via USB but boot check is OFF -> manual index required");
      }
    }

    // Remove /boot_done.flag if cold boot or one-time requested
    esp_reset_reason_t resetReason = esp_reset_reason();
    if (resetReason == ESP_RST_POWERON || resetReason == ESP_RST_SW || oneTimeReindexRequested) {
        SD_MMC.remove("/boot_done.flag");
    }

    Serial.println("[BOOT] Deferring media generation to bootCoordinatorTask (non-blocking)");

    // Now restore last-known SD usage quickly so UI has a valid starting point
    bool haveSdSnapshot = loadSdUsageFromFile();
    if (!haveSdSnapshot) {
      Serial.println("[SDBAR] No prior usage snapshot found; SDBAR will update after background scan.");
    } else {
      Serial.println("[SDBAR] Restored SD usage snapshot from disk; will skip heavy boot scan if configured.");
    }
    // Breakdown is separate from the totals above. A miss here just means an empty
    // breakdown table until the next scan.
    loadSdBreakdownFromFile();

    Set_Backlight(settings.brightness);  // now using loaded value
    updateSDBAR(); // will show cached values loaded above (if any)

    // Start the storage monitor
    nomadCreateTask(storageMonitorTask, "StorageMonitor", 4096, NULL, 1, &storageMonitorTaskHandle, 0);

    delay(2000);
#if NOMAD_ROUTES_PSRAM
    // Everything malloc'd while the routes are made goes to PSRAM. Routes are small
    // objects but there are a lot of them, and otherwise they sit in the internal RAM
    // films need. Routes are only read on the network task, never while flash is written.
    heap_caps_malloc_extmem_enable(16);
#endif
    // Captive DNS already started by startNetwork() (hotspot only, never on a home network)
    //OPDS Endpoint (still needs fixing)
    server.on("/opds/root.xml", HTTP_GET, handleOPDSRoot);
    server.on("/opds/books.xml", HTTP_GET, handleOPDSBooks);
    //.m3u playlist endpoint NEEDS UPDATE, very outdated
    server.on("/playlist.m3u", HTTP_GET, [](AsyncWebServerRequest *request){
        // walks three buckets, so it needs the SD lock like every other reader
        if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            request->send(503, "text/plain", "SD busy");
            return;
        }
        AsyncResponseStream *stream = request->beginResponseStream("audio/x-mpegurl");
        String ip = nomadLocalIP().toString();
        stream->print("#EXTM3U\n");

        stream->print("# === MOVIES ===\n");
        File movieDir = SD_MMC.open("/Movies");
        if (movieDir && movieDir.isDirectory()) {
            File file = movieDir.openNextFile();
            while (file) {
                String name = file.name();
                if (!file.isDirectory() && (name.endsWith(".mp4") || name.endsWith(".mkv"))) {
                    String fullPath = String("/Movies/") + name;
                    stream->printf("#EXTINF:-1,%s\n", name.c_str());
                    stream->printf("http://%s/media?file=%s\n", ip.c_str(), urlencode(fullPath).c_str());
                }
                file.close();
                file = movieDir.openNextFile();
            }
        }

        stream->print("# === SHOWS ===\n");
        File showsRoot = SD_MMC.open("/Shows");
        if (showsRoot && showsRoot.isDirectory()) {
            File showFolder = showsRoot.openNextFile();
            while (showFolder) {
                if (showFolder.isDirectory()) {
                    String showFolderName = String(showFolder.name());
                    if (showFolderName.startsWith("/")) showFolderName = showFolderName.substring(1);
                    String fullShowPath = "/Shows/" + showFolderName;

                    File episodeDir = SD_MMC.open(fullShowPath);
                    if (episodeDir && episodeDir.isDirectory()) {
                        File ep = episodeDir.openNextFile();
                        while (ep) {
                            String epName = ep.name();
                            if (!ep.isDirectory() && (epName.endsWith(".mp4") || epName.endsWith(".mkv"))) {
                                String fullPath = fullShowPath + "/" + epName;
                                stream->printf("#EXTINF:-1,%s\n", epName.c_str());
                                stream->printf("http://%s/media?file=%s\n", ip.c_str(), urlencode(fullPath).c_str());
                            }
                            ep.close();
                            ep = episodeDir.openNextFile();
                        }
                    }
                    if (episodeDir) episodeDir.close();
                }
                showFolder.close();
                showFolder = showsRoot.openNextFile();
            }
        }

        stream->print("# === MUSIC ===\n");
        File musicDir = SD_MMC.open("/Music");
        if (musicDir && musicDir.isDirectory()) {
            File file = musicDir.openNextFile();
            while (file) {
                String name = file.name();
                if (!file.isDirectory() && name.endsWith(".mp3")) {
                    String fullPath = String("/Music/") + name;
                    stream->printf("#EXTINF:-1,%s\n", name.c_str());
                    stream->printf("http://%s/media?file=%s\n", ip.c_str(), urlencode(fullPath).c_str());
                }
                file.close();
                file = musicDir.openNextFile();
            }
        }

        if (sdMutex) xSemaphoreGive(sdMutex);
        request->send(stream);
    });

    // nomad.m3u redirects to canonical playlist
    server.on("/nomad.m3u", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->redirect("/playlist.m3u");
    });

    // DLNA/UPnP MediaServer lives in NomadDLNA.cpp: SSDP discovery, device description
    // and ContentDirectory Browse fed by the NDJSON indexes.
    dlnaRegisterRoutes(server);

    // Set LED mode: solid (0), rainbow (1), etc.
    server.on("/led/onoff", HTTP_POST, [](AsyncWebServerRequest *request){},
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
        StaticJsonDocument<64> doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
          request->send(400, "text/plain", "Invalid JSON");
          return;
        }
        bool enabled = doc["enabled"];
        RGB_SetMode(enabled ? 1 : 0);
        request->send(200, "text/plain", "LED toggled");
      }
    );

    // /led/rainbow - Start rainbow loop
    server.on("/led/rainbow", HTTP_POST, [](AsyncWebServerRequest *request) {
      if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
      RGB_SetMode(1);  // Rainbow loop
      request->send(200, "text/plain", "Rainbow mode activated");
    });

    // /led/color - Set solid color from JSON { color: "#rrggbb" }
    server.on("/led/color", HTTP_POST, [](AsyncWebServerRequest *request){},
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
        StaticJsonDocument<64> doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
          request->send(400, "text/plain", "Invalid JSON");
          return;
        }

        const char* hex = doc["color"];
        if (!hex || strlen(hex) != 7 || hex[0] != '#') {
          request->send(400, "text/plain", "Invalid color format");
          return;
        }
        // Parse "#rrggbb"
        char rs[3] = { hex[1], hex[2], 0 };
        char gs[3] = { hex[3], hex[4], 0 };
        char bs[3] = { hex[5], hex[6], 0 };

        uint8_t r = strtol(rs, NULL, 16);
        uint8_t g = strtol(gs, NULL, 16);
        uint8_t b = strtol(bs, NULL, 16);
        RGB_SetColor(r, g, b);
        request->send(200, "text/plain", "Color set");
      }
    );

    server.on("/sdinfo", HTTP_GET, handleSDInfo);
  // Name resolution diagnostics.
    // "nomad.local doesn't work" has three causes that look the same from a browser:
    // the client was never told to ask us (dnsOffered false), it asked and we said no
    // (queries and nxdomain climbing), or it never asks over unicast because .local is
    // mDNS only. dnsQueries tells them apart.
    server.on("/api/netdiag", HTTP_GET, [](AsyncWebServerRequest *request) {
      uint32_t q = 0, a = 0, nx = 0;
      String last;
      nomadDnsStats(q, a, nx, last);
      String j = "{";
      j += "\"mode\":\"" + String(g_staMode ? "station" : "hotspot") + "\"";
      j += ",\"ip\":\"" + nomadLocalIP().toString() + "\"";
      j += ",\"mdnsRunning\":" + String(g_mdnsRunning ? "true" : "false");
      j += ",\"mdnsName\":\"" + String(MDNS_HOSTNAME) + ".local\"";
      j += ",\"unicastName\":\"" + String(UNICAST_HOSTNAME) + "\"";
      j += ",\"dnsOffered\":" + String(g_dnsOffered ? "true" : "false");
      j += ",\"captivePortalUri\":" + String(g_captivePortalUri ? "true" : "false");
      j += ",\"dnsCatchAll\":" + String(settings.dnsCatchAll ? "true" : "false");
      j += ",\"dnsQueries\":" + String((unsigned long)q);
      j += ",\"dnsAnswered\":" + String((unsigned long)a);
      j += ",\"dnsNxdomain\":" + String((unsigned long)nx);
      j += ",\"dnsLastName\":\"" + last + "\"";
      j += "}";
      request->send(200, "application/json", j);
    });

    server.on("/api/sd-breakdown", HTTP_GET, [](AsyncWebServerRequest *request){
      std::vector<std::pair<String, BreakdownStats>> entries = sortedBreakdownEntries(g_lastBreakdown);

      size_t docSize = JSON_ARRAY_SIZE(entries.size()) + entries.size() * JSON_OBJECT_SIZE(4) + entries.size() * 48 + 128;
      DynamicJsonDocument doc(docSize);
      doc["scanning"] = (bool)sdScanInProgress;
      JsonArray arr = doc.createNestedArray("breakdown");
      for (auto &e : entries) {
        JsonObject o = arr.createNestedObject();
        o["dir"] = e.first;
        o["bytes"] = e.second.bytes;
        o["files"] = e.second.files;
        o["dirs"] = e.second.dirs;
      }

      String payload;
      serializeJson(doc, payload);
      request->send(200, "application/json", payload);
    });
    server.on("/generate-media", HTTP_POST, [](AsyncWebServerRequest *request) {
      if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
      webLog("[ADMIN] Media generation requested by user interface", "info");

      if (SD_MMC.cardType() == CARD_NONE) {
        request->send(500, "text/plain", "SD card not available.");
        webLog("[ADMIN] SD card not mounted - cannot generate media", "error");
        return;
      }

      if (indexingInProgress) {
        request->send(409, "text/plain", "Index already in progress");
        webLog("[ADMIN] Index request ignored - indexing already in progress", "warning");
        return;
      }

      // Don't start indexing on top of a running full-card scan (the same heavy
      // concurrent-SD situation, from the other direction).
      if (sdScanInProgress) {
        request->send(409, "text/plain", "A full SD scan is running. Try again once it finishes.");
        webLog("[ADMIN] Index request ignored - full SD scan in progress", "warning");
        return;
      }

      if (requestIndexing) {
        request->send(202, "text/plain", "Index request already queued");
        webLog("[ADMIN] Duplicate index request ignored - already queued", "warning");
        return;
      }

      requestIndexing = true;

      // manual index is always full, so enqueue the non-bucket top-level folders too
      // (the boot enqueue only runs on detected changes). same 12 KB stack as the boot
      // call, a card with many top level folders needs it
      BaseType_t tr = nomadCreateTask(immediateEnqueueTopLevelTask, "ImmediateEnq", 12 * 1024, NULL, 1, NULL, 1);
      if (tr == pdPASS) {
        webLog("[ADMIN] Starting immediate index task", "info");
      } else {
        webLog("[ADMIN] Failed to start immediate index task", "error");
      }

      if (sdMutex == NULL || xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (SD_MMC.exists("/generate_once.flag")) {
          SD_MMC.remove("/generate_once.flag");
          webLog("[ADMIN] Removed legacy generate_once.flag file", "info");
        }
        if (sdMutex) xSemaphoreGive(sdMutex);
      }

      request->send(200, "text/plain", "Indexing queued; background task will run it.");
      webLog("[ADMIN] Index queued for background processing", "info");
    });

    // static + Archive handlers


    // Advanced-content manifest (ZIMs today; ROMs/map tiles later (hopes and prayers)).
    server.on("/api/archive-list", HTTP_GET, handleArchiveList);
    server.on("/api/games-list", HTTP_GET, handleGamesList);
    server.on("/api/maps-list", HTTP_GET, handleMapsList);

    // Multiplayer (HTTP polling, see the MpRoom store + handlers above for the design notes).
    server.on("/api/mp/create", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, handleMpCreate);
    server.on("/api/mp/join", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, handleMpJoin);
    server.on("/api/mp/move", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, handleMpMove);
    server.on("/api/mp/state", HTTP_GET, handleMpState);
    server.on("/Archive", HTTP_OPTIONS, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *response = request->beginResponse(204, "text/plain", "");
        response->addHeader("Access-Control-Allow-Origin", "*");
        response->addHeader("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS");
        response->addHeader("Access-Control-Allow-Headers", "Range, Content-Type, Accept");
        response->addHeader("Access-Control-Max-Age", "86400"); // optional: cache preflight for 1 day
        request->send(response);
    });
   // Serve /assets/* with correct MIME types & headers
    server.on("/assets/*", HTTP_GET, [](AsyncWebServerRequest *request){
      String url = request->url(); // e.g. "/assets/foliate/vendor/pdf.mjs"
      if (url.length() == 0) { request->send(400); return; }

      // sendSdFile takes sdMutex with a timeout before touching the card: a bare
      // exists() waits on the SdFat lock forever, and a background task holding it
      // parks async_tcp into the watchdog. every page pulls JS and CSS here.
      // CORS headers stay on for the libraries that fetch their own files (PDF.js
      // worker, wasm).
      sendSdFile(request, url, mimeForPath(url), "public, max-age=600", true);
    });

    // CORS preflight for /assets/*
    server.on("/assets/*", HTTP_OPTIONS, [](AsyncWebServerRequest *request){
      AsyncWebServerResponse* r = request->beginResponse(204);
      r->addHeader("Access-Control-Allow-Origin", "*");
      r->addHeader("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS");
      r->addHeader("Access-Control-Allow-Headers", "Range, Content-Type");
      r->addHeader("Access-Control-Max-Age", "1728000");
      request->send(r);
    });

    // Captive triggers for Apple & Android devices. no-cache: a cached copy
    // of the portal page is how phones keep showing a stale landing screen
    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        Serial.println("Apple captive portal request detected, serving appleindex.html");
        nomadSendCardFile(request, "/appleindex.html", "text/html", "no-cache");
    });

    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request) {
        Serial.println("Android/NORMAL captive portal request detected, serving index.html");
        nomadSendCardFile(request, "/index.html", "text/html", "no-cache");
    });
    // the rest of the OS/browser connectivity probe paths. serving the portal page
    // instead of the expected token is what flips each one into "sign in to network".
    // extension-less probes (/gen_204, /nm, /mobile/status.php ...) already land on
    // the portal via onNotFound, these dotted ones would 404 as file lookups.
    {
      static const struct { const char *path; bool apple; } kProbePaths[] = {
        { "/connecttest.txt", false },            // windows NCSI
        { "/ncsi.txt", false },                   // windows NCSI (legacy)
        { "/success.txt", false },                // firefox detectportal
        { "/canonical.html", false },             // ubuntu connectivity-check
        { "/check_network_status.txt", false },   // gnome + kde
        { "/static/hotspot.txt", false },         // fedora
        { "/kindle-wifi/wifistub.html", false },  // kindle
        { "/library/test/success.html", true },   // old-iOS probe set
      };
      for (const auto &p : kProbePaths) {
        const char *page = p.apple ? "/appleindex.html" : "/index.html";
        server.on(p.path, HTTP_GET, [page](AsyncWebServerRequest *request) {
          nomadSendCardFile(request, page, "text/html", "no-cache");
        });
      }
    }
    server.on("/listfiles", HTTP_GET, handleListFiles);
    // Protected HTML page routes, both as "/name.html" and as a short "/name" alias.
    {
      static const struct { const char* alias; const char* file; } kPageRoutes[] = {
        { "movies",      "/movies.html" },
        { "music",       "/music.html" },
        { "playlist",    "/playlist.html" },
        { "books",       "/books.html" },
        { "shows",       "/shows.html" },
        { "admin",       "/admin.html" },
        { "games",       "/games.html" },
        { "maps",        "/maps.html" },
        { "menu",        "/menu.html" },
        { "gallery",     "/gallery.html" },
        { "archive",     "/archive.html" },
        { "files",       "/files.html" },
        { "filebrowser", "/filebrowser.html" },
        { "comic",       "/comics.html" },
        { "comics",      "/comics.html" },
        { "translate",   "/translate.html" },
        { "chat",        "/chat.html" },
        { "epub",        "/epub.html" },
        { "pdf",         "/pdf.html" },
        { "songs",       "/songs.html" },
        { "queue",       "/queue.html" },
        { "cookbook",    "/cookbook.html" },
        { "workshop",    "/workshop.html" },
        // iframed by admin.html, without a route it fell to onNotFound
        { "theme-ui",    "/theme-customization-ui.html" },
      };
      for (const auto &route : kPageRoutes) {
        String filePath = String(route.file);
        String aliasPath = String("/") + route.alias;
        server.on(filePath.c_str(), HTTP_GET, [filePath](AsyncWebServerRequest *request) { serveProtectedFile(request, filePath); });
        server.on(aliasPath.c_str(), HTTP_GET, [filePath](AsyncWebServerRequest *request) { serveProtectedFile(request, filePath); });
      }
    }
    // Serve root directory and default to index.html
    server.onNotFound([](AsyncWebServerRequest *request) {
        String url = request->url();
        
        // Handle captive portal detection first
        String userAgent = "";
        if (request->hasHeader("User-Agent")) userAgent = request->header("User-Agent");
        
        // Block WhatsApp link preview requests
        if (userAgent.indexOf("WAChat") >= 0 || userAgent.indexOf("WhatsApp") >= 0) {
            request->send(204);
            return;
        }
        
        // Direct links to big, seeked files go to the ranged handler: books, archives,
        // audio and video (DLNA and external players link straight to them), anything
        // asked for by byte range, and the map packs (a map reads one .pmtiles or
        // roads.bin in many small ranges). Regex routes don't work in this build
        // (ASYNCWEBSERVER_REGEX isn't defined), so it's done here.
        // a ROM kept gzipped is sent whole, before the Range routing, which would only
        // look for the file under its own name
        if (url.startsWith("/Games/") && !url.endsWith(".gz") && nomadOnlyGzSibling(url)) {
            sendSdFile(request, url, mimeForPath(url), "public, max-age=600", false, true);
            return;
        }

        String routeLower = url;
        routeLower.toLowerCase();
        if (url.startsWith("/Books/") || url.startsWith("/Archive/") ||
            isTimedMediaExtension(routeLower) || request->hasHeader("Range") ||
            routeLower.endsWith(".pmtiles") || (url.startsWith("/Maps/") && routeLower.endsWith(".bin"))) {
            handleRangeRequest(request);
            return;
        }

        // Check if it's a file request (has extension or specific paths)
        if (url.indexOf('.') > 0 || url.startsWith("/Gallery") || url.startsWith("/Files") ||
            url.startsWith("/Movies") || url.startsWith("/Music") || url.startsWith("/Books") ||
            url.startsWith("/Shows") || url.startsWith("/Archive") || url.startsWith("/Games") ||
            url.startsWith("/Maps")) {

            String filePath = url;
            if (!filePath.startsWith("/")) filePath = "/" + filePath;
            if (isConfigPath(filePath)) {
                request->send(404, "text/plain", "File not found");
                return;
            }

            // a bare directory URL falls back to its index.html
            if (filePath.indexOf('.') < 0) {
                if (!filePath.endsWith("/")) filePath += "/";
                filePath += "index.html";
            }

            // Gallery and Files hold whatever people put there, in any case and in
            // formats getMimeType() doesn't list (.avif, .WEBP), so they use the full
            // table. Otherwise those reach the browser as text/plain and download.
            const bool userBucket = filePath.startsWith("/Gallery/") || filePath.startsWith("/Files/");
            String mime = userBucket ? mimeForPath(filePath) : getMimeType(filePath);
            // same rule as serveProtectedFile: pages revalidate every time. The
            // emulator cores (big, never edited on the card) and the two user buckets
            // can sit in the browser cache for a day.
            const char *cc = (mime == "text/html") ? "no-cache"
                           : (userBucket || filePath.startsWith("/EmulatorJS/")) ? "public, max-age=86400"
                           : "public, max-age=600";
            sendSdFile(request, filePath, mime, cc);
            return;
        }

    // Handle captive portal redirects for non-file requests (hotspot only, on a
    // home network a mistyped URL should get the normal landing page)
    if (!g_staMode && userAgent.length()) {
        if (userAgent.indexOf("iPhone") >= 0 || userAgent.indexOf("iPad") >= 0 || userAgent.indexOf("Macintosh") >= 0) {
            nomadSendCardFile(request, "/appleindex.html", "text/html", "no-cache");
            return;
        }
    }

    {
      nomadSendCardFile(request, "/index.html", "text/html", "no-cache");
    }
});
    // /Gallery and /Files are served from onNotFound, don't use serveStatic for them:
    // it skips sdMutex, the busy 503 and the OOM guard.
// what a page needs before it sends anything: is the file already there, how much of it
// is, and will it fit. Answers only where this visitor could upload.
server.on("/upload-status", HTTP_GET, [](AsyncWebServerRequest *request) {
  const String path = nomadParamValue(request, "path");
  const int slash = path.lastIndexOf('/');
  if (slash < 0) { nomadUploadJson(request, 400, "{\"error\":\"Invalid path\"}"); return; }
  if (!nomadUploadAllowed(request, path)) return;
  const uint64_t total = nomadU64Param(request, "total");
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    nomadUploadJson(request, 503, "{\"error\":\"SD busy\"}");
    return;
  }
  uint64_t size = 0, part = 0;
  time_t t;
  const bool exists = SD_MMC.fileStat64(path.c_str(), size, t) || SD_MMC.exists(path);
  const bool hasPart = total && SD_MMC.fileStat64(nomadPartPath(path, total).c_str(), part, t);
  if (sdMutex) xSemaphoreGive(sdMutex);
  char body[96];
  snprintf(body, sizeof(body), "{\"size\":%lld,\"part\":%lld,\"free\":%lld}",
           exists ? (long long)size : -1LL, hasPart ? (long long)part : -1LL, (long long)nomadFreeBytes());
  nomadUploadJson(request, 200, body);
});

// the page gave up on a file: its part goes, so it doesn't sit on the card
server.on("/upload-cancel", HTTP_POST, [](AsyncWebServerRequest *request) {
  const String path = nomadParamValue(request, "path");
  const uint64_t total = nomadU64Param(request, "total");
  if (path.lastIndexOf('/') < 0 || !total) { nomadUploadJson(request, 400, "{\"error\":\"Invalid path\"}"); return; }
  if (!nomadUploadAllowed(request, path)) return;
  const String part = nomadPartPath(path, total);
  for (auto &u : g_uploads) {
    if (u.second.part == part) { nomadUploadJson(request, 409, "{\"error\":\"busy\"}"); return; }
  }
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    nomadUploadJson(request, 503, "{\"error\":\"SD busy\"}");
    return;
  }
  SD_MMC.remove(part);
  if (sdMutex) xSemaphoreGive(sdMutex);
  nomadUploadJson(request, 200, "{\"status\":\"cancelled\"}");
});

server.on(
  "/upload", HTTP_POST,
  // the answer is sent from the last chunk, or from whichever chunk refused the upload
  [](AsyncWebServerRequest *request) {},
  [](AsyncWebServerRequest *request, const String &filename, size_t index,
     uint8_t *data, size_t len, bool final) {
    auto it = g_uploads.find(request);
    if (index != 0 && it == g_uploads.end()) return;     // refused on its first chunk

    // Every chunk touches the card: hold the SD lock for this callback. A card that stays
    // busy fails the upload with a 503 the page retries, the part keeps what was written.
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
      if (it != g_uploads.end()) {
        NomadUpload &u = it->second;
        if (!u.failCode) { u.failCode = 503; u.failMsg = "SD busy"; }
        if (final) {
          const String body = String("{\"error\":\"SD busy\",\"have\":") + String((unsigned long long)u.have) + "}";
          // the part is closed under the lock, if the card is still busy, onDisconnect does it
          if (!sdMutex || xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (u.f) u.f.close();
            if (!u.total) SD_MMC.remove(u.part);
            if (sdMutex) xSemaphoreGive(sdMutex);
            g_uploads.erase(it);
          }
          nomadUploadJson(request, 503, body);
        }
      } else {
        nomadUploadJson(request, 503, "{\"error\":\"SD busy\"}");
      }
      return;
    }
    struct Release { ~Release() { if (sdMutex) xSemaphoreGive(sdMutex); } } release;

    if (index == 0) {
      const String fullPath = nomadUploadTarget(request, filename);
      if (!fullPath.length()) return;
      const uint64_t total = nomadU64Param(request, "total");
      const uint64_t offset = nomadU64Param(request, "offset");
      const String part = nomadPartPath(fullPath, total);
      for (auto &u : g_uploads) {
        if (u.second.part == part) {               // two phones sending the same file
          nomadUploadJson(request, 409, "{\"error\":\"busy\"}");
          return;
        }
      }
      if (SD_MMC.exists(fullPath)) {
        Serial.println("[Upload] Duplicate file detected: " + fullPath);
        nomadUploadJson(request, 409, "{\"error\":\"File already exists\"}");
        return;
      }
      const int64_t freeNow = nomadFreeBytes();
      if (total && freeNow >= 0 && total > offset && (int64_t)(total - offset) > freeNow) {
        nomadUploadJson(request, 507, "{\"error\":\"Card full\",\"free\":" + String((long long)freeNow) + "}");
        return;
      }
      // Ensure directory exists
      const String folder = fullPath.substring(0, fullPath.lastIndexOf('/'));
      if (folder.length() && !SD_MMC.exists(folder)) SD_MMC.mkdir(folder);

      File f;
      if (offset == 0) {
        f = SD_MMC.open(part, FILE_WRITE);         // starts over: any earlier part is cut to 0
      } else {
        // carry on only from exactly where the part ends, so nothing is doubled or skipped
        const uint64_t have = SD_MMC.fileSize64(part.c_str());
        if (have != offset) {
          nomadUploadJson(request, 409, "{\"error\":\"offset\",\"have\":" + String((unsigned long long)have) + "}");
          return;
        }
        f = SD_MMC.open(part, FILE_APPEND);
      }
      if (!f) {
        webLogf("error", "Upload failed to open file: %s", part.c_str());
        nomadUploadJson(request, 500, "{\"error\":\"Failed to open file\"}");
        return;
      }
      NomadUpload &u = g_uploads[request];
      u.f = f;
      u.part = part;
      u.finalPath = fullPath;
      u.total = total;
      u.have = offset;
      it = g_uploads.find(request);
      // An aborted upload never delivers a final chunk. The part stays for the page to
      // carry on from. A whole-file upload (no total) can't, so its part goes.
      request->onDisconnect([request]() {
        auto d = g_uploads.find(request);
        if (d == g_uploads.end()) return;
        Serial.println("[Upload] Client disconnected mid-upload");
        if (!sdMutex || xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
          if (d->second.f) d->second.f.close();
          if (!d->second.total) SD_MMC.remove(d->second.part);
          if (sdMutex) xSemaphoreGive(sdMutex);
        }
        g_uploads.erase(d);
      });
      Serial.printf("[Upload] Started: %s at %llu of %llu\n", fullPath.c_str(),
                    (unsigned long long)offset, (unsigned long long)total);
    }

    NomadUpload &u = it->second;
    if (!u.failCode && len) {
      const size_t w = u.f.write(data, len);
      u.have += w;
      if (w != len) {
        // a write the card refused: full, or no memory for the transfer just now
        u.f.close();
        const int64_t freeNow = nomadFreeBytes();
        const bool full = freeNow >= 0 && freeNow < (int64_t)(4 * 1024 * 1024);
        u.failCode = full ? 507 : 503;
        u.failMsg = full ? "Card full" : "Write failed";
        webLogf("error", "Upload write failed at %llu: %s", (unsigned long long)u.have, u.part.c_str());
        if (!u.total) SD_MMC.remove(u.part);
      }
    }
    if (!final) return;

    if (u.failCode) {
      if (u.f) u.f.close();
      nomadUploadJson(request, u.failCode, String("{\"error\":\"") + u.failMsg + "\",\"have\":" + String((unsigned long long)u.have) + "}");
      g_uploads.erase(it);
      return;
    }
    u.f.close();
    if (u.total && u.have < u.total) {             // one slice of several: more to come
      nomadUploadJson(request, 200, "{\"status\":\"partial\",\"have\":" + String((unsigned long long)u.have) + "}");
      g_uploads.erase(it);
      return;
    }
    if (u.total && u.have != u.total) {            // more than the file's size: start over
      SD_MMC.remove(u.part);
      nomadUploadJson(request, 409, "{\"error\":\"offset\",\"have\":0}");
      g_uploads.erase(it);
      return;
    }
    // All there: into place. If the name was taken meanwhile, the next free "name (2).ext".
    String dest = u.finalPath;
    if (SD_MMC.exists(dest)) {
      const int slash = dest.lastIndexOf('/'), dot = dest.lastIndexOf('.');
      const String stem = dot > slash ? dest.substring(0, dot) : dest;
      const String ext = dot > slash ? dest.substring(dot) : String();
      for (int n = 2; n < 100 && SD_MMC.exists(dest); ++n) dest = stem + " (" + n + ")" + ext;
    }
    if (!SD_MMC.rename(u.part, dest)) {
      webLogf("error", "Upload could not be moved into place: %s", dest.c_str());
      nomadUploadJson(request, 500, "{\"error\":\"Could not save the file\"}");
      g_uploads.erase(it);
      return;
    }
    cachedUsedBytes += u.have;                     // until the next measurement
    Serial.println("[Upload] Finished: " + dest);
    String body = "{\"status\":\"done\",\"path\":\"";
    for (size_t i = 0; i < dest.length(); ++i) {
      const char c = dest.charAt(i);
      if (c == '"' || c == '\\') body += '\\';
      if ((uint8_t)c >= 0x20) body += c;
    }
    nomadUploadJson(request, 200, body + "\",\"size\":" + String((unsigned long long)u.have) + "}");
    g_uploads.erase(it);
  }
);
server.on("/list-assets", HTTP_GET, [](AsyncWebServerRequest *request){
  if (!request->hasParam("dir")) {
    request->send(400, "application/json", "{\"error\":\"Missing dir\"}");
    return;
  }

  String dir = request->getParam("dir")->value();
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
    request->send(503, "application/json", "{\"error\":\"SD busy\"}");
    return;
  }
  File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) {
    if (d) d.close();
    if (sdMutex) xSemaphoreGive(sdMutex);
    request->send(404, "application/json", "{\"error\":\"Invalid dir\"}");
    return;
  }

  AsyncResponseStream *stream = request->beginResponseStream("application/json");
  stream->print("{\"files\":[");
  bool first = true;
  File f = d.openNextFile();
  while (f) {
    if (!first) stream->print(',');
    String name = String(f.name());
    // strip leading dir prefix if present
    if (name.startsWith(dir)) {
      name = name.substring(dir.length());
    }
    // minimal escaping for JSON strings
    stream->print('\"');
    stream->print(jsonEscape(name));
    stream->print('\"');
    first = false;
    f.close();
    f = d.openNextFile();
    yield();
  }
  d.close();
  if (sdMutex) xSemaphoreGive(sdMutex);
  stream->print("]}");
  request->send(stream);
});


server.on("/mkdir", HTTP_POST, [](AsyncWebServerRequest *request) {
    // Require POST parameter "dirname"
    if (!request->hasParam("dirname", true)) {
        request->send(400, "text/plain", "Missing 'dirname' parameter");
        return;
    }

    // Get and sanitize input
    String dirName = request->getParam("dirname", true)->value();

    // Ensure it starts with a slash (absolute path)
    if (!dirName.startsWith("/")) {
        dirName = "/" + dirName;
    }

    // Prevent directory traversal (no "../")
    if (dirName.indexOf("..") >= 0) {
        request->send(400, "text/plain", "Invalid directory name");
        return;
    }
    if (isConfigPath(dirName) && !checkAdminAuth(request)) {
        request->send(401, "text/plain", "Unauthorized");
        return;
    }

    // Remove trailing slash, if present
    if (dirName.endsWith("/")) {
        dirName.remove(dirName.length() - 1);
    }

    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        request->send(503, "text/plain", "SD busy");
        return;
    }

    // Check if the path already exists
    if (SD_MMC.exists(dirName)) {
        if (sdMutex) xSemaphoreGive(sdMutex);
        request->send(409, "text/plain", "Directory already exists");
        return;
    }

    // Attempt to create the directory
    bool made = SD_MMC.mkdir(dirName);
    if (sdMutex) xSemaphoreGive(sdMutex);
    if (made) {
        webLogf("info", "Directory created: %s", dirName.c_str());
        request->send(200, "text/plain", "OK");
    } else {
        webLogf("error", "Failed to create directory: %s", dirName.c_str());
        request->send(500, "text/plain", "Failed to create directory");
    }
});

server.on("/media", HTTP_GET | HTTP_HEAD, handleRangeRequest); // THE MOST IMPORTANT ONE
server.on("/rename", HTTP_POST, handleRename);
server.on("/delete", HTTP_POST, handleDelete);
server.on("/connector", HTTP_POST, [](AsyncWebServerRequest *request){
    handleConnector(request);
});


server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
  if (!request->hasParam("filename", true) || !request->hasParam("content", true)) {
    return request->send(400, "text/plain", "Missing parameters");
  }

  String path = request->getParam("filename", true)->value();
  String content = request->getParam("content", true)->value();
  if (!path.startsWith("/")) path = "/" + path;
  if (path.indexOf("..") >= 0) {
    return request->send(400, "text/plain", "Invalid path");
  }
  if (isConfigPath(path) && !checkAdminAuth(request)) {
    return request->send(401, "text/plain", "Unauthorized");
  }
  if (!checkAdminAuth(request)) {
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE)
      return request->send(503, "text/plain", "SD busy");
    const bool newRoot = nomadMakesRootFolder(path);
    if (sdMutex) xSemaphoreGive(sdMutex);
    if (newRoot) return request->send(401, "text/plain", "Admin login required to make a new folder there");
  }

  // The config writer task does the write (see queueConfigWrite), OK means it's queued.
  // the file is about to be replaced: drop any handle still on the old one, or the next
  // read is served the old contents from it
  if (releaseStreamsFor(path) == 1) return request->send(409, "text/plain", "File is in use");
  if (!queueConfigWrite(path, content)) {
    return request->send(503, "text/plain", "Write queue full, try again");
  }
  Serial.printf("[SAVE] queued write: %s (%d bytes)\n", path.c_str(), content.length());
  request->send(200, "text/plain", "OK");
});

// GET is open (pages need it to know what to hide), POST is admin only.
// State lives in RAM so reads never touch the card.
g_menuLock = xSemaphoreCreateMutex();   // before loop() or a phone can reach it
server.on("/api/menu", HTTP_GET, handleMenuFeatures);
server.on("/api/ui-config", HTTP_GET, [](AsyncWebServerRequest *request){
  String copy = "{}";
  if (!uiCfgMutex || xSemaphoreTake(uiCfgMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    copy = g_uiConfigJson;
    if (uiCfgMutex) xSemaphoreGive(uiCfgMutex);
  }
  AsyncWebServerResponse *r = request->beginResponse(200, "application/json", copy);
  r->addHeader("Cache-Control", "no-cache, no-store");
  request->send(r);
});

server.on("/api/ui-config", HTTP_POST, [](AsyncWebServerRequest *request){
  if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
  if (!request->hasParam("body", true)) {
    request->send(400, "application/json", "{\"error\":\"Missing body\"}");
    return;
  }
  String body = request->getParam("body", true)->value();
  if (body.length() > 2048) {
    request->send(400, "application/json", "{\"error\":\"Too large\"}");
    return;
  }
  StaticJsonDocument<2048> in;
  if (deserializeJson(in, body)) {
    request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  // rebuild from known keys only, so junk can never accumulate in the file
  StaticJsonDocument<2048> out;
  out["v"] = 1;
  JsonArray hp = out.createNestedArray("hiddenPages");
  for (JsonVariant v : in["hiddenPages"].as<JsonArray>()) {
    const char *s = v.as<const char *>();
    if (s && strlen(s) < 24) hp.add(s);
  }
  out["downloadsDisabled"] = in["downloadsDisabled"] | false;
  out["uploadsDisabled"] = in["uploadsDisabled"] | false;
  // per page, only names the firmware knows
  for (const char *list : { "dlOff", "ulOff" }) {
    JsonArray a = out[list].to<JsonArray>();
    for (JsonVariant v : in[list].as<JsonArray>()) { const char *k = v | ""; if (pageBit(k)) a.add(k); }
  }
  String motd = String((const char *)(in["motd"] | ""));
  if (motd.length() > 120) motd = motd.substring(0, 120);
  out["motd"] = motd;

  String json;
  serializeJson(out, json);

  if (!uiCfgMutex || xSemaphoreTake(uiCfgMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    g_uiConfigJson = json;
    if (uiCfgMutex) xSemaphoreGive(uiCfgMutex);
  }
  refreshUiFlags(json);

  bool queued = queueConfigWrite(UI_CONFIG_PATH, json);
  webLogf("info", "UI config updated (%s)", queued ? "write queued" : "WRITE QUEUE FULL");
  request->send(queued ? 200 : 503, "application/json",
                queued ? "{\"status\":\"saved\"}" : "{\"error\":\"write queue full\"}");
});


server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
  StaticJsonDocument<512> doc;
  doc["rgbMode"] = settings.rgbMode;
  doc["rgbColor"] = settings.rgbColor;
  // Never send the real admin secret. /auth/login accepts whatever is stored here, so
  // any guest could send it straight back and become admin. The UI only checks empty vs set.
  bool adminPwSet = settings.adminPassword.length() > 0 && settings.adminPassword != "null";
  doc["adminPassword"] = adminPwSet ? "__set__" : "";
  doc["adminPasswordSet"] = adminPwSet;
  doc["wifiSSID"] = settings.wifiSSID;
  // the AP password is only for the admin's eyes: any guest already ON the
  // network could otherwise read it and share it around
  doc["wifiPassword"] = checkAdminAuth(request) ? settings.wifiPassword : "__set__";
  doc["brightness"] = settings.brightness;
  doc["autoGenerateMedia"] = settings.autoGenerateMedia;
  doc["flipScreen"] = settings.flipScreen;
  doc["dlnaEnabled"] = settings.dlnaEnabled;
  doc["dnsCatchAll"] = settings.dnsCatchAll;

  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
});



// POST to update settings
server.on("/settings", HTTP_POST, [](AsyncWebServerRequest *request){
  if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
  if (!request->hasParam("body", true)) {
    request->send(400, "application/json", "{\"error\":\"Missing body\"}");
    return;
  }

  String body = request->getParam("body", true)->value();
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  if (doc.containsKey("rgbMode")) settings.rgbMode = doc["rgbMode"].as<String>();
  if (doc.containsKey("rgbColor")) settings.rgbColor = doc["rgbColor"].as<String>();
  // older pages set the password here as typed, it still gets stored salted
  // (POST /auth/password is the normal way, and never sends the password itself)
  if (doc.containsKey("adminPassword")) {
    const String pw = doc["adminPassword"].as<String>();
    if (pw.length() == 0 || pw == "null") authStoreRecord("");
    else authSetPlain(pw);
  }
  if (doc.containsKey("wifiSSID")) {
    const String ssid = doc["wifiSSID"].as<String>();
    if (ssid.length() && ssid.length() <= 32) settings.wifiSSID = ssid;
  }
  // Only take a password WPA2 can use (8-63 chars). An empty one from a page that never
  // saw the real password would open the hotspot, so open has to be asked for (wifiOpen).
  if (doc.containsKey("wifiPassword")) {
    const String pw = doc["wifiPassword"].as<String>();
    if (pw.length() >= 8 && pw.length() <= 63 && pw != "__set__") settings.wifiPassword = pw;
    else if (pw.length() == 0 && (doc["wifiOpen"] | false)) settings.wifiPassword = "";
  }
  if (doc.containsKey("brightness")) settings.brightness = constrain(doc["brightness"].as<int>(), 0, 100);
  if (doc.containsKey("autoGenerateMedia")) settings.autoGenerateMedia = doc["autoGenerateMedia"].as<bool>();
  if (doc.containsKey("dlnaEnabled")) {
    settings.dlnaEnabled = doc["dlnaEnabled"].as<bool>();
    dlnaSetEnabled(settings.dlnaEnabled);
  }
  if (doc.containsKey("dnsCatchAll")) {
    settings.dnsCatchAll = doc["dnsCatchAll"].as<bool>();
    nomadDnsSetCatchAll(settings.dnsCatchAll);  // applies live, no restart
  }
  if (doc.containsKey("flipScreen")) {
    bool newFlip = doc["flipScreen"].as<bool>();
    if (newFlip != settings.flipScreen) {
      settings.flipScreen = newFlip;
      lcdRotatePending = true;  // applied by the LVGL-owning task, not here
    }
  }

  if (saveSettings()) {
      webLogf("info", "Settings updated successfully");
      request->send(200, "application/json", "{\"status\":\"updated\"}");
  } else {
      webLogf("error", "Failed to save settings");
      request->send(500, "application/json", "{\"error\":\"Failed to save settings\"}");
  }
});

nomadAuthRegister(server);   // /auth/challenge, /auth/login, /auth/logout, /auth/sessions, /auth/password

// WiFi Mode (admin popup).
// Status for the popup. Admin-only: it names the configured home network.
server.on("/api/wifi-mode", HTTP_GET, [](AsyncWebServerRequest *request){
  if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
  StaticJsonDocument<512> doc;
  doc["mode"] = g_staMode ? "sta" : "ap";
  doc["staSSID"] = settings.staSSID;
  doc["staPasswordSet"] = settings.staPassword.length() > 0;
  doc["staPersist"] = settings.staPersist;
  doc["staArmed"] = get_sta_once_flag();  // a one-shot join is queued for the next restart
  doc["connected"] = g_staMode && (WiFi.status() == WL_CONNECTED);
  doc["ip"] = nomadLocalIP().toString();
  if (g_staMode) {
    doc["cidr"] = maskToCidr(WiFi.subnetMask());
    doc["gateway"] = WiFi.gatewayIP().toString();
    doc["rssi"] = WiFi.RSSI();
  }
  doc["hotspotSSID"] = settings.wifiSSID;
  doc["lastError"] = g_staFailReason;
  String json;
  serializeJson(doc, json);
  AsyncWebServerResponse *r = request->beginResponse(200, "application/json", json);
  r->addHeader("Cache-Control", "no-cache, no-store");
  request->send(r);
});

// Apply a WiFi Mode change. Settings are flushed through the config writer and
// the restart is deferred to loop() so the response gets out and the write lands.
server.on("/api/wifi-mode", HTTP_POST, [](AsyncWebServerRequest *request){
  if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
  if (!request->hasParam("body", true)) {
    request->send(400, "application/json", "{\"error\":\"Missing body\"}");
    return;
  }
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, request->getParam("body", true)->value())) {
    request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  String action = doc["action"] | "";

  if (action == "connect") {
    String ssid = doc["ssid"] | "";
    ssid.trim();
    if (ssid.length() == 0 || ssid.length() > 32) {
      request->send(400, "application/json", "{\"error\":\"Network name must be 1-32 characters\"}");
      return;
    }
    // password omitted entirely = keep the stored one (UI re-connect flows)
    if (doc.containsKey("password")) {
      String pw = doc["password"].as<String>();
      if (pw.length() > 0 && (pw.length() < 8 || pw.length() > 63)) {
        request->send(400, "application/json", "{\"error\":\"WiFi passwords are 8-63 characters (leave empty for open networks)\"}");
        return;
      }
      settings.staPassword = pw;
    }
    settings.staSSID = ssid;
    settings.staPersist = doc["persist"] | false;
    // persist rides settings.json every boot, one-shot arms the NVS flag that a
    // power cycle clears. never leave both active or "disable by unplugging" lies.
    if (settings.staPersist) clear_sta_once_flag(); else set_sta_once_flag();
    if (!saveSettings()) {
      request->send(500, "application/json", "{\"error\":\"Failed to save settings\"}");
      return;
    }
    g_wifiModeRestartAt = millis() + 2500;
    webLogf("info", "WiFi Mode: restarting to join '%s' (%s)", ssid.c_str(),
            settings.staPersist ? "every boot" : "until power cycle");
    request->send(200, "application/json", "{\"status\":\"restarting\"}");

  } else if (action == "disable" || action == "forget") {
    bool wasSta = g_staMode;
    settings.staPersist = false;
    if (action == "forget") { settings.staSSID = ""; settings.staPassword = ""; }
    clear_sta_once_flag();
    if (!saveSettings()) {
      request->send(500, "application/json", "{\"error\":\"Failed to save settings\"}");
      return;
    }
    if (wasSta) g_wifiModeRestartAt = millis() + 2500;
    webLogf("info", "WiFi Mode: %s -> hotspot mode%s", action.c_str(), wasSta ? " (restarting)" : "");
    request->send(200, "application/json", wasSta ? "{\"status\":\"restarting\"}" : "{\"status\":\"saved\"}");

  } else {
    request->send(400, "application/json", "{\"error\":\"Unknown action\"}");
  }
});

// async scan for the popup's picker: GET starts a scan if none is running, the
// popup polls until "done". needs the STA iface, so on the hotspot the radio
// briefly runs AP+STA and clients stall a couple seconds. admin-only.
server.on("/api/wifi-scan", HTTP_GET, [](AsyncWebServerRequest *request){
  if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    request->send(200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }
  if (n < 0) {  // not started (or previous scan failed): kick one off
    if (!g_staMode) WiFi.mode(WIFI_AP_STA);
    WiFi.scanNetworks(true /*async*/);
    request->send(200, "application/json", "{\"status\":\"scanning\"}");
    return;
  }
  DynamicJsonDocument doc(4096);
  doc["status"] = "done";
  JsonArray nets = doc.createNestedArray("networks");
  int count = n > 20 ? 20 : n;  // results come strongest-first, the popup needs no page 2
  for (int i = 0; i < count; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;  // hidden network: nothing to click on
    bool dup = false;
    for (JsonVariant v : nets) {
      if (ssid.equals((const char *)v["ssid"])) { dup = true; break; }
    }
    if (dup) continue;
    JsonObject o = nets.createNestedObject();
    o["ssid"] = ssid;
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  if (!g_staMode) WiFi.mode(WIFI_AP);  // drop the temporary STA interface
  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
});

  server.on("/admin-status", HTTP_GET, [](AsyncWebServerRequest *request){
    // Build JSON
    StaticJsonDocument<384> doc;
    doc["ssid"]         = g_staMode ? settings.staSSID : settings.wifiSSID;
    // same rule as GET /settings: the password is for the admin's eyes only
    doc["wifiPassword"] = checkAdminAuth(request) ? settings.wifiPassword : "__set__";
    doc["users"]        = getConnectedUserCount();
    doc["wifiMode"]     = g_staMode ? "sta" : "ap";
    doc["ip"]           = nomadLocalIP().toString();

    String payload;
    serializeJson(doc, payload);
    request->send(200, "application/json", payload);
  });

  // Scan status endpoint for admin console
  server.on("/scan-status", HTTP_GET, [](AsyncWebServerRequest *request){
    StaticJsonDocument<384> doc;

    // order matters. also check indexingTasksActive or the queued/incremental
    // path reads as "Idle" while its actually rewriting an index
    String status = "Idle";
    String mode = "—";
    int queueDepth = 0;

    if (sdScanInProgress) {
      status = "Scanning SD Card";
      mode = "Initial Scan";
    } else if (indexingInProgress) {
      status = "Indexing Media";
      mode = "Background Index";
    } else if (indexingTasksActive) {
      status = "Indexing Media";
      mode = "Incremental Update";
    } else if (requestIndexing) {
      status = "Index Requested";
      mode = "Pending";
    }

    // Get queue depth if available
    if (indexQueue) {
      queueDepth = uxQueueMessagesWaiting(indexQueue);
    }

    String currentPath;
    int bucketNum = 0, totalBuckets = 0, percent = 0;
    if (indexingPathMutex && xSemaphoreTake(indexingPathMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      currentPath = currentIndexingPath;
      bucketNum = g_currentBucketNum;
      totalBuckets = g_totalBucketsForProgress;
      percent = g_indexProgressPercent;
      xSemaphoreGive(indexingPathMutex);
    }

    doc["status"] = status;
    doc["mode"] = mode;
    doc["queueDepth"] = queueDepth;
    doc["sdScanInProgress"] = sdScanInProgress;
    doc["indexingInProgress"] = indexingInProgress;
    doc["indexingTasksActive"] = indexingTasksActive;
    doc["requestIndexing"] = requestIndexing;
    doc["currentPath"] = currentPath;
    doc["currentBucketNum"] = bucketNum;
    doc["totalBuckets"] = totalBuckets;
    doc["progressPercent"] = percent;

    String payload;
    serializeJson(doc, payload);
    request->send(200, "application/json", payload);
  });

  // Web console logs endpoint
  server.on("/console-logs", HTTP_GET, [](AsyncWebServerRequest *request){
    // the log names files people delete and the networks the device joins
    if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    // heap, not stack: 50 entries with copied message Strings overflow a 2KB
    // pool and the overflowed rows silently serialize as {}
    DynamicJsonDocument doc(8192);
    JsonArray logs = doc.createNestedArray("logs");

    // hold the lock for the whole copy, a webLog() realloc mid-copy would corrupt the heap.
    // once doc owns its copies we can release and serialize
    bool locked = (webLogMutex && xSemaphoreTake(webLogMutex, pdMS_TO_TICKS(200)) == pdTRUE);
    if (webLogMutex && !locked) {
      request->send(503, "application/json", "{\"error\":\"log busy\"}");
      return;
    }

    // Add logs in chronological order (oldest first)
    int startIdx = (logCount < MAX_LOG_ENTRIES) ? 0 : logIndex;
    for (int i = 0; i < logCount; i++) {
      int idx = (startIdx + i) % MAX_LOG_ENTRIES;
      JsonObject logObj = logs.createNestedObject();
      logObj["message"] = webLogs[idx].message;
      logObj["type"] = webLogs[idx].type;
      logObj["timestamp"] = webLogs[idx].timestamp;
    }

    if (locked) xSemaphoreGive(webLogMutex);
    doc["now"] = millis();   // the entries' times are uptime too: the page shows "5 min ago" from this

    String payload;
    serializeJson(doc, payload);
    request->send(200, "application/json", payload);
  });

  // Dedicated brightness endpoint, Its LIVE NOW!
  server.on("/brightness", HTTP_POST, [](AsyncWebServerRequest *request){
      if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
      if (!request->hasParam("body", true)) {
        request->send(400, "application/json", "{\"error\":\"Missing body\"}");
        return;
      }

      String body = request->getParam("body", true)->value();
      StaticJsonDocument<128> doc;
      DeserializationError error = deserializeJson(doc, body);
      if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      if (doc.containsKey("value")) {
        // Clamp to Set_Backlight's valid 0-100 range, out-of-range values are
        // silently dropped by Set_Backlight and would corrupt the slider display.
        settings.brightness = constrain(doc["value"].as<int>(), 0, 100);
        Set_Backlight(settings.brightness);  // Apply brightness immediately, at long last
        saveSettings();  // Save to SD card
        request->send(200, "application/json", "{\"status\":\"updated\"}");
      } else {
        request->send(400, "application/json", "{\"error\":\"Missing brightness value\"}");
      }
  });

  // Safe shutdown handler
  server.on("/shutdown", HTTP_POST, [](AsyncWebServerRequest *request) {
      if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
      // Display shutdown message on LVGL screen
      lvglSendMsg("Shutting Down...", true);

      // Send response to client before shutting down
      request->send(200, "text/plain", "Server is shutting down safely");

      // Turn off RGB LEDs
      RGB_SetMode(0);

      // Unmount SD card safely
      Serial.println("Unmounting SD card...");
      SD_MMC.end();
      
      // Small delay to ensure response is sent
      delay(1000);
      
      // Enter deep sleep mode
      Serial.println("Entering deep sleep...");
      esp_deep_sleep_start();
  });
  server.on("/restart", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    webLogf("info", "Restart requested");
    request->send(200, "text/plain", "Rebooting...");
    delay(1000);
    ESP.restart();
  });

  server.on("/cpu-temp", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "application/json", String("{\"temperature\":") + currentTempC + "}");
  });

  // GET /api/index -> serves index files (NDJSON format) or builds them
  server.on("/api/index", HTTP_GET, [](AsyncWebServerRequest *request){
    String indexPath = "/";
    if (request->hasParam("path")) indexPath = normalizePath(request->getParam("path")->value());

    // Determine the correct index filename
    String indexFile;
    if (indexPath == "/" || (indexPath.startsWith("/") && indexPath.indexOf('/', 1) < 0)) {
      // Bucket root path (e.g., "/", "/Books", "/Movies")
      String bucket = indexPath == "/" ? "root" : indexPath.substring(1);
      indexFile = bucket + ".index.ndjson";
    } else {
      // Nested path (e.g., "/Books/SeriesName")
      String enc = encodeIndexName(indexPath);
      indexFile = enc + ".nested.ndjson";
    }

    String fullPath = String(INDEX_DIR) + "/" + indexFile;

    // Take sdMutex before touching the card. Without it these calls go straight to the
    // SdFat lock, which waits forever, and a background task holding it parks async_tcp
    // until the watchdog fires. Every page hits this route, so time out and 503.
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
      request->send(503, "application/json", "{\"error\":\"SD busy\"}");
      return;
    }

    // Ensure index directory exists
    ensureIndexDir();

    bool haveIndex = SD_MMC.exists(fullPath);

    // A miss only means "one is coming" if a build would produce this file, or the page
    // polls a 202 forever. exists() isn't enough, writeNDIndexForDir also gives up on a
    // path that is a file.
    const bool willBuildThisFile = (indexTargetForPath(indexPath) == indexPath) &&
                                   !shouldSkipIndexingPath(indexPath);
    bool haveDir = true;
    if (!haveIndex && willBuildThisFile) {
      haveDir = SD_MMC.exists(indexPath);
      if (haveDir) {
        File d = SD_MMC.open(indexPath);
        haveDir = (d && d.isDirectory());
        if (d) d.close();
      }
    }
    if (sdMutex) xSemaphoreGive(sdMutex);

    // If index exists, serve it. Its header carries a signature of the folder's contents,
    // used as an ETag so a repeat visit is a 304 instead of the whole index again.
    if (haveIndex) {
      const String etag = indexEtag(fullPath);
      if (etag.length() && request->hasHeader("If-None-Match") && request->header("If-None-Match") == etag) {
        AsyncWebServerResponse *same = request->beginResponse(304);
        same->addHeader("ETag", etag);
        same->addHeader("Cache-Control", "no-cache");
        request->send(same);
        return;
      }
      nomadSendCardFile(request, fullPath, "application/x-ndjson", etag.length() ? "no-cache" : "no-cache, no-store", etag);
      return;
    }

    // No index, and none is coming: the queue would build a different file (/Shows/* and
    // /Music/* fold onto their bucket root, and queuing those rebuilds the whole bucket
    // on every poll), indexing skips the path (/Archive, /config, dot folders), or there's
    // no directory here. So 404: NomadList falls back to /listfiles and the file browser
    // to /connector.
    if (!haveDir || !willBuildThisFile) {
      Serial.printf("[Index] Request for '%s' - no index for this path, not enqueuing\n",
                    indexPath.c_str());
      request->send(404, "application/json",
        "{\"error\":\"no index for this path\",\"path\":\"" + jsonEscape(indexPath) + "\"}");
      return;
    }

    // If index doesn't exist, enqueue build and return 202
    Serial.printf("[Index] Request for '%s' - index missing, enqueuing\n", indexPath.c_str());
    enqueueIndexUpdateForPath(indexPath);
    request->send(202, "application/json",
      "{\"status\":\"building\",\"path\":\"" + indexPath + "\"}");
  });

  // GET /api/index-nested -> serves nested index files (JSON format) or builds them
  server.on("/api/index-nested", HTTP_GET, [](AsyncWebServerRequest *request){
    String path = "/";
    if (request->hasParam("path")) path = normalizePath(request->getParam("path")->value());

    // Determine the correct index filename based on whether it's a bucket root
    String indexFile;
    if (path == "/" || (path.startsWith("/") && path.indexOf('/', 1) < 0)) {
      // Bucket root path (e.g., "/", "/Movies", "/Shows")
      String bucket = path == "/" ? "root" : path.substring(1);
      indexFile = bucket + ".index.ndjson";
    } else {
      // Nested path (e.g., "/Movies/SomeMovie")
      String enc = encodeIndexName(path);
      indexFile = enc + ".nested.ndjson";
    }

    String fullPath = String(INDEX_DIR) + "/" + indexFile;

    // sdMutex first, same reason as /api/index: a bare exists() waits on the
    // SdFat lock with no timeout and takes async_tcp down with it.
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
      request->send(503, "application/json", "{\"error\":\"SD busy\"}");
      return;
    }

    // Ensure index directory exists
    ensureIndexDir();

    bool haveNested = SD_MMC.exists(fullPath);

    // If the index exists, stream it from the file as JSON a piece at a time. A bucket
    // index can be hundreds of KB, so don't build it in RAM (AsyncResponseStream buffers
    // the whole body).
    if (haveNested) {
      auto st = std::make_shared<NestedIndexStream>();
      st->prefix = "{\"status\":\"ready\",\"path\":\"" + jsonEscape(path) + "\",\"entries\":[";
      const bool opened = st->f.open(fullPath.c_str());
      if (sdMutex) xSemaphoreGive(sdMutex);
      if (opened) {
        AsyncWebServerResponse *resp = request->beginChunkedResponse("application/json",
          [st](uint8_t *buf, size_t maxLen, size_t) -> size_t { return st->fill(buf, maxLen); });
        resp->addHeader("Cache-Control", "no-cache, no-store");
        request->send(resp);
        return;
      }
      if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        request->send(503, "application/json", "{\"error\":\"SD busy\"}");
        return;
      }
    }
    // Same question as /api/index, asked in the same place: inside the mutex this handler
    // still holds, and only because the index was not there to serve.
    const bool willBuildThisFile = (indexTargetForPath(path) == path) &&
                                   !shouldSkipIndexingPath(path);
    bool haveDir = true;
    if (willBuildThisFile) {
      haveDir = SD_MMC.exists(path);
      if (haveDir) {
        File d = SD_MMC.open(path);
        haveDir = (d && d.isDirectory());
        if (d) d.close();
      }
    }
    if (sdMutex) xSemaphoreGive(sdMutex);

    // 404, not 202: a 202 counts as ok() in fetch, so the file browser would cache this
    // status object as an empty listing. Same rules as /api/index above.
    if (!haveDir || !willBuildThisFile) {
      Serial.printf("[Index] Request for '%s' - no nested index for this path, not enqueuing\n",
                    path.c_str());
      request->send(404, "application/json",
        "{\"error\":\"no index for this path\",\"path\":\"" + jsonEscape(path) + "\"}");
      return;
    }

    // Always enqueue to background worker, no inline builds (prevents blocking)
    Serial.printf("[Index] Request for '%s' - index missing, enqueuing to background worker\n", path.c_str());
    enqueueIndexUpdateForPath(path);

    request->send(202, "application/json",
    "{\"status\":\"building\",\"scope\":\"nested\",\"path\":\"" + path + "\"}");
  });


  // POST /api/reindex?path=/Shows > kicks off background reindexing of one folder.
  server.on("/api/reindex", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }

    // Any indexing at all (running, its tasks still busy, or a full index queued) turns
    // this away. Checking indexingInProgress alone could mark a running full index as done.
    if (indexingInProgress || indexingTasksActive || requestIndexing) {
      request->send(409, "application/json", "{\"error\":\"Index already in progress\"}");
      webLog("[ADMIN] Reindex request ignored - indexing already in progress", "warning");
      return;
    }
    if (sdScanInProgress) {
      request->send(409, "application/json", "{\"error\":\"A full SD scan is running. Try again once it finishes.\"}");
      webLog("[ADMIN] Reindex request ignored - full SD scan in progress", "warning");
      return;
    }

    String path = "/";
    if(request->hasParam("path", true)) path = request->getParam("path", true)->value();     // from POST body
    else if(request->hasParam("path")) path = request->getParam("path")->value();            // from query
    path = normalizePath(path);

    // claimed here, before anything else can start an index, and only answered once the
    // task exists, so a task that never started doesn't look fine
    indexingInProgress = true;
    indexingTasksActive = true;
    String *arg = new String(path);
    BaseType_t ok = nomadCreateTask([](void *pv){
      String p = *((String*)pv);
      delete (String*)pv;
      Serial.printf("[ReindexTask] start %s\n", p.c_str());
      webLogf("info", "Reindex of '%s' requested", p.c_str());

      // (indexingInProgress and indexingTasksActive were claimed by the route)
      if (indexingPathMutex && xSemaphoreTake(indexingPathMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        currentIndexingPath = p;
        g_currentBucketNum = 0;
        g_totalBucketsForProgress = 0;
        g_indexProgressPercent = 0;
        xSemaphoreGive(indexingPathMutex);
      }

      // Shown immediately so there's no dead screen while the directory is opened.
      // writeNDIndexForDir takes over with live item-count progress once walking starts.
      char reindexScreenBuf[80];
      snprintf(reindexScreenBuf, sizeof(reindexScreenBuf), "Indexing: %s\n\nDo not unplug!", p.c_str());
      lvglSendMsg(reindexScreenBuf, true);

    if(p == "/"){
      // Root directory - only rebuild root
      buildBucketIndex("/");
    } else if(p == "/Shows"){
      // Shows bucket root - only rebuild Shows
      buildBucketIndex("/Shows");
    } else if(p == "/Music" || p.startsWith("/Music/")){
      // For any Music path, rebuild the entire Music bucket
      buildBucketIndex("/Music");
    } else if(p.startsWith("/Shows/")){
      buildBucketIndex("/Shows");
    } else if(p == "/Books"){
      buildBucketIndex("/Books");
    } else if(p.startsWith("/Books/")){
      buildBucketIndex("/Books");
    } else {
      // For other top-level buckets, rebuild that bucket
      buildBucketIndex(p);
    }

      indexingInProgress = false;
      indexingTasksActive = false;
      if (indexingPathMutex && xSemaphoreTake(indexingPathMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        currentIndexingPath = "";
        xSemaphoreGive(indexingPathMutex);
      }
      lvglSendMsg("", false);

      webLogf("success", "Reindex of '%s' complete", p.c_str());
      Serial.printf("[ReindexTask] finished %s\n", p.c_str());
      nomadTaskExit();
    }, "ReindexTask", 12*1024, arg, 1, NULL, 1);

    if(ok != pdPASS){
      Serial.println("[/api/reindex] task create failed");
      delete arg;
      indexingInProgress = false;
      indexingTasksActive = false;
      request->send(503, "application/json", "{\"error\":\"No memory to start the index, try again\"}");
      return;
    }
    request->send(202, "application/json", "{\"status\":\"accepted\",\"path\":\"" + path + "\"}");
  });
  // fast path: reads SD_MMC's allocation metadata instead of walking files, so it
  // answers synchronously (no task/polling), usually sub-100ms
  server.on("/api/sd-refresh-totals", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }

    uint64_t statUsed = 0;
    if (!refreshCachedTotalsFromStat(statUsed, 400)) {
      request->send(503, "application/json", "{\"error\":\"SD busy, try again\"}");
      return;
    }

    StaticJsonDocument<192> doc;
    doc["total"] = cachedTotalBytes;
    doc["used"] = statUsed;
    doc["percent"] = calcUsagePct(statUsed, cachedTotalBytes);
    doc["statTrusted"] = g_sdStatTrusted;

    String payload;
    serializeJson(doc, payload);
    request->send(200, "application/json", payload);
  });

  // Kicks off a true full-card walk (scanSDCardUsage)
  server.on("/api/sd-scan", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }

    // dont walk the whole card while indexing, two heavy SD tasks under low heap was
    // the heap corruption cause. indexing refreshes totals on its own, so just wait
    if (indexingInProgress || indexingTasksActive || requestIndexing) {
      request->send(409, "application/json",
        "{\"error\":\"Indexing in progress - try the scan again once it finishes\"}");
      webLog("[API] SD scan refused - indexing in progress", "warning");
      return;
    }
    if (sdScanInProgress) {
      request->send(409, "application/json", "{\"error\":\"A full scan is already running\"}");
      return;
    }

    const unsigned long minScanIntervalMs = 15UL * 60UL * 1000UL;
    if (lastScanTime != 0 && (millis() - lastScanTime) < minScanIntervalMs) {
      unsigned long waitMs = minScanIntervalMs - (millis() - lastScanTime);
      StaticJsonDocument<160> doc;
      doc["status"] = "cooldown";
      doc["message"] = "A full scan ran recently. Try again later.";
      doc["retryAfterMs"] = waitMs;
      String payload;
      serializeJson(doc, payload);
      request->send(429, "application/json", payload);
      return;
    }

    webLog("[API] Full SD card scan requested - this may take 10-20 minutes", "warning");

    StaticJsonDocument<64> ackDoc;
    ackDoc["status"] = "accepted";
    String ackPayload;
    serializeJson(ackDoc, ackPayload);
    request->send(202, "application/json", ackPayload);

    // stack lives in PSRAM, so the full size doesn't depend on free internal heap.
    // sdScanTask still logs its real high-water mark on exit.
    BaseType_t ok = nomadCreateTask(sdScanTask, "ManualSDScan", 20 * 1024, NULL, 1, NULL, 1);

    if (ok != pdPASS) {
      webLog("[API] SD scan task creation failed", "error");
    }
  });

  // GET /api/books -> List all CBZ/CBR files in /Books directory recursively

  // POST /api/tasks?action=restart -> restart background tasks
  server.on("/api/tasks", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    String action = "";
    if(request->hasParam("action", true)) action = request->getParam("action", true)->value();
    else if(request->hasParam("action")) action = request->getParam("action")->value();

    if (action == "restart") {
      Serial.println("[TaskMgr] Manual restart of background tasks requested");
      mediaStreamingActive = false;
      startBackgroundTasksIfNeeded();
      request->send(200, "application/json", "{\"status\":\"restarted\",\"message\":\"Background tasks restarted\"}");
    } else if (action == "shutdown") {
      Serial.println("[TaskMgr] Manual shutdown of background tasks requested");
      shutdownBackgroundTasksForStreaming();
      request->send(200, "application/json", "{\"status\":\"shutdown\",\"message\":\"Background tasks shut down\"}");
    } else if (action == "status") {
      String status = shutdownBackgroundTasks ? "shutdown" : "running";
      String streaming = mediaStreamingActive ? "true" : "false";
      String indexing = indexingTasksActive ? "true" : "false";
      request->send(200, "application/json",
        "{\"status\":\"" + status + "\",\"streaming\":\"" + streaming + "\",\"indexing\":\"" + indexing + "\"}");
    } else {
      request->send(400, "application/json", "{\"error\":\"Invalid action. Use restart, shutdown, or status\"}");
    }
  });

  // GET /api/health -> one cheap "is this thing all right" for the admin page and for
  // debugging from a phone. Every field is an existing global, nothing here touches the
  // card. Free space comes from cachedUsedBytes, NEVER the free-cluster count, which can
  // hold sdMutex for tens of seconds on a full card.
  server.on("/api/health", HTTP_GET, [](AsyncWebServerRequest *request){
    const bool cardOk   = !sdErrorFlag;
    const bool indexing = indexingInProgress;
    const bool scanning = sdScanInProgress;
    // This board has no on-device thumbnailer (covers are made on a PC), so never busy
    // with thumbnails.
    const bool thumbing = false;
    const bool busy     = indexing || scanning || thumbing;

    String json = "{";
    // Settings always live on the card on this board, so sdErrorFlag covers whether
    // they can be saved.
    json += "\"ok\":" + String(cardOk ? "true" : "false") + ",";
    json += "\"uptimeMs\":" + String((unsigned long)millis()) + ",";
    json += "\"card\":{";
    json += "\"ok\":" + String(cardOk ? "true" : "false") + ",";
    json += "\"usedBytes\":" + String((unsigned long long)cachedUsedBytes) + ",";
    json += "\"totalBytes\":" + String((unsigned long long)cachedTotalBytes);
    json += "},";
    // settingsBacked false means the config can't be written. The device is running on
    // defaults and will forget everything on the next boot, even though it looks fine.
    json += "\"settingsBacked\":" + String(cardOk ? "true" : "false") + ",";   // the card is the only backing store on this board
    json += "\"busy\":" + String(busy ? "true" : "false") + ",";
    json += "\"indexing\":" + String(indexing ? "true" : "false") + ",";
    json += "\"scanning\":" + String(scanning ? "true" : "false") + ",";
    json += "\"optimising\":" + String(thumbing ? "true" : "false") + ",";
    json += "\"mediaInFlight\":" + String((int)inFlightMediaResponses) + ",";
    json += "\"mediaSlots\":" + String((int)MEDIA_SLOTS) + ",";
    json += "\"clients\":" + String(getConnectedUserCount()) + ",";
    json += "\"heapFree\":" + String((unsigned)ESP.getFreeHeap()) + ",";
    json += "\"dmaFree\":" + String((unsigned)streamDmaFree());
    json += "}";

    AsyncWebServerResponse *r = request->beginResponse(200, "application/json", json);
    r->addHeader("Cache-Control", "no-store");
    request->send(r);
  });

  // GET /api/performance -> get performance and resource metrics
  server.on("/api/performance", HTTP_GET, [](AsyncWebServerRequest *request){
    String status = shutdownBackgroundTasks ? "optimized" : "normal";
    String streaming = mediaStreamingActive ? "true" : "false";
    String indexing = indexingTasksActive ? "true" : "false";

    // Get memory info
    size_t freeHeap = ESP.getFreeHeap();
    size_t totalHeap = ESP.getHeapSize();
    size_t usedHeap = totalHeap - freeHeap;
    float heapUsage = (float)usedHeap / (float)totalHeap * 100.0f;

    String json = "{";
    json += "\"mode\":\"" + status + "\",";
    json += "\"streaming\":" + streaming + ",";
    json += "\"indexing\":" + indexing + ",";
    json += "\"heap\":{";
    json += "\"free\":" + String(freeHeap) + ",";
    json += "\"total\":" + String(totalHeap) + ",";
    json += "\"used\":" + String(usedHeap) + ",";
    json += "\"usage\":" + String(heapUsage, 1);
    json += "},";
    // DMA-capable free is what the card actually needs. Plain free heap can look fine
    // while sdmmc is already failing to get a bounce buffer.
    json += "\"dmaFree\":" + String((unsigned)streamDmaFree()) + ",";
    json += "\"mediaInFlight\":" + String((int)inFlightMediaResponses) + ",";
    json += "\"mediaSlots\":" + String((int)MEDIA_SLOTS) + ",";
    json += "\"dmaFloor\":" + String((unsigned)STREAM_DMA_FLOOR) + ",";
    json += "\"tasks\":{";
    json += "\"indexWorker\":" + String(indexWorkerTaskHandle != nullptr ? "true" : "false") + ",";
    json += "\"storageMonitor\":" + String(storageMonitorTaskHandle != nullptr ? "true" : "false");
    json += "}";
    json += "}";

    request->send(200, "application/json", json);
  });
  // Entering ROM download mode is done in loop(), not here. This runs on async_tcp,
  // which mustn't touch LVGL, and a plain esp_restart() often skips the force-download
  // flag and boots normally.
  server.on("/flash-mode", HTTP_POST, [](AsyncWebServerRequest *request){
      if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
      Serial.println(">>> /flash-mode handler hit");
      request->send(200, "text/plain", "OK: entering ROM download (flash) mode...");
      g_enterFlashMode = true;
    });
  server.on("/enterUsb", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    Serial.println(">>> /enterUsb handler hit");
    request->send(200, "text/plain", "OK: entering USB MSC mode...");
    delay(200);                 
    set_boot_mode(USB_MODE);
    ESP.restart();             
  });


// USB mode switch: jump to USB MSC on Boot button press
attachInterrupt(BOOT_BUTTON_PIN, [](){
  bootButtonPressed = true;
}, FALLING);
// Path rules for every request, routes and catch-all alike (NomadGuardWeb.h)
  installNomadGuard(server, NomadGuardHooks{
    [](AsyncWebServerRequest *request) { return checkAdminAuth(request); },
    // the upload switches bind everyone but a signed-in admin (where there is a password:
    // without one there is no telling the owner from a guest, so they bind the owner too)
    [](AsyncWebServerRequest *request, const String &path) {
      return guestWritesOffFor(path) && !(isAdminAuthRequired() && checkAdminAuth(request));
    },
    []() { return g_staMode; },
  });
// Start the web server
#if NOMAD_BENCH
  nomadBenchRegister(server);
#endif
  nomadUpdateRegister(server);
  nomadHealthRegister(server);
  nomadActRegister(server);
  nomadStorageHealthRegister(server);
  nomadFirmwareRegister(server);
  nomadSupportRegister(server);
  nomadPartyRegister(server);
  nomadUsageRegister(server);
#ifndef NOMAD_NO_ZIMSERVE
  nomadZimRegister(server);
#endif
  nomadChatRegister(server);
#if NOMAD_ROUTES_PSRAM
  heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);   // back to the core's own rule
#endif
  server.begin();
  nomadFirmwareServerUp();
  lv_textarea_set_text(ui_MediaGen, "");
  lv_obj_add_flag(ui_MediaGen, LV_OBJ_FLAG_HIDDEN);
  if (ui_Spinner1) lv_obj_add_flag(ui_Spinner1, LV_OBJ_FLAG_HIDDEN); // boot complete, stop the perpetual redraw
  lv_timer_handler();
  updateToggleStatus(); // Reflect initial WiFi and SD status
  webLog("[SYSTEM] Web server started - ready to accept connections", "success");
  {
    BaseType_t t;

    t = xTaskCreatePinnedToCore(+[](void *param){
      (void)param;
      for (;;) {
        {
          LvglLock lock;
          if (lcdRotatePending) {
            lcdRotatePending = false;
            LCD_SetRotation180(settings.flipScreen);
            lv_obj_invalidate(lv_scr_act());  // panel RAM shows mirrored until fully repainted
          }
          Timer_Loop();
        }

        // RGB updates are time-sensitive for visual smoothness
        if (currentLEDMode == 1) {
          RGB_Lamp_Loop(2);
        }


        vTaskDelay(pdMS_TO_TICKS(10)); // ~100Hz servicing, yields CPU to other tasks
      }
      vTaskDelete(NULL);
    }, "StreamingTask", 8 * 1024, NULL, 2, NULL, 1);
    if (t == pdPASS) {
      g_lvglPumpTaskUp = true;
      webLog("[SYSTEM] Streaming task started", "success");
    } else {
      webLog("[SYSTEM] Failed to start streaming task", "error");
    }

    // UI / background task: lower frequency, handles status updates, temp, SDBAR, client count
    t = xTaskCreatePinnedToCore(+[](void *param){
      (void)param;
      uint32_t lastUpdateTimeLocal = 0;
      uint32_t lastTempReadingLocal = 0;
      uint32_t lastSdbarUpdateLocal = 0;
      for (;;) {
        uint32_t now = millis();

        if (now - lastUpdateTimeLocal > 2000) { // every 2s
          updateToggleStatus();
          lastUpdateTimeLocal = now;
        }

        if (now - lastTempReadingLocal > 12000) { // every 12s
          currentTempC = temperatureRead();
          lastTempReadingLocal = now;
        }

        if (sdbarDirty && (now - lastSdbarUpdateLocal > 500)) { // every 500ms
          updateSDBAR_UI_ThreadOnly();
          lastSdbarUpdateLocal = now;
        }

        lvglDrainQueue();

        updateClientCount();

        // Sleep a bit longer to reduce CPU usage of background tasks
        vTaskDelay(pdMS_TO_TICKS(200));
      }
      vTaskDelete(NULL);
    }, "UiTask", 8 * 1024, NULL, 1, NULL, 1); // 6KB hit a stack canary crash at boot
    if (t == pdPASS) {
      webLog("[SYSTEM] UI/background task started", "success");
    } else {
      webLog("[SYSTEM] Failed to start UI/background task", "warning");
    }
  }

  // Now that the network, UI and server are up, spawn IndexWorker so it can run
  // heavy indexing in background without blocking boot.
  static bool indexWorkerStarted = false;
  if (!indexWorkerStarted && indexQueue) {
    BaseType_t r = nomadCreateTask(indexWorkerTask, "IndexWorker", 16 * 1024, NULL, 2, &indexWorkerTaskHandle, 0);
    if (r == pdPASS) {
      Serial.println("[IndexWorker] Task started");
      webLog("[SYSTEM] Index worker task started successfully", "success");
      indexWorkerStarted = true;
    } else {
      Serial.println("[ERROR] Failed to create IndexWorker task");
      webLog("[SYSTEM] Failed to start index worker task", "error");
    }
  }

  webLog("[SYSTEM] System initialization complete - ready for use", "success");
  nomadSafeLog();
  // re-print reset reason here since USB-CDC reconnects after the early print and misses it.
  // 4=PANIC 5=INT_WDT 6=TASK_WDT 9=BROWNOUT 3=SW
  {
    esp_reset_reason_t rr = esp_reset_reason();
    const char* n = (rr==ESP_RST_POWERON)?"POWERON":(rr==ESP_RST_SW)?"SW/restart":
                    (rr==ESP_RST_PANIC)?"PANIC":(rr==ESP_RST_INT_WDT)?"INT_WDT":
                    (rr==ESP_RST_TASK_WDT)?"TASK_WDT":(rr==ESP_RST_BROWNOUT)?"BROWNOUT":
                    (rr==ESP_RST_WDT)?"OTHER_WDT":"other";
    Serial.printf("[DIAG] LAST reset_reason=%d (%s)  freeHeap=%u  minEverHeap=%u\n",
                  (int)rr, n, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());

    esp_core_dump_summary_t *cdSum = (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));
    if (cdSum && esp_core_dump_get_summary(cdSum) == ESP_OK) {
      Serial.printf("[DIAG] COREDUMP task=%s  PC=0x%08x  faultAddr=0x%08x  depth=%u corrupted=%d\n",
                    cdSum->exc_task, (unsigned)cdSum->exc_pc,
                    (unsigned)cdSum->ex_info.exc_vaddr,
                    (unsigned)cdSum->exc_bt_info.depth, (int)cdSum->exc_bt_info.corrupted);
      Serial.print("[DIAG] COREDUMP backtrace:");
      for (uint32_t cdI = 0; cdI < cdSum->exc_bt_info.depth && cdI < 16; cdI++) {
        Serial.printf(" 0x%08x", (unsigned)cdSum->exc_bt_info.bt[cdI]);
      }
      Serial.println();
    } else {
      Serial.println("[DIAG] no core dump stored (or no coredump partition in the flashed scheme)");
    }
    if (cdSum) free(cdSum);
  }

  // Spawn bootCoordinatorTask so it can schedule boot-time scans without blocking setup.
  BaseType_t br = nomadCreateTask(bootCoordinatorTask, "BootCoord", 8 * 1024, NULL, 1, NULL, 1);
  if (br != pdPASS) {
    Serial.println("[BootCoord] Failed to spawn Boot Coordinator");
  } else {
    Serial.println("[BootCoord] Task spawned");
  }
}
// main loop

void loop() {
    nomadFirmwareTick();
    nomadSafeTick();
    nomadUsageTick();
    if (g_enterFlashMode) {
      g_enterFlashMode = false;
      // paint the notice on the screen already up, no re-init. the ST7789 keeps its
      // framebuffer through the reset. held under the LVGL lock like every other
      // post-setup lv_* call
      {
        LvglLock lock;
        lv_obj_clear_flag(ui_MediaGen, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_set_text(ui_MediaGen, "Flashing Mode, Ready for Update");
        for (int i = 0; i < 6; ++i) { lv_timer_handler(); delay(30); }
      }
      delay(150);   // let the HTTP response finish leaving the radio
      // The core's own download-mode path (same one the 1200-baud touch uses): registers a
      // shutdown handler so RTC_CNTL_FORCE_DOWNLOAD_BOOT is written last, and switches the
      // USB PHY back to the hardware JTAG/serial unit so the ROM enumerates for esptool.
      usb_persist_restart(RESTART_BOOTLOADER);
      // only reached if the shutdown-handler slot was full: set the flag by
      // hand and take a CPU-only reset, which cannot clear it on the way down
      REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
      SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_PROCPU_RST);
    }
    if (bootButtonPressed) {
      bootButtonPressed = false;
      set_boot_mode(USB_MODE);
      ESP.restart();
    }
    // deferred restart after a WiFi Mode change: gives the config writer time to
    // flush settings.json and the HTTP response time to leave before rebooting
    if (g_wifiModeRestartAt && (int32_t)(millis() - g_wifiModeRestartAt) > 0) {
      g_wifiModeRestartAt = 0;
      Serial.println("[WiFiMode] Applying WiFi mode change -> restart");
      ESP.restart();
    }
    // StreamingTask pumps LVGL and the rainbow at 100 Hz. loop() only steps in if that
    // task never started, two tasks in lv_timer_handler() at once is trouble.
    if (!g_lvglPumpTaskUp) {
      LvglLock lock;
      Timer_Loop();
      if (currentLEDMode == 1) RGB_Lamp_Loop(2);
    }
    if (sdErrorFlag) {
        if (millis() > sdErrorCooldownUntil && tryRecoverSDCard()) {
            sdErrorFlag = false;  // we’re back in business
        } else {
            delay(5);  // keep feeding WDT while waiting
            return;    // skip rest of loop until recovery works
        }
    }
    // Check for streaming timeout to restart background tasks
    if (millis() - lastUpdateTime > 2000) {
        lastUpdateTime = millis();
        checkStreamingTimeout();
    }

    dlnaTick();  // periodic SSDP alive announcements, cheap no-op when off
    nomadReapTasks();  // free background tasks that have finished (their stacks are in PSRAM)
    nomadChatTick();   // new chat messages onto the card, once a second
    nomadMenuTick();   // the menu's optional tiles, looked at again every 30 s

    delay(10);
}


void RGB_SetMode(uint8_t mode) {
    currentLEDMode = mode;

    if (mode == 0) {
        // Turn off LED immediately
        Set_Color(0, 0, 0);
    } else if (mode == 2) {
        Set_Color(solidG, solidR, solidB);
    }
}
