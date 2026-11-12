// NomadReadAhead.h - serve media fills out of whole, aligned card reads.
//
// The fill buffer ESPAsyncWebServer hands the range handler is a plain malloc, and on
// this core anything over 4 KB comes from PSRAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL).
// The S3's SD controller can't DMA into PSRAM, and SdFat lands the buffer at whatever
// alignment the file offset gives. ESP-IDF's sdmmc_read_sectors() then takes its bounce
// path: a DMA buffer allocated on every call and one 512-byte sector per command, on the
// async_tcp task, with sdMutex held. That per-read DMA allocation is what the admission
// floor protects, and it's probably most of the gap between card speed and WiFi speed.
//
// So read whole sectors into one internal, DMA-capable buffer and keep them per response
// in PSRAM. Later fills are a memcpy, and the card only sees aligned multi-sector reads.
//
// Off unless turned on (g_nomadAlignedReads), so it can be compared against the normal
// path on the same build. One response owns one cache. Nothing here is shared between
// responses except the DMA buffer, which is only touched with sdMutex held.

#ifndef NOMAD_READ_AHEAD_H
#define NOMAD_READ_AHEAD_H

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>   // esp_ptr_external_ram
#include <esp_timer.h>
#include <memory>
#include "NomadSD.h"

// 16 KB reads are fast into internal DMA memory on the S3. 32 KB isn't available as one
// internal DMA block on a running Mk4 (largest free block ~31-36 KB).
static const size_t NOMAD_RA_MAX = 16384;          // the DMA buffer, allocated once
static volatile bool g_nomadAlignedReads = false;
static volatile size_t g_nomadRaBytes = 16384;     // per refill, <= NOMAD_RA_MAX, multiple of 512
static uint8_t *g_nomadRaDma = nullptr;

// Allocate the shared DMA buffer. Called when the mode is turned on, so a build that never
// uses it never pays for it. Internal RAM, 64-byte aligned (a superset of what sdmmc wants).
static bool nomadReadAheadEnsureBuffer() {
  if (g_nomadRaDma) return true;
  g_nomadRaDma = (uint8_t *)heap_caps_aligned_alloc(64, NOMAD_RA_MAX,
                                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  return g_nomadRaDma != nullptr;
}

// What the media fill path is actually asked to do: how big the fills are, whether the
// buffer it gets is in PSRAM, whether the file position is sector-aligned, and how long
// the card read takes. Only the async_tcp task writes it, /bench/stats reads it.
static const int NOMAD_FILL_HIST = 6;   // maxLen <=1K, <=2K, <=4K, <=8K, <=16K, more
struct NomadFillStats {
  uint32_t fills, refills, retries, psramBuffer, unalignedPos;
  uint64_t bytes, readUs;
  uint32_t hist[NOMAD_FILL_HIST];
};
static NomadFillStats g_fillStats = {};

static inline void nomadFillStatsReset() { memset(&g_fillStats, 0, sizeof g_fillStats); }

static inline void nomadFillNote(size_t maxLen, const uint8_t *buffer, uint64_t pos,
                                 size_t got, uint32_t us, bool retry) {
  NomadFillStats &s = g_fillStats;
  if (retry) { s.retries++; return; }
  s.fills++;
  s.bytes += got;
  s.readUs += us;
  if (esp_ptr_external_ram(buffer)) s.psramBuffer++;
  if (pos & 511) s.unalignedPos++;
  const int b = maxLen <= 1024 ? 0 : maxLen <= 2048 ? 1 : maxLen <= 4096 ? 2
              : maxLen <= 8192 ? 3 : maxLen <= 16384 ? 4 : 5;
  s.hist[b]++;
}

// The prebuilt core gives every TCP connection a 5,744-byte send budget. Raising
// pcb->snd_buf on a fresh connection, before anything is queued, gives that one connection
// a larger window without rebuilding the core. TCP_SND_QUEUELEN (16 segments, compiled in)
// still caps it near 23 KB, and above ~11 KB fills grow past 4 KB and land in PSRAM, which
// the card reads badly. 0 = leave it alone.
#include "lwip/tcpip.h"
#include "lwip/tcp.h"
#include <ESPAsyncWebServer.h>
static inline void nomadRaiseSndBuf(AsyncWebServerRequest *request, uint32_t bytes) {
  if (!bytes || !request || !request->client()) return;
  tcp_pcb *pcb = request->client()->pcb();
  if (!pcb) return;
  LOCK_TCPIP_CORE();
  if (pcb->snd_queuelen == 0 && pcb->snd_buf < bytes) pcb->snd_buf = (tcpwnd_size_t)bytes;
  UNLOCK_TCPIP_CORE();
}

struct NomadReadAheadCache {
  uint8_t *buf = nullptr;     // PSRAM, NOMAD_RA_MAX
  uint64_t start = 0;         // file offset of buf[0], always a multiple of 512
  size_t len = 0;             // valid bytes in buf
  ~NomadReadAheadCache() { if (buf) heap_caps_free(buf); }
};

// Fill `out` with up to `want` bytes from `pos`. Returns bytes copied. 0 with sdBusy set
// means the card lock was not available and the caller should retry, 0 without it means
// end of file or a failed read. The caller holds no SD lock: this takes sdMutex itself,
// and only for the refill.
static size_t nomadReadAheadFill(NomadReadAheadCache &c, NomadFile64 &file, uint64_t pos,
                                 uint8_t *out, size_t want, SemaphoreHandle_t mutex,
                                 bool &sdBusy) {
  sdBusy = false;
  if (!(c.len && pos >= c.start && pos < c.start + c.len)) {
    if (!c.buf) {
      c.buf = (uint8_t *)heap_caps_malloc(NOMAD_RA_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!c.buf) return 0;
    }
    size_t chunk = g_nomadRaBytes;
    if (chunk > NOMAD_RA_MAX) chunk = NOMAD_RA_MAX;
    chunk &= ~(size_t)511;
    if (chunk < 512) chunk = 512;
    const uint64_t aligned = pos & ~(uint64_t)511;
    if (mutex && xSemaphoreTake(mutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
      sdBusy = true;
      return 0;
    }
    const size_t got = file.readAt(aligned, g_nomadRaDma, chunk);
    if (mutex) xSemaphoreGive(mutex);
    g_fillStats.refills++;
    if (got == 0) { c.len = 0; return 0; }
    memcpy(c.buf, g_nomadRaDma, got);
    c.start = aligned;
    c.len = got;
    if (pos >= c.start + c.len) return 0;   // short read that ended before pos: EOF
  }
  const size_t off = (size_t)(pos - c.start);
  size_t n = c.len - off;
  if (n > want) n = want;
  memcpy(out, c.buf + off, n);
  return n;
}

#endif  // NOMAD_READ_AHEAD_H
