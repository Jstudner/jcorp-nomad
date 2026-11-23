// NomadUsage.h - what got used, for the admin page's Overview: phones at the busiest
// moment, data sent, plays, the most played files and which pages people open. On the
// device only, nothing leaves it.
//
// Built to cost next to nothing: a page served adds one to a counter, a new viewer of a
// file (Activity already sees that) adds one to that file's count, data sent is the media
// fill counter that already exists plus the size of each page file, and the number of
// phones is looked at every 30 s from loop(). Saved through the config writer to
// /config/usage.json at most every 10 minutes, and only when something changed. Nomad has
// no clock, so there are two views: since this start, and all time.
//
//   GET  /api/usage          {"now":{...},"all":{...},"top":[[path,plays],...],"pages":{...}}  (admin)
//   POST /api/usage/reset    start counting again from zero (admin)
#ifndef NOMAD_USAGE_H
#define NOMAD_USAGE_H
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

bool queueConfigWrite(const String &path, const String &content, bool createOnly);
int getConnectedUserCount();

static const char *USAGE_PATH = "/config/usage.json";
static const int USAGE_TOP = 20;
static const int USAGE_PAGES = sizeof(kMenuPages) / sizeof(kMenuPages[0]);
static const uint32_t USAGE_SAVE_MS = 10UL * 60 * 1000;

struct UsageTop { char path[120]; uint32_t plays; };
struct UsageCounts {
  uint32_t peak, plays;
  uint64_t staticBytes;
  uint32_t pages[USAGE_PAGES];
};
static UsageCounts g_useNow = {};        // since this start
static UsageCounts g_useBefore = {};     // all time, as saved before this start
static uint32_t g_useBoots = 0;
// How every start so far began, kept with the totals: a unit that restarts on its own shows
// here even after it has been unplugged (the support file's own reason is this start's only).
enum { USE_RST_CRASH = 0, USE_RST_WATCHDOG, USE_RST_BROWNOUT, USE_RST_POWER, USE_RST_RESTART, USE_RST_KINDS };
static uint32_t g_useResets[USE_RST_KINDS] = {0};
static int usageResetKind() {
  if (g_prevRunHung) return USE_RST_WATCHDOG;      // restarted by the loop/UI heartbeat (NomadSupport.h)
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC: return USE_RST_CRASH;
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return USE_RST_WATCHDOG;
    case ESP_RST_BROWNOUT: return USE_RST_BROWNOUT;
    case ESP_RST_POWERON: return USE_RST_POWER;
    default: return USE_RST_RESTART;
  }
}
static UsageTop *g_useTop = nullptr;     // all time, in PSRAM
static volatile bool g_useDirty = false;
static uint32_t g_useSampleMs = 0, g_useSavedMs = 0;
static portMUX_TYPE g_useMux = portMUX_INITIALIZER_UNLOCKED;

static inline void usageNotePage(int idx) {
  if (idx < 0 || idx >= USAGE_PAGES) return;
  portENTER_CRITICAL(&g_useMux); g_useNow.pages[idx]++; portEXIT_CRITICAL(&g_useMux);
  g_useDirty = true;
}
static inline void usageNoteSent(uint64_t bytes) {
  portENTER_CRITICAL(&g_useMux); g_useNow.staticBytes += bytes; portEXIT_CRITICAL(&g_useMux);
}
// a new device started this file (NomadActivity's new row): one play
static void usageNotePlay(const String &path) {
  if (!g_useTop) return;
  portENTER_CRITICAL(&g_useMux);
  g_useNow.plays++;
  int at = -1, low = 0;
  for (int i = 0; i < USAGE_TOP; i++) {
    if (g_useTop[i].plays && strcmp(g_useTop[i].path, path.c_str()) == 0) { at = i; break; }
    if (g_useTop[i].plays < g_useTop[low].plays) low = i;
  }
  if (at < 0 && path.length() < sizeof(g_useTop[0].path)) {
    // a new file takes the place of the least played, starting from just above it, so a
    // newcomer can climb without the table forgetting the favourites
    at = low;
    const uint32_t floor = g_useTop[low].plays;
    strlcpy(g_useTop[at].path, path.c_str(), sizeof(g_useTop[at].path));
    g_useTop[at].plays = floor;
  }
  if (at >= 0) g_useTop[at].plays++;
  portEXIT_CRITICAL(&g_useMux);
  g_useDirty = true;
}

static uint64_t usageMediaBytes() { return g_fillStats.bytes; }

static String usageJson(bool forFile) {
  UsageCounts now, before;
  portENTER_CRITICAL(&g_useMux); now = g_useNow; before = g_useBefore; portEXIT_CRITICAL(&g_useMux);
  now.peak = max(now.peak, (uint32_t)getConnectedUserCount());   // between samples, right now counts too
  JsonDocument d;
  d["v"] = 1;
  const uint64_t sent = now.staticBytes + usageMediaBytes();
  if (forFile) {
    // the file holds the all-time totals, this start included
    d["boots"] = g_useBoots;
    JsonArray rs = d["resets"].to<JsonArray>();
    for (int i = 0; i < USE_RST_KINDS; i++) rs.add(g_useResets[i]);
    d["peak"] = max(before.peak, now.peak);
    d["plays"] = before.plays + now.plays;
    d["bytes"] = before.staticBytes + sent;
    JsonArray pg = d["pages"].to<JsonArray>();
    for (int i = 0; i < USAGE_PAGES; i++) pg.add(before.pages[i] + now.pages[i]);
  } else {
    JsonObject n = d["now"].to<JsonObject>();
    n["up"] = millis() / 1000; n["peak"] = now.peak; n["plays"] = now.plays; n["bytes"] = sent;
    n["phones"] = getConnectedUserCount();
    JsonObject a = d["all"].to<JsonObject>();
    a["boots"] = g_useBoots; a["peak"] = max(before.peak, now.peak); a["plays"] = before.plays + now.plays;
    a["bytes"] = before.staticBytes + sent;
    JsonObject pg = d["pages"].to<JsonObject>();
    for (int i = 0; i < USAGE_PAGES; i++) pg[kMenuPages[i]] = before.pages[i] + now.pages[i];
  }
  JsonArray top = d["top"].to<JsonArray>();
  if (g_useTop) {
    // most played first
    int idx[USAGE_TOP];
    for (int i = 0; i < USAGE_TOP; i++) idx[i] = i;
    for (int a = 0; a < USAGE_TOP; a++) for (int b = a + 1; b < USAGE_TOP; b++)
      if (g_useTop[idx[b]].plays > g_useTop[idx[a]].plays) { int x = idx[a]; idx[a] = idx[b]; idx[b] = x; }
    for (int i = 0; i < USAGE_TOP; i++) {
      const UsageTop &t = g_useTop[idx[i]];
      if (!t.plays || !t.path[0]) continue;
      JsonArray row = top.add<JsonArray>();
      row.add(t.path); row.add(t.plays);
    }
  }
  String out;
  serializeJson(d, out);
  return out;
}

// setup(): the all-time totals so far, and one more start
static void nomadUsageLoad() {
  g_useTop = (UsageTop *)heap_caps_calloc(USAGE_TOP, sizeof(UsageTop), MALLOC_CAP_SPIRAM);
  File f = SD_MMC.open(USAGE_PATH, FILE_READ);
  if (f) {
    JsonDocument d;
    if (!deserializeJson(d, f)) {
      g_useBoots = d["boots"] | 0;
      JsonArray rs = d["resets"];
      for (int i = 0; i < USE_RST_KINDS && i < (int)rs.size(); i++) g_useResets[i] = rs[i] | 0;
      g_useBefore.peak = d["peak"] | 0;
      g_useBefore.plays = d["plays"] | 0;
      g_useBefore.staticBytes = d["bytes"] | (uint64_t)0;
      JsonArray pg = d["pages"];
      for (int i = 0; i < USAGE_PAGES && i < (int)pg.size(); i++) g_useBefore.pages[i] = pg[i] | 0;
      JsonArray top = d["top"];
      for (int i = 0; i < USAGE_TOP && i < (int)top.size() && g_useTop; i++) {
        strlcpy(g_useTop[i].path, top[i][0] | "", sizeof(g_useTop[i].path));
        g_useTop[i].plays = top[i][1] | 0;
      }
    }
    f.close();
  }
  g_useBoots++;
  g_useResets[usageResetKind()]++;
  g_useDirty = true;
}

// loop(): count the phones now and then, and save when it's time
static void nomadUsageTick() {
  const uint32_t now = millis();
  if (now - g_useSampleMs >= 30000) {
    g_useSampleMs = now;
    const uint32_t phones = getConnectedUserCount();
    if (phones > g_useNow.peak) { g_useNow.peak = phones; g_useDirty = true; }
  }
  // a start that was not a plain power-on is saved 20 s in: a unit that keeps crashing may
  // not last the 10 minutes to the first regular save
  static bool earlySaved = false;
  if (!earlySaved && now > 20000) {
    earlySaved = true;
    if (usageResetKind() != USE_RST_POWER && queueConfigWrite(USAGE_PATH, usageJson(true))) { g_useDirty = false; g_useSavedMs = now; }
  }
  if (g_useDirty && now - g_useSavedMs >= USAGE_SAVE_MS && now > 60000) {
    if (queueConfigWrite(USAGE_PATH, usageJson(true))) { g_useDirty = false; g_useSavedMs = now; }
  }
}

static void nomadUsageRegister(AsyncWebServer &server) {
  server.on("/api/usage", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    AsyncWebServerResponse *res = r->beginResponse(200, "application/json", usageJson(false));
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });
  server.on("/api/usage/reset", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    portENTER_CRITICAL(&g_useMux);
    g_useBefore = UsageCounts();
    g_useNow = UsageCounts();
    if (g_useTop) memset(g_useTop, 0, USAGE_TOP * sizeof(UsageTop));
    portEXIT_CRITICAL(&g_useMux);
    g_useBoots = 1;
    queueConfigWrite(USAGE_PATH, usageJson(true));
    g_useSavedMs = millis(); g_useDirty = false;
    webLogf("info", "Usage counts reset");
    r->send(200, "application/json", "{\"ok\":true}");
  });
}

#endif  // NOMAD_USAGE_H
