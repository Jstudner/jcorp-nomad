// NomadStorageHealth.h - how the SD card is doing, for the admin page.
//
// Two parts, both cheap:
//   watching   the card driver counts every read and write, the ones that failed and the
//              ones that took over NOMAD_SD_SLOW_MS (NomadSD.cpp). Nothing extra is read.
//   checking   on request, CHECK_SAMPLES short reads spread over the whole card, each under
//              the SD lock with a pause after it, so a film playing at the time only ever
//              waits for one of them. About 12 MB in all, a few seconds.
//
//   GET  /api/storage-health          the card, the counts since boot, the last check  (admin)
//   POST /api/storage-health/check    start a check                                   (admin)
#ifndef NOMAD_STORAGE_HEALTH_H
#define NOMAD_STORAGE_HEALTH_H
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include "NomadSD.h"

static const int CHECK_SAMPLES = 384;

struct StorageCheck {
  volatile bool running = false;
  bool ran = false;
  uint32_t done = 0, errors = 0, slow = 0, skipped = 0;
  uint32_t worstUs = 0;
  uint64_t bytes = 0, readUs = 0;
  uint32_t startedMs = 0, tookMs = 0;
  uint32_t badSectors[4] = {};
  uint32_t chunkKb = 0;
};
static StorageCheck g_sdCheck;

static void storageCheckTask(void *) {
  StorageCheck &c = g_sdCheck;
  // a big DMA buffer reads like a film does. With memory short, small reads still find bad spots
  const size_t room = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  size_t chunk = room >= 72 * 1024 ? 32 * 1024 : room >= 56 * 1024 ? 16 * 1024 : 4096;
  uint8_t *buf = (uint8_t *)heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!buf) { chunk = 4096; buf = (uint8_t *)heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL); }
  const uint32_t secs = (uint32_t)(NomadSD.cardSize() / 512);
  const uint32_t n = chunk / 512;
  c.chunkKb = chunk / 1024;
  if (buf && secs > n) {
    for (int i = 0; i < CHECK_SAMPLES; i++) {
      const uint32_t sector = esp_random() % (secs - n);
      if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1500)) != pdTRUE) { c.skipped++; vTaskDelay(pdMS_TO_TICKS(50)); continue; }
      const int64_t t0 = esp_timer_get_time();
      const bool ok = NomadSD.readRAWMulti(buf, sector, n);
      const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
      if (sdMutex) xSemaphoreGive(sdMutex);
      c.done++;
      c.readUs += us;
      if (us > c.worstUs) c.worstUs = us;
      if (us > NOMAD_SD_SLOW_MS * 1000) c.slow++;
      if (ok) c.bytes += chunk;
      else { if (c.errors < 4) c.badSectors[c.errors] = sector; c.errors++; }
      vTaskDelay(pdMS_TO_TICKS(12));   // the card is someone else's between reads
    }
  }
  if (buf) heap_caps_free(buf);
  c.tookMs = millis() - c.startedMs;
  c.ran = true;
  c.running = false;
  const double mbps = c.readUs ? (double)c.bytes / c.readUs : 0;   // bytes per us = MB/s
  webLogf(c.errors ? "error" : "success", "Card check: %u reads, %u failed, %u slow, %.1f MB/s",
          (unsigned)c.done, (unsigned)c.errors, (unsigned)c.slow, mbps);
  nomadTaskExit();   // a nomadCreateTask task leaves this way, never vTaskDelete
}

static void nomadStorageHealthRegister(AsyncWebServer &server) {
  server.on("/api/storage-health", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    DynamicJsonDocument doc(1536);
    NomadSDCardInfo ci;
    if (nomadSDCardInfo(ci)) {
      JsonObject card = doc.createNestedObject("card");
      card["mid"] = ci.mid;
      card["oid"] = ci.oid;
      card["name"] = ci.name;
      card["rev"] = ci.rev;
      card["serial"] = ci.serial;
      card["made"] = String(ci.year) + "-" + (ci.month < 10 ? "0" : "") + String(ci.month);
      card["bytes"] = ci.bytes;
      card["sdhc"] = ci.sdhc;
      card["mhz"] = ci.freqKhz / 1000.0;
      card["bus"] = ci.busWidth;
      card["au"] = ci.allocUnitKb;
      card["fs"] = NomadSD.fsTypeName();
    }
    NomadSDStats st;
    nomadSDStats(st);
    JsonObject boot = doc.createNestedObject("boot");
    boot["up"] = millis() / 1000;
    boot["reads"] = st.reads;
    boot["readErrors"] = st.readErrors;
    boot["writes"] = st.writes;
    boot["writeErrors"] = st.writeErrors;
    boot["slow"] = st.slowReads;
    boot["worstMs"] = st.maxReadUs / 1000;
    boot["mbRead"] = (uint32_t)(st.sectorsRead / 2048);
    boot["mbWritten"] = (uint32_t)(st.sectorsWritten / 2048);
    boot["lastErrorAgo"] = st.lastErrorMs ? (long)((millis() - st.lastErrorMs) / 1000) : -1L;
    const StorageCheck &c = g_sdCheck;
    JsonObject chk = doc.createNestedObject("check");
    chk["state"] = c.running ? "running" : c.ran ? "done" : "never";
    chk["done"] = c.done;
    chk["total"] = CHECK_SAMPLES;
    chk["errors"] = c.errors;
    chk["slow"] = c.slow;
    chk["skipped"] = c.skipped;
    chk["worstMs"] = c.worstUs / 1000;
    chk["mbps"] = c.readUs ? (double)c.bytes / c.readUs : 0;
    chk["chunkKb"] = c.chunkKb;
    chk["tookS"] = c.tookMs / 1000;
    chk["ago"] = c.ran ? (millis() - c.startedMs) / 1000 : -1;
    JsonArray bad = chk.createNestedArray("bad");
    for (uint32_t i = 0; i < c.errors && i < 4; i++) bad.add(c.badSectors[i]);
    // Everything above is the card's. A library can be more than the card (a P4-NANO merges
    // a USB drive in), so its volumes are listed too. "boot" and "check" are the card alone.
    JsonArray vols = doc["volumes"].to<JsonArray>();
    for (int v = 0; v < NOMAD_VOL_COUNT; v++) {
      if (!NomadSD.volumeMounted(v)) continue;
      JsonObject o = vols.add<JsonObject>();
      o["id"] = v;
      o["label"] = NomadSD.volumeLabel(v);
      o["fs"] = NomadSD.volumeFsTypeName(v);
      o["bytes"] = NomadSD.volumeTotalBytes(v);
    }
    String out;
    serializeJson(doc, out);
    AsyncWebServerResponse *res = r->beginResponse(200, "application/json", out);
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });

  server.on("/api/storage-health/check", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    if (g_sdCheck.running) { r->send(409, "application/json", "{\"error\":\"A check is already running\"}"); return; }
    // a P4 can run with no card in: there is nothing to read, and 0 reads isn't a pass
    if (NomadSD.cardType() == CARD_NONE) { r->send(409, "application/json", "{\"error\":\"There is no SD card in Nomad\"}"); return; }
    g_sdCheck = StorageCheck();
    g_sdCheck.running = true;
    g_sdCheck.startedMs = millis();
    if (nomadCreateTask(storageCheckTask, "CardCheck", 4096, NULL, 1, NULL, 1) != pdPASS) {
      g_sdCheck.running = false;
      r->send(503, "application/json", "{\"error\":\"Not enough memory to start\"}");
      return;
    }
    webLogf("info", "Card check started");
    r->send(202, "application/json", "{\"state\":\"running\"}");
  });
}

#endif  // NOMAD_STORAGE_HEALTH_H
