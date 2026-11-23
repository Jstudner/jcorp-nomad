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
#include "src/AsyncWebServer/ESPAsyncWebServer.h"   // carried here, with Nomad's limits (src/AsyncWebServer/README.md)
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_app_desc.h>
#include <esp_core_dump.h>
#include <esp_heap_caps.h>

static const uint32_t SAFE_MAGIC = 0x5AFE0DE5;
static const uint32_t SAFE_AFTER = 3;
static const uint32_t SAFE_WINDOW_MS = 5UL * 60 * 1000;
// Level 2: five crashes in a row and the start keeps to the basics: hotspot only, no TV
// support, no archives, no chat history, no storage breakdown, no index work. The hotspot
// name, its password and the admin password still come from the card, so the owner can get in.
static const uint32_t SAFE_DEEP_AFTER = 5;
// Crashes a little more than five minutes apart never reach SAFE_AFTER (the counter clears at
// five quiet minutes). A second counter clears only after an hour up: four of those is safe mode too.
static const uint32_t SAFE_SLOW_AFTER = 4;
static const uint32_t SAFE_SLOW_WINDOW_MS = 60UL * 60 * 1000;
// Brownouts: two in a row and the next start goes easy on power: radio and backlight down, no
// index work. A start that stays up five minutes clears it. RTC memory may not survive every
// brownout; then the count starts again at one.
static const uint32_t LOW_POWER_AFTER = 2;
RTC_NOINIT_ATTR static uint32_t g_safeMagic;
RTC_NOINIT_ATTR static uint32_t g_crashRun;    // crash restarts in a row, each soon after the last start
RTC_NOINIT_ATTR static uint32_t g_crashSlow;   // crash restarts with no start between them that lasted an hour
RTC_NOINIT_ATTR static uint32_t g_brownRun;    // brownout restarts in a row

static uint32_t g_crashRunAtBoot = 0;          // what the counter said when this start began
// The run before this one, for the support file: how long it lasted and how low internal
// memory got. Kept in RTC memory, which a crash, a watchdog or a restart leaves alone (a
// power cut does not), and refreshed every few seconds from loop().
RTC_NOINIT_ATTR static uint32_t g_runMagic;
RTC_NOINIT_ATTR static uint32_t g_runUpS;
RTC_NOINIT_ATTR static uint32_t g_runMemMin;
// 1 while an upload chunk is being written. On a full FAT32 card that write can search the
// whole table and outlast the network task's watchdog, and the page resends the chunk after
// each restart. A watchdog restart with this set is taken as a full card (g_prevRunDiedWriting).
RTC_NOINIT_ATTR static uint32_t g_runGrowWrite;
static bool g_prevRunDiedWriting = false;
// The main loop and the UI task each stamp a time. Only the network task is on the core's
// watchdog, and a hung loop() silently stops the button, card recovery, restarts asked for by
// the admin page and the freeing of finished tasks while the web pages stay up. A timer looks
// every 5 s, and after NOMAD_HEART_MS without a stamp from either it restarts the board,
// leaving which one behind in RTC memory.
static const uint32_t NOMAD_HEART_MS = 90000;
RTC_NOINIT_ATTR static uint32_t g_runHung;      // 1 loop, 2 the UI task
// Where it hung, for the next start's support file and admin log: the step loop() was on, and
// the tasks holding the app's card lock and the card driver's own lock when the heartbeat gave up.
RTC_NOINIT_ATTR static uint32_t g_runLoopAt;
RTC_NOINIT_ATTR static char g_runHeldBy[2][16];
static uint32_t g_prevLoopAt = 0;
static char g_prevHeldBy[2][16] = { "", "" };
static const char *const kLoopSteps[] = { "", "firmware", "safe mode", "usage", "bench", "flash mode", "button",
  "power", "wifi", "screen", "scan", "card", "streams", "dlna", "tasks", "chat", "menu", "idle" };
#define LOOP_AT(n) (g_runLoopAt = (n))
static volatile uint32_t g_beatLoop = 0, g_beatUi = 0;
static volatile bool g_beatUiOn = false;
static void nomadHeartCheck(void *) {
  const uint32_t now = millis();
  const int which = (now - g_beatLoop > NOMAD_HEART_MS) ? 1 : (g_beatUiOn && now - g_beatUi > NOMAD_HEART_MS) ? 2 : 0;
  if (!which) return;
  g_runHung = which;
  extern SemaphoreHandle_t sdMutex;
  TaskHandle_t held[2] = { sdMutex ? xSemaphoreGetMutexHolder(sdMutex) : nullptr, SD_MMC.lockHolder() };
  for (int i = 0; i < 2; i++) strlcpy(g_runHeldBy[i], held[i] ? pcTaskGetName(held[i]) : "-", sizeof(g_runHeldBy[i]));
  esp_rom_printf("[HEART] %s stopped for %u s, restarting\n", which == 1 ? "loop" : "UiTask", (unsigned)(NOMAD_HEART_MS / 1000));
  esp_restart();
}
// end of setup(): from here loop() runs
static void nomadHeartStart(bool uiTaskUp) {
  g_beatLoop = g_beatUi = millis();
  g_beatUiOn = uiTaskUp;
  esp_timer_create_args_t a = {};
  a.callback = nomadHeartCheck;
  a.name = "heart";
  esp_timer_handle_t t = nullptr;
  if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_periodic(t, 5000000ULL);
}
static bool g_prevRunKnown = false;
static uint32_t g_prevRunUpS = 0, g_prevRunMemMin = 0;
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
  if (g_runMagic == SAFE_MAGIC && g_runUpS > 0 && rr != ESP_RST_POWERON && rr != ESP_RST_BROWNOUT) {
    g_prevRunKnown = true; g_prevRunUpS = g_runUpS; g_prevRunMemMin = g_runMemMin;
  }
  g_prevRunDiedWriting = g_runMagic == SAFE_MAGIC && g_runGrowWrite == 1 &&
                         (rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT);
  g_prevRunHung = (g_runMagic == SAFE_MAGIC && rr == ESP_RST_SW && (g_runHung == 1 || g_runHung == 2)) ? (uint8_t)g_runHung : 0;
  if (g_prevRunHung) {
    g_prevLoopAt = g_runLoopAt < sizeof(kLoopSteps) / sizeof(kLoopSteps[0]) ? g_runLoopAt : 0;
    for (int i = 0; i < 2; i++) {
      g_runHeldBy[i][15] = 0;
      bool ok = true;
      for (const char *c = g_runHeldBy[i]; *c; ++c) ok &= (*c >= 0x20 && *c < 0x7f);
      strlcpy(g_prevHeldBy[i], ok ? g_runHeldBy[i] : "?", sizeof(g_prevHeldBy[i]));
    }
  }
  g_runLoopAt = 0;
  g_runMagic = SAFE_MAGIC; g_runUpS = 0; g_runMemMin = 0; g_runGrowWrite = 0; g_runHung = 0;
  const bool fresh = g_safeMagic != SAFE_MAGIC;
  if (fresh || rr == ESP_RST_POWERON || rr == ESP_RST_BROWNOUT) {
    g_safeMagic = SAFE_MAGIC;
    g_crashRun = 0;
    g_crashSlow = 0;
  }
  // counters newer than the magic can hold an older build's RTC words: no real run reaches
  // these, so anything past them is left over, not counted
  if (g_crashSlow > 1000) g_crashSlow = 0;
  if (g_brownRun > 1000) g_brownRun = 0;
  if (rr == ESP_RST_BROWNOUT) g_brownRun = fresh ? 1 : g_brownRun + 1;
  else if (fresh || rr == ESP_RST_POWERON) g_brownRun = 0;
  if (resetIsCrash(rr) || g_prevRunHung) { g_crashRun++; g_crashSlow++; }   // a hang restart counts toward safe mode like a crash
  else g_crashRun = 0;
  g_crashRunAtBoot = g_crashRun;
  g_safeMode = g_crashRun >= SAFE_AFTER || g_crashSlow >= SAFE_SLOW_AFTER;
  g_safeDeep = g_crashRun >= SAFE_DEEP_AFTER;
  g_lowPower = g_brownRun >= LOW_POWER_AFTER;

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
    Serial.printf("[SAFE] %u crash restarts in a row (%u within the hour): starting in safe mode%s\n",
                  (unsigned)g_crashRun, (unsigned)g_crashSlow,
                  g_safeDeep ? ", basics only (hotspot, pages, admin)" : " (no boot index check, no DLNA)");
  }
  if (g_lowPower) Serial.printf("[POWER] %u power dips in a row: radio and backlight down, no index work this start\n", (unsigned)g_brownRun);
}

// once the server is up, so the admin's log has it
static void nomadSafeLog() {
  if (g_prevRunHung)
    webLogf("warning", "Restarted: %s stopped for 90 s%s%s (card lock: %s, card driver: %s)",
            g_prevRunHung == 1 ? "the main loop" : "the screen task",
            g_prevRunHung == 1 ? " at " : "", g_prevRunHung == 1 ? kLoopSteps[g_prevLoopAt] : "",
            g_prevHeldBy[0], g_prevHeldBy[1]);
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
  static uint32_t runNoted = 0;
  if (now - runNoted >= 5000) {
    runNoted = now;
    g_runUpS = now / 1000;
    g_runMemMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  }
  if (!g_safeCleared && now > SAFE_WINDOW_MS) { g_crashRun = 0; g_brownRun = 0; g_safeCleared = true; }
  static bool slowCleared = false;
  if (!slowCleared && now > SAFE_SLOW_WINDOW_MS) { g_crashSlow = 0; slowCleared = true; }
  if (g_supportRestartAt && (int32_t)(now - g_supportRestartAt) >= 0) {
    g_supportRestartAt = 0;
    statusBeforeRestart(SI_REFRESH, "Restarting");
    nomadClockSave();
    nomadFirmwareBeforeRestart(); nomadQuietCard(); ESP.restart();
  }
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
  start["safeLevel"] = g_safeDeep ? 2 : (g_safeMode ? 1 : 0);
  start["crashesWithinHour"] = g_crashSlow;
  start["powerDipsInARow"] = g_brownRun;
  start["lowPower"] = g_lowPower;
  if (g_prevRunKnown) {
    JsonObject prev = d["previous"].to<JsonObject>();
    prev["up"] = g_prevRunUpS;            // seconds the run before this one lasted
    prev["memMin"] = g_prevRunMemMin;     // lowest free internal memory it saw
    if (g_prevRunHung) {
      prev["hung"] = g_prevRunHung == 1 ? "loop" : "ui";   // it was restarted by the heartbeat
      if (g_prevRunHung == 1) prev["at"] = kLoopSteps[g_prevLoopAt];
      prev["cardLock"] = g_prevHeldBy[0];
      prev["driverLock"] = g_prevHeldBy[1];
    }
  }
  JsonObject resets = d["resets"].to<JsonObject>();   // every start so far, by how it began
  resets["crash"] = g_useResets[USE_RST_CRASH];
  resets["watchdog"] = g_useResets[USE_RST_WATCHDOG];
  resets["brownout"] = g_useResets[USE_RST_BROWNOUT];
  resets["power"] = g_useResets[USE_RST_POWER];
  resets["restart"] = g_useResets[USE_RST_RESTART];

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
    g_crashSlow = 0;
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
