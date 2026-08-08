// Jcorp Nomad Backend
//<!-- Version 4.2.2 -->
#include <Arduino.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_heap_caps.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <map>
#include <time.h>
#include "board_config.h"
#include "Display_Driver.h"
#include "LVGL_Driver.h"
#include "ui.h" 
#include "nomad_ui.h"
#include "nomad_hw.h"
#include "RGB_lamp.h"
#include <SPIFFS.h>
#include <Preferences.h>
#include "esp_wifi.h"
#if defined(ARDUINO_ARCH_ESP32)
  #include "soc/soc.h"
  #include "soc/rtc_cntl_reg.h"
  #include "esp_system.h"
  #include "esp_core_dump.h"
#endif
#include "usb_mode.h"
#include "boot_mode.h" // library for firmware switching

struct BreakdownStats {
  uint64_t bytes = 0;
  uint32_t files = 0;
  uint32_t dirs = 0;
};

void handleRangeRequest(AsyncWebServerRequest *request);
String urlDecode(const String& str);
void enqueueIndexUpdateForPath(const String& path);
void RGB_SetMode(uint8_t mode);
void applyRGBSettings();
String sanitizeToken(const String &s);
bool saveSettings();
String humanSize(size_t bytes);
void launch_usb_mode() {
extern void usb_setup();
extern void usb_loop();
  usb_setup();

  for (;;) {
    usb_loop();
  }
}
// BOOT_BUTTON_PIN comes from board_config.h
#include <vector>
#include <algorithm>
#include <atomic>
#ifndef GLOBAL_INDEX_BUF
#define GLOBAL_INDEX_BUF 1024
#endif
enum { HALF_INDEX_BUF = GLOBAL_INDEX_BUF / 2 };

static char g_lineBuf[GLOBAL_INDEX_BUF];
static uint8_t g_fileBuf[4096];                 // file reads/writes
static std::map<String, unsigned long> g_lastIndexSkipLog;
static inline void freeString(String &s) { s = String(); }
static inline void freeVectorString(std::vector<String> &v) { std::vector<String>().swap(v); }
static inline void freeVectorUInt32(std::vector<uint32_t> &v) { std::vector<uint32_t>().swap(v); }
static inline void closeFile(File &f) { if (f) f.close(); }

struct LvglMsg {
  char text[80];
  bool show;
};
static QueueHandle_t lvglQueue = NULL;

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
  if (!Lvgl_Lock(20)) return;   // UI task mid-update; drain on the next pass
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
  Lvgl_Unlock();
}

static volatile bool bootButtonPressed = false;
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

static void writeIndexEntryToFile(File &fout, char t, const String &name, const String &path, uint64_t sz = 0, uint64_t mt = 0) {
  char escName[HALF_INDEX_BUF];
  char escPath[HALF_INDEX_BUF];
  jsonEscapeToBuf(name, escName, HALF_INDEX_BUF);
  jsonEscapeToBuf(path, escPath, HALF_INDEX_BUF);

  int pos = snprintf(g_lineBuf, GLOBAL_INDEX_BUF,
                     "{\"t\":\"%c\",\"n\":\"%s\",\"p\":\"%s\"", t, escName, escPath);
  if (pos < 0) pos = 0;
  if (t == 'f') {
    pos += snprintf(g_lineBuf + pos, (pos < (int)GLOBAL_INDEX_BUF) ? (GLOBAL_INDEX_BUF - pos) : 0,
                    ",\"sz\":%llu,\"mt\":%llu}\n",
                    (unsigned long long)sz, (unsigned long long)mt);
  } else {
    pos += snprintf(g_lineBuf + pos, (pos < (int)GLOBAL_INDEX_BUF) ? (GLOBAL_INDEX_BUF - pos) : 0,
                    "}\n");
  }

  size_t wlen = strlen(g_lineBuf);
  if (wlen) fout.write((const uint8_t*)g_lineBuf, wlen);
}
int screenBrightness = 100; // 0-100, default full brightness
void handleConnector(AsyncWebServerRequest *request);
unsigned long lastTempReading = 0;
float currentTempC = 0.0;
volatile bool mediaStreamingActive = false; // Flag to indicate active media streaming
volatile unsigned long lastStreamIoMs = 0;  // last time a streaming request/fill touched the SD
static bool sdScanned = false;
const uint32_t SD_SCAN_DELAY = 5000;  // milliseconds after boot
SemaphoreHandle_t sdMutex = NULL;
#include <map>
static uint32_t nextStreamId = 1;
struct StreamHandle {
  File file;
  String path;
  unsigned long lastActivity;
  size_t lastEndByte;
};
static std::map<uint32_t, StreamHandle> streamingFiles;
static std::map<String, uint32_t> streamPathIndex;
static SemaphoreHandle_t streamingFilesMutex = NULL;
static const int MAX_CONCURRENT_STREAMS = 8;

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

// Task management for performance optimization
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

// ---- Multiplayer room store (HTTP polling, no WebSockets) ----
// Fixed-size, plain-char-buffer, no Arduino String anywhere in this store or its
// handlers, and NO SD I/O -- this is the same class of hazard that previously
// caused heap corruption via unsynchronized Strings shared across tasks
// (see AGENTS.md / webLogMutex history). Everything here is guarded by gameMutex.
#define MP_MAX_ROOMS 4
#define MP_CODE_LEN 5      // 4-char room code + NUL
#define MP_GAME_LEN 16     // e.g. "tictactoe", "chess"
#define MP_TOKEN_LEN 9     // 8 hex chars + NUL
#define MP_STATE_LEN 512   // opaque client-owned state blob (chess FEN ~90B, TTT ~32B)
#define MP_ROOM_IDLE_MS (2UL * 60UL * 1000UL)  // idle rooms are reclaimed after ~2 min

struct MpRoom {
  bool active;
  char code[MP_CODE_LEN];
  char game[MP_GAME_LEN];
  char token[2][MP_TOKEN_LEN];  // seat 0/1 tokens; empty string = seat open
  char state[MP_STATE_LEN];
  uint32_t seq;
  unsigned long lastMs;
};
static MpRoom mpRooms[MP_MAX_ROOMS];
SemaphoreHandle_t gameMutex = NULL;

// Function declarations for task management
void shutdownBackgroundTasksForStreaming();
void handleBootButton();
void refreshDeviceStats();
void startBackgroundTasksIfNeeded();
void checkStreamingTimeout();
void immediateEnqueueTopLevelTask(void *param);
void triggerIndexingIfNeeded(const String& filePath);
// Last time UI bar was updated
unsigned long lastSdbarUpdate = 0;
void updateSDBAR() {
  sdbarDirty = true;
}
void updateSDBAR_UI_ThreadOnly() {
  if (cachedTotalBytes == 0) return;
  
  int usage = (int)((cachedUsedBytes * 100) / cachedTotalBytes);
  if (usage > 100) usage = 100;
  if (usage < 0) usage = 0;
  
  NomadUI_SetSdUsage(usage, cachedUsedBytes, cachedTotalBytes);
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

// The filesystem object: SD_MMC on the 4-bit boards, SD on the SPI-card
// dongle. nomad_sd.h picks one and calls it NomadSD.
#include "nomad_sd.h"
#define INDEXER_SLEEP_MS 300000 // 5 minutes between background scans
#define MAX_CLIENTS 8 // SoftAP max_connection; keep in sync with WiFi.softAP() calls below
String encodeIndexName(const String &path);

struct AdminSettings {
  String rgbMode = "off";
  String rgbColor = "#ff0000";
  String adminPassword = "";
  String wifiSSID = "Jcorp_Nomad";
  String wifiPassword = "password";
  int brightness = 100;            // percent 0-100 (Set_Backlight range); 230 was out-of-range and ignored
  bool autoGenerateMedia = true;   // check for new files on boot (default on)
  bool flipScreen = false;         // rotate LCD 180 deg (USB port upside down, e.g. car mounts)
};

AdminSettings settings;
// Set by the /settings handler (async_tcp task); consumed by the task that owns
// LVGL flushes. MADCTL must never be written mid-flush from another task.
volatile bool lcdRotatePending = false;
const char* SETTINGS_PATH = "/config/settings.json";

// --- Admin session auth ---
// Not perfect, but designed to make it a bit more secure and properly gated, unlikly this will ever be needed, but some users want it as an option. 
String adminSessionToken = "";

String generateSessionToken() {
  char buf[33];
  for (int i = 0; i < 4; i++) {
    snprintf(buf + (i * 8), 9, "%08x", (unsigned)esp_random());
  }
  buf[32] = '\0';
  return String(buf);
}

bool isAdminAuthRequired() {
  return settings.adminPassword.length() > 0;
}

// Returns true if the request is allowed to perform an admin/state-changing action.
bool checkAdminAuth(AsyncWebServerRequest *request) {
  if (!isAdminAuthRequired()) return true; // admin password explicitly disabled
  if (adminSessionToken.length() == 0) return false; // nobody has logged in since boot
  if (!request->hasHeader("X-Admin-Token")) return false;
  return request->getHeader("X-Admin-Token")->value().equals(adminSessionToken);
}

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
// guards webLogs[] - lots of tasks write it and /console-logs reads it, unsynced
// String read/writes race the heap and panic. made in setup() before the server,
// so early-boot logs (NULL mutex) skip locking safely.
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
// SD_CLK_PIN / SD_CMD_PIN / SD_D0..D3_PIN come from board_config.h
const char *INDEX_DIR = "/.system-index";   // on-SD folder for index files
const size_t INDEX_WRITE_CHUNK = 4096;      // flush buffer when larger than this
// Normalize path: ensure leading '/', remove trailing '/'
String normalizePath(const String &p_in){
  if (p_in.length() == 0) return "/";
  String p = p_in;
  if (!p.startsWith("/")) p = "/" + p;
  while (p.length() > 1 && p.endsWith("/")) p = p.substring(0, p.length()-1);
  return p;
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

// Append one NDJSON entry (t: 'f'|'d', n: filename, p: absolute path, optional sz/mt)
String buildIndexEntry(const char t, const String &name, const String &path, uint64_t sz=0, uint64_t mt=0){
  String line;
  line.reserve(160);
  line += "{\"t\":\"";
  line += t;
  line += "\",\"n\":\"";
  line += jsonEscape(name);
  line += "\",\"p\":\"";
  line += jsonEscape(path);
  if (t == 'f') {
    line += "\",\"sz\":";
    line += String((unsigned long long)sz);
    line += ",\"mt\":";
    line += String((unsigned long long)mt);
    line += "}\n";
  } else {
    line += "\"}\n";
  }
  return line;
}

#define MAX_NESTED_AUTOGEN_ITEMS 40
bool readIndexHeaderSig(const String &indexPath, String &outSig, uint32_t &outCount) {
  outSig = "";
  outCount = 0;
  if (!NomadSD.exists(indexPath)) return false;
  File f = NomadSD.open(indexPath, FILE_READ);
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
// ---------- Media file detector + safe recursive counter ----------
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
static bool isComicFolder(const String &dirPath) {
  File d = NomadSD.open(dirPath);
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
unsigned int countMediaFiles(const String &dirPath) {
  unsigned int count = 0;

  File d = NomadSD.open(dirPath);
  if (!d || !d.isDirectory()) {
    if (d) d.close();
    return 0;
  }

  d.rewindDirectory();
  File e;
  while ((e = d.openNextFile())) {
    if (e.isDirectory()) {
      // e.name() returns the full path; recurse on it
      String sub = String(e.name());
      count += countMediaFiles(sub);
    } else {
      // Lower-case filename once for efficient extension checks
      String name = String(e.name());
      name.toLowerCase();

      if (isMediaFile(name)) {
        ++count;
      }
    }
    e.close();
    yield(); // keep watchdog happy during recursion/long scans
  }

  d.close();
  return count;
}

// compatibility wrapper, some callers expect countDirItems()
unsigned int countDirItems(const String &p) {
  return countMediaFiles(p);
}

// Ensure INDEX_DIR exists (creates it if missing.. usualy)
void ensureIndexDir(){
  if (!NomadSD.exists(INDEX_DIR)) {
    if (!NomadSD.mkdir(INDEX_DIR)) {
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
  if (NomadSD.exists(dst)) NomadSD.remove(dst);
  if (NomadSD.rename(src, dst)) return true;

  File fsrc = NomadSD.open(src, FILE_READ);
  if (!fsrc) return false;
  File fdst = NomadSD.open(dst, FILE_WRITE);
  if (!fdst) { fsrc.close(); return false; }

  uint8_t buf[512];
  while (fsrc.available()) {
    size_t r = fsrc.read(buf, sizeof(buf));
    if (r > 0) fdst.write(buf, r);
  }
  fsrc.close();
  fdst.close();
  NomadSD.remove(src);
  return true;
}

// Atomic write helper - writes tmp then moves to final (uses renameOrCopy)
bool atomicWriteFile(const String &tmpPath, const String &finalPath) {
  // final renameOrCopy already does the heavy lifting; here just ensure final exists
  return renameOrCopy(tmpPath, finalPath);
}
void dumpSDRoot() {
  Serial.println("[Index] dumpSDRoot(): listing /");
  File r = NomadSD.open("/");
  if (!r) {
    Serial.println("[Index] dumpSDRoot(): FAILED to open root '/'");
    return;
  }
  r.rewindDirectory();
  while (true) {
    File e = r.openNextFile();
    if (!e) break;
    Serial.printf("[Index] root-entry: %s %s\n", e.name(), e.isDirectory() ? "(dir)" : "(file)");
  }
  r.close();
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
  if (!NomadSD.exists(INDEX_DIR)) NomadSD.mkdir(INDEX_DIR);

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
  if (!NomadSD.exists(normPath)) {
    Serial.printf("[Index] Path does not exist: %s\n", normPath.c_str());
    if (sdMutex && mutexHeld) xSemaphoreGive(sdMutex);
    return false;
  }

  // Open once to verify directory
  File root = NomadSD.open(normPath);
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

    File d = NomadSD.open(path);
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

  File fout = NomadSD.open(tmpPath, FILE_WRITE);
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

    File d = NomadSD.open(path);
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
      NomadSD.remove(tmpPath);
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
    uint64_t fsz = (uint64_t)e.size();
    uint64_t fmt = 0;
    int pos = snprintf(g_lineBuf, GLOBAL_INDEX_BUF,
    "{\"t\":\"f\",\"n\":\"%s\",\"p\":\"%s\",\"sz\":%llu,\"mt\":%llu}\n",
    escName, escPath, (unsigned long long)fsz, (unsigned long long)fmt);
    if (pos < 0) pos = 0;
    size_t wlen = strlen(g_lineBuf);
    if (wlen) fout.write((const uint8_t*)g_lineBuf, wlen);
    } else {
    bool isComic = full.startsWith("/Books/") && isComicFolder(full);
    int pos = snprintf(g_lineBuf, GLOBAL_INDEX_BUF,
    "{\"t\":\"d\",\"n\":\"%s\",\"p\":\"%s\"%s}\n",
    escName, escPath, isComic ? ",\"comic\":true" : "");
    if (pos < 0) pos = 0;
    size_t wlen = strlen(g_lineBuf);
    if (wlen) fout.write((const uint8_t*)g_lineBuf, wlen);
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
        NomadSD.remove(tmpPath);
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
  if (NomadSD.exists(newFinal)) NomadSD.remove(newFinal);

  bool moved = NomadSD.rename(tmpPath, newFinal);
  if (!moved) {
    File fsrc = NomadSD.open(tmpPath, FILE_READ);
    if (fsrc) {
    File fdst = NomadSD.open(newFinal, FILE_WRITE);
    if (fdst) {
    uint8_t buf[512];
    while (fsrc.available()) {
    size_t r = fsrc.read(buf, sizeof(buf));
    if (r > 0) fdst.write(buf, r);
    }
    fsrc.close();
    fdst.close();
    NomadSD.remove(tmpPath);
    moved = true;
    } else {
    fsrc.close();
    }
    }
  }

  if (!moved) {
    Serial.printf("[Index] FAILED staging -> %s from tmp %s\n", newFinal.c_str(), tmpPath.c_str());
    NomadSD.remove(tmpPath);
    if (sdMutex && mutexHeld) xSemaphoreGive(sdMutex);
    return false;
  }

  if (NomadSD.exists(finalPath)) NomadSD.remove(finalPath);

  if (!NomadSD.rename(newFinal, finalPath)) {
    if (!renameOrCopy(newFinal, finalPath)) {
    Serial.printf("[Index] FAILED atomic replace %s -> %s\n", newFinal.c_str(), finalPath.c_str());
    NomadSD.remove(newFinal);
    webLogf("error", "Failed atomic replace %s -> %s", newFinal.c_str(), finalPath.c_str());
    if (sdMutex && mutexHeld) xSemaphoreGive(sdMutex);
    return false;
    } else {
    NomadSD.remove(newFinal);
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

    File metaFile = NomadSD.open(metaPath, FILE_WRITE);
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

// ---------------- write bucket-level index ----------------
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

  if (!NomadSD.exists(SETTINGS_PATH)) {
    Serial.println("Settings file not found. Generating default.");
    if (sdMutex) xSemaphoreGive(sdMutex);
    return saveSettings();  // Save defaults
  }

  File file = NomadSD.open(SETTINGS_PATH);
  if (!file || file.isDirectory()) {
    Serial.println("Failed to open settings file.");
    if (sdMutex) xSemaphoreGive(sdMutex);
    return false;
  }

  StaticJsonDocument<512> doc;
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

  return true;
}
bool saveSettings() {
  NomadSD.mkdir("/config"); // Ensure directory exists

  File file = NomadSD.open(SETTINGS_PATH, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open settings file for writing.");
    return false;
  }

  StaticJsonDocument<512> doc;
  doc["rgbMode"] = settings.rgbMode;
  doc["rgbColor"] = settings.rgbColor;
  doc["adminPassword"] = settings.adminPassword;
  doc["wifiSSID"] = settings.wifiSSID;
  doc["wifiPassword"] = settings.wifiPassword;
  doc["brightness"] = settings.brightness;
  doc["autoGenerateMedia"] = settings.autoGenerateMedia;
  doc["flipScreen"] = settings.flipScreen;

  bool success = serializeJson(doc, file) > 0;
  file.close();
  return success;
}

// --------------- Media Generation Stuff ------------
bool isAlwaysGenerateEnabled() {
    return NomadSD.exists("/always_generate.flag");
}

void enableAlwaysGenerate() {
    File f = NomadSD.open("/always_generate.flag", FILE_WRITE);
    if (f) {
        f.print("1");
        f.close();
    }
}

void disableAlwaysGenerate() {
    NomadSD.remove("/always_generate.flag");
}

bool isOneTimeGenerateRequested() {
    return NomadSD.exists("/generate_once.flag");
}

void requestOneTimeGenerate() {
    File f = NomadSD.open("/generate_once.flag", FILE_WRITE);
    if (f) {
        f.print("1");
        f.close();
    }
}

void clearOneTimeGenerate() {
    NomadSD.remove("/generate_once.flag");
}
//------------------- delete recursive -------------
bool deleteRecursive(String path) {
  File entry = NomadSD.open(path);
  if (!entry) return false;

  if (!entry.isDirectory()) {
    entry.close();
    return NomadSD.remove(path);
  }

  File child;
  while ((child = entry.openNextFile())) {
    String childPath = String(path) + "/" + child.name();
    deleteRecursive(childPath);
    child.close();
  }

  entry.close();
  return NomadSD.rmdir(path);
}


// ───────────────── SD‑recovery globals ───────────────
volatile bool sdErrorFlag            = false;      
unsigned long sdErrorCooldownUntil   = 0;          

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

        NomadSD.end();          // unmount
        delay(1000);           // give hardware a breather
        bool ok = NomadSD_Mount(12).mounted;

        if (sdHeld && sdMutex) xSemaphoreGive(sdMutex);
        xSemaphoreGive(streamingFilesMutex);
        Serial.println(ok ? "[SD] Recovery OK." : "[SD] Recovery failed.");
        return ok;
    }

    // couldnt get the stream map (something wedged holding it), remount under sdMutex alone.
    // give back only what we took - releasing a mutex we don't hold frees it under its owner
    bool sdTaken = (!sdMutex) || (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) == pdTRUE);
    if (!sdTaken) Serial.println("[SD] Recovery: sdMutex timeout, remounting without it.");
    NomadSD.end();
    delay(1000);
    bool ok = NomadSD_Mount(12).mounted;
    if (sdTaken && sdMutex) xSemaphoreGive(sdMutex);
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

// Captive portal DNS setup
const byte DNS_PORT = 53;
DNSServer dnsServer;
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
    // Set_Color() takes plain R, G, B; the driver applies the LED's own channel
    // order. Passing (g, r, b) here swapped red and green for every solid
    // colour picked in the admin page.
    Set_Color(r, g, b);
}
extern lv_obj_t *ui_wifi;
extern lv_obj_t *ui_SDcard;
bool lastWifiStatus = false;
bool lastSDStatus = false;
//Globals for SD scan
unsigned long lastUpdateTime = 0;
volatile bool requestIndexing = false;     // set by admin endpoint; consumed by index worker
// true only when the boot change detector actually found changes and queued a reindex.
// this is the CHECK's result, not the toggle - a boot with no changes indexes nothing.
volatile bool bootReindexQueued = false;
volatile bool indexingInProgress = false;  // guard so we never run multiple index runs concurrently
volatile bool settingsReady = false;       // set to true after loadSettings() runs

// New scan/index coordination flags
volatile bool sdScanInProgress = false;    // true while SD scan is performing its initial pass
volatile bool sdScanCompleted = false;     // set to true after the initial SD scan completes

volatile bool bootIndexAllowed = true;   

unsigned long lastSDScanTime = 0;
const unsigned long SD_SCAN_INTERVAL = 60000; // 60 seconds

// ---------- background indexing control ----------

// Update the UI with the number of connected users
void updateUI(int userCount) {
    NomadUI_SetUsers(userCount);
}
void updateToggleStatus() {
    bool currentWifiStatus = WiFi.softAPIP();
    if (currentWifiStatus != lastWifiStatus) {
        NomadUI_SetWifiOk(currentWifiStatus);
        if (currentWifiStatus) {
            if (!lastWifiStatus) {
                webLog("[SYSTEM] WiFi AP verified successfully", "success");
            }
        } else {
            webLog("[SYSTEM] WiFi AP failure detected - attempting recovery", "error");
        }
        lastWifiStatus = currentWifiStatus;
    }
}

void updateSDStatus() {
    bool currentSDStatus = NomadSD.cardType() != CARD_NONE;
    if (currentSDStatus != lastSDStatus) {
        NomadUI_SetSdOk(currentSDStatus);
        if (currentSDStatus) {
            if (!lastSDStatus) {
                webLog("[SYSTEM] SD card verified successfully", "success");
            }
        } else {
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

// Utility: extract base name (no path, no extension)
String getFileBaseName(const String& name) {
    String base = name.substring(name.lastIndexOf('/') + 1);
    int dotIndex = base.lastIndexOf('.');
    if (dotIndex != -1) base = base.substring(0, dotIndex);
    return base;
}
bool isValidExtension(const String& filename, const std::vector<String>& exts) {
  String nameLower = filename;
  nameLower.toLowerCase();
  for (const auto& ext : exts) {
    if (nameLower.endsWith(ext)) return true;
  }
  return false;
}

void generateMediaJson(){
  webLog("[MEDIA] Starting media index generation for all buckets", "info");
  Serial.println("[Index] generateMediaJson() -> switched to NDJSON writer");
  webLogf("indexing_progress", "Starting full media index generation for all buckets");

  // ensure index dir
  ensureIndexDir();

  buildBucketIndex("/");       // writes root.index.ndjson
  webLogf("indexing_progress", "Completed indexing root bucket (/)");
  buildBucketIndex("/Shows");  // writes Shows.index.ndjson
  webLogf("indexing_progress", "Completed indexing Shows bucket");
  buildBucketIndex("/Music");  // writes Music.index.ndjson
  webLogf("indexing_progress", "Completed indexing Music bucket");
  buildBucketIndex("/Games");  // writes Games.index.ndjson (flat layout, no nested per-subfolder pass)
  webLogf("indexing_progress", "Completed indexing Games bucket");

File showsDir = NomadSD.open("/Shows");
  if(showsDir){
    while(true){
    File s = showsDir.openNextFile();
    if(!s) break;
    if(s.isDirectory()){
    String showName = String(s.name());
    String showPath = "/Shows";
    if(!showPath.endsWith("/")) showPath += "/";
    showPath += showName;
    String fileToken = "Shows__" + sanitizeToken(showName) + ".nested.ndjson";
    writeNDIndexForDir(showPath, fileToken);
    Serial.printf("[Index] wrote per-show nested %s for %s\n", fileToken.c_str(), showPath.c_str());
    }
    s.close();
    }
    showsDir.close();
  }

  // Generate nested indexes for Music subfolders (Artist/Playlist level)
  File musicDir = NomadSD.open("/Music");
  if(musicDir){
    while(true){
    File m = musicDir.openNextFile();
    if(!m) break;
    if(m.isDirectory()){
    String musicSubName = String(m.name());
    // Extract just the folder name (not full path)
    int lastSlash = musicSubName.lastIndexOf('/');
    if(lastSlash >= 0) musicSubName = musicSubName.substring(lastSlash + 1);
    
    String musicSubPath = "/Music/" + musicSubName;
    String fileToken = "Music__" + sanitizeToken(musicSubName) + ".nested.ndjson";
    writeNDIndexForDir(musicSubPath, fileToken);
    Serial.printf("[Index] wrote per-music-folder nested %s for %s\n", fileToken.c_str(), musicSubPath.c_str());
    }
    m.close();
    }
    musicDir.close();
  }

  buildBucketIndex("/Books");
  webLogf("indexing_progress", "Completed indexing Books bucket");
  
  File booksDir = NomadSD.open("/Books");
  if(booksDir){
    while(true){
      File b = booksDir.openNextFile();
      if(!b) break;
      if(b.isDirectory()){
        String bookSubName = String(b.name());
        int lastSlash = bookSubName.lastIndexOf('/');
        if(lastSlash >= 0) bookSubName = bookSubName.substring(lastSlash + 1);
        
        String bookSubPath = "/Books/" + bookSubName;
        
        if (!isComicFolder(bookSubPath)) {
          String fileToken = "Books__" + sanitizeToken(bookSubName) + ".nested.ndjson";
          writeNDIndexForDir(bookSubPath, fileToken);
          Serial.printf("[Index] wrote per-books-folder nested %s for %s\n", fileToken.c_str(), bookSubPath.c_str());
        } else {
          Serial.printf("[Index] Skipping comic folder nested index: %s\n", bookSubPath.c_str());
        }
      }
      b.close();
    }
    booksDir.close();
  }
  String summary = "{\n  \"generated\": true,\n  \"buckets\": {\n";
  // read index files to include counts
  File idx = NomadSD.open(INDEX_DIR);
  if(idx){
    while(true){
      File f = idx.openNextFile();
      if(!f) break;
      String fname = String(f.name());
      if(fname.endsWith(".index.ndjson") || fname.endsWith(".nested.ndjson")){
        // read first line header
        String header = f.readStringUntil('\n');
        // try to parse count quickly by locating '"count":' substring
        int pos = header.indexOf("\"count\":");
        String countStr = "0";
        if(pos >= 0){
          int start = pos + 8;
          int end = start;
          while(end < header.length() && isDigit(header.charAt(end))) end++;
          countStr = header.substring(start, end);
        }
        summary += "    \"" + fname + "\": " + countStr + ",\n";
      }
      f.close();
    }
    idx.close();
  }
  if(summary.endsWith(",\n")) summary = summary.substring(0, summary.length()-2) + "\n";
  summary += "  }\n}\n";

  // write summary to /media.json (small)
  File mf = NomadSD.open("/media.json", FILE_WRITE);
  if(mf){
    mf.print(summary);
    mf.close();
  } else {
    Serial.println("[Index] failed to write /media.json");
    webLogf("indexing_progress", "Full media index generation completed successfully");
  }

  webLog("[MEDIA] Media index generation completed successfully", "success");
  Serial.println("[Index] Media JSON generation complete");
}

String absURL(const String &path) {
    return "http://" + WiFi.softAPIP().toString() + path;
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
      File dir = NomadSD.open(dirPath);
      if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        return;
      }
      
      dir.rewindDirectory();
      while (true) {
        File file = dir.openNextFile();
        if (!file) break;
        
        String fullPath = String(file.name());
        String fileName = fullPath;
        int lastSlash = fileName.lastIndexOf('/');
        if (lastSlash >= 0) fileName = fileName.substring(lastSlash + 1);
        
        if (file.isDirectory()) {
          if (isComicFolder(fullPath)) {
            String base = fileName;
            String safeTitle = xmlEscape(base);
            String safeId = "urn:uuid:nomad-comic-" + slugify(base);
            
            String coverPath = "placeholder.jpg";
            File comicDir = NomadSD.open(fullPath);
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
        if (!NomadSD.exists(coverPath)) {
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

void handleRangeRequest(AsyncWebServerRequest *request) {
  if (indexingTasksActive) {
    Serial.println("[RangeHandler] Blocked: indexing in progress");
    request->send(503, "text/plain", "Indexing in progress. Please wait...");
    return;
  }

  String filePath;
  if (request->hasParam("file")) {
    filePath = request->getParam("file")->value();
  } else {
    filePath = urlDecode(request->url());
  }

  if (!filePath.startsWith("/")) filePath = "/" + filePath;
  filePath = normalizePath(filePath);

  // OPTIMIZATION: Reduce mutex timeout from 5000ms to 1000ms
  if (sdMutex) {
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
      Serial.printf("[RangeHandler] SD mutex timeout for: %s\n", filePath.c_str());
      request->send(503, "text/plain", "SD busy — retrying shortly");
      return;
    }
  }

  auto releaseSd = [&](){
    if (sdMutex) xSemaphoreGive(sdMutex);
  };

  bool fileExists = NomadSD.exists(filePath);

  if (!fileExists) {
    Serial.printf("[RangeHandler] File not found: %s\n", filePath.c_str());
    releaseSd();
    request->send(404, "text/plain", "File not found");
    return;
  }

  File file = NomadSD.open(filePath, "r");
  releaseSd(); 

  if (!file) {
    Serial.printf("[SD] open() failed for '%s'\n", filePath.c_str());
    sdErrorFlag = true;
    sdErrorCooldownUntil = millis() + 5000;
    request->send(503, "text/plain", "SD error");
    return;
  }

  size_t fileSize = file.size();

  if (request->method() == HTTP_HEAD) {
    AsyncWebServerResponse *headResponse = request->beginResponse(200, "application/octet-stream", "");
    headResponse->addHeader("Accept-Ranges", "bytes");
    headResponse->addHeader("Content-Length", String(fileSize));
    headResponse->addHeader("Cache-Control", "public, max-age=3600");
    headResponse->addHeader("Pragma", "no-cache");
    // close under sdMutex like every other SD op; this one used to run bare
    {
      bool sdTaken = (!sdMutex) || (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) == pdTRUE);
      file.close();
      if (sdTaken && sdMutex) xSemaphoreGive(sdMutex);
    }
    request->send(headResponse);
    return;
  }

  String rangeHeader = "";
  if (request->hasHeader("Range")) rangeHeader = request->header("Range");

  size_t startByte = 0;
  size_t endByte = fileSize - 1;
  bool openEndedRange = false;

  if (rangeHeader.length() && rangeHeader.startsWith("bytes=")) {
    int dashIndex = rangeHeader.indexOf('-');
    // strtoul not toInt() - toInt() is signed 32-bit and overflows past 2GB (ZIM parts hit 4GB)
    if (dashIndex > 6) {
      startByte = (size_t)strtoul(rangeHeader.substring(6, dashIndex).c_str(), nullptr, 10);
    }
    if (dashIndex + 1 < rangeHeader.length()) {
      String endStr = rangeHeader.substring(dashIndex + 1);
      if (endStr.length() > 0) {
        endByte = (size_t)strtoul(endStr.c_str(), nullptr, 10);
      } else {
        openEndedRange = true;
      }
    } else {
      openEndedRange = true;
    }
  }

  if (openEndedRange && (endByte - startByte) > (8 * 1024 * 1024)) {
    endByte = startByte + (8 * 1024 * 1024) - 1;
  }

  if (endByte >= fileSize) endByte = fileSize - 1;
  if (startByte > endByte) startByte = endByte;
  size_t contentLength = endByte - startByte + 1;

  String mimeType = "application/octet-stream";
  String pLower = filePath;
  pLower.toLowerCase();

  bool isMediaStream = false;
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
  else if (pLower.endsWith(".cbz")) mimeType = "application/vnd.comicbook+zip";
  else if (pLower.endsWith(".cbr")) mimeType = "application/vnd.comicbook-rar";

  if (isMediaStream && startByte < 65536) {
    String fileName = filePath.substring(filePath.lastIndexOf('/') + 1);
    webLog("Playing: " + fileName + " (" + humanSize(fileSize) + ") [Media]", "media");
  }

  if (isMediaStream && contentLength > 10000) {
    mediaStreamingActive = true;
    lastStreamIoMs = millis();
    shutdownBackgroundTasksForStreaming();
  }

  if (pLower.indexOf("/archive/") >= 0) {
    mediaStreamingActive = true;
    lastStreamIoMs = millis();
    shutdownBackgroundTasksForStreaming();
  }

  // the take can time out on a busy card; an unconditional give would release
  // IndexWorker's lock. closing our own handle unlocked is fine in that rare case.
  {
    bool sdTaken = (!sdMutex) || (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) == pdTRUE);
    file.close();
    if (sdTaken && sdMutex) xSemaphoreGive(sdMutex);
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

      bool stdio2GBGuard = (eit != streamingFiles.end() && eit->second.file &&
                            eit->second.lastEndByte >= 0x7FFFF000UL);
      if (eit != streamingFiles.end() && eit->second.file && !stdio2GBGuard) {
        eit->second.file.seek(startByte);
        eit->second.lastActivity = millis();
        eit->second.lastEndByte = endByte;
        streamId = existingId;
        Serial.printf("[Stream] Reuse #%u seek %lu: %s (heap=%u)\n",
                      streamId, (unsigned long)startByte, filePath.c_str(),
                      (unsigned)ESP.getFreeHeap());
        if (sdMutex) xSemaphoreGive(sdMutex);
        xSemaphoreGive(streamingFilesMutex);
        goto stream_ready;
      } else {
        if (stdio2GBGuard) {
          Serial.printf("[Stream] Reopen #%u (pos>2GB stdio guard): %s\n",
                        existingId, filePath.c_str());
          eit->second.file.close();
        }
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
          streamPathIndex.erase(ait->second.path);
          ait->second.file.close();
          ait = streamingFiles.erase(ait);
        } else {
          ++ait;
        }
      }
    }

    if ((int)streamingFiles.size() >= MAX_CONCURRENT_STREAMS) {
      uint32_t oldestId = 0;
      unsigned long oldestTime = 0xFFFFFFFFUL;
      for (auto &kv : streamingFiles) {
        if (kv.second.lastActivity < oldestTime) {
          oldestTime = kv.second.lastActivity;
          oldestId = kv.first;
        }
      }
      if (oldestId) {
        auto it = streamingFiles.find(oldestId);
        if (it != streamingFiles.end()) {
          Serial.printf("[Stream] Evicting #%u (%s)\n", oldestId, it->second.path.c_str());
          streamPathIndex.erase(it->second.path);
          it->second.file.close();
          streamingFiles.erase(it);
        }
      }
    }

    File f = NomadSD.open(filePath, "r");
    if (!f) {
      Serial.printf("[Stream] Failed to open: %s\n", filePath.c_str());
      if (sdMutex) xSemaphoreGive(sdMutex);
      xSemaphoreGive(streamingFilesMutex);
      request->send(503, "text/plain", "Failed to open file");
      return;
    }
    f.seek(startByte);
    streamId = nextStreamId++;
    if (nextStreamId == 0) nextStreamId = 1;
    StreamHandle sh;
    sh.file = f;
    sh.path = filePath;
    sh.lastActivity = millis();
    sh.lastEndByte = endByte;
    streamingFiles[streamId] = sh;
    streamPathIndex[filePath] = streamId;
    activeStreams = streamingFiles.size();
    Serial.printf("[Stream] New #%u at %lu: %s (active=%d heap=%u)\n",
                  streamId, (unsigned long)startByte, filePath.c_str(), activeStreams,
                  (unsigned)ESP.getFreeHeap());
    if (sdMutex) xSemaphoreGive(sdMutex);
    xSemaphoreGive(streamingFilesMutex);
  } else {
    Serial.printf("[Stream] Mutex timeout for: %s\n", filePath.c_str());
    request->send(503, "text/plain", "Server busy");
    return;
  }

  stream_ready:

  AsyncWebServerResponse *response = request->beginResponse(
    mimeType,
    contentLength,
    [streamId, filePath, startByte](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      // busy mutex isnt end-of-data: returning 0 truncates the response, TRY_AGAIN retries later
      if (streamingFilesMutex && xSemaphoreTake(streamingFilesMutex, pdMS_TO_TICKS(150)) != pdTRUE) {
        return RESPONSE_TRY_AGAIN;
      }

      auto it = streamingFiles.find(streamId);
      if (it == streamingFiles.end()) {
        // evicted mid-send. returning 0 would look like end-of-body and truncate
        // the response (browser sees NS_ERROR_NET_RESET), so reopen and resume.
        bool sdHeld = (!sdMutex) || (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE);
        if (!sdHeld) {
          xSemaphoreGive(streamingFilesMutex);
          return RESPONSE_TRY_AGAIN;
        }
        File rf = NomadSD.open(filePath, "r");
        if (!rf) {
          if (sdMutex) xSemaphoreGive(sdMutex);
          xSemaphoreGive(streamingFilesMutex);
          Serial.printf("[Stream] Reopen failed for #%u (%s) - truncating\n",
                        streamId, filePath.c_str());
          return 0;  // genuinely unrecoverable
        }
        rf.seek(startByte + index);
        StreamHandle sh;
        sh.file = rf;
        sh.path = filePath;
        sh.lastActivity = millis();
        sh.lastEndByte = startByte + index;
        streamingFiles[streamId] = sh;
        streamPathIndex[filePath] = streamId;
        if (sdMutex) xSemaphoreGive(sdMutex);
        Serial.printf("[Stream] Reopened #%u at %lu (evicted mid-send): %s\n",
                      streamId, (unsigned long)(startByte + index), filePath.c_str());
        it = streamingFiles.find(streamId);
      }

      File &file = it->second.file;
      it->second.lastActivity = millis();
      lastStreamIoMs = it->second.lastActivity;

      size_t toRead = maxLen;
      if (toRead > 16384) toRead = 16384;

      size_t bytesRead = 0;
      bool sdBusy = false;
      if (!sdMutex) {
        bytesRead = file.read(buffer, toRead);
      } else if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1500)) == pdTRUE) {
        bytesRead = file.read(buffer, toRead);
        xSemaphoreGive(sdMutex);
      } else {
        sdBusy = true;  // transient: retry this fill later, don't truncate
      }
      xSemaphoreGive(streamingFilesMutex);

      return sdBusy ? RESPONSE_TRY_AGAIN : bytesRead;
    }
  );

  // NULL on low heap. dereferencing it panics async_tcp and kills the server
  if (!response) {
    Serial.printf("[Stream] beginResponse OOM for %s (heap=%u)\n",
                  filePath.c_str(), (unsigned)ESP.getFreeHeap());
    closeStreamById(streamId);
    request->send(503, "text/plain", "Server low on memory - retry");
    return;
  }

  if (rangeHeader.length() > 0) {
    response->setCode(206);
    response->addHeader("Content-Range", "bytes " + String(startByte) + "-" + String(endByte) + "/" + String(fileSize));
  } else {
    response->setCode(200);
  }

  response->addHeader("Access-Control-Allow-Origin", "*");
  response->addHeader("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS");
  response->addHeader("Access-Control-Allow-Headers", "Range, Content-Type, Accept");
  response->addHeader("Accept-Ranges", "bytes");
  response->addHeader("Content-Length", String(contentLength));
  response->addHeader("Cache-Control", "public, max-age=3600");
  response->addHeader("Connection", "close");
  request->send(response);
}
void handleListFiles(AsyncWebServerRequest *request) {
    if (!request->hasParam("dir")) {
        request->send(400, "application/json", "{\"error\":\"Missing 'dir' parameter\"}");
        return;
    }
    String dir = request->getParam("dir")->value();

    if (!NomadSD.exists(dir)) {
        request->send(404, "application/json", "{\"error\":\"Directory not found\"}");
        return;
    }

    File directory = NomadSD.open(dir);
    if (!directory || !directory.isDirectory()) {
        request->send(404, "application/json", "{\"error\":\"Not a directory\"}");
        return;
    }

    DynamicJsonDocument doc(8192);
    JsonArray arr = doc.to<JsonArray>();

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
            f["size"] = file.size();
            f["isDir"] = false;
        }
        file = directory.openNextFile();
    }

    // Serialize and send
    String response;
    serializeJson(arr, response);
    directory.close();
    request->send(200, "application/json", response);
}

// ------------------------- ARCHIVE MANIFEST -------------------------
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

  File root = NomadSD.open("/Archive");
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

        // id = filename minus extension; split parts (.zimaa) drop the suffix too so they share one id
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
        g->parts.push_back(ArchivePart{ String("/Archive/") + name, (uint64_t)file.size() });
      }
    }
    file.close();
    file = root.openNextFile();
    yield();
  }
  root.close();
  if (sdLocked) xSemaphoreGive(sdMutex);

  String resp = "{\"archives\":[";
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

// Cheap directory scan so menu.html can reveal the Games tile only when the
// /Games folder actually has content -- same pattern as handleArchiveList,
// deliberately reads no file CONTENT (zero big-file/2GB-stdio risk).
void handleGamesList(AsyncWebServerRequest *request) {
  bool sdLocked = false;
  if (sdMutex) {
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
      request->send(503, "application/json", "{\"error\":\"SD busy\"}");
      return;
    }
    sdLocked = true;
  }

  File root = NomadSD.open("/Games");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    if (sdLocked) xSemaphoreGive(sdMutex);
    AsyncWebServerResponse *r = request->beginResponse(200, "application/json", "{\"games\":[]}");
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
    return;
  }

  String resp = "{\"games\":[";
  bool first = true;
  int fileCount = 0;
  File file = root.openNextFile();
  while (file && fileCount < 256) {
    if (!file.isDirectory()) {
      String name = String(file.name());
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      // Skip dotfiles and sibling cover art -- only ROM files count toward "has content".
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

// Cheap directory scan for menu.html's Maps tile: lists region subfolders under
// /Maps that contain a manifest.json (per-region tile detail is fetched by the
// client directly from /Maps/<region>/manifest.json, not parsed on-device).
void handleMapsList(AsyncWebServerRequest *request) {
  bool sdLocked = false;
  if (sdMutex) {
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
      request->send(503, "application/json", "{\"error\":\"SD busy\"}");
      return;
    }
    sdLocked = true;
  }

  File root = NomadSD.open("/Maps");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    if (sdLocked) xSemaphoreGive(sdMutex);
    AsyncWebServerResponse *r = request->beginResponse(200, "application/json", "{\"regions\":[]}");
    r->addHeader("Access-Control-Allow-Origin", "*");
    request->send(r);
    return;
  }

  String resp = "{\"regions\":[";
  bool first = true;
  int regionCount = 0;
  File entry = root.openNextFile();
  while (entry && regionCount < 64) {
    if (entry.isDirectory()) {
      String name = String(entry.name());
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (!name.startsWith(".")) {
        String manifestPath = "/Maps/" + name + "/manifest.json";
        if (NomadSD.exists(manifestPath)) {
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
  if (sdLocked) xSemaphoreGive(sdMutex);

  resp += "]}";
  AsyncWebServerResponse *r = request->beginResponse(200, "application/json", resp);
  r->addHeader("Access-Control-Allow-Origin", "*");
  request->send(r);
  Serial.printf("[MAPS] /api/maps-list: %d region(s), %u bytes\n", regionCount, (unsigned)resp.length());
}

// ---- Multiplayer (HTTP polling) helpers -- all assume gameMutex is already held ----

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

// Takes a slot index rather than a MpRoom& -- Arduino's auto-generated function
// prototypes are inserted at the very top of the translation unit, before the
// MpRoom struct definition, so a custom-type parameter here would fail to compile.
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
  if (deserializeJson(doc, data)) { request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return; }
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
  room.seq = 0;  // 0 = empty/waiting state; turn = seq % 2, so seat 0 moves first
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
  if (deserializeJson(doc, data)) { request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return; }
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
  xSemaphoreGive(gameMutex);

  String resp = String("{\"token\":\"") + tokenOut + "\",\"seat\":1}";
  AsyncWebServerResponse *r = request->beginResponse(200, "application/json", resp);
  r->addHeader("Access-Control-Allow-Origin", "*");
  request->send(r);
  Serial.printf("[MP] Seat 1 joined room '%s'\n", code);
}

// POST /api/mp/move {code, token, move, seq} -> {seq}. `move` is an opaque, client-computed
// new-state blob (legality is validated CLIENT-SIDE, e.g. by chess.js) -- the server only
// enforces turn ownership (seat == seq%2) and that `seq` matches the room's current seq
// (optimistic-concurrency guard against stale/racing moves), then relays + bumps seq.
void handleMpMove(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  StaticJsonDocument<896> doc;  // MP_STATE_LEN (512) + code/token/seq overhead
  if (deserializeJson(doc, data)) { request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return; }
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

    Serial.printf("[RENAME] Attempting to rename: '%s' -> '%s'\n", oldName.c_str(), newName.c_str());

    if (streamingFilesMutex && xSemaphoreTake(streamingFilesMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool inUse = false;
        for (auto &kv : streamingFiles) {
          if (kv.second.path == oldName) { inUse = true; break; }
        }
        xSemaphoreGive(streamingFilesMutex);
        if (inUse) {
            Serial.printf("[RENAME] File is currently streaming: %s\n", oldName.c_str());
            request->send(409, "application/json", "{\"error\":\"File is currently in use\"}");
            return;
        }
    }

    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        Serial.printf("[RENAME] Mutex timeout\n");
        request->send(503, "application/json", "{\"error\":\"SD card busy\"}");
        return;
    }

    if (!NomadSD.exists(oldName)) {
        Serial.printf("[RENAME] Source file not found: %s\n", oldName.c_str());
        if (sdMutex) xSemaphoreGive(sdMutex);
        request->send(404, "application/json", "{\"error\":\"Original file not found\"}");
        return;
    }

    if (NomadSD.exists(newName)) {
        Serial.printf("[RENAME] Target already exists: %s\n", newName.c_str());
        if (sdMutex) xSemaphoreGive(sdMutex);
        request->send(409, "application/json", "{\"error\":\"Target file already exists\"}");
        return;
    }

    bool wasDirectory = false;
    File sourceFile = NomadSD.open(oldName);
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

    vTaskDelay(pdMS_TO_TICKS(100));

    bool renameSuccess = NomadSD.rename(oldName, newName);

    if (!renameSuccess) {
        Serial.printf("[RENAME] NomadSD.rename() failed: %s -> %s\n", oldName.c_str(), newName.c_str());
        webLogf("error", "Rename failed: %s → %s", oldName.c_str(), newName.c_str());
        if (sdMutex) xSemaphoreGive(sdMutex);
        request->send(500, "application/json", "{\"error\":\"Rename operation failed\"}");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(150));

    if (NomadSD.exists(oldName)) {
        Serial.printf("[RENAME] ERROR: Source still exists after rename: %s\n", oldName.c_str());
        if (sdMutex) xSemaphoreGive(sdMutex);
        request->send(500, "application/json", "{\"error\":\"Rename verification failed - source still exists\"}");
        return;
    }

    if (!NomadSD.exists(newName)) {
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

        if (NomadSD.exists(oldNestedPath)) {
            vTaskDelay(pdMS_TO_TICKS(50));

            if (NomadSD.remove(oldNestedPath)) {
                Serial.printf("[RENAME] Removed old nested index: %s\n", oldNestedPath.c_str());
            } else {
                Serial.printf("[RENAME] Warning: Failed to remove old nested index: %s\n", oldNestedPath.c_str());
            }
        }
    } else {
        Serial.printf("[RENAME] File renamed successfully: '%s' -> '%s'\n", oldName.c_str(), newName.c_str());
    }

    if (sdMutex) xSemaphoreGive(sdMutex);

    vTaskDelay(pdMS_TO_TICKS(100));

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

    if (streamingFilesMutex && xSemaphoreTake(streamingFilesMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool inUse = false;
        for (auto &kv : streamingFiles) {
          if (kv.second.path == filename) { inUse = true; break; }
        }
        xSemaphoreGive(streamingFilesMutex);
        if (inUse) {
            Serial.printf("[DELETE] File is currently streaming: %s\n", filename.c_str());
            request->send(409, "application/json", "{\"error\":\"File is currently in use\"}");
            return;
        }
    }

    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        Serial.printf("[DELETE] Mutex timeout\n");
        request->send(503, "application/json", "{\"error\":\"SD card busy\"}");
        return;
    }

    if (!NomadSD.exists(filename)) {
    if (sdMutex) xSemaphoreGive(sdMutex);
    request->send(404, "application/json", "{\"error\":\"File not found\"}");
    return;
    }

    bool success = false;
    bool wasDirectory = false;
    File f = NomadSD.open(filename);
    if (f && f.isDirectory()) {
        wasDirectory = true;
    }
    if (f) f.close();

    webLogf("info", "Deleting %s: '%s'", wasDirectory ? "folder" : "file", filename.c_str());
    Serial.printf("Deleting %s: %s\n", wasDirectory ? "directory" : "file", filename.c_str());

    if (wasDirectory) {
        success = deleteRecursive(filename);
    } else {
        success = NomadSD.remove(filename);
    }

    if (sdMutex) xSemaphoreGive(sdMutex);

    if (success) {
        if (wasDirectory) {
            String nestedIndexName = encodeIndexName(filename) + ".nested.ndjson";
            String nestedIndexPath = String(INDEX_DIR) + "/" + nestedIndexName;
            if (NomadSD.exists(nestedIndexPath)) {
                NomadSD.remove(nestedIndexPath);
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
void createSimpleUploadHandler(const String& mediaFolder, const char* endpoint) {
    server.on(endpoint, HTTP_POST,
    [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"status\":\"Upload finished\"}");
    },
    [mediaFolder](AsyncWebServerRequest *request, const String& filename, size_t index,
    uint8_t *data, size_t len, bool final) {

    if (index == 0) {
    String fullPath = "/" + mediaFolder + "/" + filename;
    Serial.println("[Upload] Starting upload to: " + fullPath);
    File f = NomadSD.open(fullPath, FILE_WRITE);
    if (!f) {
    webLogf("error", "Upload failed: could not open file for writing");
    Serial.println("[Upload] Failed to open file for writing");
    return;
    }
    activeUploads[request] = f;
    }

    if (activeUploads.count(request)) {
    activeUploads[request].write(data, len);
    Serial.printf("[Upload] Written %u bytes to %s\n", len, filename.c_str());
    }

    if (final && activeUploads.count(request)) {
        String fullPath = "/" + mediaFolder + "/" + filename;
        Serial.println("[Upload] Upload complete for: " + filename);
        activeUploads[request].close();
        activeUploads.erase(request);

        String parentDir = parentDirFromPath(fullPath);
        enqueueIndexUpdateForPath(parentDir);
        
        String bucketRoot = "/" + mediaFolder;
        if (bucketRoot != parentDir) {
            enqueueIndexUpdateForPath(bucketRoot);
        }
    }
    }
    );
}
// ---------- SD usage persistence helpers ----------
// File used to persist the last known usage %
// Save usage atomically
void saveSdUsageToFile(uint64_t totalBytes, uint64_t usedBytes, unsigned long tsMillis = 0) {
  DynamicJsonDocument doc(256);
  doc["total"] = totalBytes;
  doc["used"]  = usedBytes;
  doc["ts"]    = tsMillis ? tsMillis : millis();
  doc["statTrusted"] = g_sdStatTrusted;

  String tmp = String(SD_USAGE_FILE) + ".tmp";
  File f = NomadSD.open(tmp, FILE_WRITE);
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

  if (NomadSD.exists(SD_USAGE_FILE)) NomadSD.remove(SD_USAGE_FILE);
  if (!NomadSD.rename(tmp, SD_USAGE_FILE)) {
    File ft = NomadSD.open(tmp, FILE_READ);
    File ff = NomadSD.open(SD_USAGE_FILE, FILE_WRITE);
    if (ft && ff) {
      while (ft.available()) ff.write(ft.read());
      ft.close();
      ff.close();
      NomadSD.remove(tmp);
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
  if (!NomadSD.exists(SD_USAGE_FILE)) return false;
  File f = NomadSD.open(SD_USAGE_FILE, FILE_READ);
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
  uint64_t total = NomadSD.totalBytes();
  uint64_t used = NomadSD.usedBytes();
  if (sdMutex) xSemaphoreGive(sdMutex);

  if (total == 0) {
    Serial.println("[SDBAR] refreshCachedTotalsFromStat: NomadSD reported 0 total bytes, skipping update");
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

// checks a real walk's usage against NomadSD.usedBytes() and trusts the fast stat path
// only if they agree. this is the only place g_sdStatTrusted is set true.
void reconcileStatTrust(uint64_t walkedUsedBytes) {
  uint64_t statUsed = NomadSD.usedBytes();
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
    webLogf("warning", "NomadSD.usedBytes() disagrees with a real scan by %.1f%% - fast totals will be marked unverified", diffPct);
  }

  g_sdStatTrusted = agrees;
}

// ---------- Disk-usage ----------

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

  // Sized off actual entry count rather than a flat guess, same pattern already used
  // for handleListFiles's DynamicJsonDocument(8192).
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
  File f = NomadSD.open(tmp, FILE_WRITE);
  if (!f) {
    Serial.println("[SDBreakdown] Warning: couldn't open temp breakdown file for write");
    return;
  }
  serializeJson(doc, f);
  f.close();

  if (NomadSD.exists(SD_BREAKDOWN_FILE)) NomadSD.remove(SD_BREAKDOWN_FILE);
  if (!NomadSD.rename(tmp, SD_BREAKDOWN_FILE)) {
    File ft = NomadSD.open(tmp, FILE_READ);
    File ff = NomadSD.open(SD_BREAKDOWN_FILE, FILE_WRITE);
    if (ft && ff) {
      while (ft.available()) ff.write(ft.read());
      ft.close();
      ff.close();
      NomadSD.remove(tmp);
    } else {
      if (ft) ft.close();
      if (ff) ff.close();
      Serial.println("[SDBreakdown] Warning: rename fallback failed for breakdown file");
    }
  }
}

bool loadSdBreakdownFromFile() {
  if (!NomadSD.exists(SD_BREAKDOWN_FILE)) return false;
  File f = NomadSD.open(SD_BREAKDOWN_FILE, FILE_READ);
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

void scanSDCardUsage() {
  const unsigned long minScanIntervalMs = 15UL * 60UL * 1000UL;
  if (lastScanTime != 0 && (millis() - lastScanTime) < minScanIntervalMs) {
    Serial.printf("[SDBAR] scan skipped: last scan %lu ms ago (min %lu ms)\n", millis() - lastScanTime, minScanIntervalMs);
    return;
  }

  cachedUsedBytes  = 0;
  uint64_t reportedTotal = NomadSD.cardSize();
  uint32_t startMs = millis();
  uint64_t lastSavedBytes = 0;

  if (reportedTotal > 0) {
    if (reportedTotal < cachedUsedBytes) {
      if ((reportedTotal * 512ULL) >= cachedUsedBytes) {
        reportedTotal *= 512ULL;
      } else if ((reportedTotal * 1024ULL) >= cachedUsedBytes) {
        reportedTotal *= 1024ULL;
      } else {
        reportedTotal *= 512ULL;
      }
    }
  } else {
    reportedTotal = cachedTotalBytes > 0 ? cachedTotalBytes : (64ULL * 1024ULL * 1024ULL * 1024ULL);
  }
  cachedTotalBytes = reportedTotal;

  const uint32_t tickBudgetMs = 50;
  uint32_t lastYield = millis();
  uint64_t lastAnnounced = 0;

  g_lastBreakdown.clear();
  auto topLevelBucketFor = [](const String &path) -> String {
    int slash = path.indexOf('/', 1);
    return (slash < 0) ? String("/") : path.substring(0, slash);
  };

  std::vector<String> dirStack;
  dirStack.push_back("/");

  while (!dirStack.empty()) {
    String currentPath = dirStack.back();
    dirStack.pop_back();

    File dir = NomadSD.open(currentPath);
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
        if (entryPath != "/") g_lastBreakdown[topLevelBucketFor(entryPath)].dirs++;
        entry.close();
      } else {
        uint64_t fileSize = entry.size();
        entry.close();

        cachedUsedBytes += fileSize;
        BreakdownStats &bucket = g_lastBreakdown[topLevelBucketFor(entryPath)];
        bucket.bytes += fileSize;
        bucket.files++;

        if ((cachedUsedBytes - lastAnnounced) > (2ULL * 1024ULL * 1024ULL * 1024ULL)) {
          sdbarDirty = true;
          lastAnnounced = cachedUsedBytes;
          float usedGB = (float)cachedUsedBytes / (1024.0 * 1024.0 * 1024.0);
          float totalGB = (float)cachedTotalBytes / (1024.0 * 1024.0 * 1024.0);
          webLogf("info", "SD scan progress: %.1f GB / %.1f GB scanned", usedGB, totalGB);

          // Same checkpoint (every 2GB) doubles as the screen-progress throttle -
          // no separate timer needed, and it can't flood the small lvglQueue.
          int pct = (totalGB > 0.0f) ? (int)((usedGB / totalGB) * 100.0f) : 0;
          char screenBuf[80];
          snprintf(screenBuf, sizeof(screenBuf), "Scanning SD card...\n%.1f / %.1f GB (%d%%)\n\nDo not unplug!",
                    usedGB, totalGB, pct);
          lvglSendMsg(screenBuf, true);
        }
        
        if ((cachedUsedBytes - lastSavedBytes) > (32ULL * 1024ULL * 1024ULL)) {
          saveSdUsageToFile(cachedTotalBytes, cachedUsedBytes, millis());
          lastSavedBytes = cachedUsedBytes;
        }
      }

      if ((millis() - lastYield) > tickBudgetMs) {
        lastYield = millis();
        vTaskDelay(pdMS_TO_TICKS(1));
      }
    }
    dir.close();
  }

  reconcileStatTrust(cachedUsedBytes);

  saveSdUsageToFile(cachedTotalBytes, cachedUsedBytes, millis());
  saveSdBreakdownToFile(g_lastBreakdown);

  lastScanTime = millis();
  sdbarDirty = true;


  Serial.printf("[SDBAR] SD card scan complete: used=%llu bytes, total=%llu bytes, duration=%u ms\n",
    (unsigned long long)cachedUsedBytes, (unsigned long long)cachedTotalBytes, (unsigned) (millis() - startMs));

  float usedMB = (float)cachedUsedBytes / (1024.0 * 1024.0);
  float totalMB = (float)cachedTotalBytes / (1024.0 * 1024.0);
  float usedGB = usedMB / 1024.0;
  float totalGB = totalMB / 1024.0;
  float percent = (totalGB > 0.0f) ? (usedGB / totalGB) * 100.0f : 0.0f;

  webLogf("success", "SD card scan complete: %.1f GB used of %.1f GB total (%.1f%% full)",
    usedGB, totalGB, percent);
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

bool checkGenerateFlagFile() {
    if (NomadSD.exists("/.generate_flag")) {
        Serial.println("[BOOT] Found /.generate_flag, will generate media.json");
        NomadSD.remove("/.generate_flag");
        return true;
    }
    return false;
}
void handleConnector(AsyncWebServerRequest *request) {
  // 1) Get 'dir' from POST body
  String dir = "/";
  if (request->hasParam("dir", true)) {
    dir = request->getParam("dir", true)->value();
  }
  if (!dir.endsWith("/")) dir += "/";

  // 2) Acquire mutex before SD card access
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
    request->send(503, "text/plain", "SD card busy");
    return;
  }

  // 3) Open the directory
  File root = NomadSD.open(dir);
  if (!root || !root.isDirectory()) {
    if (sdMutex) xSemaphoreGive(sdMutex);
    request->send(400, "text/plain", "Invalid directory");
    return;
  }

  // 4) Build the HTML <ul> tree
  String html = "<ul class=\"jqueryFileTree\" style=\"display: none;\">";
  root.rewindDirectory();
  File entry;
  while ((entry = root.openNextFile())) {
    String name = entry.name();
    if (entry.isDirectory()) {
      html += "<li class=\"directory collapsed\">"
           "<a href=\"#\" rel=\"" + dir + name + "/\">" + name + "</a>"
           "</li>";
    } else {
      int dot = name.lastIndexOf('.');
      String ext = dot > 0 ? name.substring(dot+1) : "";
      html += "<li class=\"file ext_" + ext + "\">"
           "<a href=\"#\" rel=\"" + dir + name + "\">" + name + "</a>"
           "</li>";
    }
    entry.close();
  }
  html += "</ul>";

  // 4) Respond
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

    if (NomadSD.mkdir(dirPath)) {
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
// mDNS responder: makes http://nomad.local/ work alongside 192.168.4.1.
// Apple/Android/Linux resolve .local ONLY via multicast, so the wildcard
// captive DNS alone doesn't cover it. Must be restarted whenever the AP
// interface is torn down (softAPdisconnect kills the responder's netif).
const char* MDNS_HOSTNAME = "nomad";
// MDNS.end() is an unguarded mdns_free(), so calling it before the first
// begin() panics. Only tear down a responder we actually started.
static bool mdnsStarted = false;
void startNomadMDNS() {
  if (mdnsStarted) {
    MDNS.end();  // required before re-begin after an AP restart
    mdnsStarted = false;
  }
  if (MDNS.begin(MDNS_HOSTNAME)) {
    mdnsStarted = true;
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mDNS] Responder started: http://%s.local/\n", MDNS_HOSTNAME);
  } else {
    Serial.println("[mDNS] Failed to start responder (nomad.local unavailable; IP access unaffected)");
  }
}
void applyWiFiSettings() {
  Serial.print("Stopping existing WiFi Access Point...");
  WiFi.softAPdisconnect(true);  // Stop AP and clear config
  delay(100);  // Give time for cleanup

  Serial.print("Starting WiFi with SSID: ");
  Serial.println(settings.wifiSSID);
  WiFi.softAP(settings.wifiSSID.c_str(), settings.wifiPassword.c_str(), 1, 0, MAX_CLIENTS);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  startNomadMDNS();
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

  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(10000)) == pdTRUE) {
    scanSDCardUsage(); // pushes its own throttled screen progress as it walks
    sdbarDirty = true;
    xSemaphoreGive(sdMutex);
  } else {
    Serial.println("[SDScan] Failed to acquire mutex for SD scan");
    webLog("[SD Scan] Failed to acquire SD lock - will retry later", "warning");
  }

  sdScanInProgress = false;
  sdScanCompleted = true;
  lvglSendMsg("", false);
  Serial.println("[SDScan] SD scan complete; sdScanCompleted = true");
  // Measured, not assumed: log real stack headroom so this can be verified on-device
  // rather than sized from theory.
  Serial.printf("[SDScan] Task stack high-water mark: %u bytes free (minimum seen)\n",
                (unsigned)uxTaskGetStackHighWaterMark(NULL));

  vTaskDelete(NULL);
}
void generateMediaJSON(){ generateMediaJson(); }

// --- converts “how busy” into small work budgets and pauses.
static inline int getServerLoadScore() {
  // 0 = idle … higher = busier
  // Count stations + uploads; cheap approximation but good enough to throttle.
  extern std::map<AsyncWebServerRequest*, File> activeUploads;
  int users   = getConnectedUserCount();
  int uploads = (int)activeUploads.size();
  return users * 3 + uploads * 5;
}

// Returns a pair: { entriesToProcessBeforePause, pauseMsAfterBatch }
static inline std::pair<int,int> chooseWorkBudget() {
  // Check if media streaming is active and reduce background work if so
  if (mediaStreamingActive) {
    // Reduce background work when streaming media
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
      filePath.startsWith("/Games/")) {

    // Extract the parent directory for indexing
    String parentDir = parentDirFromPath(filePath);

    // Only enqueue if we're not already streaming media
    if (!mediaStreamingActive) {
      enqueueIndexUpdateForPath(parentDir);
    }
  }
}

void indexWorkerTask(void *param) {
  const char *buckets[] = { "/Shows", "/Music", "/Movies", "/Books", "/Gallery", "/Files", "/Games",  "/", NULL };

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
      vTaskDelete(NULL);
      return;
    }

    if (requestIndexing && !indexingInProgress) {
      // MEMORY CLEANUP: Free up as much memory as possible before indexing
      Serial.printf("[IndexWorker] Pre-index heap check: free=%u\n", ESP.getFreeHeap());

      // Force garbage collection of any lingering allocations
      vTaskDelay(pdMS_TO_TICKS(200));

      if (!enoughHeapForIndex(20000)) {
        Serial.println("[IndexWorker] Insufficient heap for indexing, clearing caches and retrying");

        // Clear any cached data structures
        g_lastIndexSkipLog.clear();
        lastIndexRequestMs.clear();

        vTaskDelay(pdMS_TO_TICKS(3000));

        if (!enoughHeapForIndex(20000)) {
          // Try with minimum safe threshold for constrained systems
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

        // MEMORY CLEANUP: Clear temporary maps after each bucket
        g_lastIndexSkipLog.clear();

        // Extended yield between buckets to prevent watchdog and allow background tasks
        Serial.printf("[IndexWorker] Bucket complete, yielding (heap=%u)\n", ESP.getFreeHeap());
        vTaskDelay(pdMS_TO_TICKS(500));

        // Process queued items after each bucket
        IndexBuildArgs *msg = nullptr;
        int processedCount = 0;
        while (indexQueue && xQueueReceive(indexQueue, &msg, 0) == pdTRUE && msg && processedCount < 5) {
          if (shutdownBackgroundTasks) {
            delete msg;
            break;
          }

          Serial.printf("[IndexWorker] Processing queued item (heap=%u)\n", ESP.getFreeHeap());
          writeNDIndexForDir(msg->dir, msg->out);
          delete msg;
          processedCount++;

          // MEMORY CLEANUP: Yield and clear after each queued item
          g_lastIndexSkipLog.clear();
          vTaskDelay(pdMS_TO_TICKS(pauseMs));
        }
      }

      if (!shutdownBackgroundTasks) {
        File f = NomadSD.open("/boot_done.flag", FILE_WRITE);
        if (f) { f.print("1"); f.close(); }

        // Show completion and trigger a storage scan (covers both first-time and
        // regular full-index runs; there's no case where a reboot is needed here).
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

      // MEMORY CLEANUP: Final cleanup after full indexing
      lastIndexRequestMs.clear();
      g_lastIndexSkipLog.clear();

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
        vTaskDelete(NULL);
        return;
      }

      // Check heap before processing 
      if (!enoughHeapForIndex(15000)) {  // 30KB total with 15K min heap
        Serial.printf("[IndexWorker] Low heap (%u bytes), re-queuing %s\n", 
                     ESP.getFreeHeap(), msg->dir.c_str());
        
        // MEMORY CLEANUP: Try clearing caches before re-queuing
        g_lastIndexSkipLog.clear();
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

      // MEMORY CLEANUP: Clear after each live update
      g_lastIndexSkipLog.clear();

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

void enqueueIndexUpdateForPath(const String &path) {
  // Final safety net: never queue work for our own internal/system folders, no
  // matter which caller reaches this function.
  if (shouldSkipIndexingPath(path)) {
    Serial.printf("[Index] Refusing to enqueue system/internal folder '%s'\n", path.c_str());
    return;
  }

  String targetPath = path;
  if (path.startsWith("/Music/")) {
    targetPath = "/Music";
    Serial.printf("[Index] Redirecting '%s' -> '%s' (Music bucket)\n", path.c_str(), targetPath.c_str());
  } else if (path.startsWith("/Shows/")) {
    targetPath = "/Shows";
    Serial.printf("[Index] Redirecting '%s' -> '%s' (Shows bucket)\n", path.c_str(), targetPath.c_str());
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
            vTaskDelete(NULL);
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
  File root = NomadSD.open("/");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    Serial.println("[Index] immediateEnqueueTopLevelTask: root not available, exiting task");
    vTaskDelete(NULL);
    return;
  }

  // Known bucket roots that the indexWorker loop's full-scan path already handles
  // directly, skip them here to avoid redundantly queuing a duplicate rebuild.
  const char* buckets[] = { "Shows", "Music", "Movies", "Books", "Gallery", "Files", "Games", NULL };

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
  vTaskDelete(NULL);
}

// Task lifecycle management functions for performance optimization
void shutdownBackgroundTasksForStreaming() {
  // Don't shut down if indexing is explicitly in progress
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

void startBackgroundTasksIfNeeded() {
  if (shutdownBackgroundTasks) {
    Serial.println("[TaskMgr] Restarting background tasks");
    webLog("[PERFORMANCE] Background tasks restarted", "success");
    shutdownBackgroundTasks = false;

    // Restart indexWorkerTask if it was shut down
    if (indexWorkerTaskHandle == nullptr) {
      // Small delay to prevent immediate resource contention
      vTaskDelay(pdMS_TO_TICKS(50));
      BaseType_t r = xTaskCreatePinnedToCore(indexWorkerTask, "IndexWorker", 16 * 1024, NULL, 2, &indexWorkerTaskHandle, 0);
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
      BaseType_t r = xTaskCreatePinnedToCore(storageMonitorTask, "StorageMonitor", 4096, NULL, 1, &storageMonitorTaskHandle, 0);
      if (r != pdPASS) {
        Serial.println("[TaskMgr] Failed to restart StorageMonitor task");
        webLog("[ERROR] Failed to restart StorageMonitor task", "error");
      } else {
        webLog("[PERFORMANCE] StorageMonitor task restarted", "success");
      }
    }
  }
}

// Simple streaming timeout check 
void checkStreamingTimeout() {
  const unsigned long STREAMING_IDLE_TIMEOUT = 15000; // 15 seconds

  // Restart background tasks once streaming has really been idle for a while.
  if (mediaStreamingActive && (millis() - lastStreamIoMs > STREAMING_IDLE_TIMEOUT)) {
    Serial.println("[TaskMgr] Streaming idle timeout - restarting background tasks");
    mediaStreamingActive = false;
    startBackgroundTasksIfNeeded();
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
            streamPathIndex.erase(it->second.path);
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

  // old firmware used one /media.json instead of per-bucket NDJSON. if its still here, nuke it and rebuild
  bool legacyUpgradeNeeded = false;
  if (NomadSD.exists("/media.json")) {
    Serial.println("[BootCoord] Found legacy media.json - upgrading to NDJSON index system");
    lvglSendMsg("Updating to new index system...", true);
    if (NomadSD.remove("/media.json")) {
      Serial.println("[BootCoord] Removed legacy media.json file");
    } else {
      Serial.println("[BootCoord] WARNING: failed to remove legacy media.json file");
    }
    legacyUpgradeNeeded = true;
  }

  // "Has this device ever completed an index?" 
  String rootIndexFile = String(INDEX_DIR) + "/root.index.ndjson";
  if (legacyUpgradeNeeded || !NomadSD.exists(INDEX_DIR) || !NomadSD.exists(rootIndexFile)) {
    Serial.println("[BootCoord] Index missing or incomplete - triggering normal index build");

    lvglSendMsg("First-time setup detected\nBuilding media indexes...\nThis will take a few minutes", true);
    Serial.println("[BootCoord] LVGL screen updated with first-time setup message");

    sdScanCompleted = true;
    sdScanInProgress = false;

    requestIndexing = true;
    Serial.println("[BootCoord] First-time index build queued via normal scan path");
    Serial.println("[BootCoord] coordination done; exiting task");
    vTaskDelete(NULL);
    return;
  }

  // index exists. only enqueue top-level folders if the boot change-detector queued a
  // reindex (bootReindexQueued). nothing changed = no indexing
  if (bootReindexQueued) {
    Serial.println("[BootCoord] Boot reindex active -> enqueuing top-level folders");

    sdScanCompleted = false;
    sdScanInProgress = false;

    BaseType_t ir = xTaskCreatePinnedToCore(immediateEnqueueTopLevelTask, "IncScanBoot", 12 * 1024, NULL, 1, NULL, 1);
    if (ir == pdPASS) {
      Serial.println("[BootCoord] Top-level enqueue task started to populate index queue");
    }

    // Note: requestIndexing was already set by setup() and may already be processing
    Serial.println("[BootCoord] Reindex queued by setup(), directories being enqueued");
  } else {
    Serial.println("[BootCoord] No boot reindex -> indexes exist, storage loads from cache/on-demand");

    sdScanCompleted = true;
    sdScanInProgress = false;
  }

  vTaskDelay(pdMS_TO_TICKS(100));
  Serial.println("[BootCoord] boot coordination done; exiting task");
  vTaskDelete(NULL);
}

// --- Captive portal throttled logging ---
// Keeps a small table of recent clients and when they were last logged
// so we only print one message per device per INTERVAL_MS.
#define CAPTIVE_MAX_CLIENT_LOGS 32

struct CaptiveLogEntry {
  String id;
  unsigned long ts;
};

static CaptiveLogEntry captiveLogs[CAPTIVE_MAX_CLIENT_LOGS];
static uint8_t captiveLogCount = 0;

// ---------- me-readable byte formatting ----------
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
  if (path.endsWith(".ico"))  return "image/x-icon";
  return "application/octet-stream"; // fallback safe binary
}
void serveProtectedFile(AsyncWebServerRequest *request, const String& filePath) {
    if (sdMutex) {
        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
            request->send(503, "text/plain", "SD busy");
            return;
        }
    }
    
    auto releaseSd = [&](){
        if (sdMutex) xSemaphoreGive(sdMutex);
    };
    
    if (!NomadSD.exists(filePath)) {
        releaseSd();
        request->send(404, "text/plain", "File not found");
        return;
    }
    
    String mime = getMimeType(filePath);
    AsyncWebServerResponse *response = request->beginResponse(NomadSD, filePath, mime);
    releaseSd();
    if (!response) {
        Serial.printf("[Static] beginResponse OOM for %s (heap=%u)\n",
                      filePath.c_str(), (unsigned)ESP.getFreeHeap());
        request->send(503, "text/plain", "Server low on memory - retry");
        return;
    }
    response->addHeader("Cache-Control", "public, max-age=600");
    request->send(response);
}
// ------------- Main Setup -------------------
void setup() {
    Serial.begin(115200);
    delay(100);
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
    NomadButton_Init();

    if (get_boot_mode() == USB_MODE) {
      clear_boot_mode();    // next boot will go back to MEDIA
      delay(500);
      Serial.println(">>> USB mode: mounting SD & starting MSC");
      NomadUI_Init();
      btStop(); //Stops bluetooth (dont need)
      NomadUI_Message("USB Mass-Storage Mode\n\nEject or press the button\nto return to media mode");
      NomadUI_Flush();
      
      launch_usb_mode();
      return;
    }


    NomadUI_Init();
    NomadHW_PrintBoardInfo(Serial);
    NomadUI_Message("Booting...");
    NomadUI_Flush();
    delay(200);


    webLog("[SYSTEM] Nomad System Captive Portal & SDMMC Online", "info");

    // Start WiFi Access Point
    webLogf("info", "Starting WiFi Access Point with SSID: '%s'", settings.wifiSSID.c_str());

    // the DHCP server claims its lease pool during softAP(). if that fails on low
    // heap the AP still comes up but clients get no address (169.254.x.x APIPA).
    Serial.printf("[DIAG] pre-AP  internal=%u largest=%u  psramFound=%d spiram=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (int)psramFound(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    WiFi.softAP(settings.wifiSSID.c_str(), settings.wifiPassword.c_str(), 1, 0, MAX_CLIENTS);

    Serial.printf("[DIAG] post-AP internal=%u largest=%u  apIP=%s\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  WiFi.softAPIP().toString().c_str());

    // check it actually started, and restart it if not
    {
      esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
      esp_netif_dhcp_status_t st = ESP_NETIF_DHCP_INIT;
      if (ap && esp_netif_dhcps_get_status(ap, &st) == ESP_OK) {
        Serial.printf("[DIAG] DHCP server status=%d (%s)\n", (int)st,
                      st == ESP_NETIF_DHCP_STARTED ? "STARTED" : "NOT RUNNING");
        if (st != ESP_NETIF_DHCP_STARTED) {
          Serial.println("[DIAG] DHCP not started - clients will self-assign 169.254.x.x. Retrying.");
          esp_netif_dhcps_start(ap);
          if (esp_netif_dhcps_get_status(ap, &st) == ESP_OK) {
            Serial.printf("[DIAG] DHCP after retry status=%d (%s)\n", (int)st,
                          st == ESP_NETIF_DHCP_STARTED ? "STARTED" : "STILL NOT RUNNING");
          }
        }
      } else {
        Serial.println("[DIAG] could not query AP netif for DHCP status");
      }
    }

    webLogf("success", "WiFi Access Point started successfully - IP: %s", WiFi.softAPIP().toString().c_str());
    startNomadMDNS();
    webLogf("info", "Device also reachable at http://%s.local/", MDNS_HOSTNAME);
  

// Initialize SD card with full diagnostics
Serial.println("Initializing SD Card...");

// NomadSD_Mount() walks 4-bit/40 MHz down to 1-bit/probe speed until the card
// answers, so a marginal board still comes up. It never formats on failure.
NomadSdMountResult sdMount = NomadSD_Mount(12);
if (!sdMount.mounted) {
    Serial.println("ERROR: SDMMC Card initialization failed.");
    webLog("[SD] Card mount failed - check the card and the pins in board_config.h", "error");
    NomadUI_Message("No SD card\n\nInsert a FAT32 card\nand reboot");
    NomadUI_Flush();
    NomadUI_SetSdOk(false);
    return;
}

Serial.println("SD Card initialized successfully!");

    webLogf("success", "SD Card mounted (%s @ %lu MHz) - Size: %.1f GB",
            sdMount.fourBit ? "4-bit" : "1-bit",
            (unsigned long)(sdMount.freqHz / 1000000UL),
            (float)sdMount.cardSizeBytes / (1024.0 * 1024.0 * 1024.0));

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

    Serial.println("Loading Settings...");
    loadSettings();
    settingsReady = true; // signal background tasks the settings are loaded
    Serial.printf("[SETTINGS] autoGenerateMedia = %s\n", settings.autoGenerateMedia ? "true" : "false");
    applyWiFiSettings();
    applyRGBSettings();
    if (settings.flipScreen) {
      // setup() is still single-threaded here (LVGL tasks not started yet),
      // so writing MADCTL directly is safe. Repaint so the boot screen
      // isn't left mirrored from the pre-flip draws above.
      LCD_SetRotation180(true);
      lv_obj_invalidate(lv_scr_act());
      lv_timer_handler();
    }
    NomadUI_SetSSID(settings.wifiSSID.c_str());
    NomadUI_SetIP(WiFi.softAPIP().toString().c_str());
    Serial.print("settings.brightness = ");
    Serial.println(settings.brightness);
    // legacy one-time flag + did USB mode write data last session. the indexer only sees
    // web-UI changes, so a USB write is otherwise invisible to it
    bool generateOnce = NomadSD.exists("/generate_once.flag");
    bool needsReindexAfterUsb = get_needs_reindex_flag();

    // Log state
    Serial.print("[BOOT] autoGenerateMedia (from settings) = ");
    Serial.println(settings.autoGenerateMedia ? "true" : "false");
    Serial.print("[BOOT] generate_once.flag = ");
    Serial.println(generateOnce ? "true" : "false");
    Serial.print("[BOOT] needs_reindex (post-USB) = ");
    Serial.println(needsReindexAfterUsb ? "true" : "false");

    bool oneTimeReindexRequested = generateOnce || needsReindexAfterUsb;

    // "check for new files on boot" (autoGenerateMedia) gates the change detector,
    // it does NOT reindex every boot. ON = reindex only if something changed (USB write
    // or one-time flag), OFF = boot normally and clear pending flags (manual index only).
    // a never-indexed card is force-built earlier in bootCoordinatorTask regardless.

    if (settings.autoGenerateMedia && oneTimeReindexRequested) {
      requestIndexing = true;
      bootReindexQueued = true;
      Serial.println("[BOOT] Boot check ON + filesystem changes detected -> queued reindex");

      if (generateOnce && NomadSD.exists("/generate_once.flag")) {
        NomadSD.remove("/generate_once.flag");
        Serial.println("[BOOT] Cleared /generate_once.flag");
      }
      if (needsReindexAfterUsb) {
        clear_needs_reindex_flag();
        Serial.println("[BOOT] Cleared post-USB reindex flag");
      }
    } else if (settings.autoGenerateMedia) {
      // toggle on but nothing changed - no blind reindex, thats the whole point
      Serial.println("[BOOT] Boot check ON but no filesystem changes detected -> booting normally");
    } else {
      Serial.println("[BOOT] Boot check OFF -> booting normally (manual index only)");

      // Clear any pending one-time signals so a stale flag can't surprise-index later.
      if (generateOnce && NomadSD.exists("/generate_once.flag")) {
        NomadSD.remove("/generate_once.flag");
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
        NomadSD.remove("/boot_done.flag");
    }

    Serial.println("[BOOT] Deferring media generation to bootCoordinatorTask (non-blocking)");

    // Now restore last-known SD usage quickly so UI has a valid starting point
    bool haveSdSnapshot = loadSdUsageFromFile();
    if (!haveSdSnapshot) {
      Serial.println("[SDBAR] No prior usage snapshot found; SDBAR will update after background scan.");
    } else {
      Serial.println("[SDBAR] Restored SD usage snapshot from disk; will skip heavy boot scan if configured.");
    }
    // Breakdown is separate from the totals snapshot above and is purely additive, a miss here just means an empty breakdown
    // table until the next scan, not a degraded boot.
    loadSdBreakdownFromFile();

    Set_Backlight(settings.brightness);  // now using loaded value
    updateSDBAR(); // will show cached values loaded above (if any)

    // Start the storage monitor (unchanged)
    xTaskCreatePinnedToCore(storageMonitorTask, "StorageMonitor", 4096, NULL, 1, &storageMonitorTaskHandle, 0);

    // Continue registering handlers (unchanged)
    createSimpleUploadHandler("Movies", "/upload-movie");
    createSimpleUploadHandler("Music", "/upload-music");
    createSimpleUploadHandler("Books", "/upload-book");

    delay(2000);
    // Start Captive DNS redirection
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    //OPDS Endpoint (still needs fixing)
    server.on("/opds/root.xml", HTTP_GET, handleOPDSRoot);
    server.on("/opds/books.xml", HTTP_GET, handleOPDSBooks);
    //.m3u playlist endpoint NEEDS UPDATE, very outdated
    server.on("/playlist.m3u", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncResponseStream *stream = request->beginResponseStream("audio/x-mpegurl");
        String ip = WiFi.softAPIP().toString();
        stream->print("#EXTM3U\n");

        stream->print("# === MOVIES ===\n");
        File movieDir = NomadSD.open("/Movies");
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
        File showsRoot = NomadSD.open("/Shows");
        if (showsRoot && showsRoot.isDirectory()) {
            File showFolder = showsRoot.openNextFile();
            while (showFolder) {
                if (showFolder.isDirectory()) {
                    String showFolderName = String(showFolder.name());
                    if (showFolderName.startsWith("/")) showFolderName = showFolderName.substring(1);
                    String fullShowPath = "/Shows/" + showFolderName;

                    File episodeDir = NomadSD.open(fullShowPath);
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
        File musicDir = NomadSD.open("/Music");
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

        request->send(stream);
    });

    // nomad.m3u redirects to canonical playlist
    server.on("/nomad.m3u", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->redirect("/playlist.m3u");
    });

    //fAKE dlna dISCOVERY (this is uhhh probably never going to work, Im yet to find a TV that actualy falls for it)
    server.on("/dlna/device.xml", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(200, "text/xml", R"rawliteral(
        <?xml version="1.0"?>
        <root xmlns="urn:schemas-upnp-org:device-1-0">
          <specVersion>
            <major>1</major>
            <minor>0</minor>
          </specVersion>
          <device>
            <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
            <friendlyName>Nomad Media Server</friendlyName>
            <manufacturer>Jcorp</manufacturer>
            <modelName>Nomad DLNA</modelName>
            <UDN>uuid:ESP32-DLNA-NOMAD</UDN>
          </device>
        </root>
      )rawliteral");
    });
    server.on("/ssdp/device-desc.xml", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(200, "text/xml", R"rawliteral(
        <?xml version="1.0"?>
        <root xmlns="urn:schemas-upnp-org:device-1-0">
          <specVersion>
            <major>1</major>
            <minor>0</minor>
          </specVersion>
          <device>
            <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
            <friendlyName>Nomad Media Server</friendlyName>
            <manufacturer>Jcorp</manufacturer>
            <modelName>Nomad</modelName>
            <modelNumber>1</modelNumber>
            <UDN>uuid:ESP32-DLNA-FAKE-1234</UDN>
          </device>
        </root>
      )rawliteral");
    });
    server.on("/dlna/description.xml", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(200, "text/xml", R"rawliteral(
        <?xml version="1.0"?>
        <root xmlns="urn:schemas-upnp-org:device-1-0">
          <specVersion>
            <major>1</major>
            <minor>0</minor>
          </specVersion>
          <device>
            <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
            <friendlyName>Nomad Media</friendlyName>
            <manufacturer>JCorp</manufacturer>
            <modelName>ESP32-Nomad</modelName>
            <UDN>uuid:nomad-dlna-esp32</UDN>
          </device>
        </root>
      )rawliteral");
    });
    server.on("/description.xml", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *response = request->beginResponse(302, "text/plain", "");
        response->addHeader("Application-URL", "http://" + WiFi.softAPIP().toString() + "/dlna/");
        response->addHeader("Location", "/dlna/desc.xml");  // HTTP redirect target
        request->send(response);
    });
    
    // Set LED mode: solid (0), rainbow (1), etc.
    server.on("/led/onoff", HTTP_POST, [](AsyncWebServerRequest *request){},
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
        StaticJsonDocument<64> doc;
        DeserializationError err = deserializeJson(doc, data);
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
        DeserializationError err = deserializeJson(doc, data);
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

      if (NomadSD.cardType() == CARD_NONE) {
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
        request->send(409, "text/plain", "A full SD scan is running - try again once it finishes");
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
      // (the boot enqueue only runs on detected changes now, cant rely on it)
      BaseType_t tr = xTaskCreatePinnedToCore(immediateEnqueueTopLevelTask, "ImmediateEnq", 6 * 1024, NULL, 1, NULL, 1);
      if (tr == pdPASS) {
        webLog("[ADMIN] Starting immediate index task", "info");
      } else {
        webLog("[ADMIN] Failed to start immediate index task", "error");
      }

      if (NomadSD.exists("/generate_once.flag")) {
        NomadSD.remove("/generate_once.flag");
        webLog("[ADMIN] Removed legacy generate_once.flag file", "info");
      }

      request->send(200, "text/plain", "Indexing queued; background task will run it.");
      webLog("[ADMIN] Index queued for background processing", "info");
    });

    // ---------------- static + Archive handlers ----------------

    // Make sure OPTIONS preflight for asset routes will not block
    server.on("^\\/assets\\/.*$", HTTP_OPTIONS, [](AsyncWebServerRequest *request){
      AsyncWebServerResponse *resp = request->beginResponse(200, "text/plain", "");
      resp->addHeader("Access-Control-Allow-Origin", "*");
      resp->addHeader("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS");
      resp->addHeader("Access-Control-Allow-Headers", "Range, Content-Type, Accept");
      request->send(resp);
    });

    // Note: /Archive/* GET/HEAD/etc. is handled by the single HTTP_ANY route registered below,
    // which delegates to handleRangeRequest and fully covers GET.

    server.on("/debug/sdexists", HTTP_GET, [](AsyncWebServerRequest *request){
      if (!request->hasParam("path")) {
        request->send(400, "text/plain", "supply ?path=/assets/.. or /Archive/..");
        return;
      }
      String p = request->getParam("path")->value();
      bool exist = NomadSD.exists(p.c_str());
      String out = String("{\"path\":\"") + p + String("\",\"exists\":") + (exist ? "true" : "false") + "}";
      request->send(200, "application/json", out);
    });

    server.on("/assets/check", HTTP_GET, [](AsyncWebServerRequest *request){
      // query param ?file=/assets/... 
      if (request->hasParam("file")) {
        String p = request->getParam("file")->value();
        bool ok = NomadSD.exists(p);
        String res = String("{\"file\":") + "\"" + p + "\"" + ",\"exists\":" + (ok?"true":"false") + "}";
        request->send(200, "application/json", res);
      } else {
        request->send(400, "text/plain", "use ?file=/assets/..");
      }
    });
    // Advanced-content manifest (ZIMs today; ROMs/map tiles later (hopes and prayers)).
    // /zim-list is kept as a legacy alias for the same data.
    server.on("/api/archive-list", HTTP_GET, handleArchiveList);
    server.on("/zim-list", HTTP_GET, handleArchiveList);
    server.on("/api/games-list", HTTP_GET, handleGamesList);
    server.on("/api/maps-list", HTTP_GET, handleMapsList);

    // Multiplayer (HTTP polling; see the MpRoom store + handlers above for the design notes).
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
   // ---------- Serve /assets/* with correct MIME types & headers ----------
    server.on("/assets/*", HTTP_GET, [](AsyncWebServerRequest *request){
      String url = request->url(); // e.g. "/assets/foliate/vendor/pdf.mjs"
      if (url.length() == 0) { request->send(400); return; }

      const String sdPath = url;

      if (!NomadSD.exists(sdPath.c_str())) {
        Serial.printf("[ASSETS] not found: %s\n", sdPath.c_str());
        request->send(404, "text/plain", "not found");
        return;
      }

      String mime = mimeForPath(sdPath); // call the top-level helper
      AsyncWebServerResponse *resp = request->beginResponse(NomadSD, sdPath.c_str(), mime.c_str());

      // Helpful headers for libraries (PDF.js, wasm, fonts, etc.)
      resp->addHeader("Access-Control-Allow-Origin", "*");
      resp->addHeader("Accept-Ranges", "bytes");
      resp->addHeader("Cache-Control", "public, max-age=600");
      request->send(resp);
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

    // Note: /Books/* GET/HEAD/etc. is handled by the single HTTP_ANY route registered below,
    // which delegates to handleRangeRequest and fully covers GET.

    // Captive triggers for Apple & Android devices
    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *request) {
        Serial.println("Apple captive portal request detected, serving appleindex.html");
        request->send(NomadSD, "/appleindex.html", "text/html");
    });
    
    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request) {
        Serial.println("Android/NORMAL captive portal request detected, serving index.html");
        request->send(NomadSD, "/index.html", "text/html");
    });
    server.on("/dlna/desc.xml", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(200, "text/xml", R"rawliteral(
        <?xml version="1.0"?>
        <root xmlns="urn:schemas-upnp-org:device-1-0">
          <specVersion>
            <major>1</major>
            <minor>0</minor>
          </specVersion>
          <device>
            <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>
            <friendlyName>Nomad Media Server</friendlyName>
            <manufacturer>Jcorp</manufacturer>
            <modelName>Nomad</modelName>
            <UDN>uuid:ESP32-DLNA-FAKE-1234</UDN>
          </device>
        </root>
      )rawliteral");
    });

    server.on("/dlna/contentdir.xml", HTTP_GET, [](AsyncWebServerRequest *request){
      AsyncResponseStream *stream = request->beginResponseStream("text/xml");
      stream->print("<?xml version=\"1.0\"?><ContentDirectory>");
      File root = NomadSD.open("/Movies");
      if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
          if (!file.isDirectory()) {
            // escape simple XML-critical characters (very small cost)
            String name = String(file.name());
            stream->print("<item><title>");
            stream->print(name);
            stream->print("</title><res protocolInfo=\"http-get:*:video/mp4:*\">");
            // URL encode minimal chars, safer to include raw name only if it contains no spaces
            stream->print("http://192.168.4.1/Movies/");
            stream->print(name);
            stream->print("</res></item>");
          }
          file.close();
          file = root.openNextFile();
          yield(); // keep watchdog happy
        }
      }
      stream->print("</ContentDirectory>");
      request->send(stream);
    });
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
        
        // Check if it's a file request (has extension or specific paths)
        if (url.indexOf('.') > 0 || url.startsWith("/Gallery") || url.startsWith("/Files") ||
            url.startsWith("/Movies") || url.startsWith("/Music") || url.startsWith("/Books") ||
            url.startsWith("/Shows") || url.startsWith("/Archive") || url.startsWith("/Games") ||
            url.startsWith("/Maps")) {

            // ex-serveStatic buckets: route them through handleRangeRequest so reads
            // stay under sdMutex. Only these two - root pages must serve during indexing.
            if (url.startsWith("/Gallery/") || url.startsWith("/Files/")) {
                handleRangeRequest(request);
                return;
            }

            // Handle as file request with SD mutex protection
            String filePath = url;
            if (!filePath.startsWith("/")) filePath = "/" + filePath;
            
            // Take SD mutex before any SD operations
            if (sdMutex) {
                if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
                    request->send(503, "text/plain", "SD busy");
                    return;
                }
            }
            
            auto releaseSd = [&](){
                if (sdMutex) xSemaphoreGive(sdMutex);
            };
            // Check if file exists
                          
            bool fileExists = NomadSD.exists(filePath);

            if (!fileExists) {
                releaseSd();
                
                // Try index.html for directory requests
                if (!filePath.endsWith("/")) filePath += "/";
                filePath += "index.html";
                
                if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
                    request->send(503, "text/plain", "SD busy");
                    return;
                }
                
                bool indexExists = NomadSD.exists(filePath);

                if (!indexExists) {
                    releaseSd();
                    request->send(404, "text/plain", "File not found");
                    return;
                }
            }
            
            // Serve the file
            String mime = getMimeType(filePath);
            AsyncWebServerResponse *response = request->beginResponse(NomadSD, filePath, mime);
            releaseSd();
            
            response->addHeader("Accept-Ranges", "bytes");
            response->addHeader("Cache-Control", "public, max-age=600");
            request->send(response);
            return;
        }
    
    // Handle captive portal redirects for non-file requests
    if (userAgent.length()) {
        if (userAgent.indexOf("iPhone") >= 0 || userAgent.indexOf("iPad") >= 0 || userAgent.indexOf("Macintosh") >= 0) {
            request->send(NomadSD, "/appleindex.html", "text/html");
            return;
        }
    }
    
    request->send(NomadSD, "/index.html", "text/html");
});
    // /Gallery and /Files were serveStatic mounts - the only SD reads with no
    // sdMutex, no busy-503 and no OOM guard. Now handled in onNotFound instead.
server.on(
  "/upload", HTTP_POST,
  // Final response when upload is complete
  [](AsyncWebServerRequest *request) {
    // Response is handled during final chunk or error
  },
  // Upload handler
  [](AsyncWebServerRequest *request, const String &filename, size_t index,
     uint8_t *data, size_t len, bool final) {

    static std::map<AsyncWebServerRequest *, File> uploads;

    // Begin upload
    if (index == 0) {
      String dir = "/";
      if (request->hasParam("dir", true)) {
        dir = request->getParam("dir", true)->value();
      }

      if (!dir.startsWith("/")) dir = "/" + dir;
      if (dir.endsWith("/")) dir.remove(dir.length() - 1);

      String fullPath = dir + "/" + filename;

      // Check for duplicate
      if (NomadSD.exists(fullPath)) {
        Serial.println("[Upload] Duplicate file detected: " + fullPath);
        request->send(409, "application/json", "{\"error\":\"File already exists\"}");
        return;
      }

      // Ensure directory exists
      int slashPos = fullPath.lastIndexOf('/');
      if (slashPos != -1) {
        String folder = fullPath.substring(0, slashPos);
        if (!NomadSD.exists(folder)) {
          NomadSD.mkdir(folder);
        }
      }

      File f = NomadSD.open(fullPath, FILE_WRITE);
      if (!f) {
        webLogf("error", "Upload failed to open file: %s", fullPath.c_str());
        Serial.println("[Upload] Failed to open file: " + fullPath);
        request->send(500, "application/json", "{\"error\":\"Failed to open file\"}");
        return;
      }

      uploads[request] = f;
      Serial.println("[Upload] Started: " + fullPath);
    }

    // Continue writing data
    if (uploads.count(request)) {
      uploads[request].write(data, len);
    }

    // Finalize upload
    if (final && uploads.count(request)) {
      uploads[request].close();
      uploads.erase(request);
      Serial.println("[Upload] Finished");
      request->send(200, "application/json", "{\"status\":\"Upload successful\"}");
    }
  }
);

server.on("/list-assets", HTTP_GET, [](AsyncWebServerRequest *request){
  if (!request->hasParam("dir")) {
    request->send(400, "application/json", "{\"error\":\"Missing dir\"}");
    return;
  }

  String dir = request->getParam("dir")->value();
  File d = NomadSD.open(dir);
  if (!d || !d.isDirectory()) {
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

    // Remove trailing slash, if present
    if (dirName.endsWith("/")) {
        dirName.remove(dirName.length() - 1);
    }

    // Check if the path already exists
    if (NomadSD.exists(dirName)) {
        request->send(409, "text/plain", "Directory already exists");
        return;
    }

    // Attempt to create the directory
    if (NomadSD.mkdir(dirName)) {
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
server.on("^\\/Books\\/.*$", HTTP_ANY, [](AsyncWebServerRequest *request){
  Serial.printf("[BOOKS ROUTE ANY] delegating to handleRangeRequest for %s (method=%d)\n", request->url().c_str(), request->method());
  handleRangeRequest(request);
});
server.on("^\\/Archive\\/.*$", HTTP_ANY, [](AsyncWebServerRequest *request){
  Serial.printf("[ARCHIVE ROUTE ANY] delegating to handleRangeRequest for %s (method=%d)\n", request->url().c_str(), request->method());
  handleRangeRequest(request);
});
// NOTE: the regex routes above need ASYNCWEBSERVER_REGEX, which this build doesn't
// define - they never match, so /Books and /Archive fall through to onNotFound.


server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
  Serial.println("[SAVE] Request received");

  if (!request->hasParam("filename", true) || !request->hasParam("content", true)) {
    return request->send(400, "text/plain", "Missing parameters");
  }

  String path = request->getParam("filename", true)->value();
  String content = request->getParam("content", true)->value();

  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.println("[SAVE] Mutex timeout");
    return request->send(503, "text/plain", "SD card busy");
  }

  // ATOMIC WRITE: Use temporary file pattern
  String tempPath = path + ".tmp." + String(millis());

  Serial.printf("[SAVE] Atomic write: %s -> %s\n", tempPath.c_str(), path.c_str());

  // Step 1: Write to temporary file
  File tempFile = NomadSD.open(tempPath, FILE_WRITE);
  if (!tempFile) {
    if (sdMutex) xSemaphoreGive(sdMutex);
    return request->send(500, "text/plain", "Cannot create temp file");
  }

  size_t bytesWritten = tempFile.write((const uint8_t*)content.c_str(), content.length());
  tempFile.flush();
  tempFile.close();

  // Step 2: Force filesystem sync (ESP32 specific)
  vTaskDelay(pdMS_TO_TICKS(100));  // Allow OS buffer flush

  // Step 3: Verify temp file was written correctly
  File verifyFile = NomadSD.open(tempPath, FILE_READ);
  if (!verifyFile || verifyFile.size() != content.length()) {
    if (verifyFile) verifyFile.close();
    NomadSD.remove(tempPath);
    if (sdMutex) xSemaphoreGive(sdMutex);
    return request->send(500, "text/plain", "Write verification failed");
  }
  verifyFile.close();

  // Step 4: Atomic rename (temp -> final)
  if (NomadSD.exists(path)) {
    NomadSD.remove(path);
    vTaskDelay(pdMS_TO_TICKS(50));  // Allow cleanup
  }

  if (!NomadSD.rename(tempPath, path)) {
    NomadSD.remove(tempPath);  // Cleanup temp file
    if (sdMutex) xSemaphoreGive(sdMutex);
    return request->send(500, "text/plain", "Atomic rename failed");
  }

  // Step 5: Final verification
  vTaskDelay(pdMS_TO_TICKS(100));
  if (!NomadSD.exists(path)) {
    if (sdMutex) xSemaphoreGive(sdMutex);
    return request->send(500, "text/plain", "Final verification failed");
  }

  if (sdMutex) xSemaphoreGive(sdMutex);

  Serial.printf("[SAVE] Atomic write successful: %s (%d bytes)\n", path.c_str(), content.length());
  webLogf("info", "File saved: %s (%d bytes)", path.c_str(), content.length());
  request->send(200, "text/plain", "OK");
});


server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
  StaticJsonDocument<512> doc;
  doc["rgbMode"] = settings.rgbMode;
  doc["rgbColor"] = settings.rgbColor;
  doc["adminPassword"] = settings.adminPassword;
  doc["wifiSSID"] = settings.wifiSSID;
  doc["wifiPassword"] = settings.wifiPassword;
  doc["brightness"] = settings.brightness;
  doc["autoGenerateMedia"] = settings.autoGenerateMedia;
  doc["flipScreen"] = settings.flipScreen;


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
  if (doc.containsKey("adminPassword")) {
    settings.adminPassword = doc["adminPassword"].as<String>();
    adminSessionToken = ""; // password changed/cleared - existing sessions must re-authenticate
  }
  if (doc.containsKey("wifiSSID")) settings.wifiSSID = doc["wifiSSID"].as<String>();
  if (doc.containsKey("wifiPassword")) settings.wifiPassword = doc["wifiPassword"].as<String>();
  if (doc.containsKey("brightness")) settings.brightness = constrain(doc["brightness"].as<int>(), 0, 100);
  if (doc.containsKey("autoGenerateMedia")) settings.autoGenerateMedia = doc["autoGenerateMedia"].as<bool>();
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

// POST /auth/login - verify the SHA-256 password hash and issue a session token.
server.on("/auth/login", HTTP_POST, [](AsyncWebServerRequest *request){
  if (!isAdminAuthRequired()) {
    adminSessionToken = generateSessionToken();
    request->send(200, "application/json", "{\"token\":\"" + adminSessionToken + "\"}");
    return;
  }
  if (!request->hasParam("body", true)) {
    request->send(400, "application/json", "{\"error\":\"Missing body\"}");
    return;
  }
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, request->getParam("body", true)->value());
  if (error) {
    request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  String hash = doc["hash"] | "";
  if (hash.length() == 0 || !hash.equals(settings.adminPassword)) {
    request->send(401, "application/json", "{\"error\":\"Invalid password\"}");
    return;
  }
  adminSessionToken = generateSessionToken();
  request->send(200, "application/json", "{\"token\":\"" + adminSessionToken + "\"}");
});

// POST /auth/logout - drop the current session token.
server.on("/auth/logout", HTTP_POST, [](AsyncWebServerRequest *request){
  adminSessionToken = "";
  request->send(200, "application/json", "{\"status\":\"ok\"}");
});

  server.on("/admin-status", HTTP_GET, [](AsyncWebServerRequest *request){
    // Build JSON
    StaticJsonDocument<256> doc;
    doc["ssid"]         = settings.wifiSSID;
    doc["wifiPassword"] = settings.wifiPassword;
    doc["users"]        = getConnectedUserCount();

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
    StaticJsonDocument<2048> doc;
    JsonArray logs = doc.createNestedArray("logs");

    // hold the lock for the whole copy - a webLog() realloc mid-copy would corrupt the heap.
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
        // Clamp to Set_Backlight's valid 0-100 range; out-of-range values are
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
      NomadSD.end();
      
      // Small delay to ensure response is sent
      delay(1000);
      
      // Enter deep sleep mode
      Serial.println("Entering deep sleep...");
      esp_deep_sleep_start();
  });
  // 2) Restart the device
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

    // Ensure index directory exists
    ensureIndexDir();

    // If index exists, serve it
    if (NomadSD.exists(fullPath)) {
      AsyncWebServerResponse *resp = request->beginResponse(NomadSD, fullPath, "application/x-ndjson");
      resp->addHeader("Cache-Control", "no-cache, no-store");
      request->send(resp);
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

    // Ensure index directory exists
    ensureIndexDir();

    // If index exists already, serve it as JSON
    if (NomadSD.exists(fullPath)) {
      File file = NomadSD.open(fullPath, FILE_READ);
      if (file) {
        AsyncResponseStream *stream = request->beginResponseStream("application/json");
        stream->addHeader("Cache-Control", "no-cache, no-store");
        stream->printf("{\"status\":\"ready\",\"path\":\"%s\",\"entries\":[", path.c_str());

        bool headerSkipped = false;
        bool first = true;
        while (file.available()) {
          String line = file.readStringUntil('\n');
          line.trim();
          if (line.length() == 0) continue;
          if (!headerSkipped) {
            headerSkipped = true;
            continue;
          }
          if (!first) stream->print(",");
          stream->print(line);
          first = false;
        }
        file.close();

        stream->print("]}");
        request->send(stream);
        return;
      }
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

    if (indexingInProgress) {
      request->send(409, "application/json", "{\"error\":\"Index already in progress\"}");
      webLog("[ADMIN] Reindex request ignored - indexing already in progress", "warning");
      return;
    }
    if (sdScanInProgress) {
      request->send(409, "application/json", "{\"error\":\"A full SD scan is running - try again once it finishes\"}");
      webLog("[ADMIN] Reindex request ignored - full SD scan in progress", "warning");
      return;
    }

    String path = "/";
    if(request->hasParam("path", true)) path = request->getParam("path", true)->value();     // from POST body
    else if(request->hasParam("path")) path = request->getParam("path")->value();            // from query
    path = normalizePath(path);

    // respond immediately
    String j = "{\"status\":\"accepted\",\"path\":\"" + path + "\"}";
    request->send(202, "application/json", j);

    // spawn FreeRTOS task to do reindex
    String *arg = new String(path);
    BaseType_t ok = xTaskCreatePinnedToCore([](void *pv){
      String p = *((String*)pv);
      delete (String*)pv;
      Serial.printf("[ReindexTask] start %s\n", p.c_str());
      webLogf("info", "Reindex of '%s' requested", p.c_str());

      indexingInProgress = true;
      indexingTasksActive = true;
      if (indexingPathMutex && xSemaphoreTake(indexingPathMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        currentIndexingPath = p;
        g_currentBucketNum = 0;
        g_totalBucketsForProgress = 0;
        g_indexProgressPercent = 0;
        xSemaphoreGive(indexingPathMutex);
      }

      // Shown immediately so there's no dead screen while the directory is opened;
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
      vTaskDelete(NULL);
    }, "ReindexTask", 12*1024, arg, 1, NULL, 1);

    if(ok != pdPASS){
      Serial.println("[/api/reindex] task create failed");
      delete arg;
    }
  });
  // fast path: reads NomadSD's allocation metadata instead of walking files, so it
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

    // dont walk the whole card while indexing - two heavy SD tasks under low heap was
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
      doc["message"] = "A full scan ran recently - please wait before running another";
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

    // sdScanTask logs a real uxTaskGetStackHighWaterMark() on exit so this size gets
    // verified on-device rather than assumed.
    uint32_t stackSize = 12 * 1024;
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap > 35000) {
      stackSize = 20 * 1024;
    } else {
      Serial.printf("[API] Heap too low for full-size deep-scan stack (%u bytes), using smaller stack\n", freeHeap);
    }

    BaseType_t ok = xTaskCreatePinnedToCore(sdScanTask, "ManualSDScan", stackSize, NULL, 1, NULL, 1);

    if (ok != pdPASS) {
      webLog("[API] SD scan task creation failed", "error");
    }
  });
  server.on("/api/comic-pages", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->hasParam("path")) {
      request->send(400, "application/json", "{\"error\":\"Missing path parameter\"}");
      return;
    }

    String path = normalizePath(request->getParam("path")->value());

    if (!path.startsWith("/Books/") || !isComicFolder(path)) {
      request->send(400, "application/json", "{\"error\":\"Not a comic folder\"}");
      return;
    }

    // Check heap before processing
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < 50000) {
      Serial.printf("[ComicPages] Low heap (%u bytes), rejecting request for: %s\n", freeHeap, path.c_str());
      request->send(503, "application/json", "{\"error\":\"Low memory, please retry\"}");
      return;
    }

    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
      Serial.printf("[ComicPages] Mutex timeout for: %s\n", path.c_str());
      request->send(503, "application/json", "{\"error\":\"SD busy, please retry\"}");
      return;
    }

    File d = NomadSD.open(path);
    if (!d || !d.isDirectory()) {
      if (d) d.close();
      if (sdMutex) xSemaphoreGive(sdMutex);
      request->send(404, "application/json", "{\"error\":\"Folder not found\"}");
      return;
    }

    std::vector<String> pages;
    pages.reserve(100);
    const size_t MAX_PAGES = 1000;

    d.rewindDirectory();
    File e;
    int itemCount = 0;
    while ((e = d.openNextFile())) {
      if (pages.size() >= MAX_PAGES) {
        Serial.printf("[ComicPages] Hit max page limit (%d) for: %s\n", MAX_PAGES, path.c_str());
        e.close();
        break;
      }

      if (!e.isDirectory()) {
        String name = String(e.name());
        int lastSlash = name.lastIndexOf('/');
        if (lastSlash >= 0) name = name.substring(lastSlash + 1);

        String lower = name;
        lower.toLowerCase();
        if (lower.endsWith(".png") || lower.endsWith(".jpg") ||
            lower.endsWith(".jpeg") || lower.endsWith(".webp") ||
            lower.endsWith(".gif") || lower.endsWith(".bmp")) {
          pages.push_back(name);
        }
      }
      e.close();

      itemCount++;
      if (itemCount % 20 == 0) {
        yield();
      }
    }
    d.close();

    if (sdMutex) xSemaphoreGive(sdMutex);

    // Sort pages after releasing mutex
    std::sort(pages.begin(), pages.end());

    Serial.printf("[ComicPages] Streaming %d pages for: %s (heap: %u)\n", pages.size(), path.c_str(), ESP.getFreeHeap());

    // Use AsyncResponseStream for chunked response - no memory buildup
    AsyncResponseStream *stream = request->beginResponseStream("application/json");
    stream->print("{\"path\":\"");
    stream->print(jsonEscape(path));
    stream->print("\",\"pages\":[");

    for (size_t i = 0; i < pages.size(); i++) {
      if (i > 0) stream->print(",");
      stream->print("\"");
      stream->print(jsonEscape(pages[i]));
      stream->print("\"");

      if (i % 10 == 0) {
        yield();
      }
    }

    stream->print("]}");
    request->send(stream);

    Serial.printf("[ComicPages] Response sent for: %s\n", path.c_str());
  });

  // GET /api/books -> List all CBZ/CBR files in /Books directory recursively
  server.on("/api/books", HTTP_GET, [](AsyncWebServerRequest *request){
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
      request->send(503, "application/json", "{\"error\":\"SD busy\"}");
      return;
    }

    AsyncResponseStream *stream = request->beginResponseStream("application/json");
    stream->print("{\"books\":[");

    std::vector<String> dirStack;
    dirStack.push_back("/Books");
    bool first = true;
    int bookCount = 0;
    const int MAX_BOOKS = 1000;

    while (!dirStack.empty() && bookCount < MAX_BOOKS) {
      String currentPath = dirStack.back();
      dirStack.pop_back();

      if (!NomadSD.exists(currentPath)) continue;

      File dir = NomadSD.open(currentPath);
      if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        continue;
      }

      File entry;
      while ((entry = dir.openNextFile())) {
        String entryPath = String(entry.path());
        String entryName = String(entry.name());

        if (entry.isDirectory()) {
          dirStack.push_back(entryPath);
        } else {
          String lower = entryName;
          lower.toLowerCase();
          if (lower.endsWith(".cbz") || lower.endsWith(".cbr")) {
            if (!first) stream->print(",");

            stream->print("{");
            stream->print("\"path\":\"");
            stream->print(jsonEscape(entryPath));
            stream->print("\",\"name\":\"");
            stream->print(jsonEscape(entryName));
            stream->print("\",\"size\":");
            stream->print(entry.size());
            stream->print("}");

            first = false;
            bookCount++;

            if (bookCount >= MAX_BOOKS) {
              entry.close();
              break;
            }
          }
        }
        entry.close();

        if (bookCount % 10 == 0) {
          yield();
        }
      }
      dir.close();

      yield();
    }

    stream->print("],\"count\":");
    stream->print(bookCount);
    stream->print("}");

    if (sdMutex) xSemaphoreGive(sdMutex);

    request->send(stream);
    Serial.printf("[API Books] Listed %d CBZ/CBR files\n", bookCount);
  });

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
    json += "\"tasks\":{";
    json += "\"indexWorker\":" + String(indexWorkerTaskHandle != nullptr ? "true" : "false") + ",";
    json += "\"storageMonitor\":" + String(storageMonitorTaskHandle != nullptr ? "true" : "false");
    json += "}";
    json += "}";

    request->send(200, "application/json", json);
  });
  server.on("/flash-mode", HTTP_POST, [](AsyncWebServerRequest *request){
      if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
      Serial.println(">>> /flash-mode handler hit");
      request->send(200, "text/plain", "OK: attempting to enter ROM download (flash) mode...");

      // Give the HTTP response a moment to flush
      delay(80);

      Serial.println(">>> Preparing display to show FLASH mode message...");
      NomadUI_Init();
      btStop(); // stop bluetooth tasks if applicable (dont use it for anything, give wifi full control of antenna)

      NomadUI_Message("Flashing Mode\nReady for Update");
      // Give LVGL a few cycles to flush to the screen so the user sees the message
      NomadUI_Flush();
      NomadUI_Flush();

  #if defined(ARDUINO_ARCH_ESP32)
      Serial.println(">>> Writing force-download flag and restarting (RTC_CNTL_FORCE_DOWNLOAD_BOOT).");
      REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
      esp_restart(); // low-level restart into ROM download mode
  #else
      Serial.println(">>> Platform fallback: set_boot_mode(FLASH_MODE) and restart.");
      set_boot_mode(FLASH_MODE);
      ESP.restart();
  #endif
    });
  server.on("/enterUsb", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    Serial.println(">>> /enterUsb handler hit");
    request->send(200, "text/plain", "OK: entering USB MSC mode...");
    delay(200);                 
    set_boot_mode(USB_MODE);
    ESP.restart();             
  });


// ─── USB-mode switch is handled by handleBootButton(), polled from loop() ───
// A bare FALLING interrupt rebooted from ISR context and fired on the
// slightest contact bounce, dropping the server mid-stream.
// Start the web server
  server.begin();
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
        dnsServer.processNextRequest();

        if (lcdRotatePending) {
          lcdRotatePending = false;
          LCD_SetRotation180(settings.flipScreen);
          lv_obj_invalidate(lv_scr_act());  // panel RAM shows mirrored until fully repainted
        }
        Timer_Loop();

        // RGB updates are time-sensitive for visual smoothness
        if (currentLEDMode == 1) {
          RGB_Lamp_Loop(20);   // hue step every 20 ms
        }


        vTaskDelay(pdMS_TO_TICKS(10)); // ~100Hz servicing; yields CPU to other tasks
      }
      vTaskDelete(NULL);
    }, "StreamingTask", 8 * 1024, NULL, 2, NULL, 1);
    if (t == pdPASS) {
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

        static uint32_t lastStatsLocal = 0;
        if (now - lastStatsLocal > 1000) {
          refreshDeviceStats();
          lastStatsLocal = now;
        }

        lvglDrainQueue();

        updateClientCount();

        // Sleep a bit longer to reduce CPU usage of background tasks
        vTaskDelay(pdMS_TO_TICKS(200));
      }
      vTaskDelete(NULL);
    }, "UiTask", 8 * 1024, NULL, 1, NULL, 1); // was 6KB, bumped after a stack canary crash at boot
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
    BaseType_t r = xTaskCreatePinnedToCore(indexWorkerTask, "IndexWorker", 16 * 1024, NULL, 2, &indexWorkerTaskHandle, 0);
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
  BaseType_t br = xTaskCreatePinnedToCore(bootCoordinatorTask, "BootCoord", 8 * 1024, NULL, 1, NULL, 1);
  if (br != pdPASS) {
    Serial.println("[BootCoord] Failed to spawn Boot Coordinator");
  } else {
    Serial.println("[BootCoord] Task spawned");
  }
}
// ==================== MAIN LOOP ====================

void loop() {
    handleBootButton();
    if (bootButtonPressed) {
      bootButtonPressed = false;
      set_boot_mode(USB_MODE);
      ESP.restart();
    }
    dnsServer.processNextRequest();
    Timer_Loop();

    if (currentLEDMode == 1) {
        RGB_Lamp_Loop(20);   // hue step every 20 ms
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
static unsigned long lastSweepMs = 0;
void memorySweepIfNeeded() {
  unsigned long now = millis();
  if (now - lastSweepMs < 5000) return; // run every 5s
  lastSweepMs = now;

  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < 70000) {  //might tune this a bit 
    Serial.printf("[MEMSWEEP] low freeHeap=%u; doing sweep\n", (unsigned)freeHeap);
    yield();
  }
}

// ─── Boot button ──────────────────────────────────────────────────────────
// Tap  : next on-screen page (connection / system / storage)
// Hold : reboot into USB mass-storage mode
void handleBootButton() {
    switch (NomadButton_Poll()) {
        case NOMAD_BTN_SHORT:
            NomadUI_NextPage();
            break;

        case NOMAD_BTN_LONG:
            webLog("[SYSTEM] Boot button held - switching to USB mass storage", "info");
            NomadUI_Message("Switching to\nUSB Mass-Storage...");
            NomadUI_Flush();
            set_boot_mode(USB_MODE);
            delay(150);
            esp_restart();
            break;

        default:
            break;
    }
}

// Feed the on-device system page. Cheap enough to run once a second.
void refreshDeviceStats() {
    NomadUI_SetSysStats((uint32_t)(ESP.getFreeHeap() / 1024),
                        (uint32_t)(ESP.getFreePsram() / 1024),
                        currentTempC,
                        (uint32_t)(millis() / 1000UL));
}
