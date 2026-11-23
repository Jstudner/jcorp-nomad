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
#include "src/AsyncWebServer/ESPAsyncWebServer.h"   // carried here, with Nomad's limits (src/AsyncWebServer/README.md)
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

// Requests driven over USB serial, for when there is no WiFi client to hand. A line typed
// at the serial port is run by a task on the board as a web request to the board itself:
//   get /path?query [first-last]                (a byte range, as a player asks)
//   post /path a=b&c=d                          (a form body, already URL-encoded)
//   up /Files name.bin 1500000 [total offset]   (a multipart upload of that many made-up bytes)
// The answer comes back as "[Cmd] <code> <ms> ms <bytes> B" and the start of the body. The
// radio is not in the path, so this tests what the firmware does, not how WiFi carries it.
extern volatile uint32_t g_lcdFlushes;
static QueueHandle_t s_benchCmdQ = nullptr;
static void benchCmdRun(const String &line) {
  const int sp = line.indexOf(' ');
  const String verb = sp < 0 ? line : line.substring(0, sp);
  String rest = sp < 0 ? String() : line.substring(sp + 1);
  rest.trim();
  String path = rest, arg;
  const int sp2 = rest.indexOf(' ');
  if (sp2 > 0) { path = rest.substring(0, sp2); arg = rest.substring(sp2 + 1); arg.trim(); }
  const bool dump = verb == "dump";      // dump /path: a get whose whole body (to 12 KB) is printed, in short lines
  if (!path.startsWith("/") || (verb != "get" && verb != "post" && verb != "up" && !dump)) {
    Serial.println("[Cmd] ? get /path | post /path body | up /dir name bytes [total offset]");
    return;
  }
  WiFiClient c;
  c.setTimeout(60000);
  const uint32_t t0 = millis();
  if (!c.connect(IPAddress(127, 0, 0, 1), 80, 3000) && !c.connect(WiFi.softAPIP(), 80, 3000)) {
    Serial.println("[Cmd] could not connect");
    return;
  }
  const char *host = "Host: 192.168.4.1\r\nConnection: close\r\n";
  if (verb == "get" || dump) {
    if (arg.length()) c.printf("GET %s HTTP/1.1\r\n%sRange: bytes=%s\r\n\r\n", path.c_str(), host, arg.c_str());   // get /path 0-65535
    else c.printf("GET %s HTTP/1.1\r\n%s\r\n", path.c_str(), host);
  } else if (verb == "post") {
    c.printf("POST %s HTTP/1.1\r\n%sContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %u\r\n\r\n",
             path.c_str(), host, (unsigned)arg.length());
    c.print(arg);
  } else {
    // up <dir> <name> <bytes> [total offset]
    String name = arg, nums;
    const int a = arg.indexOf(' ');
    if (a > 0) { name = arg.substring(0, a); nums = arg.substring(a + 1); }
    unsigned long bytes = 0, total = 0, offset = 0;
    sscanf(nums.c_str(), "%lu %lu %lu", &bytes, &total, &offset);
    if (!total) total = bytes;
    const String head = String("--nomadbench\r\nContent-Disposition: form-data; name=\"dir\"\r\n\r\n") + path +
                        "\r\n--nomadbench\r\nContent-Disposition: form-data; name=\"file\"; filename=\"" + name +
                        "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
    const char *tail = "\r\n--nomadbench--\r\n";
    c.printf("POST /upload?dir=%s&total=%lu&offset=%lu HTTP/1.1\r\n%sContent-Type: multipart/form-data; boundary=nomadbench\r\nContent-Length: %lu\r\n\r\n",
             path.c_str(), total, offset, host, (unsigned long)(head.length() + bytes + strlen(tail)));
    c.print(head);
    uint8_t blk[1024];
    for (size_t i = 0; i < sizeof(blk); i++) blk[i] = (uint8_t)(i * 7 + 3);
    unsigned long left = bytes;
    while (left && c.connected()) {
      const size_t n = left > sizeof(blk) ? sizeof(blk) : left;
      const size_t w = c.write(blk, n);
      if (!w) { if (millis() - t0 > 120000) break; delay(5); continue; }
      left -= w;
    }
    c.print(tail);
  }
  // the reply: status line, then everything until the board closes
  String first, body;
  size_t got = 0;
  bool inBody = false;
  String hdr;
  const uint32_t t1 = millis();
  while ((c.connected() || c.available()) && millis() - t1 < 120000) {
    if (!c.available()) { delay(5); continue; }
    if (inBody && body.length() >= (dump ? 12000u : 200u)) {        // the rest is only counted
      uint8_t skip[1024];
      const int k = c.read(skip, sizeof(skip));
      if (k > 0) got += k;
      continue;
    }
    const int ch = c.read();
    if (ch < 0) continue;
    if (!inBody) {
      hdr += (char)ch;
      if (hdr.endsWith("\r\n\r\n")) { inBody = true; first = hdr.substring(0, hdr.indexOf('\r')); hdr = String(); }
      else if (hdr.length() > 2048) hdr.remove(0, 1024);
      continue;
    }
    got++;
    if (body.length() < (dump ? 12000u : 200u)) body += (dump || (ch >= 0x20 && ch < 0x7f)) ? (char)ch : ' ';
  }
  c.stop();
  Serial.printf("[Cmd] %s %s -> %s  %lu ms  %u B\n", verb.c_str(), path.c_str(),
                first.length() ? first.c_str() + (first.startsWith("HTTP/1.1 ") ? 9 : 0) : "no reply",
                (unsigned long)(millis() - t0), (unsigned)got);
  if (dump) {
    // hex, 80 bytes a line, so nothing in the body can break a line or be lost to the console
    for (size_t at = 0; at < body.length(); at += 80) {
      char hex[176];
      size_t k = 0;
      for (size_t i = at; i < body.length() && i < at + 80; i++) k += snprintf(hex + k, sizeof(hex) - k, "%02x", (uint8_t)body[i]);
      Serial.printf("[Dump] %s\n", hex);
      delay(12);
    }
    Serial.println("[Dump] end");
  } else if (body.length()) { Serial.print("[Cmd] | "); Serial.println(body); }
}
static void benchCmdTask(void *) {
  for (;;) {
    String *line = nullptr;
    if (xQueueReceive(s_benchCmdQ, &line, portMAX_DELAY) != pdTRUE || !line) continue;
    benchCmdRun(*line);
    delete line;
  }
}
static void benchCmdTick() {
  static String buf;
  while (Serial.available()) {
    const int ch = Serial.read();
    if (ch == '\r') continue;
    if (ch != '\n') { if (buf.length() < 400) buf += (char)ch; continue; }
    buf.trim();
    if (buf.length()) {
      if (!s_benchCmdQ) {
        s_benchCmdQ = xQueueCreate(8, sizeof(String *));
        xTaskCreatePinnedToCore(benchCmdTask, "BenchCmd", 8 * 1024, NULL, 1, NULL, 1);
      }
      String *line = new String(buf);
      if (!s_benchCmdQ || xQueueSend(s_benchCmdQ, &line, 0) != pdTRUE) delete line;
    }
    buf = String();
  }
}

// loop(), bench builds: one serial line every 30 s with free memory and every task's lowest
// stack headroom, so memory and stack changes can be measured over USB serial alone.
static void benchSerialTick() {
  benchCmdTick();
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 30000) return;
  last = now;
  const UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;
  TaskStatus_t *st = (TaskStatus_t *)heap_caps_malloc(cap * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!st) return;
  const UBaseType_t n = uxTaskGetSystemState(st, cap, NULL);
  // short lines: the USB serial buffer is 256 bytes, and a longer write is cut before its newline
  Serial.printf("[Bench] up %lu s  dma %u (low %u)  heap low %u\n", (unsigned long)(now / 1000),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  String line = "[Bench] stacks:";
  for (UBaseType_t i = 0; i < n; i++) {
    const char *nm = st[i].pcTaskName;
    if (!strncmp(nm, "IDLE", 4) || !strncmp(nm, "ipc", 3)) continue;
    line += " " + String(nm) + "=" + String((unsigned)st[i].usStackHighWaterMark);
    if (line.length() > 150) { Serial.println(line); line = "[Bench] stacks:"; }
  }
  if (line.length() > 15) Serial.println(line);
  free(st);
}

// /bench/memwhy: where internal memory goes under load. A 20 ms sampler keeps the lowest
// internal free and, at each new low, what was holding it: TCP data queued or waiting for
// an ack (and whether lwIP put those buffers in internal RAM or PSRAM), connections, media
// replies. Every failed allocation is kept too (size, caps, task), and a 1 s timeline in
// PSRAM. ?reset=1 starts over. ?csv=1 gives the timeline.
#include "lwip/priv/tcp_priv.h"
#include <esp_memory_utils.h>
struct MwTcp { uint16_t pcbs, tw, segs; uint32_t unsent, unacked, bufInt, bufExt, ooseq; };
struct MwSnap { uint32_t ms, freeInt, largestInt, freeDma; MwTcp t; int16_t media, streams; uint16_t qlen; };
// the async_tcp event queue, if the AsyncTCP build has these probes (weak: absent = not counted)
extern "C" size_t asynctcp_queue_len() __attribute__((weak));
extern "C" size_t asynctcp_queue_peak_reset() __attribute__((weak));
struct MwFail { uint32_t ms, size, caps; char task[12]; char fn[20]; };
static volatile uint32_t s_mwLow = UINT32_MAX;
static MwSnap s_mwAtLow = {};
static MwFail s_mwFails[24];
static volatile uint32_t s_mwFailN = 0;
static MwSnap *s_mwLine = nullptr;          // PSRAM, one a second
static const uint32_t MW_LINE = 1800;       // 30 minutes
static volatile uint32_t s_mwLineN = 0;
static TaskHandle_t s_mwTask = nullptr;
// used internal blocks by size, at the low: who holds the memory
static const int MW_B = 9;
static const uint16_t s_mwEdge[MW_B] = {64, 256, 1024, 1590, 1700, 2800, 2950, 8192, 65535};
struct MwWalk { uint32_t n[MW_B], bytes[MW_B]; uint16_t topSize[8]; uint16_t topN[8]; uint32_t freeBlocks, freeBytes; };
static MwWalk s_mwWalkAtLow = {};
struct MwTally { MwWalk w; uint16_t sz[48]; uint16_t cnt[48]; };
// ?blocks=1: every used internal block of 256 B or less, with its first two words, so the
// new ones at the low can be told apart and a vtable or function address in them named
struct MwBlk { uint32_t ptr; uint16_t size; uint32_t w0, w1; };
static MwBlk *s_mwBlk = nullptr;          // PSRAM
static const uint32_t MW_BLK = 1400;
static volatile uint32_t s_mwBlkN = 0;
static volatile bool s_mwBlkWant = false;
static bool mwWalkCb(walker_heap_into_t, walker_block_info_t b, void *u) {
  MwTally &t = *(MwTally *)u;
  if (!b.used) { t.w.freeBlocks++; t.w.freeBytes += b.size; return true; }
  int i = 0; while (i < MW_B - 1 && b.size > s_mwEdge[i]) i++;
  t.w.n[i]++; t.w.bytes[i] += b.size;
  if (s_mwBlkWant && b.size <= 256 && s_mwBlk && s_mwBlkN < MW_BLK) {
    const uint32_t *w = (const uint32_t *)b.ptr;
    s_mwBlk[s_mwBlkN++] = MwBlk{(uint32_t)(uintptr_t)b.ptr, (uint16_t)w[1], b.size >= 12 ? w[2] : 0, b.size >= 16 ? w[3] : 0};   // w[0] is the poisoning canary, w[1] the size asked for
  }
  if (b.size >= 256) {
    const uint16_t z = b.size > 65535 ? 65535 : b.size;
    for (int k = 0; k < 48; k++) {
      if (t.cnt[k] && t.sz[k] == z) { t.cnt[k]++; break; }
      if (!t.cnt[k]) { t.sz[k] = z; t.cnt[k] = 1; break; }
    }
  }
  return true;
}
static void mwWalk(MwWalk &out) {
  static MwTally t;   // big for a 3 KB stack, and only this task walks
  memset(&t, 0, sizeof t);
  heap_caps_walk(MALLOC_CAP_INTERNAL, mwWalkCb, &t);
  for (int r = 0; r < 8; r++) {   // the eight commonest sizes of 256 B and up
    int best = -1;
    for (int k = 0; k < 48; k++) if (t.cnt[k] && (best < 0 || t.cnt[k] > t.cnt[best])) best = k;
    if (best < 0) break;
    t.w.topSize[r] = t.sz[best]; t.w.topN[r] = t.cnt[best]; t.cnt[best] = 0;
  }
  out = t.w;
}
static String mwWalkJson(const MwWalk &w) {
  String j = "{\"buckets\":[";
  for (int i = 0; i < MW_B; i++) {
    char b[64]; snprintf(b, sizeof b, "%s[%u,%u,%u]", i ? "," : "", (unsigned)s_mwEdge[i], (unsigned)w.n[i], (unsigned)w.bytes[i]); j += b;
  }
  j += "],\"top\":[";
  for (int r = 0; r < 8 && w.topN[r]; r++) { char b[32]; snprintf(b, sizeof b, "%s[%u,%u]", r ? "," : "", w.topSize[r], w.topN[r]); j += b; }
  char b[64]; snprintf(b, sizeof b, "],\"freeBlocks\":%u,\"freeBytes\":%u}", (unsigned)w.freeBlocks, (unsigned)w.freeBytes); j += b;
  return j;
}

static void mwFailed(size_t size, uint32_t caps, const char *fn) {
  const uint32_t i = s_mwFailN++;
  MwFail &f = s_mwFails[i % 24];
  f.ms = millis(); f.size = size; f.caps = caps;
  strlcpy(f.task, pcTaskGetName(NULL), sizeof f.task);
  strlcpy(f.fn, fn ? fn : "?", sizeof f.fn);
}

static void mwCountPbufs(struct tcp_seg *s, MwTcp &t, uint32_t &bytes) {
  for (; s; s = s->next) {
    t.segs++;
    bytes += s->len;
    for (struct pbuf *q = s->p; q; q = q->next) {
      // a PBUF_RAM pbuf is one block: struct then payload. Count where that block lives.
      const uint32_t sz = q->len + sizeof(struct pbuf);
      if (esp_ptr_external_ram(q)) t.bufExt += sz; else t.bufInt += sz;
    }
  }
}

static MwTcp mwTcp() {
  MwTcp t = {};
  LOCK_TCPIP_CORE();
  for (struct tcp_pcb *p = tcp_active_pcbs; p; p = p->next) {
    t.pcbs++;
    mwCountPbufs(p->unsent, t, t.unsent);
    mwCountPbufs(p->unacked, t, t.unacked);
#if TCP_QUEUE_OOSEQ
    uint32_t oo = 0; mwCountPbufs(p->ooseq, t, oo); t.ooseq += oo;
#endif
  }
  for (struct tcp_pcb *p = tcp_tw_pcbs; p; p = p->next) t.tw++;
  UNLOCK_TCPIP_CORE();
  return t;
}

static MwSnap mwSnap() {
  MwSnap s;
  s.ms = millis();
  s.freeInt = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  s.largestInt = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  s.freeDma = heap_caps_get_free_size(MALLOC_CAP_DMA);
  s.t = mwTcp();
  s.media = inFlightMediaResponses;
  s.streams = activeStreams;
  s.qlen = asynctcp_queue_len ? (uint16_t)asynctcp_queue_len() : 0xFFFF;
  return s;
}

static void mwTaskFn(void *) {
  uint32_t lastLine = 0;
  for (;;) {
    const uint32_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t now = millis();
    if (f < s_mwLow) {
      s_mwLow = f; s_mwAtLow = mwSnap();
      static uint32_t lastWalk = 0;
      if (now - lastWalk >= 250) { lastWalk = now; mwWalk(s_mwWalkAtLow); }
    }
    if (s_mwLine && now - lastLine >= 1000) {
      lastLine = now;
      s_mwLine[s_mwLineN % MW_LINE] = mwSnap();
      s_mwLineN++;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void mwStart() {
  if (s_mwTask) return;
  s_mwLine = (MwSnap *)heap_caps_calloc(MW_LINE, sizeof(MwSnap), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  heap_caps_register_failed_alloc_callback(mwFailed);
  xTaskCreatePinnedToCore(mwTaskFn, "memwhy", 4096, nullptr, 2, &s_mwTask, 0);
}

static String mwSnapJson(const MwSnap &s) {
  char b[300];
  snprintf(b, sizeof b, "{\"ms\":%u,\"freeInt\":%u,\"largestInt\":%u,\"freeDma\":%u,\"pcbs\":%u,\"tw\":%u,\"segs\":%u,"
           "\"unsent\":%u,\"unacked\":%u,\"tcpBufInt\":%u,\"tcpBufExt\":%u,\"ooseq\":%u,\"media\":%d,\"streams\":%d,\"qlen\":%u}",
           (unsigned)s.ms, (unsigned)s.freeInt, (unsigned)s.largestInt, (unsigned)s.freeDma, s.t.pcbs, s.t.tw, s.t.segs,
           (unsigned)s.t.unsent, (unsigned)s.t.unacked, (unsigned)s.t.bufInt, (unsigned)s.t.bufExt, (unsigned)s.t.ooseq,
           s.media, s.streams, (unsigned)s.qlen);
  return String(b);
}

static void benchMemWhy(AsyncWebServerRequest *r) {
  mwStart();
  if (r->hasParam("reset")) { memset(&s_mwWalkAtLow, 0, sizeof s_mwWalkAtLow); s_mwLow = UINT32_MAX; s_mwFailN = 0; s_mwLineN = 0; g_fillMemWaits = 0; g_fillMemTrims = 0; g_fillShareWaits = 0; g_fillNudges = 0; g_sendGateWaits = 0; }
  if (r->hasParam("csv")) {
    String c = "ms,freeInt,largestInt,freeDma,pcbs,tw,segs,unsent,unacked,tcpBufInt,tcpBufExt,ooseq,media,streams,qlen\n";
    const uint32_t n = s_mwLineN, from = n > MW_LINE ? n - MW_LINE : 0;
    for (uint32_t i = from; i < n; i++) {
      const MwSnap &s = s_mwLine[i % MW_LINE];
      char b[160];
      snprintf(b, sizeof b, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%u\n", (unsigned)s.ms, (unsigned)s.freeInt,
               (unsigned)s.largestInt, (unsigned)s.freeDma, s.t.pcbs, s.t.tw, s.t.segs, (unsigned)s.t.unsent,
               (unsigned)s.t.unacked, (unsigned)s.t.bufInt, (unsigned)s.t.bufExt, (unsigned)s.t.ooseq, s.media, s.streams, (unsigned)s.qlen);
      c += b;
    }
    r->send(200, "text/csv", c);
    return;
  }
  if (r->hasParam("walk")) { MwWalk w; mwWalk(w); r->send(200, "application/json", mwWalkJson(w)); return; }
  if (r->hasParam("blocks")) {
    if (!s_mwBlk) s_mwBlk = (MwBlk *)heap_caps_calloc(MW_BLK, sizeof(MwBlk), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_mwBlkN = 0; s_mwBlkWant = true; MwWalk w; mwWalk(w); s_mwBlkWant = false;
    String c; c.reserve(s_mwBlkN * 34);
    for (uint32_t i = 0; i < s_mwBlkN; i++) { char b[48]; snprintf(b, sizeof b, "%08x %u %08x %08x\n", (unsigned)s_mwBlk[i].ptr, s_mwBlk[i].size, (unsigned)s_mwBlk[i].w0, (unsigned)s_mwBlk[i].w1); c += b; }
    r->send(200, "text/plain", c);
    return;
  }
  String j = "{\"now\":" + mwSnapJson(mwSnap()) + ",\"atLow\":" + mwSnapJson(s_mwAtLow) + ",\"walkAtLow\":" + mwWalkJson(s_mwWalkAtLow) +
             ",\"fillReserve\":" + String((unsigned)g_fillDmaReserve) + ",\"fillMemWaits\":" + String((unsigned)g_fillMemWaits) +
             ",\"fillMemTrims\":" + String((unsigned)g_fillMemTrims) + ",\"fillShareWaits\":" + String((unsigned)g_fillShareWaits) + ",\"fillNudges\":" + String((unsigned)g_fillNudges) + ",\"sendGateWaits\":" + String((unsigned)g_sendGateWaits) +
             ",\"queuePeak\":" + String(asynctcp_queue_peak_reset ? (unsigned)asynctcp_queue_peak_reset() : 0) +
             ",\"failCount\":" + String((unsigned)s_mwFailN) + ",\"fails\":[";
  const uint32_t n = s_mwFailN, from = n > 24 ? n - 24 : 0;
  for (uint32_t i = from; i < n; i++) {
    const MwFail &f = s_mwFails[i % 24];
    char b[120];
    snprintf(b, sizeof b, "%s{\"ms\":%u,\"size\":%u,\"caps\":\"0x%x\",\"task\":\"%s\",\"fn\":\"%s\"}", i > from ? "," : "",
             (unsigned)f.ms, (unsigned)f.size, (unsigned)f.caps, f.task, f.fn);
    j += b;
  }
  j += "]}";
  r->send(200, "application/json", j);
}

static void nomadBenchRegister(AsyncWebServer &server) {
  server.on("/bench/memwhy", HTTP_GET, benchMemWhy);
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
  // Test trees, made on a task of their own. /bench/mk?dir=/Files/zdeep&depth=10&files=20
  // makes a chain of folders that deep with that many empty files in each;
  // /bench/mk?dir=/Files/zbig&n=3000 makes one folder of n empty files. Serial says when done.
  server.on("/bench/mk", HTTP_ANY, [](AsyncWebServerRequest *r) {
    struct Job { String dir; int depth, files, n; };
    auto num = [&](const char *k) { return r->hasParam(k) ? (int)r->getParam(k)->value().toInt() : 0; };
    Job *j = new Job{ r->hasParam("dir") ? r->getParam("dir")->value() : String(), num("depth"), num("files"), num("n") };
    if (!j->dir.startsWith("/Files/z")) { delete j; r->send(400, "text/plain", "dir must be under /Files/z"); return; }
    xTaskCreatePinnedToCore([](void *pv) {
      Job *j = (Job *)pv;
      const uint32_t t0 = millis();
      int made = 0;
      auto touch = [&](const String &folder, int count) {
        for (int i = 0; i < count; i++) {
          char nm[40];
          snprintf(nm, sizeof(nm), "/bench file %05d.txt", i);
          if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
          File f = SD_MMC.open(folder + nm, FILE_WRITE);
          if (f) { f.close(); made++; }
          if (sdMutex) xSemaphoreGive(sdMutex);
          if ((i & 15) == 15) vTaskDelay(1);
        }
      };
      String path = j->dir;
      if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
      SD_MMC.mkdir(path);
      if (sdMutex) xSemaphoreGive(sdMutex);
      touch(path, j->n);
      for (int d = 1; d <= j->depth; d++) {
        path += "/level " + String(d) + " of a tree copied from a phone";
        if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
        const bool ok = SD_MMC.mkdir(path);
        if (sdMutex) xSemaphoreGive(sdMutex);
        if (!ok) { Serial.printf("[Bench] mk: mkdir failed at level %d\n", d); break; }
        touch(path, j->files);
      }
      Serial.printf("[Bench] mk done: %d files in %lu ms under %s\n", made, (unsigned long)(millis() - t0), j->dir.c_str());
      delete j;
      vTaskDelete(NULL);
    }, "BenchMk", 8 * 1024, j, 1, NULL, 1);
    r->send(202, "text/plain", "started");
  });
  // Fault injection. sdfail=N: every Nth one-sector card read fails after its transfer.
  // sddirect=1: those reads go straight to the caller with no second try.
  // cardgone=S: the card stops answering for S seconds, as if pulled and pushed back.
  // fillguard=0: film and archive replies ask for a whole socket on a first piece.
  // cfggap=ms / updgap=ms: pause inside the config writer and the interface update swap, to
  // pull power in. hold=ms: a task keeps the card lock that long. sderr=1: ask for card recovery.
  // 0 turns a setting off.
  server.on("/bench/fault", HTTP_ANY, [](AsyncWebServerRequest *r) {
    auto num = [&](const char *k, volatile uint32_t &dst) {
      if (r->hasParam(k)) dst = (uint32_t)r->getParam(k)->value().toInt();
    };
    num("sdfail", g_benchSdFailEvery);
    num("sddirect", g_benchSdDirect);
    num("full", g_benchVolFull);
    num("idxdirect", g_benchIdxDirect);
    num("badread", g_benchBadRead);
    num("hang", g_benchHang);
    num("recount", g_benchRecountNow);
    num("labelalways", g_benchLabelAlways);
    if (r->hasParam("cardgone")) g_benchCardGoneUntil = millis() + 1000UL * (uint32_t)r->getParam("cardgone")->value().toInt();
    if (r->hasParam("fillguard")) g_benchNoFillGuard = r->getParam("fillguard")->value().toInt() ? 0 : 1;
    num("cfggap", g_benchCfgGapMs);
    num("fillreserve", g_fillDmaReserve);
    num("fillbudget", g_fillBudget);        // 0 = no per-reply shares
    num("updgap", g_benchUpdGapMs);
    if (r->hasParam("sderr")) { sdErrorFlag = true; sdErrorCooldownUntil = millis(); }
    if (r->hasParam("hold")) {
      const uint32_t ms = (uint32_t)r->getParam("hold")->value().toInt();
      xTaskCreate([](void *arg) {
        const uint32_t holdMs = (uint32_t)(uintptr_t)arg;
        if (sdMutex && xSemaphoreTake(sdMutex, portMAX_DELAY) == pdTRUE) {
          Serial.printf("[Bench] card lock held for %u ms\n", (unsigned)holdMs);
          vTaskDelay(pdMS_TO_TICKS(holdMs));
          xSemaphoreGive(sdMutex);
        }
        vTaskDelete(NULL);
      }, "benchHold", 3072, (void *)(uintptr_t)ms, 1, NULL);
    }
    String j = "{\"sdfail\":" + String((unsigned)g_benchSdFailEvery);
    { NomadSDStats st; nomadSDStats(st); j += ",\"cardReads\":" + String((unsigned)st.reads) + ",\"cardWrites\":" + String((unsigned)st.writes); }
    j += ",\"dmaLow\":" + String((unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)) + ",\"dmaNow\":" + String((unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    j += ",\"lcdFlushes\":" + String((unsigned)g_lcdFlushes);
    j += ",\"recountMs\":" + String((unsigned)g_benchRecountMs) + ",\"wifiMode\":" + String((int)WiFi.getMode());
    j += ",\"badread\":" + String((unsigned)g_benchBadRead) + ",\"givenUp\":" + String((unsigned)g_repliesGivenUp);
    j += ",\"sddirect\":" + String((unsigned)g_benchSdDirect);
    j += ",\"full\":" + String((unsigned)g_benchVolFull) + ",\"sdFull\":" + String(nomadSDFull() ? 1 : 0) + ",\"cardFull\":" + String(g_cardFull ? 1 : 0) + ",\"fullRefused\":" + String((unsigned)nomadSDFullRefused());
    j += ",\"fillguard\":" + String(g_benchNoFillGuard ? 0 : 1);
    j += ",\"cfggap\":" + String((unsigned)g_benchCfgGapMs);
    j += ",\"updgap\":" + String((unsigned)g_benchUpdGapMs);
    j += ",\"sdErrorFlag\":" + String(sdErrorFlag ? "true" : "false") + "}";
    r->send(200, "application/json", j);
  });
  // Every task's lowest stack headroom since it started, in bytes.
  server.on("/bench/tasks", HTTP_GET, [](AsyncWebServerRequest *r) {
    const UBaseType_t cap = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *st = (TaskStatus_t *)heap_caps_malloc(cap * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!st) { r->send(503, "application/json", "{\"error\":\"no memory\"}"); return; }
    const UBaseType_t n = uxTaskGetSystemState(st, cap, NULL);
    String j = "{\"uptimeMs\":" + String((unsigned long)millis()) + ",\"heap\":" + benchHeapJson() + ",\"tasks\":[";
    for (UBaseType_t i = 0; i < n; i++) {
      if (i) j += ",";
      j += "{\"name\":\"" + String(st[i].pcTaskName) + "\",\"prio\":" + String((unsigned)st[i].uxCurrentPriority);
      j += ",\"stackFree\":" + String((unsigned)st[i].usStackHighWaterMark) + "}";
    }
    j += "]}";
    free(st);
    r->send(200, "application/json", j);
  });
  Serial.println("[Bench] /bench/* endpoints registered (NOMAD_BENCH build)");
}

#endif  // NOMAD_BENCH_H
