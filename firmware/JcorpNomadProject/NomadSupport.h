// NomadSupport.h - safe mode after repeated crashes, and the part of the admin page's support
// file that no other route has: the build, the chip, memory, why this start happened, and the
// crash Nomad keeps in flash.
//
// Safe mode: a counter in RTC memory, which a crash restart keeps and a power cycle clears,
// counts crash restarts that each came less than SAFE_WINDOW_MS after the start before.
// At SAFE_AFTER in a row the next start is a safe one: no boot index check, no first-time
// index build, no DLNA. The hotspot, the pages and the admin page still run, so the admin
// can see the crash, save a support file and start normally again. Five quiet minutes clear
// the counter, so crashes days apart never add up. Going back from bad firmware is
// NomadFirmware.h's job, this covers a bad card or a page that trips a bug.
//
// The crash: the ESP-IDF panic handler writes a core dump to the coredump partition. Its
// summary (task, PC, backtrace addresses, the crashing build's ELF hash) is read once at
// boot. Function names need that build's ELF, which Nomad doesn't carry, so the
// addresses get turned into names on a computer.
//
//   GET  /api/support               build, chip, memory, start, safe mode, crash   (admin)
//   POST /api/support/normal        clear the counter and restart normally         (admin)
//   POST /api/support/forget-crash  erase the stored crash                         (admin)
//   POST /api/support/crash         crash on purpose (NOMAD_FW_BENCH builds only)  (admin)
#ifndef NOMAD_SUPPORT_H
#define NOMAD_SUPPORT_H
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_app_desc.h>
#include <esp_core_dump.h>
#include <esp_heap_caps.h>

static const uint32_t SAFE_MAGIC = 0x5AFE0DE5;
static const uint32_t SAFE_AFTER = 3;
static const uint32_t SAFE_WINDOW_MS = 5UL * 60 * 1000;

RTC_NOINIT_ATTR static uint32_t g_safeMagic;
RTC_NOINIT_ATTR static uint32_t g_crashRun;    // crash restarts in a row, each soon after the last start

static uint32_t g_crashRunAtBoot = 0;          // what the counter said when this start began
static bool g_safeCleared = false;
static volatile uint32_t g_supportRestartAt = 0;   // loop() restarts after the reply has gone
static volatile uint32_t g_supportCrashAt = 0;     // bench: loop() crashes on purpose

struct NomadCrash {
  bool stored = false, corrupted = false;
  char task[16] = "";
  char elf[APP_ELF_SHA256_SZ + 1] = "";
  uint32_t pc = 0, cause = 0, addr = 0, depth = 0;
  uint32_t bt[16] = {};
};
static NomadCrash g_crash;

static const char *resetName(esp_reset_reason_t rr) {
  switch (rr) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: return "hang (interrupt watchdog)";
    case ESP_RST_TASK_WDT: return "hang (task watchdog)";
    case ESP_RST_WDT: return "hang (watchdog)";
    case ESP_RST_BROWNOUT: return "power dip (brownout)";
    case ESP_RST_DEEPSLEEP: return "wake from sleep";
    case ESP_RST_USB: return "USB reset";
    case ESP_RST_EXT: return "reset pin";
    default: return "other";
  }
}
static bool resetIsCrash(esp_reset_reason_t rr) {
  return rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT;
}

// setup(), first thing after nomadFirmwareBoot: count this start, decide on safe mode, and
// read the stored crash while nothing else is using flash
static void nomadSafeBoot() {
  const esp_reset_reason_t rr = esp_reset_reason();
  if (g_safeMagic != SAFE_MAGIC || rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT) {
    g_safeMagic = SAFE_MAGIC;
    g_crashRun = 0;
  }
  if (resetIsCrash(rr)) g_crashRun++;
  else g_crashRun = 0;
  g_crashRunAtBoot = g_crashRun;
  g_safeMode = g_crashRun >= SAFE_AFTER;

#if CONFIG_IDF_TARGET_ARCH_XTENSA
  esp_core_dump_summary_t *s = (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));
  if (s && esp_core_dump_get_summary(s) == ESP_OK) {
    g_crash.stored = true;
    strlcpy(g_crash.task, s->exc_task, sizeof(g_crash.task));
    memcpy(g_crash.elf, s->app_elf_sha256, APP_ELF_SHA256_SZ);
    g_crash.elf[APP_ELF_SHA256_SZ] = 0;
    g_crash.pc = s->exc_pc;
    g_crash.cause = s->ex_info.exc_cause;
    g_crash.addr = s->ex_info.exc_vaddr;
    g_crash.corrupted = s->exc_bt_info.corrupted;
    g_crash.depth = min((uint32_t)16, (uint32_t)s->exc_bt_info.depth);
    for (uint32_t i = 0; i < g_crash.depth; i++) g_crash.bt[i] = s->exc_bt_info.bt[i];
  }
  free(s);
#endif

  if (g_safeMode) {
    Serial.printf("[SAFE] %u crash restarts in a row: starting in safe mode (no boot index check, no DLNA)\n",
                  (unsigned)g_crashRun);
  }
}

// once the server is up, so the admin's log has it
static void nomadSafeLog() {
  if (!g_safeMode) return;
  if (g_crash.stored)
    webLogf("error", "Safe mode: crashed %u times in a row, last in %s at 0x%08x. Indexing and TV support are off until a normal start.",
            (unsigned)g_crashRunAtBoot, g_crash.task, (unsigned)g_crash.pc);
  else
    webLogf("error", "Safe mode: crashed %u times in a row. Indexing and TV support are off until a normal start.",
            (unsigned)g_crashRunAtBoot);
}

// loop(): five quiet minutes and the crashes so far stop counting, and restarts asked for by the page
static void nomadSafeTick() {
  const uint32_t now = millis();
  if (!g_safeCleared && now > SAFE_WINDOW_MS) { g_crashRun = 0; g_safeCleared = true; }
  if (g_supportRestartAt && (int32_t)(now - g_supportRestartAt) >= 0) { g_supportRestartAt = 0; ESP.restart(); }
  if (g_supportCrashAt && (int32_t)(now - g_supportCrashAt) >= 0) { volatile uint32_t *p = nullptr; *p = 0xDEAD; }
}

static String supportJson() {
  JsonDocument d;
  d["v"] = 1;
  d["version"] = NOMAD_FW_VERSION;
  d["board"] = NOMAD_BOARD_ID;
  d["channel"] = NOMAD_FW_CHANNEL;
  // the whole hash (esp_app_get_elf_sha256 gives only a short prefix), so a crash can be
  // matched to its build and decoded later
  const esp_app_desc_t *app = esp_app_get_description();
  char elf[65];
  for (int i = 0; i < 32; i++) snprintf(elf + i * 2, 3, "%02x", app->app_elf_sha256[i]);
  d["elf"] = elf;
  d["built"] = __DATE__ " " __TIME__;   // the app description's date is the core library's, not this build's
  d["idf"] = app->idf_ver;
  const esp_partition_t *run = esp_ota_get_running_partition();
  d["slot"] = run ? run->label : "?";

  esp_chip_info_t ci;
  esp_chip_info(&ci);
  JsonObject chip = d["chip"].to<JsonObject>();
  chip["model"] = ESP.getChipModel();
  chip["rev"] = ci.revision;
  chip["cores"] = ci.cores;
  chip["mhz"] = ESP.getCpuFreqMHz();
  chip["flashMb"] = ESP.getFlashChipSize() / (1024 * 1024);
  chip["psramMb"] = ESP.getPsramSize() / (1024 * 1024);
  chip["tempC"] = temperatureRead();

  JsonObject mem = d["mem"].to<JsonObject>();
  mem["free"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  mem["min"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  mem["largest"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  mem["dma"] = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  mem["psramFree"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  mem["psramMin"] = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
  mem["tasks"] = uxTaskGetNumberOfTasks();

  const esp_reset_reason_t rr = esp_reset_reason();
  JsonObject start = d["start"].to<JsonObject>();
  start["up"] = millis() / 1000;
  start["reason"] = resetName(rr);
  start["crashed"] = resetIsCrash(rr);
  start["inARow"] = g_crashRunAtBoot;
  start["safe"] = g_safeMode;

  JsonObject net = d["net"].to<JsonObject>();
  net["mode"] = g_staMode ? "wifi" : "hotspot";
  net["phones"] = getConnectedUserCount();
  net["dlna"] = settings.dlnaEnabled && !g_safeMode;

  if (g_crash.stored) {
    JsonObject c = d["crash"].to<JsonObject>();
    c["thisStart"] = resetIsCrash(rr);   // the stored one is what ended the last run
    c["task"] = g_crash.task;
    c["elf"] = g_crash.elf;
    c["sameBuild"] = strncmp(elf, g_crash.elf, strlen(g_crash.elf)) == 0;
    char hex[11];
    snprintf(hex, sizeof(hex), "0x%08x", (unsigned)g_crash.pc); c["pc"] = hex;
    snprintf(hex, sizeof(hex), "0x%08x", (unsigned)g_crash.addr); c["addr"] = hex;
    c["cause"] = g_crash.cause;
    c["corrupted"] = g_crash.corrupted;
    JsonArray bt = c["bt"].to<JsonArray>();
    for (uint32_t i = 0; i < g_crash.depth; i++) { snprintf(hex, sizeof(hex), "0x%08x", (unsigned)g_crash.bt[i]); bt.add(hex); }
  }
  String out;
  serializeJson(d, out);
  return out;
}

static void nomadSupportRegister(AsyncWebServer &server) {
  server.on("/api/support", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    AsyncWebServerResponse *res = r->beginResponse(200, "application/json", supportJson());
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });
  server.on("/api/support/normal", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    g_crashRun = 0;
    webLog("Starting normally, at the admin's request", "info");
    g_supportRestartAt = millis() + 800;
    r->send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/support/forget-crash", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    const esp_err_t e = esp_core_dump_image_erase();
    if (e == ESP_OK) g_crash = NomadCrash();
    r->send(e == ESP_OK ? 200 : 500, "application/json", e == ESP_OK ? "{\"ok\":true}" : "{\"error\":\"Couldn't erase it\"}");
  });
#ifdef NOMAD_FW_BENCH
  // bench only: a real panic, so the crash record and safe mode can be tested end to end
  server.on("/api/support/crash", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    g_supportCrashAt = millis() + 500;
    r->send(200, "application/json", "{\"ok\":true}");
  });
#endif
}

#endif  // NOMAD_SUPPORT_H
