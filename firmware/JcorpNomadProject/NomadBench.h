// NomadBench.h - measurement endpoints for performance testing.
//
// Only compiled with -DNOMAD_BENCH=1. Release builds don't have any of this.
//
//   GET  /bench/blast?bytes=N[&us=D]   N bytes from RAM through the same response class as
//                                      /media: the network path with the card taken out.
//                                      us=D busy-waits D microseconds per fill, to model a
//                                      card read of that cost.
//   GET  /bench/stats[?reset=1]        what the media fill path has been asked to do
//   POST /bench/stream?aligned=0|1[&chunk=N]   switch the read-ahead (NomadReadAhead.h)
//   POST /bench/sd?file=P&mode=dma|psram|mis&chunk=N&mb=M[&off=O]   card read benchmark,
//                                      on its own task, poll GET /bench/sd for the result
//   GET  /bench/radio                  channel, width, protocols, power, clients, heap
//   POST /bench/radio?bw=20|40&proto=bgn|gn&no11b=0|1&ch=C&csa=K&txq=Q   change them at
//                                      runtime (not saved: a reboot is the baseline again)
//   POST /bench/air?ms=T               sniff our own channel for T ms, GET /bench/air reads it
//
// Every result is JSON.

#ifndef NOMAD_BENCH_H
#define NOMAD_BENCH_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <rom/ets_sys.h>
#include "NomadSD.h"
#include "NomadReadAhead.h"
#include "lwip/tcpip.h"
#include "lwip/tcp.h"

// Per-connection send buffer.
// The prebuilt core gives every TCP connection a 5,744-byte send budget (TCP_SND_BUF).
// Raising pcb->snd_buf on a fresh connection, before anything is queued, gives that one
// connection a bigger window without rebuilding the core. TCP_SND_QUEUELEN is still 16
// segments, which caps the useful size near 23 KB. Experimental.
static volatile uint32_t g_benchSndBuf = 0;   // 0 = leave what the core gave

// Returns true when the bench has set a size, so the build's own default stays out of it.
static bool benchApplySndBuf(AsyncWebServerRequest *request) {
  if (!g_benchSndBuf) return false;
  nomadRaiseSndBuf(request, g_benchSndBuf);
  return true;
}

static String g_benchRadioLog = "";   // what the last radio change returned

static String benchHeapJson() {
  String j = "{";
  j += "\"internalFree\":" + String((unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  j += ",\"internalLargest\":" + String((unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  j += ",\"internalMin\":" + String((unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  j += ",\"dmaFree\":" + String((unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
  j += ",\"dmaLargest\":" + String((unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
  j += ",\"psramFree\":" + String((unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  j += "}";
  return j;
}

static String benchMac(const uint8_t *m) {
  char b[18];
  snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

// blast

static void benchBlast(AsyncWebServerRequest *request) {
  size_t bytes = request->hasParam("bytes") ? (size_t)request->getParam("bytes")->value().toInt() : 8u << 20;
  if (bytes == 0 || bytes > (256u << 20)) bytes = 8u << 20;
  const uint32_t us = request->hasParam("us") ? (uint32_t)request->getParam("us")->value().toInt() : 0;
  AsyncWebServerResponse *r = request->beginResponse(
    "application/octet-stream", bytes,
    [us](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      size_t n = maxLen > 16384 ? 16384 : maxLen;   // the media filler's own cap
      memset(buffer, 'N', n);
      if (us) ets_delay_us(us);
      return n;
    });
  if (!r) { request->send(503, "text/plain", "low memory"); return; }
  r->addHeader("Cache-Control", "no-store");
  r->addHeader("Connection", "close");   // as /media does
  if (!benchApplySndBuf(request)) nomadRaiseSndBuf(request, NOMAD_MEDIA_SNDBUF);
  request->send(r);
}

// stats

static String benchStatsJson() {
  const NomadFillStats s = g_fillStats;
  String j = "{";
  j += "\"fills\":" + String(s.fills);
  j += ",\"bytes\":" + String((unsigned long long)s.bytes);
  j += ",\"readUs\":" + String((unsigned long long)s.readUs);
  j += ",\"refills\":" + String(s.refills);
  j += ",\"retries\":" + String(s.retries);
  j += ",\"psramBuffer\":" + String(s.psramBuffer);
  j += ",\"unalignedPos\":" + String(s.unalignedPos);
  j += ",\"maxLenHist\":{";
  static const char *labels[NOMAD_FILL_HIST] = {"le1k", "le2k", "le4k", "le8k", "le16k", "gt16k"};
  for (int i = 0; i < NOMAD_FILL_HIST; i++) {
    if (i) j += ",";
    j += "\"" + String(labels[i]) + "\":" + String(s.hist[i]);
  }
  j += "},\"aligned\":" + String(g_nomadAlignedReads ? "true" : "false");
  j += ",\"raChunk\":" + String((unsigned)g_nomadRaBytes);
  j += ",\"sndBuf\":" + String((unsigned)g_benchSndBuf);
  j += ",\"heap\":" + benchHeapJson();
  j += ",\"uptimeMs\":" + String((unsigned long)millis());
  j += "}";
  return j;
}

// card read benchmark

struct BenchSdJob {
  String file;
  String mode;
  size_t chunk;
  uint32_t mb;
  uint64_t off;
};
static volatile bool g_benchSdRunning = false;
static String g_benchSdResult = "{\"state\":\"idle\"}";

static void benchSdTask(void *arg) {
  BenchSdJob *job = (BenchSdJob *)arg;
  String res;
  uint8_t *raw = nullptr, *buf = nullptr;
  if (job->mode == "psram") {
    raw = (uint8_t *)heap_caps_malloc(job->chunk + 64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    buf = raw;
  } else {
    raw = (uint8_t *)heap_caps_aligned_alloc(64, job->chunk + 64, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    buf = raw ? (job->mode == "mis" ? raw + 3 : raw) : nullptr;
  }
  NomadFile64 f;
  bool opened = false;
  if (buf) {
    if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
    opened = f.open(job->file.c_str());
    if (sdMutex) xSemaphoreGive(sdMutex);
  }
  if (!buf) {
    res = "{\"state\":\"error\",\"error\":\"no buffer\"}";
  } else if (!opened) {
    res = "{\"state\":\"error\",\"error\":\"open failed\"}";
  } else {
    const uint64_t total = (uint64_t)job->mb << 20;
    uint64_t pos = job->off, done = 0;
    uint32_t reads = 0, worstUs = 0;
    const int64_t t0 = esp_timer_get_time();
    while (done < total) {
      const int64_t r0 = esp_timer_get_time();
      if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
      const size_t got = f.readAt(pos, buf, job->chunk);
      if (sdMutex) xSemaphoreGive(sdMutex);
      const uint32_t dt = (uint32_t)(esp_timer_get_time() - r0);
      if (dt > worstUs) worstUs = dt;
      if (got == 0) break;
      pos += got; done += got; reads++;
    }
    const int64_t us = esp_timer_get_time() - t0;
    if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
    f.close();
    if (sdMutex) xSemaphoreGive(sdMutex);
    const double mbps = us > 0 ? (double)done / (double)us : 0;   // bytes/us == MB/s (1e6)
    res = "{\"state\":\"done\",\"mode\":\"" + job->mode + "\",\"chunk\":" + String((unsigned)job->chunk) +
          ",\"bytes\":" + String((unsigned long long)done) + ",\"us\":" + String((long long)us) +
          ",\"reads\":" + String(reads) + ",\"worstReadUs\":" + String(worstUs) +
          ",\"MBps\":" + String(mbps, 3) + ",\"off\":" + String((unsigned long long)job->off) +
          ",\"bufInPsram\":" + String(esp_ptr_external_ram(buf) ? "true" : "false") +
          ",\"bufAlign4\":" + String(((uintptr_t)buf & 3) ? "false" : "true") + "}";
  }
  if (raw) heap_caps_free(raw);
  g_benchSdResult = res;
  delete job;
  g_benchSdRunning = false;
  vTaskDelete(NULL);
}

static void benchSdStart(AsyncWebServerRequest *request) {
  if (g_benchSdRunning) { request->send(409, "application/json", "{\"error\":\"running\"}"); return; }
  if (!request->hasParam("file", true) && !request->hasParam("file")) {
    request->send(400, "application/json", "{\"error\":\"file=\"}");
    return;
  }
  auto pv = [&](const char *k, const char *def) -> String {
    if (request->hasParam(k, true)) return request->getParam(k, true)->value();
    if (request->hasParam(k)) return request->getParam(k)->value();
    return String(def);
  };
  BenchSdJob *job = new BenchSdJob();
  job->file = pv("file", "");
  job->mode = pv("mode", "dma");
  job->chunk = (size_t)pv("chunk", "16384").toInt();
  if (job->chunk < 512 || job->chunk > 65536) job->chunk = 16384;
  job->mb = (uint32_t)pv("mb", "16").toInt();
  if (job->mb == 0 || job->mb > 512) job->mb = 16;
  job->off = (uint64_t)strtoull(pv("off", "0").c_str(), nullptr, 10);
  g_benchSdRunning = true;
  g_benchSdResult = "{\"state\":\"running\"}";
  if (xTaskCreatePinnedToCore(benchSdTask, "benchSd", 6144, job, 5, NULL, 1) != pdPASS) {
    g_benchSdRunning = false;
    delete job;
    request->send(503, "application/json", "{\"error\":\"no task\"}");
    return;
  }
  request->send(202, "application/json", "{\"state\":\"running\"}");
}

// radio

static String benchRadioJson() {
  String j = "{";
  uint8_t pri = 0; wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&pri, &sec);
  wifi_bandwidth_t bw = WIFI_BW_HT20;
  esp_wifi_get_bandwidth(WIFI_IF_AP, &bw);
  uint8_t proto = 0;
  esp_wifi_get_protocol(WIFI_IF_AP, &proto);
  int8_t txq = 0;
  esp_wifi_get_max_tx_power(&txq);
  wifi_config_t cfg = {};
  esp_wifi_get_config(WIFI_IF_AP, &cfg);
  uint8_t mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_AP, mac);
  j += "\"bssid\":\"" + benchMac(mac) + "\"";
  j += ",\"channel\":" + String(pri);
  j += ",\"secondary\":\"" + String(sec == WIFI_SECOND_CHAN_ABOVE ? "above" : sec == WIFI_SECOND_CHAN_BELOW ? "below" : "none") + "\"";
  j += ",\"bandwidth\":" + String(bw == WIFI_BW_HT40 ? 40 : 20);
  j += ",\"protocol\":\"";
  if (proto & WIFI_PROTOCOL_11B) j += "b";
  if (proto & WIFI_PROTOCOL_11G) j += "g";
  if (proto & WIFI_PROTOCOL_11N) j += "n";
  if (proto & WIFI_PROTOCOL_LR) j += "+lr";
  j += "\",\"txPowerDbm\":" + String(txq / 4.0f, 2);
  j += ",\"beaconInterval\":" + String(cfg.ap.beacon_interval);
  j += ",\"dtim\":" + String(cfg.ap.dtim_period);
  j += ",\"csaCount\":" + String(cfg.ap.csa_count);
  j += ",\"radioLog\":\"" + g_benchRadioLog + "\"";
  j += ",\"maxConnection\":" + String(cfg.ap.max_connection);
  j += ",\"stations\":[";
  wifi_sta_list_t sl = {};
  if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK) {
    for (int i = 0; i < sl.num; i++) {
      const wifi_sta_info_t &s = sl.sta[i];
      if (i) j += ",";
      j += "{\"mac\":\"" + benchMac(s.mac) + "\",\"rssi\":" + String(s.rssi) + ",\"phy\":\"";
      if (s.phy_11b) j += "b";
      if (s.phy_11g) j += "g";
      if (s.phy_11n) j += "n";
      if (s.phy_lr) j += "+lr";
      j += "\"}";
    }
  }
  j += "],\"heap\":" + benchHeapJson() + "}";
  return j;
}

struct BenchRadioChange {
  int bw = 0;          // 20 / 40 / 0 = leave
  int proto = -1;      // 0 = bgn, 1 = gn, -1 = leave
  int no11b = -1;      // 1 = 11b rates off, 0 = on, -1 = leave
  int ch = 0;          // 0 = leave
  int csa = -1;        // beacons of warning before a channel move, -1 = core default
  int txq = 0;         // quarter-dBm, 0 = leave
};

// A live radio change restarts the hotspot for under a second. Some clients miss that and
// sit on a dead association, since the restarted AP doesn't know them and never tells them.
// So a persisted change is stored in RTC memory, the radio goes off long enough for every
// client to notice, and the board reboots and applies it before anyone joins.
// RTC_NOINIT only survives a soft reset, so unplugging the board goes back to defaults.
#include <esp_attr.h>
static const uint32_t BENCH_RADIO_MAGIC = 0x4e4d5244;   // "NMRD"
RTC_NOINIT_ATTR static uint32_t g_benchBootRadioMagic;
// plain ints: a type with default member values would get re-initialised at every boot
RTC_NOINIT_ATTR static int32_t g_benchBootRadio[6];   // bw, proto, no11b, ch, csa, txq

// atBoot: the hotspot just started and nobody has joined, so the radio can be restarted.
// esp_wifi_config_11b_rate() only works between esp_wifi_init() and esp_wifi_start(), and
// IDF 5.5 has no g/n-only protocol on 2.4 GHz (b, bg, bgn or LR), so turning 11b rates
// off needs a stop and a start.
static String benchRadioApply(const BenchRadioChange *c, bool atBoot = false) {
  String log;
  if (atBoot && c->no11b == 1) {
    esp_wifi_stop();
    esp_err_t e11 = esp_wifi_config_11b_rate(WIFI_IF_AP, true);
    esp_wifi_start();
    ensureApDhcpServer("bench-11b-off");
    log += "11b_rate_off(restart):" + String(esp_err_to_name(e11)) + " ";
  }
  esp_err_t e;
  if (c->proto >= 0) {
    const uint8_t bm = c->proto == 1 ? (WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N)
                                     : (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    e = esp_wifi_set_protocol(WIFI_IF_AP, bm);
    log += "proto:" + String(esp_err_to_name(e)) + " ";
  }
  if (c->no11b >= 0 && !atBoot) {
    e = esp_wifi_config_11b_rate(WIFI_IF_AP, c->no11b == 1);
    log += "11b_rate:" + String(esp_err_to_name(e)) + " ";
  }
  if (c->bw) {
    e = esp_wifi_set_bandwidth(WIFI_IF_AP, c->bw == 40 ? WIFI_BW_HT40 : WIFI_BW_HT20);
    log += "bw:" + String(esp_err_to_name(e)) + " ";
  }
  if (c->ch) {
    wifi_config_t cfg = {};
    esp_wifi_get_config(WIFI_IF_AP, &cfg);
    cfg.ap.channel = (uint8_t)c->ch;
    if (c->csa >= 0) cfg.ap.csa_count = (uint8_t)c->csa;
    e = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    log += "ch:" + String(esp_err_to_name(e)) + " ";
  }
  if (c->txq) {
    e = esp_wifi_set_max_tx_power((int8_t)c->txq);
    log += "txq:" + String(esp_err_to_name(e)) + " ";
  }
  return log;
}

static void benchRadioApplyTask(void *arg) {
  BenchRadioChange *c = (BenchRadioChange *)arg;
  vTaskDelay(pdMS_TO_TICKS(300));   // let the HTTP answer leave before the radio moves
  g_benchRadioLog = benchRadioApply(c);
  Serial.printf("[Bench] radio change: %s\n", g_benchRadioLog.c_str());
  delete c;
  vTaskDelete(NULL);
}

static void benchRadioRebootTask(void *arg) {
  const uint32_t offMs = (uint32_t)(uintptr_t)arg;
  vTaskDelay(pdMS_TO_TICKS(300));
  Serial.printf("[Bench] radio off for %u ms, then reboot into the stored radio state\n", (unsigned)offMs);
  esp_wifi_stop();
  vTaskDelay(pdMS_TO_TICKS(offMs));
  esp_restart();
}

// Called by startNetwork() right after the hotspot comes up.
static void benchApplyBootRadio() {
  if (g_benchBootRadioMagic != BENCH_RADIO_MAGIC) return;
  BenchRadioChange c;
  c.bw = g_benchBootRadio[0]; c.proto = g_benchBootRadio[1]; c.no11b = g_benchBootRadio[2];
  c.ch = g_benchBootRadio[3]; c.csa = g_benchBootRadio[4]; c.txq = g_benchBootRadio[5];
  g_benchRadioLog = "boot: " + benchRadioApply(&c, true);
  Serial.printf("[Bench] boot radio state applied: %s\n", g_benchRadioLog.c_str());
}

static void benchRadioChange(AsyncWebServerRequest *request) {
  auto pi = [&](const char *k, int def) -> int {
    if (request->hasParam(k, true)) return request->getParam(k, true)->value().toInt();
    if (request->hasParam(k)) return request->getParam(k)->value().toInt();
    return def;
  };
  auto ps = [&](const char *k) -> String {
    if (request->hasParam(k, true)) return request->getParam(k, true)->value();
    if (request->hasParam(k)) return request->getParam(k)->value();
    return String();
  };
  BenchRadioChange *c = new BenchRadioChange();
  c->bw = pi("bw", 0);
  const String p = ps("proto");
  c->proto = p == "gn" ? 1 : p == "bgn" ? 0 : -1;
  c->no11b = pi("no11b", -1);
  c->ch = pi("ch", 0);
  c->csa = pi("csa", -1);
  c->txq = pi("txq", 0);
  if (c->ch < 0 || c->ch > 13) c->ch = 0;
  if (pi("reset", 0) == 1) {               // forget the stored state, the next boot is back to defaults
    g_benchBootRadioMagic = 0;
    delete c;
    request->send(200, "application/json", "{\"state\":\"reset\"}");
    return;
  }
  if (pi("persist", 0) == 1) {
    g_benchBootRadio[0] = c->bw; g_benchBootRadio[1] = c->proto; g_benchBootRadio[2] = c->no11b;
    g_benchBootRadio[3] = c->ch; g_benchBootRadio[4] = c->csa; g_benchBootRadio[5] = c->txq;
    g_benchBootRadioMagic = BENCH_RADIO_MAGIC;
    delete c;
    const uint32_t offMs = (uint32_t)pi("offms", 15000);
    xTaskCreatePinnedToCore(benchRadioRebootTask, "benchReboot", 3072, (void *)(uintptr_t)offMs, 3, NULL, 1);
    request->send(202, "application/json", "{\"state\":\"rebooting\"}");
    return;
  }
  if (xTaskCreatePinnedToCore(benchRadioApplyTask, "benchRadio", 4096, c, 3, NULL, 1) != pdPASS) {
    delete c;
    request->send(503, "application/json", "{\"error\":\"no task\"}");
    return;
  }
  request->send(202, "application/json", "{\"state\":\"applying\",\"last\":\"" + g_benchRadioLog + "\"}");
}

// airtime on our channel

// Promiscuous capture is only on for the requested window. The sniffer costs throughput,
// so don't measure throughput while it runs.
static const int BENCH_AIR_TOP = 8;
struct BenchAirTally {
  uint32_t frames, oursFrames, othersFrames, mgmt, data;
  uint64_t oursUs, othersUs;
  int32_t noiseSum; uint32_t noiseN;
  uint8_t topBssid[BENCH_AIR_TOP][6];
  uint64_t topUs[BENCH_AIR_TOP];
  uint32_t topFrames[BENCH_AIR_TOP];
};
static BenchAirTally g_air;
static uint8_t g_airOurMac[6];
static portMUX_TYPE g_airMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool g_benchAirRunning = false;
static String g_benchAirResult = "{\"state\":\"idle\"}";

// Rough on-air time of one received frame: preamble plus payload at the PHY rate. It
// ignores contention, SIFS and the ACK, so it's a lower bound.
static uint32_t benchAirUs(const wifi_pkt_rx_ctrl_t &rx) {
  const uint32_t bits = (uint32_t)rx.sig_len * 8;
  if (rx.sig_mode == 0) {
    float mbps; uint32_t pre;
    switch (rx.rate) {
      case 0x00: mbps = 1;    pre = 192; break;
      case 0x01: mbps = 2;    pre = 192; break;
      case 0x02: mbps = 5.5f; pre = 192; break;
      case 0x03: mbps = 11;   pre = 192; break;
      case 0x05: mbps = 2;    pre = 96;  break;
      case 0x06: mbps = 5.5f; pre = 96;  break;
      case 0x07: mbps = 11;   pre = 96;  break;
      case 0x08: mbps = 48;   pre = 20;  break;
      case 0x09: mbps = 24;   pre = 20;  break;
      case 0x0A: mbps = 12;   pre = 20;  break;
      case 0x0B: mbps = 6;    pre = 20;  break;
      case 0x0C: mbps = 54;   pre = 20;  break;
      case 0x0D: mbps = 36;   pre = 20;  break;
      case 0x0E: mbps = 18;   pre = 20;  break;
      default:   mbps = 9;    pre = 20;  break;
    }
    return pre + (uint32_t)(bits / mbps);
  }
  static const float ht20[8] = {6.5f, 13, 19.5f, 26, 39, 52, 58.5f, 65};
  float mbps = ht20[rx.mcs & 7] * (float)((rx.mcs >> 3) + 1);
  if (rx.cwb) mbps *= 2.077f;
  if (rx.sgi) mbps *= 1.111f;
  return 36 + (uint32_t)(bits / mbps);
}

static void benchAirCb(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
  const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
  if (p->rx_ctrl.sig_len < 24) return;
  const uint8_t *h = p->payload;
  const uint8_t *a1 = h + 4, *a2 = h + 10, *a3 = h + 16;
  const bool ours = !memcmp(a1, g_airOurMac, 6) || !memcmp(a2, g_airOurMac, 6) || !memcmp(a3, g_airOurMac, 6);
  const uint32_t us = benchAirUs(p->rx_ctrl);
  portENTER_CRITICAL_ISR(&g_airMux);
  g_air.frames++;
  if (type == WIFI_PKT_MGMT) g_air.mgmt++; else g_air.data++;
  g_air.noiseSum += p->rx_ctrl.noise_floor; g_air.noiseN++;
  if (ours) { g_air.oursFrames++; g_air.oursUs += us; }
  else {
    g_air.othersFrames++; g_air.othersUs += us;
    int slot = -1, empty = -1;
    for (int i = 0; i < BENCH_AIR_TOP; i++) {
      if (g_air.topFrames[i] == 0) { if (empty < 0) empty = i; continue; }
      if (!memcmp(g_air.topBssid[i], a3, 6)) { slot = i; break; }
    }
    if (slot < 0 && empty >= 0) { slot = empty; memcpy(g_air.topBssid[slot], a3, 6); }
    if (slot >= 0) { g_air.topFrames[slot]++; g_air.topUs[slot] += us; }
  }
  portEXIT_CRITICAL_ISR(&g_airMux);
}

static void benchAirTask(void *arg) {
  const uint32_t ms = (uint32_t)(uintptr_t)arg;
  memset(&g_air, 0, sizeof g_air);
  esp_wifi_get_mac(WIFI_IF_AP, g_airOurMac);
  wifi_promiscuous_filter_t filt = {};
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(benchAirCb);
  const int64_t t0 = esp_timer_get_time();
  esp_err_t e = esp_wifi_set_promiscuous(true);
  vTaskDelay(pdMS_TO_TICKS(ms));
  esp_wifi_set_promiscuous(false);
  const int64_t us = esp_timer_get_time() - t0;
  BenchAirTally t;
  portENTER_CRITICAL(&g_airMux);
  t = g_air;
  portEXIT_CRITICAL(&g_airMux);
  uint8_t pri = 0; wifi_second_chan_t sec;
  esp_wifi_get_channel(&pri, &sec);
  String j = "{\"state\":\"done\",\"err\":\"" + String(esp_err_to_name(e)) + "\",\"channel\":" + String(pri) +
             ",\"windowUs\":" + String((long long)us) + ",\"frames\":" + String(t.frames) +
             ",\"mgmt\":" + String(t.mgmt) + ",\"data\":" + String(t.data) +
             ",\"oursFrames\":" + String(t.oursFrames) + ",\"oursUs\":" + String((unsigned long long)t.oursUs) +
             ",\"othersFrames\":" + String(t.othersFrames) + ",\"othersUs\":" + String((unsigned long long)t.othersUs) +
             ",\"othersPct\":" + String(us > 0 ? 100.0 * (double)t.othersUs / (double)us : 0, 2) +
             ",\"noiseFloor\":" + String(t.noiseN ? (float)t.noiseSum / t.noiseN : 0, 1) + ",\"top\":[";
  bool first = true;
  for (int i = 0; i < BENCH_AIR_TOP; i++) {
    if (!t.topFrames[i]) continue;
    if (!first) j += ",";
    first = false;
    j += "{\"bssid\":\"" + benchMac(t.topBssid[i]) + "\",\"frames\":" + String(t.topFrames[i]) +
         ",\"us\":" + String((unsigned long long)t.topUs[i]) + "}";
  }
  j += "]}";
  g_benchAirResult = j;
  g_benchAirRunning = false;
  vTaskDelete(NULL);
}

static void benchAirStart(AsyncWebServerRequest *request) {
  if (g_benchAirRunning) { request->send(409, "application/json", "{\"error\":\"running\"}"); return; }
  uint32_t ms = 2000;
  if (request->hasParam("ms", true)) ms = request->getParam("ms", true)->value().toInt();
  else if (request->hasParam("ms")) ms = request->getParam("ms")->value().toInt();
  if (ms < 100 || ms > 10000) ms = 2000;
  g_benchAirRunning = true;
  g_benchAirResult = "{\"state\":\"running\"}";
  if (xTaskCreatePinnedToCore(benchAirTask, "benchAir", 4096, (void *)(uintptr_t)ms, 3, NULL, 1) != pdPASS) {
    g_benchAirRunning = false;
    request->send(503, "application/json", "{\"error\":\"no task\"}");
    return;
  }
  request->send(202, "application/json", "{\"state\":\"running\"}");
}

// routes

static void nomadBenchRegister(AsyncWebServer &server) {
  // Don't allocate the read-ahead buffer at boot. Holding 16 KB of DMA heap pushes the
  // board under the streaming floor and covers and seeks get refused during playback.
  // /bench/stream?aligned=1 allocates it on demand.
  server.on("/bench/blast", HTTP_GET, benchBlast);
  server.on("/bench/stats", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (r->hasParam("reset")) nomadFillStatsReset();
    r->send(200, "application/json", benchStatsJson());
  });
  server.on("/bench/stream", HTTP_POST, [](AsyncWebServerRequest *r) {
    auto pv = [&](const char *k) -> String {
      if (r->hasParam(k, true)) return r->getParam(k, true)->value();
      if (r->hasParam(k)) return r->getParam(k)->value();
      return String();
    };
    const String a = pv("aligned"), c = pv("chunk"), sb = pv("sndbuf");
    if (sb.length()) {
      long v = sb.toInt();
      g_benchSndBuf = (v <= 0) ? 0 : (uint32_t)(v < 5744 ? 5744 : v > 65535 ? 65535 : v);
    }
    if (c.length()) {
      size_t n = (size_t)c.toInt() & ~(size_t)511;
      if (n >= 512 && n <= NOMAD_RA_MAX) g_nomadRaBytes = n;
    }
    if (a == "1") {
      if (!nomadReadAheadEnsureBuffer()) { r->send(503, "application/json", "{\"error\":\"no DMA buffer\"}"); return; }
      g_nomadAlignedReads = true;
    } else if (a == "0") {
      g_nomadAlignedReads = false;
    }
    r->send(200, "application/json", benchStatsJson());
  });
  server.on("/bench/sd", HTTP_POST, benchSdStart);
  server.on("/bench/sd", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "application/json", g_benchSdResult);
  });
  server.on("/bench/radio", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "application/json", benchRadioJson());
  });
  server.on("/bench/radio", HTTP_POST, benchRadioChange);
  server.on("/bench/air", HTTP_POST, benchAirStart);
  server.on("/bench/air", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "application/json", g_benchAirResult);
  });
  Serial.println("[Bench] /bench/* endpoints registered (NOMAD_BENCH build)");
}

#endif  // NOMAD_BENCH_H
