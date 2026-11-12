// NomadActivity.h - what is playing right now, for the admin page's Activity section, and
// a way to stop it.
//
// One row per device and file: handleRangeRequest notes every film, song and download
// reply as it goes out (nomadActNote) and the reply's end (nomadActGone, from its
// onDisconnect). A row keeps the reply's own read position, so the progress shown is
// what has actually been sent, a download included, not where the last request began.
//
//   GET  /api/activity              {"now":ms,"rows":[...]}                       (admin)
//   POST /api/activity/stop  id=    closes that device's replies for the file and
//                                   refuses it the file for ACT_BLOCK_MS          (admin)
//   POST /api/activity/allow id=    lifts that refusal                           (admin)
//
// Threading: every caller runs on the async_tcp task (the handlers, the reply
// disconnects), so the mutex is just a safety net. Stop closes the other
// connection from the admin's own request: AsyncClient::close() runs that reply's
// disconnect synchronously, which deletes it and lands back in nomadActGone, so the
// mutex is never held across a close.
#ifndef NOMAD_ACTIVITY_H
#define NOMAD_ACTIVITY_H
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <memory>

bool checkAdminAuth(AsyncWebServerRequest *request);

static const int ACT_MAX = 12;                       // rows kept
static const int ACT_LIVE = 4;                       // replies tracked per row
static const uint32_t ACT_KEEP_MS = 15UL * 60 * 1000;   // a row nobody touched for this long goes
static const uint32_t ACT_BLOCK_MS = 30UL * 60 * 1000;  // how long a stop holds
static const int ACT_BLOCKS = 8;

struct ActRow {
  uint32_t id = 0;              // 0: free
  uint32_t ip = 0;
  String path;
  uint64_t size = 0;
  uint64_t end = 0;             // the last byte any reply for this row was due to send
  uint32_t first = 0, last = 0;
  uint32_t replies = 0;
  bool download = false;
  char dev[10] = "";
  AsyncWebServerRequest *live[ACT_LIVE] = {};
  std::shared_ptr<uint64_t> pos;   // the newest reply's read position
  uint64_t samplePos = 0;          // for a rate between two looks
  uint32_t sampleMs = 0;
};
struct ActBlock { uint32_t ip = 0; String path; uint32_t until = 0; };

static void usageNotePlay(const String &path);   // NomadUsage.h: a new viewer of a file is a play

static ActRow g_act[ACT_MAX];
static ActBlock g_actBlock[ACT_BLOCKS];
static uint32_t g_actNextId = 1;
static SemaphoreHandle_t g_actMx = nullptr;

struct ActLock {
  bool ok;
  ActLock() { if (!g_actMx) g_actMx = xSemaphoreCreateMutex(); ok = g_actMx && xSemaphoreTake(g_actMx, pdMS_TO_TICKS(100)) == pdTRUE; }
  ~ActLock() { if (ok) xSemaphoreGive(g_actMx); }
};

static uint32_t actIp(AsyncWebServerRequest *r) {
  return r && r->client() ? (uint32_t)r->client()->remoteIP() : 0;
}

// a word for the device, from its User-Agent: enough to tell two viewers apart
static void actDevice(AsyncWebServerRequest *r, char *out, size_t n) {
  const String ua = r->hasHeader("User-Agent") ? r->header("User-Agent") : String();
  const char *k = "Device";
  if (ua.indexOf("iPhone") >= 0) k = "iPhone";
  else if (ua.indexOf("iPad") >= 0) k = "iPad";
  else if (ua.indexOf("Tizen") >= 0 || ua.indexOf("Web0S") >= 0 || ua.indexOf("webOS") >= 0 || ua.indexOf("SMART-TV") >= 0 ||
           ua.indexOf("Roku") >= 0 || ua.indexOf("AppleTV") >= 0 || ua.indexOf("BRAVIA") >= 0 || ua.indexOf("DLNA") >= 0) k = "TV";
  else if (ua.indexOf("VLC") >= 0 || ua.indexOf("LibVLC") >= 0) k = "VLC";
  else if (ua.indexOf("Kodi") >= 0) k = "Kodi";
  else if (ua.indexOf("Android") >= 0) k = "Android";
  else if (ua.indexOf("Windows") >= 0) k = "Windows";
  else if (ua.indexOf("Macintosh") >= 0) k = "Mac";
  else if (ua.indexOf("CrOS") >= 0) k = "Chromebook";
  else if (ua.indexOf("Linux") >= 0) k = "Linux";
  strlcpy(out, k, n);
}

static void actPrune(uint32_t now) {
  for (auto &a : g_act) {
    if (!a.id) continue;
    bool live = false;
    for (auto *l : a.live) if (l) live = true;
    if (!live && now - a.last > ACT_KEEP_MS) a = ActRow();
  }
  for (auto &b : g_actBlock) if (b.until && (int32_t)(now - b.until) >= 0) b = ActBlock();
}

// true when the admin stopped this device on this file
static bool nomadActBlocked(AsyncWebServerRequest *r, const String &path) {
  const uint32_t ip = actIp(r), now = millis();
  ActLock lk;
  if (!lk.ok) return false;
  for (auto &b : g_actBlock)
    if (b.until && (int32_t)(now - b.until) < 0 && b.ip == ip && b.path == path) return true;
  return false;
}

// a reply going out, returns the row's id for nomadActGone
static uint32_t nomadActNote(AsyncWebServerRequest *r, const String &path, uint64_t size, uint64_t end,
                             bool download, std::shared_ptr<uint64_t> pos) {
  const uint32_t ip = actIp(r), now = millis();
  ActLock lk;
  if (!lk.ok) return 0;
  actPrune(now);
  ActRow *row = nullptr, *freeRow = nullptr, *oldest = nullptr;
  for (auto &a : g_act) {
    if (a.id && a.ip == ip && a.path == path) { row = &a; break; }
    if (!a.id && !freeRow) freeRow = &a;
    if (a.id && (!oldest || a.last < oldest->last)) oldest = &a;
  }
  if (!row) {
    row = freeRow ? freeRow : oldest;   // full: the one quiet longest makes room
    *row = ActRow();
    row->id = g_actNextId++;
    if (!g_actNextId) g_actNextId = 1;
    row->ip = ip;
    row->path = path;
    row->first = now;
    actDevice(r, row->dev, sizeof(row->dev));
    usageNotePlay(path);
  }
  row->size = size;
  row->end = end;
  row->last = now;
  row->replies++;
  row->download = row->download || download;
  row->pos = pos;
  for (auto &l : row->live) if (!l) { l = r; break; }
  return row->id;
}

// a reply ended
static void nomadActGone(uint32_t id, AsyncWebServerRequest *r) {
  if (!id) return;
  ActLock lk;
  if (!lk.ok) return;
  for (auto &a : g_act) {
    if (a.id != id) continue;
    for (auto &l : a.live) if (l == r) l = nullptr;
    a.last = millis();
    if (a.pos && *a.pos > a.end + 1) a.end = *a.pos - 1;
    break;
  }
}

static void nomadActRegister(AsyncWebServer &server) {
  server.on("/api/activity", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    const uint32_t now = millis();
    DynamicJsonDocument doc(6144);
    doc["now"] = now;
    JsonArray rows = doc.createNestedArray("rows");
    {
      ActLock lk;
      if (lk.ok) {
        actPrune(now);
        for (auto &a : g_act) {
          if (!a.id) continue;
          int live = 0;
          for (auto *l : a.live) if (l) live++;
          const uint64_t at = (live && a.pos) ? *a.pos : a.end + 1;
          JsonObject o = rows.createNestedObject();
          o["id"] = a.id;
          o["ip"] = IPAddress(a.ip).toString();
          o["dev"] = a.dev;
          o["path"] = a.path;
          o["size"] = a.size;
          o["at"] = at > a.size ? a.size : at;
          // bytes a second since the last look, while a reply is going
          uint32_t rate = 0;
          if (live && a.sampleMs && now - a.sampleMs >= 500 && at >= a.samplePos)
            rate = (uint32_t)((at - a.samplePos) * 1000ULL / (now - a.sampleMs));
          a.samplePos = at; a.sampleMs = now;
          o["rate"] = rate;
          o["since"] = (now - a.first) / 1000;
          o["ago"] = (now - a.last) / 1000;
          o["live"] = live;
          o["dl"] = a.download;
          for (auto &b : g_actBlock)
            if (b.until && b.ip == a.ip && b.path == a.path) o["stopped"] = (uint32_t)(b.until - now) / 1000;
        }
      }
    }
    String out;
    serializeJson(doc, out);
    AsyncWebServerResponse *res = r->beginResponse(200, "application/json", out);
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });

  server.on("/api/activity/stop", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    const uint32_t id = r->hasParam("id", true) ? r->getParam("id", true)->value().toInt() : 0;
    AsyncWebServerRequest *toClose[ACT_LIVE] = {};
    String path;
    {
      ActLock lk;
      if (!lk.ok) { r->send(503, "application/json", "{\"error\":\"busy\"}"); return; }
      ActRow *row = nullptr;
      for (auto &a : g_act) if (a.id && a.id == id) { row = &a; break; }
      if (!row) { r->send(404, "application/json", "{\"error\":\"Not playing any more\"}"); return; }
      // refuse the file to that device first, so a player that reconnects at once is turned away
      ActBlock *slot = &g_actBlock[0];
      for (auto &b : g_actBlock) {
        if (b.until && b.ip == row->ip && b.path == row->path) { slot = &b; break; }
        if (!b.until || b.until < slot->until) slot = &b;
      }
      slot->ip = row->ip; slot->path = row->path; slot->until = millis() + ACT_BLOCK_MS;
      if (!slot->until) slot->until = 1;
      for (int i = 0; i < ACT_LIVE; i++) { toClose[i] = row->live[i]; row->live[i] = nullptr; }
      path = row->path;
    }
    int closed = 0;
    for (auto *c : toClose) {
      if (c && c->client()) { c->client()->close(true); closed++; }   // its disconnect runs here and frees it
    }
    webLogf("info", "Stopped %s (%d connection%s)", path.c_str(), closed, closed == 1 ? "" : "s");
    r->send(200, "application/json", String("{\"closed\":") + closed + "}");
  });

  server.on("/api/activity/allow", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    const uint32_t id = r->hasParam("id", true) ? r->getParam("id", true)->value().toInt() : 0;
    ActLock lk;
    if (!lk.ok) { r->send(503, "application/json", "{\"error\":\"busy\"}"); return; }
    for (auto &a : g_act) {
      if (!a.id || a.id != id) continue;
      for (auto &b : g_actBlock) if (b.until && b.ip == a.ip && b.path == a.path) b = ActBlock();
      r->send(200, "application/json", "{\"ok\":true}");
      return;
    }
    r->send(404, "application/json", "{\"error\":\"Not found\"}");
  });
}

#endif  // NOMAD_ACTIVITY_H
