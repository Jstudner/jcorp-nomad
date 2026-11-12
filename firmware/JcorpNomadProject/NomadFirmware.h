// NomadFirmware.h - firmware updates without a cable: uploaded as a file from the admin page,
// or fetched from nomadflash in WiFi Mode.
//
// The file is a .nomadfw: the app image as arduino-cli builds it, then a 512-byte trailer
//   "NOMADFW1" | image size (u32 LE) | SHA-256 of the image | meta length (u16 LE) | meta JSON
//   | signature length (u16 LE) | ECDSA P-256 signature (DER) over SHA-256 of everything
//   before the signature length | zero padding
// The meta has the board, version and channel, and is covered by the signature.
//
// What keeps a bad update from bricking it:
//   - only the slot that isn't running gets written, so a failure before the switch
//     leaves the Nomad as it was
//   - it won't switch unless the image checks out (IDF image check, SHA-256, signature,
//     this board, newer unless asked otherwise)
//   - after the restart the new firmware has to pass a self-test (card, network, web server,
//     memory, 60 s up) or it rolls back. A crash or hang rolls back too (bootloader
//     rollback and a 120 s deadline).
//
//   GET  /api/firmware                    what is running, what is staged, the last result (admin)
//   POST /api/firmware/begin?size=N[&older=1]   start receiving a file of N bytes (admin)
//   POST /api/firmware/stage?offset=O     raw body, the next piece, at most 64 KB (admin)
//   POST /api/firmware/pull[?older=1]     fetch from nomadflash, WiFi Mode only (admin)
//   POST /api/firmware/abort              drop what is staged (admin)
//   POST /api/firmware/apply              restart into the staged firmware (admin)
#ifndef NOMAD_FIRMWARE_H
#define NOMAD_FIRMWARE_H
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>

// the public half of the update key. Bench builds trust a throwaway key instead
#if NOMAD_FW_DEVKEY
#include "NomadFirmwareKeyDev.h"
#else
#include "NomadFirmwareKey.h"
#endif

// which board this firmware drives: an image for another one is refused (a USB-C image
// on a USB-A board runs fine with a dark screen, which no self-test can see)
#ifndef NOMAD_BOARD_ID
#if defined(NOMAD_BOARD_P4_NANO)
#define NOMAD_BOARD_ID "p4-nano"
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
#define NOMAD_BOARD_ID "p4-wifi6"
#elif BOARD_USB_C
#define NOMAD_BOARD_ID "s3-usbc"
#else
#define NOMAD_BOARD_ID "s3-usba"
#endif
#endif
// Test builds only: NOMAD_FW_BENCH=1 pulls from a test machine on the hotspot, and
// NOMAD_FW_BENCH_FAIL=1/2/3 makes the new image crash, hang, or fail its self-test, to test
// each rollback path
#if NOMAD_FW_BENCH == 1
#define NOMAD_FW_BASE "http://192.168.4.2:8981/update/v1/"
#define NOMAD_FW_PULL_ANY_MODE 1
#elif NOMAD_FW_BENCH == 2   // real nomadflash, bench-test channel, WiFi Mode only
#define NOMAD_FW_CHANNEL "bench-test"
#endif
#ifndef NOMAD_FW_BENCH_FAIL
#define NOMAD_FW_BENCH_FAIL 0
#endif
// where the Nomad looks in WiFi Mode: <base><channel>/firmware.json, file urls relative to <base>
#ifndef NOMAD_FW_BASE
#define NOMAD_FW_BASE NOMAD_UPDATE_BASE
#endif
#ifndef NOMAD_FW_CHANNEL
#define NOMAD_FW_CHANNEL NOMAD_UPDATE_CHANNEL
#endif
#ifndef NOMAD_FW_PULL_ANY_MODE
#define NOMAD_FW_PULL_ANY_MODE 0   // 1: allow a pull without WiFi Mode (bench only)
#endif

bool checkAdminAuth(AsyncWebServerRequest *request);
extern bool g_staMode;
extern volatile bool g_fwPausingMedia;

static const size_t FW_TRAILER = 512;
static const size_t FW_SLICE_MAX = 64 * 1024;
static const uint32_t FW_PROVE_MS = 60000;      // up this long, with the checks passing
static const uint32_t FW_DEADLINE_MS = 120000;  // still not proven by now: go back

enum FwState : uint8_t { FW_IDLE, FW_RECEIVING, FW_CHECKING, FW_READY, FW_FAILED, FW_PULLING };
static const char *const FW_STATE_NAMES[] = { "idle", "receiving", "checking", "ready", "failed", "pulling" };

struct FwStage {
  volatile FwState state = FW_IDLE;
  uint32_t total = 0;           // the whole .nomadfw
  volatile uint32_t accepted = 0;   // bytes taken in (queued or written)
  volatile uint32_t written = 0;    // bytes through the writer
  bool older = false;
  esp_ota_handle_t ota = 0;
  const esp_partition_t *part = nullptr;
  mbedtls_sha256_context sha;
  uint8_t trailer[FW_TRAILER];
  String error;
  String version, board, channel;   // from the signed trailer, once checked
};
static FwStage g_fw;
static SemaphoreHandle_t g_fwMx = nullptr;
// one piece of an upload: the request owns it (and frees it, whatever happens) until it is
// queued, then the writer does
struct FwBlock { size_t len; uint8_t data[]; };
static QueueHandle_t g_fwQ = nullptr;
static volatile bool g_fwServerUp = false;
static bool g_fwProving = false;
static esp_timer_handle_t g_fwDeadline = nullptr;

// "4.6.1" against "4.7": >0 when a is newer
static int fwCompare(const String &a, const String &b) {
  int ia = 0, ib = 0;
  for (int k = 0; k < 4; k++) {
    const int va = ia < (int)a.length() ? a.substring(ia).toInt() : 0;
    const int vb = ib < (int)b.length() ? b.substring(ib).toInt() : 0;
    if (va != vb) return va > vb ? 1 : -1;
    int na = a.indexOf('.', ia), nb = b.indexOf('.', ib);
    ia = na < 0 ? a.length() : na + 1;
    ib = nb < 0 ? b.length() : nb + 1;
  }
  return 0;
}

static void fwFail(const String &why) {
  if (g_fw.ota) { esp_ota_abort(g_fw.ota); g_fw.ota = 0; }
  g_fw.error = why;
  g_fw.state = FW_FAILED;
  webLogf("error", "Firmware update stopped: %s", why.c_str());
}

static bool fwBegin(uint32_t total, bool older) {
  if (total <= FW_TRAILER + 4096) { g_fw.error = "That file is too small to be firmware."; g_fw.state = FW_FAILED; return false; }
  if (g_fw.ota) { esp_ota_abort(g_fw.ota); g_fw.ota = 0; }
  g_fw.part = esp_ota_get_next_update_partition(nullptr);
  if (!g_fw.part) { g_fw.error = "Nomad has no spare firmware slot (it needs one USB flash first)."; g_fw.state = FW_FAILED; return false; }
  if (total - FW_TRAILER > g_fw.part->size) { g_fw.error = "That firmware is bigger than the slot it would go in."; g_fw.state = FW_FAILED; return false; }
  // erases as it goes: erasing 3 MB up front blocks for seconds
  esp_err_t e = esp_ota_begin(g_fw.part, OTA_WITH_SEQUENTIAL_WRITES, &g_fw.ota);
  if (e != ESP_OK) { g_fw.error = String("Couldn't start writing (") + esp_err_to_name(e) + ")."; g_fw.state = FW_FAILED; return false; }
  g_fw.total = total;
  g_fw.accepted = 0;
  g_fw.written = 0;
  g_fw.older = older;
  g_fw.error = "";
  g_fw.version = g_fw.board = g_fw.channel = "";
  memset(g_fw.trailer, 0, sizeof(g_fw.trailer));
  mbedtls_sha256_init(&g_fw.sha);
  mbedtls_sha256_starts(&g_fw.sha, 0);
  g_fw.state = FW_RECEIVING;
  return true;
}

// the trailer: is it ours, does it match, is it signed, is it for this board
static bool fwCheckTrailer(const uint8_t imageSha[32]) {
  const uint8_t *t = g_fw.trailer;
  if (memcmp(t, "NOMADFW1", 8) != 0) { fwFail("That isn't a Nomad firmware file (.nomadfw)."); return false; }
  const uint32_t size = t[8] | (t[9] << 8) | (t[10] << 16) | ((uint32_t)t[11] << 24);
  if (size != g_fw.total - FW_TRAILER) { fwFail("The file is cut short or padded."); return false; }
  if (memcmp(t + 12, imageSha, 32) != 0) { fwFail("The firmware doesn't match its own checksum. It was damaged on the way."); return false; }
  const size_t metaLen = t[44] | (t[45] << 8);
  if (metaLen == 0 || 46 + metaLen + 2 > FW_TRAILER) { fwFail("The firmware's description is unreadable."); return false; }
  const size_t sigAt = 46 + metaLen;
  const size_t sigLen = t[sigAt] | (t[sigAt + 1] << 8);
  if (sigLen == 0 || sigAt + 2 + sigLen > FW_TRAILER) { fwFail("The firmware isn't signed."); return false; }
  if (NOMAD_FW_PUBKEY_LEN == 0) { fwFail("This firmware build has no update key, so it can't check what it is given. Update it over USB."); return false; }
  uint8_t signedSha[32];
  mbedtls_sha256(t, sigAt, signedSha, 0);
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int rc = mbedtls_pk_parse_public_key(&pk, NOMAD_FW_PUBKEY, NOMAD_FW_PUBKEY_LEN);
  if (rc == 0) rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, signedSha, 32, t + sigAt + 2, sigLen);
  mbedtls_pk_free(&pk);
  if (rc != 0) { fwFail("The signature doesn't check out. Only firmware from nomadflash can go on over the air."); return false; }
  StaticJsonDocument<384> meta;
  if (deserializeJson(meta, (const char *)(t + 46), metaLen)) { fwFail("The firmware's description is unreadable."); return false; }
  g_fw.board = String((const char *)(meta["board"] | ""));
  g_fw.version = String((const char *)(meta["version"] | ""));
  g_fw.channel = String((const char *)(meta["channel"] | ""));
  if (g_fw.board != NOMAD_BOARD_ID) { fwFail("That firmware is for a " + g_fw.board + " board and this one is " NOMAD_BOARD_ID "."); return false; }
  if (!g_fw.older && fwCompare(g_fw.version, NOMAD_FW_VERSION) <= 0) {
    fwFail("That's " + g_fw.version + ", not newer than the " NOMAD_FW_VERSION " already running. Tick Allow older versions to go back.");
    return false;
  }
  return true;
}

// the next bytes of the file, in order. Image bytes go to the slot, the last 512 to the trailer
static bool fwFeed(const uint8_t *data, size_t len) {
  if (g_fw.state != FW_RECEIVING && g_fw.state != FW_PULLING) return false;
  const uint32_t imageEnd = g_fw.total - FW_TRAILER;
  while (len) {
    const uint32_t at = g_fw.written;
    if (at < imageEnd) {
      const size_t n = min((size_t)(imageEnd - at), len);
      mbedtls_sha256_update(&g_fw.sha, data, n);
      esp_err_t e = esp_ota_write(g_fw.ota, data, n);
      if (e != ESP_OK) { fwFail(String("Writing the firmware failed (") + esp_err_to_name(e) + ")."); return false; }
      g_fw.written += n; data += n; len -= n;
    } else if (at < g_fw.total) {
      const size_t n = min((size_t)(g_fw.total - at), len);
      memcpy(g_fw.trailer + (at - imageEnd), data, n);
      g_fw.written += n; data += n; len -= n;
    } else {
      fwFail("More bytes arrived than the file has.");
      return false;
    }
  }
  if (g_fw.written < g_fw.total) return true;
  // the whole file: check it before it can ever be booted
  g_fw.state = FW_CHECKING;
  uint8_t sha[32];
  mbedtls_sha256_finish(&g_fw.sha, sha);
  mbedtls_sha256_free(&g_fw.sha);
  // trailer checks first, they're cheap and give clearer errors
  if (!fwCheckTrailer(sha)) return false;
  esp_err_t e = esp_ota_end(g_fw.ota);   // then the IDF's own check of the image
  g_fw.ota = 0;
  if (e != ESP_OK) { fwFail(e == ESP_ERR_OTA_VALIDATE_FAILED ? "The firmware image is damaged." : String("Finishing the write failed (") + esp_err_to_name(e) + ")."); return false; }
  g_fw.state = FW_READY;
  webLogf("success", "Firmware %s checked and ready to restart into", g_fw.version.c_str());
  return true;
}

// Upload pieces get written on their own task. A flash write turns the cache off, which can't
// happen inside a request callback. The stack has to be in internal RAM, not PSRAM like
// nomadCreateTask's: with the cache off PSRAM is unreachable too and the IDF aborts.
// The task only exists during an upload so the 8 KB isn't held otherwise.
static TaskHandle_t g_fwWriter = nullptr;
static const uint32_t FW_IDLE_MS = 5UL * 60 * 1000;   // an upload nobody carries on with is dropped
static void fwWriterTask(void *) {
  FwBlock *b;
  uint32_t lastPiece = millis();
  for (;;) {
    if (xQueueReceive(g_fwQ, &b, pdMS_TO_TICKS(2000)) == pdTRUE) {
      lastPiece = millis();
      xSemaphoreTake(g_fwMx, portMAX_DELAY);
      if (g_fw.state == FW_RECEIVING) fwFeed(b->data, b->len);
      xSemaphoreGive(g_fwMx);
      free(b);
      continue;
    }
    xSemaphoreTake(g_fwMx, portMAX_DELAY);
    if (g_fw.state == FW_RECEIVING && millis() - lastPiece > FW_IDLE_MS)
      fwFail("The upload stopped part way and nothing more came. Nothing was changed.");
    const bool done = g_fw.state != FW_RECEIVING && uxQueueMessagesWaiting(g_fwQ) == 0;
    if (done) g_fwWriter = nullptr;   // under the lock, so a new upload starts a new writer
    xSemaphoreGive(g_fwMx);
    if (done) break;
  }
  vTaskDelete(NULL);
}
// internal RAM stack, see above
static bool fwStartWriter() {
  if (g_fwWriter) return true;
  return xTaskCreatePinnedToCore(fwWriterTask, "FwWriter", 8192, NULL, 2, &g_fwWriter, 1) == pdPASS;
}

// Fetching in WiFi Mode. Media is paused during it: TLS needs ~40 KB of the internal memory
// streaming uses, and the Nomad restarts after anyway. The server certificate isn't checked
// (no CA bundle to carry around). The file is trusted on its signature, so a tampered or
// old file still gets refused.
static String g_fwPullUrl;
// the work, in a function of its own so its TLS client and strings are destroyed when it
// returns: a task ended with vTaskDelete never runs its locals' destructors (~40 KB of TLS)
static void fwPullWork(bool older) {
  g_fwPausingMedia = true;
  vTaskDelay(pdMS_TO_TICKS(1500));   // let replies already going finish
  String err;
  const String base = NOMAD_FW_BASE;
  WiFiClientSecure tls;
  WiFiClient plain;
  tls.setInsecure();
  const bool https = base.startsWith("https://");
  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  String url, version;
  uint32_t size = 0;
  do {
    // cache buster, so a proxy can't hand back an old list
    const String list = base + NOMAD_FW_CHANNEL + "/firmware.json?t=" + String(esp_random());
    if (!http.begin(https ? (WiFiClient &)tls : plain, list)) { err = "Couldn't reach nomadflash."; break; }
    int code = http.GET();
    if (code != 200) { err = "nomadflash answered " + String(code) + " for the firmware list."; http.end(); break; }
    DynamicJsonDocument m(4096);
    DeserializationError de = deserializeJson(m, http.getStream());
    http.end();
    if (de) { err = "The firmware list from nomadflash is unreadable."; break; }
    JsonObject b = m["boards"][NOMAD_BOARD_ID];
    if (b.isNull()) { err = "nomadflash has no firmware for this board (" NOMAD_BOARD_ID ") on the " NOMAD_FW_CHANNEL " channel."; break; }
    version = String((const char *)(b["version"] | ""));
    url = String((const char *)(b["url"] | ""));
    size = b["size"] | 0;
    if (b["usbOnly"] | false) { err = version + " has to go on over USB, from nomadflash's Flash Firmware tab."; break; }
    if (!older && fwCompare(version, NOMAD_FW_VERSION) <= 0) { err = "Already up to date (" NOMAD_FW_VERSION ")."; break; }
    if (!url.length() || !size) { err = "The firmware list from nomadflash is missing the file."; break; }
    if (!url.startsWith("http")) url = base + url;
    g_fwPullUrl = url;
    if (!http.begin(https ? (WiFiClient &)tls : plain, url)) { err = "Couldn't reach nomadflash."; break; }
    code = http.GET();
    if (code != 200) { err = "nomadflash answered " + String(code) + " for the firmware file."; http.end(); break; }
    // a reply sent without a length (chunked) is fine: the bytes are counted and the trailer checked
    if (http.getSize() > 0 && (uint32_t)http.getSize() != size) { err = "The firmware file isn't the size nomadflash listed."; http.end(); break; }
    xSemaphoreTake(g_fwMx, portMAX_DELAY);
    const bool ok = fwBegin(size, older);
    if (ok) g_fw.state = FW_PULLING;
    xSemaphoreGive(g_fwMx);
    if (!ok) { http.end(); break; }
    WiFiClient *s = http.getStreamPtr();
    uint8_t *buf = (uint8_t *)malloc(4096);
    uint32_t got = 0, quiet = millis();
    while (buf && got < size && g_fw.state == FW_PULLING) {
      const int avail = s->available();
      if (avail <= 0) {
        if (!s->connected() || millis() - quiet > 20000) break;
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      const int n = s->readBytes(buf, min((uint32_t)4096, min((uint32_t)avail, size - got)));
      if (n <= 0) continue;
      quiet = millis();
      xSemaphoreTake(g_fwMx, portMAX_DELAY);
      fwFeed(buf, n);
      g_fw.accepted = g_fw.written;
      xSemaphoreGive(g_fwMx);
      got += n;
    }
    free(buf);
    http.end();
    if (g_fw.state == FW_PULLING) {   // the download stopped short
      xSemaphoreTake(g_fwMx, portMAX_DELAY);
      fwFail("The download stopped part way. Nothing was changed, try again.");
      xSemaphoreGive(g_fwMx);
    }
  } while (false);
  if (err.length()) {
    xSemaphoreTake(g_fwMx, portMAX_DELAY);
    if (g_fw.ota) { esp_ota_abort(g_fw.ota); g_fw.ota = 0; }
    g_fw.error = err;
    g_fw.state = (err.startsWith("Already up to date")) ? FW_IDLE : FW_FAILED;
    xSemaphoreGive(g_fwMx);
    webLogf("info", "Firmware check: %s", err.c_str());
  }
  g_fwPausingMedia = false;
}
static void fwPullTask(void *arg) {
  fwPullWork((bool)(intptr_t)arg);
  vTaskDelete(NULL);
}

// after a switch

// Arduino would bless a new image the moment it boots, this waits for the self-test instead
extern "C" bool verifyRollbackLater() { return true; }

static void fwDeadlineHit(void *) {
  // a hang: the loop never got to prove the new firmware. Go back.
  Preferences p;
  if (p.begin("nomadfw", false)) { p.putString("why", "it stopped responding after the restart"); p.end(); }
  esp_ota_mark_app_invalid_rollback_and_reboot();
}

// early in setup(): a new image proving itself, or an old one that was gone back to
static void nomadFirmwareBoot() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  g_fwProving = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
  Preferences p;
  if (!p.begin("nomadfw", false)) return;
  const String pend = p.getString("pend", "");
  if (g_fwProving) {
#if NOMAD_FW_BENCH_FAIL == 1
    abort();                                       // bench: crash while starting
#endif
    const esp_timer_create_args_t a = { .callback = fwDeadlineHit, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK, .name = "fw_deadline", .skip_unhandled_events = true };
    if (esp_timer_create(&a, &g_fwDeadline) == ESP_OK) esp_timer_start_once(g_fwDeadline, (uint64_t)FW_DEADLINE_MS * 1000);
  } else if (pend.length()) {
    // a switch was asked for and this is not a new image proving itself: it went back
    String why = p.getString("why", "");
    if (!why.length()) why = esp_ota_get_last_invalid_partition() ? "it crashed while starting" : "it didn't start";
    p.putString("last", "back|" + pend + "|" + why);
    p.remove("pend");
    p.remove("why");
  }
  p.end();
#if NOMAD_FW_BENCH_FAIL == 2
  if (g_fwProving) for (;;) delay(1000);           // bench: hang while starting
#endif
}

// from loop(): once it has been up long enough with everything working, keep it
static void nomadFirmwareTick() {
  if (!g_fwProving || millis() < FW_PROVE_MS) return;
  const bool sd = SD_MMC.cardType() != CARD_NONE;
  const bool net = !(NOMAD_FW_BENCH_FAIL == 3) &&   // bench: fail the check
      (g_staMode ? WiFi.status() == WL_CONNECTED : WiFi.softAPIP() != IPAddress(0, 0, 0, 0));
  const bool heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > 40 * 1024;
  if (sd && net && g_fwServerUp && heap) {
    g_fwProving = false;
    if (g_fwDeadline) esp_timer_stop(g_fwDeadline);
    esp_ota_mark_app_valid_cancel_rollback();
    Preferences p;
    if (p.begin("nomadfw", false)) { p.putString("last", "ok|" NOMAD_FW_VERSION "|"); p.remove("pend"); p.remove("why"); p.end(); }
    webLogf("success", "Firmware " NOMAD_FW_VERSION " started fine and is kept");
  } else if (millis() > FW_DEADLINE_MS - 5000) {
    g_fwProving = false;
    String why = !sd ? "it couldn't read the SD card" : !net ? "its WiFi didn't come up" : !g_fwServerUp ? "its web server didn't start" : "it ran short of memory";
    Preferences p;
    if (p.begin("nomadfw", false)) { p.putString("why", why); p.end(); }
    webLogf("error", "Firmware " NOMAD_FW_VERSION " failed its start-up check (%s), going back", why.c_str());
    delay(300);
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

static void nomadFirmwareServerUp() { g_fwServerUp = true; }

// routes

static void nomadFirmwareRegister(AsyncWebServer &server) {
  g_fwMx = xSemaphoreCreateMutex();
  g_fwQ = xQueueCreate(2, sizeof(FwBlock *));

  server.on("/api/firmware", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    DynamicJsonDocument d(1024);
    d["version"] = NOMAD_FW_VERSION;
    d["board"] = NOMAD_BOARD_ID;
    d["channel"] = NOMAD_FW_CHANNEL;
    const esp_partition_t *run = esp_ota_get_running_partition();
    d["slot"] = run ? run->label : "?";
    d["spare"] = esp_ota_get_next_update_partition(nullptr) != nullptr;
    d["signed"] = NOMAD_FW_PUBKEY_LEN > 0;
    d["wifiMode"] = g_staMode && WiFi.status() == WL_CONNECTED;
    d["canPull"] = NOMAD_FW_PULL_ANY_MODE || (g_staMode && WiFi.status() == WL_CONNECTED);
    d["proving"] = g_fwProving;
    d["state"] = FW_STATE_NAMES[g_fw.state];
    d["total"] = g_fw.total;
    d["have"] = g_fw.accepted;
    d["written"] = g_fw.written;
    if (g_fw.error.length()) d["error"] = g_fw.error;
    if (g_fw.state == FW_READY) {
      JsonObject s = d.createNestedObject("ready");
      s["version"] = g_fw.version; s["board"] = g_fw.board; s["channel"] = g_fw.channel;
    }
    Preferences p;
    if (p.begin("nomadfw", true)) {
      const String last = p.getString("last", "");
      p.end();
      if (last.length()) {
        JsonObject l = d.createNestedObject("last");
        const int a = last.indexOf('|'), b = last.indexOf('|', a + 1);
        l["result"] = last.substring(0, a);
        l["version"] = last.substring(a + 1, b < 0 ? last.length() : b);
        if (b > 0) l["why"] = last.substring(b + 1);
      }
    }
    String out;
    serializeJson(d, out);
    AsyncWebServerResponse *res = r->beginResponse(200, "application/json", out);
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });

  server.on("/api/firmware/begin", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    const uint32_t size = r->hasParam("size") ? r->getParam("size")->value().toInt() : 0;
    const bool older = r->hasParam("older") && r->getParam("older")->value() == "1";
    if (xSemaphoreTake(g_fwMx, pdMS_TO_TICKS(500)) != pdTRUE) { r->send(503, "application/json", "{\"error\":\"busy\"}"); return; }
    if (g_fw.state == FW_PULLING || g_fw.state == FW_CHECKING) { xSemaphoreGive(g_fwMx); r->send(409, "application/json", "{\"error\":\"An update is already going\"}"); return; }
    bool ok = fwBegin(size, older);
    if (ok && !fwStartWriter()) { fwFail("Not enough memory to take the upload."); ok = false; }
    const String err = g_fw.error;
    xSemaphoreGive(g_fwMx);
    if (!ok) { StaticJsonDocument<256> e; e["error"] = err; String o; serializeJson(e, o); r->send(400, "application/json", o); return; }
    webLogf("info", "Firmware upload started (%u bytes)", (unsigned)size);
    r->send(200, "application/json", "{\"have\":0}");
  });

  server.on("/api/firmware/stage", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    FwBlock *b = (FwBlock *)r->_tempObject;
    const uint32_t offset = r->hasParam("offset") ? r->getParam("offset")->value().toInt() : 0;
    if (g_fw.state != FW_RECEIVING) { r->send(409, "application/json", g_fw.error.length() ? "{\"error\":\"failed\"}" : "{\"error\":\"not receiving\"}"); return; }
    if (!b || !b->len) { r->send(400, "application/json", "{\"error\":\"empty\"}"); return; }
    if (offset != g_fw.accepted) { r->send(409, "application/json", String("{\"error\":\"offset\",\"have\":") + g_fw.accepted + "}"); return; }
    if (offset + b->len > g_fw.total) { r->send(400, "application/json", "{\"error\":\"past the end\"}"); return; }
    if (xQueueSend(g_fwQ, &b, 0) != pdTRUE) { r->send(503, "application/json", "{\"error\":\"busy\"}"); return; }
    r->_tempObject = nullptr;   // the writer has it now
    g_fw.accepted += b->len;
    r->send(200, "application/json", String("{\"have\":") + g_fw.accepted + "}");
  }, nullptr, [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total) {
    if (index == 0) {
      if (total > FW_SLICE_MAX || !checkAdminAuth(r)) return;
      // in PSRAM, freed by the request (free() takes any heap) unless handed to the writer
      FwBlock *b = (FwBlock *)heap_caps_malloc(sizeof(FwBlock) + total, MALLOC_CAP_SPIRAM);
      if (!b) return;
      b->len = 0;
      r->_tempObject = b;
    }
    FwBlock *b = (FwBlock *)r->_tempObject;
    if (!b || index + len > total || total > FW_SLICE_MAX) return;
    memcpy(b->data + index, data, len);
    b->len = index + len;
  });

  server.on("/api/firmware/pull", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    if (!(NOMAD_FW_PULL_ANY_MODE || (g_staMode && WiFi.status() == WL_CONNECTED))) {
      r->send(409, "application/json", "{\"error\":\"Nomad needs to be in WiFi Mode, on a network with internet, to fetch firmware itself.\"}");
      return;
    }
    if (g_fw.state == FW_PULLING || g_fw.state == FW_CHECKING || g_fw.state == FW_RECEIVING) { r->send(409, "application/json", "{\"error\":\"An update is already going\"}"); return; }
    const bool older = r->hasParam("older") && r->getParam("older")->value() == "1";
    g_fw.state = FW_PULLING; g_fw.error = ""; g_fw.accepted = 0; g_fw.written = 0; g_fw.total = 0;
    // internal RAM stack: it writes flash (see fwWriterTask), and TLS wants it anyway
    if (xTaskCreatePinnedToCore(fwPullTask, "FwPull", 12 * 1024, (void *)(intptr_t)older, 1, NULL, 1) != pdPASS) {
      g_fw.state = FW_FAILED; g_fw.error = "Not enough memory to start the download.";
      r->send(503, "application/json", "{\"error\":\"Not enough memory to start the download\"}");
      return;
    }
    webLogf("info", "Firmware check started");
    r->send(202, "application/json", "{\"state\":\"pulling\"}");
  });

  server.on("/api/firmware/abort", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    if (xSemaphoreTake(g_fwMx, pdMS_TO_TICKS(2000)) != pdTRUE) { r->send(503, "application/json", "{\"error\":\"busy\"}"); return; }
    if (g_fw.ota) { esp_ota_abort(g_fw.ota); g_fw.ota = 0; }
    g_fw.state = FW_IDLE; g_fw.error = ""; g_fw.total = 0; g_fw.accepted = 0; g_fw.written = 0;
    xSemaphoreGive(g_fwMx);
    r->send(200, "application/json", "{\"state\":\"idle\"}");
  });

  server.on("/api/firmware/apply", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    if (g_fw.state != FW_READY || !g_fw.part) { r->send(409, "application/json", "{\"error\":\"Nothing checked and ready\"}"); return; }
    esp_err_t e = esp_ota_set_boot_partition(g_fw.part);
    if (e != ESP_OK) { r->send(500, "application/json", String("{\"error\":\"") + esp_err_to_name(e) + "\"}"); return; }
    Preferences p;
    if (p.begin("nomadfw", false)) { p.putString("pend", g_fw.version); p.remove("why"); p.remove("last"); p.end(); }
    webLogf("info", "Restarting into firmware %s", g_fw.version.c_str());
    r->send(200, "application/json", "{\"restarting\":true}");
    g_wifiModeRestartAt = millis() + 1500;   // the loop's deferred restart, after this reply is out
  });
}

#endif  // NOMAD_FIRMWARE_H
