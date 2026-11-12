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
// The manifest and the files live as static files on the flasher site (tools/ui-manifest.py).
//
// Boards with the interface in flash (both P4s, NomadUI.h): a page in flash is what gets
// served, and the card's copy of it never is, so writing the card would "succeed" and change
// nothing. Each path goes where it is served from: flash for what NomadUI keeps there (root
// pages, styles and scripts, /assets), the card for the rest (/EmulatorJS and other root
// files). Hashing reads from the same place, so the phone compares what is really served.
// Staging in flash needs room for the new copy beside the old one. A file that won't fit is
// refused before a byte is written, and the old interface stays as it was.

#ifndef NOMAD_UPDATE_H
#define NOMAD_UPDATE_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <map>
#include <vector>
#include "NomadSD.h"
#include "NomadGuard.h"
#include "NomadUI.h"

#ifndef NOMAD_FW_VERSION
#define NOMAD_FW_VERSION "4.8"
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

// Where an interface file is served from, and so where the updater reads and writes it:
// the flash volume, or nullptr for the card.
//   served now: flash if flash holds it (hashing, and a file being replaced)
//   a new file: flash if NomadUI's rule puts it there (a page or a small script, the maps
//               stack and anything over NOMAD_UI_FLASH_MAX_FILE stay on the card)
//   staged:     flash if its .nomad-new is there (commit and abort)
static fs::FS *nomadUpdateServedFrom(const String &p) {
  fs::FS *fl = nomadUiFlash();
  return (fl && fl->exists(p)) ? fl : nullptr;
}
static fs::FS *nomadUpdateTargetFor(const String &p, uint64_t size) {
  fs::FS *fl = nomadUiFlash();
  return (fl && (fl->exists(p) || nomadUiBelongsInFlash(p, size))) ? fl : nullptr;
}
static fs::FS *nomadUpdateStagedIn(const String &p) {
  fs::FS *fl = nomadUiFlash();
  return (fl && fl->exists(p + NOMAD_STAGE_SUFFIX)) ? fl : nullptr;
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
    if (fs::FS *fl = nomadUpdateServedFrom(p)) {
      // served from flash: hash that copy (no card lock, flash isn't on the card's bus)
      File ff = fl->open(p, "r");
      if (ff && !ff.isDirectory()) {
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        uint64_t pos = 0;
        size_t n;
        while ((n = ff.read(buf, 4096)) > 0) {
          mbedtls_sha256_update(&ctx, buf, n);
          pos += n;
          if ((pos & 0x3FFFF) == 0) vTaskDelay(1);
        }
        uint8_t digest[32];
        mbedtls_sha256_finish(&ctx, digest);
        mbedtls_sha256_free(&ctx);
        if (!first) j += ",";
        first = false;
        j += "\"" + escapeJsonString(p) + "\":[" + String((unsigned long long)pos) + ",\"" + nomadHex(digest, 32) + "\"]";
      }
      if (ff) ff.close();
      g_updHashDone = (int)i + 1;
      continue;
    }
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
  fs::FS *flash = nullptr;   // staged in flash (NomadUI), or nullptr for the card
  File file;
  size_t expect = 0, got = 0;
  String sha;
  mbedtls_sha256_context ctx;
  bool failed = false;
  bool noRoom = false;       // flash hasn't the room for it beside the old copy
};
static std::map<AsyncWebServerRequest *, NomadStage *> g_updStaging;
static std::vector<String> g_updStaged;   // paths with a verified .nomad-new beside them

static void nomadStageDrop(AsyncWebServerRequest *r, bool removeFile) {
  auto it = g_updStaging.find(r);
  if (it == g_updStaging.end()) return;
  NomadStage *s = it->second;
  if (s->flash) {
    if (s->file) s->file.close();
    if (removeFile) s->flash->remove(s->path + NOMAD_STAGE_SUFFIX);
  } else {
    if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
    if (s->file) s->file.close();
    if (removeFile) SD_MMC.remove(s->path + NOMAD_STAGE_SUFFIX);
    if (sdMutex) xSemaphoreGive(sdMutex);
  }
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
    s->flash = nomadUpdateTargetFor(path, size);
    if (s->flash) {
      s->flash->remove(path + NOMAD_STAGE_SUFFIX);
      // the new copy sits beside the old one until commit, plus LittleFS's own slack
      if (nomadUiFreeBytes() < (uint64_t)size + 32 * 1024) {
        s->failed = true;
        s->noRoom = true;
      } else {
        s->file = s->flash->open(path + NOMAD_STAGE_SUFFIX, "w", true);   // makes missing folders
      }
    } else {
      if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
      const int slash = path.lastIndexOf('/');
      if (slash > 0) {                                                // new folder under /assets
        const String dir = path.substring(0, slash);
        if (!SD_MMC.exists(dir)) SD_MMC.mkdir(dir);
      }
      SD_MMC.remove(path + NOMAD_STAGE_SUFFIX);
      s->file = SD_MMC.open(path + NOMAD_STAGE_SUFFIX, FILE_WRITE);
      if (sdMutex) xSemaphoreGive(sdMutex);
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
  if (s->flash) {
    if (s->file.write(data, len) != len) s->failed = true;
  } else {
    if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
    if (s->file.write(data, len) != len) s->failed = true;
    if (sdMutex) xSemaphoreGive(sdMutex);
  }
  s->got += len;
}

static void nomadStageDone(AsyncWebServerRequest *request) {
  if (!checkAdminAuth(request)) { request->send(401, "application/json", "{\"error\":\"admin\"}"); return; }
  auto it = g_updStaging.find(request);
  if (it == g_updStaging.end()) {
    request->send(400, "application/json", "{\"error\":\"path, size or sha256 refused\"}");
    return;
  }
  NomadStage *s = it->second;
  uint8_t digest[32];
  mbedtls_sha256_finish(&s->ctx, digest);
  const bool ok = !s->failed && s->got == s->expect && nomadHex(digest, 32) == s->sha;
  const String path = s->path;
  const bool noRoom = s->noRoom;
  if (s->flash) {
    if (s->file) s->file.close();
  } else {
    if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
    s->file.close();
    if (sdMutex) xSemaphoreGive(sdMutex);
  }
  if (!ok) {
    nomadStageDrop(request, true);
    if (noRoom) request->send(507, "application/json", "{\"error\":\"Not enough room in Nomad's flash for this page\"}");
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
// will not go into place, so a failure never leaves the path empty. fsys is the card or the
// flash volume, whichever the file was staged on.
static bool nomadSwapIn(fs::FS &fsys, const String &path) {
  const String neu = path + NOMAD_STAGE_SUFFIX, old = path + NOMAD_OLD_SUFFIX;
  if (!fsys.exists(neu)) return false;
  fsys.remove(old);
  const bool had = fsys.exists(path);
  if (had && !fsys.rename(path, old)) return false;
  if (!fsys.rename(neu, path)) {
    if (had) fsys.rename(old, path);
    return false;
  }
  if (had) fsys.remove(old);
  return true;
}

// routes

// Needs escapeJsonString, webLogf, g_staMode and checkAdminAuth from the sketch, so it is
// included after them.
static void nomadUpdateRegister(AsyncWebServer &server) {
  server.on("/api/update/env", HTTP_GET, [](AsyncWebServerRequest *r) {
    String j = "{\"mode\":\"" + String(g_staMode ? "station" : "hotspot") + "\"";
    j += ",\"firmware\":\"" NOMAD_FW_VERSION "\",\"channel\":\"" NOMAD_UPDATE_CHANNEL "\"";
    j += ",\"manifest\":\"" NOMAD_UPDATE_BASE NOMAD_UPDATE_CHANNEL "/manifest.json\"";
    // where the pages live: "flash" on a board that serves them from its own flash (the
    // updater writes there), "card" otherwise
    j += String(",\"pages\":\"") + (nomadUiFlash() ? "flash" : "card") + "\"";
    if (nomadUiFlash()) j += ",\"flashFree\":" + String((unsigned long long)nomadUiFreeBytes());
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
      // Take the body and destroy the String in place: the server frees _tempObject with
      // free(), which would skip the destructor and leak the String's buffer.
      String body;
      if (r->_tempObject) {
        String *held = (String *)r->_tempObject;
        body = *held;
        held->~String();
      }
      if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"admin\"}"); return; }
      if (g_updHashRunning) { r->send(409, "application/json", "{\"error\":\"running\"}"); return; }
      // the body handler drops a list over 32 KB, and a 202 with nothing to hash would read
      // as "none of these files are on Nomad"
      if (r->contentLength() > 32768) { r->send(413, "application/json", "{\"error\":\"at most 32 KB of paths at a time\"}"); return; }
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
      // the path list is small (a few KB). _tempObject is freed with free() by the server,
      // so it holds a malloc'd String shell, not a new'd one
      if (index == 0) {
        if (total > 32768) return;
        void *mem = malloc(sizeof(String));
        r->_tempObject = new (mem) String();
        ((String *)r->_tempObject)->reserve(total);
      }
      if (r->_tempObject) ((String *)r->_tempObject)->concat((const char *)data, len);
    });

  server.on("/api/update/stage", HTTP_POST, nomadStageDone, nullptr, nomadStageBody);

  server.on("/api/update/commit", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"admin\"}"); return; }
    int ok = 0, inFlash = 0;
    String failed;
    // where each one was staged, decided before anything moves (a swapped file's .nomad-new
    // is gone afterwards)
    std::vector<fs::FS *> where;
    for (auto &p : g_updStaged) where.push_back(nomadUpdateStagedIn(p));
    // flash first and without the card lock, so films aren't held up behind flash erases
    for (size_t i = 0; i < g_updStaged.size(); i++) {
      if (!where[i]) continue;
      if (nomadSwapIn(*where[i], g_updStaged[i])) { ok++; inFlash++; }
      else failed += (failed.length() ? "\",\"" : "") + escapeJsonString(g_updStaged[i]);
    }
    if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
    for (size_t i = 0; i < g_updStaged.size(); i++) {
      if (where[i]) continue;
      if (nomadSwapIn(SD_MMC, g_updStaged[i])) ok++;
      else failed += (failed.length() ? "\",\"" : "") + escapeJsonString(g_updStaged[i]);
    }
    if (sdMutex) xSemaphoreGive(sdMutex);
    g_updStaged.clear();
    if (inFlash) nomadUiNoteUpdated();
    webLogf(failed.length() ? "warning" : "success", "Interface update: %d file(s) replaced%s", ok,
            failed.length() ? ", some failed" : "");
    r->send(failed.length() ? 500 : 200, "application/json",
            "{\"replaced\":" + String(ok) + ",\"failed\":[" + (failed.length() ? "\"" + failed + "\"" : "") + "]}");
  });

  server.on("/api/update/abort", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"admin\"}"); return; }
    for (auto &p : g_updStaged) {
      if (fs::FS *fl = nomadUpdateStagedIn(p)) fl->remove(p + NOMAD_STAGE_SUFFIX);
    }
    if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
    for (auto &p : g_updStaged) {
      SD_MMC.remove(p + NOMAD_STAGE_SUFFIX);   // no-op where it was staged in flash
    }
    if (sdMutex) xSemaphoreGive(sdMutex);
    const int n = g_updStaged.size();
    g_updStaged.clear();
    r->send(200, "application/json", "{\"removed\":" + String(n) + "}");
  });
}

#endif  // NOMAD_UPDATE_H
