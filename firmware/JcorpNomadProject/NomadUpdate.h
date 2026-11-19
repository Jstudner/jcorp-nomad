// NomadUpdate.h - updating the web interface over the internet, from the admin panel.
//
// The phone does the downloading: on home WiFi it already has the internet, and TLS on the
// device would cost ~40 KB of the internal heap the streaming path is short of. The device's
// part is to say what it has, and to accept a file only if it is the one the manifest names.
//
//   GET  /api/update/env            mode, firmware, channel, manifest URL (anyone)
//   POST /api/update/hash           body: one path per line, hashes them on a task (admin)
//   GET  /api/update/hash           {"state":"running","done":n,"total":m} | {"state":"done","files":{...}}
//   POST /api/update/stage?path=P&size=N&sha256=H   body: the file (admin). Written beside the
//                                   original as P.nomad-new, kept only if size and SHA-256 match.
//   POST /api/update/commit         swap every staged file in (admin)
//   POST /api/update/abort          delete every staged file (admin)
//
// Only interface files can be written: what NomadGuard's isInterfacePath() calls the web
// interface (root files with an extension, /assets, /EmulatorJS). Media, /config and the
// dotfiles are out of reach whatever the request says.
//
// The manifest and the files live as static files on the flasher site.

#ifndef NOMAD_UPDATE_H
#define NOMAD_UPDATE_H

#include <Arduino.h>
#include "src/AsyncWebServer/ESPAsyncWebServer.h"   // carried here, with Nomad's limits (src/AsyncWebServer/README.md)
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <esp_task_wdt.h>
#include <map>
#include <vector>
#include "NomadSD.h"
#include "NomadGuard.h"

#ifndef NOMAD_FW_VERSION
#define NOMAD_FW_VERSION "5.0"
#endif
#ifndef NOMAD_UPDATE_CHANNEL
#define NOMAD_UPDATE_CHANNEL "experimental"
#endif
#ifndef NOMAD_UPDATE_BASE
#define NOMAD_UPDATE_BASE "https://nomadflash.jcorptech.net/update/v1/"
#endif

static const char *NOMAD_STAGE_SUFFIX = ".nomad-new";
static const char *NOMAD_OLD_SUFFIX = ".nomad-old";
static const int NOMAD_UPDATE_MAX_FILES = 400;

static bool nomadUpdatablePath(const String &p) {
  return p.startsWith("/") && isCleanWritePath(p) && isInterfacePath(p) &&
         !isConfigPath(p) && !isSystemPath(p) && !hasShortNameAlias(p) &&
         !p.endsWith(NOMAD_STAGE_SUFFIX) && !p.endsWith(NOMAD_OLD_SUFFIX);
}

static String nomadHex(const uint8_t *b, size_t n) {
  static const char *h = "0123456789abcdef";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) { s += h[b[i] >> 4]; s += h[b[i] & 15]; }
  return s;
}

// hashing job

static volatile bool g_updHashRunning = false;
static volatile int g_updHashDone = 0, g_updHashTotal = 0;
static String g_updHashResult = "{\"state\":\"idle\"}";

// The work, in its own function so every String and file in it is destroyed before the task
// deletes itself. vTaskDelete(NULL) doesn't unwind the stack, so anything left would leak.
static void nomadUpdateHashRun(std::vector<String> *paths) {
  // Sector-aligned internal DMA buffer and sequential reads: the card's fast path.
  uint8_t *buf = (uint8_t *)heap_caps_aligned_alloc(64, 4096, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  String j = "{\"state\":\"done\",\"files\":{";
  bool first = true;
  for (size_t i = 0; buf && i < paths->size(); i++) {
    const String &p = (*paths)[i];
    NomadFile64 f;
    bool opened = false;
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
      opened = f.open(p.c_str());
      xSemaphoreGive(sdMutex);
    }
    if (opened) {
      mbedtls_sha256_context ctx;
      mbedtls_sha256_init(&ctx);
      mbedtls_sha256_starts(&ctx, 0);
      uint64_t pos = 0;
      bool ok = true;
      while (true) {
        size_t n = 0;
        if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) { ok = false; break; }
        n = f.readAt(pos, buf, 4096);
        if (sdMutex) xSemaphoreGive(sdMutex);
        if (n == 0) break;
        mbedtls_sha256_update(&ctx, buf, n);
        pos += n;
        if ((pos & 0x3FFFF) == 0) vTaskDelay(1);   // let streaming have the card every 256 KB
      }
      uint8_t digest[32];
      mbedtls_sha256_finish(&ctx, digest);
      mbedtls_sha256_free(&ctx);
      if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
      f.close();
      if (sdMutex) xSemaphoreGive(sdMutex);
      if (ok) {
        if (!first) j += ",";
        first = false;
        j += "\"" + escapeJsonString(p) + "\":[" + String((unsigned long long)pos) + ",\"" + nomadHex(digest, 32) + "\"]";
      }
    }
    g_updHashDone = (int)i + 1;
  }
  j += "}}";
  if (!buf) j = "{\"state\":\"error\",\"error\":\"no buffer\"}";
  if (buf) heap_caps_free(buf);
  g_updHashResult = j;
}

static void nomadUpdateHashTask(void *arg) {
  std::vector<String> *paths = (std::vector<String> *)arg;
  nomadUpdateHashRun(paths);
  delete paths;
  g_updHashRunning = false;
  vTaskDelete(NULL);
}

// staging

struct NomadStage {
  String path;
  File file;
  size_t expect = 0, got = 0;
  String sha;
  mbedtls_sha256_context ctx;
  bool failed = false;
  bool busy = false;     // failed because the card was busy: answered 503 so the page sends it again
};
// The card lock for a handler on the network task: a short wait, never for ever (waiting out
// a scan or a delete trips the watchdog). false: not taken.
static bool nomadUpdLock() {
  return !sdMutex || xSemaphoreTake(sdMutex, pdMS_TO_TICKS(NOMAD_NET_CARD_WAIT_MS)) == pdTRUE;
}

static std::map<AsyncWebServerRequest *, NomadStage *> g_updStaging;
static std::vector<String> g_updStaged;   // paths with a verified .nomad-new beside them

static void nomadStageDrop(AsyncWebServerRequest *r, bool removeFile) {
  auto it = g_updStaging.find(r);
  if (it == g_updStaging.end()) return;
  NomadStage *s = it->second;
  // the file has to be closed whatever the card is doing, so a wait that runs out goes ahead
  const bool locked = nomadUpdLock();
  if (s->file) s->file.close();
  if (removeFile) SD_MMC.remove(s->path + NOMAD_STAGE_SUFFIX);
  if (locked && sdMutex) xSemaphoreGive(sdMutex);
  mbedtls_sha256_free(&s->ctx);
  delete s;
  g_updStaging.erase(it);
}

static void nomadStageBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  if (index == 0) {
    if (!checkAdminAuth(request)) return;                          // answered in the request handler
    const String path = request->hasParam("path") ? request->getParam("path")->value() : "";
    const String sha = request->hasParam("sha256") ? request->getParam("sha256")->value() : "";
    const size_t size = request->hasParam("size") ? (size_t)request->getParam("size")->value().toInt() : 0;
    if (!nomadUpdatablePath(path) || sha.length() != 64 || size != total) return;
    NomadStage *s = new NomadStage();
    s->path = path; s->expect = size; s->sha = sha;
    s->sha.toLowerCase();
    mbedtls_sha256_init(&s->ctx);
    mbedtls_sha256_starts(&s->ctx, 0);
    if (nomadUpdLock()) {
      const int slash = path.lastIndexOf('/');
      if (slash > 0) {                                                // new folder under /assets
        const String dir = path.substring(0, slash);
        if (!SD_MMC.exists(dir)) SD_MMC.mkdir(dir);
      }
      SD_MMC.remove(path + NOMAD_STAGE_SUFFIX);
      s->file = SD_MMC.open(path + NOMAD_STAGE_SUFFIX, FILE_WRITE);
      if (sdMutex) xSemaphoreGive(sdMutex);
    } else {
      s->busy = true;                                               // answered 503, the page sends it again
    }
    if (!s->file) s->failed = true;
    g_updStaging[request] = s;
    request->onDisconnect([request]() { nomadStageDrop(request, true); });
  }
  auto it = g_updStaging.find(request);
  if (it == g_updStaging.end()) return;
  NomadStage *s = it->second;
  if (s->failed) return;
  mbedtls_sha256_update(&s->ctx, data, len);
  if (!nomadUpdLock()) { s->failed = s->busy = true; return; }
  if (s->file.write(data, len) != len) s->failed = true;
  if (sdMutex) xSemaphoreGive(sdMutex);
  s->got += len;
}

static void nomadStageDone(AsyncWebServerRequest *request) {
  if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"admin\"}"); return; }
  auto it = g_updStaging.find(request);
  if (it == g_updStaging.end()) {
    // An empty file has no body, so nomadStageBody never ran for it (the map fonts carry
    // hundreds of empty glyph ranges).
    const String path = request->hasParam("path") ? request->getParam("path")->value() : "";
    String sha = request->hasParam("sha256") ? request->getParam("sha256")->value() : "";
    sha.toLowerCase();
    const bool empty = request->hasParam("size") && request->getParam("size")->value() == "0" &&
                       sha == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    if (empty && nomadUpdatablePath(path)) {
      if (!nomadUpdLock()) { nomadSendBusy(request, "application/json", "{\"error\":\"busy\"}"); return; }
      const int slash = path.lastIndexOf('/');
      if (slash > 0 && !SD_MMC.exists(path.substring(0, slash))) SD_MMC.mkdir(path.substring(0, slash));
      SD_MMC.remove(path + NOMAD_STAGE_SUFFIX);
      File f = SD_MMC.open(path + NOMAD_STAGE_SUFFIX, FILE_WRITE);
      const bool made = (bool)f;
      if (f) f.close();
      if (sdMutex) xSemaphoreGive(sdMutex);
      if (!made) { request->send(500, "application/json", "{\"error\":\"could not write\"}"); return; }
      bool known = false;
      for (auto &p : g_updStaged) known |= (p == path);
      if (!known) g_updStaged.push_back(path);
      request->send(200, "application/json", "{\"staged\":" + String((int)g_updStaged.size()) + "}");
      return;
    }
    request->send(400, "application/json", "{\"error\":\"path, size or sha256 refused\"}");
    return;
  }
  NomadStage *s = it->second;
  uint8_t digest[32];
  mbedtls_sha256_finish(&s->ctx, digest);
  const bool ok = !s->failed && s->got == s->expect && nomadHex(digest, 32) == s->sha;
  const String path = s->path;
  const bool wasBusy = s->busy;
  {
    const bool locked = nomadUpdLock();
    if (s->file) s->file.close();
    if (locked && sdMutex) xSemaphoreGive(sdMutex);
  }
  if (!ok) {
    nomadStageDrop(request, true);
    if (wasBusy) nomadSendBusy(request, "application/json", "{\"error\":\"busy\"}");
    else request->send(422, "application/json", "{\"error\":\"size or sha256 did not match\"}");
    return;
  }
  nomadStageDrop(request, false);
  bool known = false;
  for (auto &p : g_updStaged) known |= (p == path);
  if (!known) g_updStaged.push_back(path);
  request->send(200, "application/json", "{\"staged\":" + String((int)g_updStaged.size()) + "}");
}

// Swap one staged file in. The original is renamed aside first and restored if the new one
// will not go into place, so a failure never leaves the path empty.
static bool nomadSwapIn(const String &path) {
  const String neu = path + NOMAD_STAGE_SUFFIX, old = path + NOMAD_OLD_SUFFIX;
  if (!SD_MMC.exists(neu)) return false;
  SD_MMC.remove(old);
  const bool had = SD_MMC.exists(path);
  if (had && !SD_MMC.rename(path, old)) return false;
#if NOMAD_BENCH
  if (g_benchUpdGapMs) {
    Serial.printf("[Bench] %s moved aside, swapping in %u ms\n", path.c_str(), (unsigned)g_benchUpdGapMs);
    delay(g_benchUpdGapMs);
  }
#endif
  if (!SD_MMC.rename(neu, path)) {
    if (had) SD_MMC.rename(old, path);
    return false;
  }
  if (had) SD_MMC.remove(old);
  return true;
}

// Commit and abort run on the network task, which Async TCP keeps on the 5 s task watchdog.
// Hundreds of swaps in big FAT32 folders take longer than that, so feed it between files.
static void nomadUpdFeed(bool subscribed) {
  if (subscribed) esp_task_wdt_reset();
}

// The staged list is only in memory, so before the first file is swapped the paths are
// written here, and the file goes once every swap is done. Power lost in between leaves it,
// and the next start finishes the job: each file still waiting as .nomad-new is swapped in,
// and a path left with nothing at it gets its .nomad-old back.
static const char *const NOMAD_UPDATE_JOURNAL = "/.system-index/update-swap.txt";

static void nomadUpdateJournalWrite() {
  File j = SD_MMC.open(NOMAD_UPDATE_JOURNAL, FILE_WRITE);
  if (!j) return;
  for (auto &p : g_updStaged) { j.print(p); j.print('\n'); }
  j.close();
}

// setup(), once the card is mounted and before the server takes requests
static void nomadUpdateRecover() {
  if (!SD_MMC.exists(NOMAD_UPDATE_JOURNAL)) return;
  File j = SD_MMC.open(NOMAD_UPDATE_JOURNAL, FILE_READ);
  int put = 0, n = 0;
  while (j && j.available() && n < NOMAD_UPDATE_MAX_FILES) {
    String p = j.readStringUntil('\n');
    p.trim();
    if (!nomadUpdatablePath(p)) continue;
    n++;
    const String neu = p + NOMAD_STAGE_SUFFIX, old = p + NOMAD_OLD_SUFFIX;
    // every staged file was checked against its hash before the swap began, so the update
    // is finished, not undone: a card left half old and half new is the worse outcome
    if (SD_MMC.exists(neu)) { if (nomadSwapIn(p)) put++; }
    else if (!SD_MMC.exists(p) && SD_MMC.exists(old) && SD_MMC.rename(old, p)) put++;
    if (SD_MMC.exists(p)) SD_MMC.remove(old);
  }
  if (j) j.close();
  SD_MMC.remove(NOMAD_UPDATE_JOURNAL);
  Serial.printf("[Update] an interface update was cut short: %d file(s) checked, %d finished\n", n, put);
}

// routes

// Needs escapeJsonString, webLogf, g_staMode and checkAdminAuth from the sketch, so it is
// included after them.
static void nomadUpdateRegister(AsyncWebServer &server) {
  server.on("/api/update/env", HTTP_GET, [](AsyncWebServerRequest *r) {
    String j = "{\"mode\":\"" + String(g_staMode ? "station" : "hotspot") + "\"";
    j += ",\"firmware\":\"" NOMAD_FW_VERSION "\",\"channel\":\"" NOMAD_UPDATE_CHANNEL "\"";
    j += ",\"manifest\":\"" NOMAD_UPDATE_BASE NOMAD_UPDATE_CHANNEL "/manifest.json\"";
#if NOMAD_BENCH
    j += ",\"testMode\":true";
#endif
    j += "}";
    r->send(200, "application/json", j);
  });

  server.on("/api/update/hash", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (g_updHashRunning) {
      r->send(200, "application/json", "{\"state\":\"running\",\"done\":" + String(g_updHashDone) +
                                       ",\"total\":" + String(g_updHashTotal) + "}");
      return;
    }
    r->send(200, "application/json", g_updHashResult);
  });

  server.on("/api/update/hash", HTTP_POST,
    [](AsyncWebServerRequest *r) {
      String body;
      if (r->_tempObject) {
        body = (const char *)r->_tempObject;
        free(r->_tempObject);
        r->_tempObject = nullptr;
      }
      if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"admin\"}"); return; }
      if (g_updHashRunning) { r->send(409, "application/json", "{\"error\":\"running\"}"); return; }
      std::vector<String> *paths = new std::vector<String>();
      {
        int start = 0;
        while (start < (int)body.length() && (int)paths->size() < NOMAD_UPDATE_MAX_FILES) {
          int nl = body.indexOf('\n', start);
          if (nl < 0) nl = body.length();
          String p = body.substring(start, nl);
          p.trim();
          if (nomadUpdatablePath(p)) paths->push_back(p);
          start = nl + 1;
        }
      }
      g_updHashTotal = paths->size();
      g_updHashDone = 0;
      g_updHashRunning = true;
      g_updHashResult = "{\"state\":\"running\"}";
      if (xTaskCreatePinnedToCore(nomadUpdateHashTask, "updHash", 6144, paths, 2, NULL, 1) != pdPASS) {
        g_updHashRunning = false;
        delete paths;
        r->send(503, "application/json", "{\"error\":\"no task\"}");
        return;
      }
      r->send(202, "application/json", "{\"state\":\"running\",\"total\":" + String(g_updHashTotal) + "}");
    },
    nullptr,
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
      // The path list (a few KB) is one plain block, which the server can free() by itself
      // when the request never reaches the handler above. This runs before the guard, so it
      // checks the sign-in itself, and never writes past the Content-Length it sized from.
      if (index == 0 && !r->_tempObject) {
        if (total == 0 || total > 32768 || !checkAdminAuth(r)) return;
        char *held = (char *)heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!held) held = (char *)malloc(total + 1);
        if (!held) return;
        held[0] = 0;
        r->_tempObject = held;
      }
      char *held = (char *)r->_tempObject;
      if (!held || total > 32768 || index + len > total) return;
      memcpy(held + index, data, len);
      held[index + len] = 0;
    });

  server.on("/api/update/stage", HTTP_POST, nomadStageDone, nullptr, nomadStageBody);

  server.on("/api/update/commit", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"admin\"}"); return; }
    int ok = 0;
    String failed;
    // nothing has been replaced yet, so a busy card is "ask again", not a failed update
    if (!nomadUpdLock()) { nomadSendBusy(r, "application/json", "{\"error\":\"busy\"}"); return; }
    nomadUpdateJournalWrite();
    const bool wdt = esp_task_wdt_status(NULL) == ESP_OK;
    const uint32_t t0 = millis();
    for (auto &p : g_updStaged) {
      nomadUpdFeed(wdt);
      if (nomadSwapIn(p)) ok++;
      else failed += (failed.length() ? "\",\"" : "") + escapeJsonString(p);
    }
    SD_MMC.remove(NOMAD_UPDATE_JOURNAL);
    if (sdMutex) xSemaphoreGive(sdMutex);
    Serial.printf("[Update] %d of %d file(s) swapped in %lu ms\n", ok, (int)g_updStaged.size(), (unsigned long)(millis() - t0));
    g_updStaged.clear();
    webLogf(failed.length() ? "warning" : "success", "Interface update: %d file(s) replaced%s", ok,
            failed.length() ? ", some failed" : "");
    r->send(failed.length() ? 500 : 200, "application/json",
            "{\"replaced\":" + String(ok) + ",\"failed\":[" + (failed.length() ? "\"" + failed + "\"" : "") + "]}");
  });

  server.on("/api/update/abort", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"admin\"}"); return; }
    const bool locked = nomadUpdLock();
    const bool wdt = esp_task_wdt_status(NULL) == ESP_OK;
    for (auto &p : g_updStaged) { nomadUpdFeed(wdt); SD_MMC.remove(p + NOMAD_STAGE_SUFFIX); }
    if (locked && sdMutex) xSemaphoreGive(sdMutex);
    const int n = g_updStaged.size();
    g_updStaged.clear();
    r->send(200, "application/json", "{\"removed\":" + String(n) + "}");
  });
}

#endif  // NOMAD_UPDATE_H
