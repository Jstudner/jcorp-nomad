// NomadZimServe.h - the archive reader's pages, served by Nomad itself:
//
//   GET /zimview/<n>/<namespace>/<path>     one entry of archive <n>, as the page asked for it
//   GET /api/zimserve                        {"v":1,...}: archive.html's "can Nomad do it"
//
// <n> is the archive's idx in /Archive/.nomad-zim/manifest.json, the same address the
// service worker (zim-sw.js) answers, so archive.html opens the same URL either way. Where
// the phone has no service worker (plain http: most phones), this is what makes website
// copies, TED, PhET, Gutenberg and the video channels work: their pages fetch their own
// scripts, styles, fonts and videos, and only a server can answer those.
//
// The work happens off the network task. The "zim" task looks the entry up and handles
// redirects, misses and uncompressed entries (images, video, fonts, which libzim stores
// as-is and go out off the card with Range). An entry in a zstd cluster goes on to the
// "unzip" task, so a page's pictures never wait behind a stylesheet being unpacked. The
// answer itself is always sent from the network task (see "answers").
#ifndef NOMAD_ZIM_SERVE_H
#define NOMAD_ZIM_SERVE_H

#include <memory>
#include <new>
#include <atomic>
#include <map>
#include "NomadZimCore.h"
#define XZ_INTERNAL_CRC32 0      // as src/xz/xz_nomad.c builds it (CRC32 from the ROM)
#include "src/xz/xz.h"

namespace zimsrv {

static TaskHandle_t g_task = nullptr, g_utask = nullptr;   // lookups, unpacking
static QueueHandle_t g_q = nullptr, g_uq = nullptr;
static nzim::Clusters *g_clusters = nullptr;
static int g_slots = 0;
static const char *MANIFEST = "/Archive/.nomad-zim/manifest.json";
static const int MAX_OPEN = 6;

struct Arc {
  int idx = -1;
  uint32_t gen = 0;
  std::vector<NomadFile64 *> files;
  nzim::Archive a;
  std::atomic<int> refs{0};     // responses still sending from it
  uint32_t used = 0;
};
static std::vector<Arc *> g_open;
// the manifest as raw text in PSRAM, an archive's parts are looked up in it when it's
// opened. Parsing it into a map of strings would take ~20 KB of internal RAM (allocations
// up to 4 KB land there), which the SD card and WiFi need
static char *g_manBuf = nullptr;
static size_t g_manLen = 0;
static uint32_t g_gen = 0, g_tick = 0, g_lastCheck = 0;
static uint64_t g_manifestSize = (uint64_t)-1;

// zim task: 0 JobP*. unzip task: 0 JobP* to unpack, 1 Clusters::Slot* to unpin, 2 Arc* to close
struct Msg { uint8_t kind; void *p; };

// bytes of small files copied out of big clusters (see unpack)
static std::atomic<int32_t> s_copyBytes{0};
static const int32_t COPY_BUDGET = 1536 << 10, COPY_MAX = 1 << 20;

// counters for /api/zimserve
static std::atomic<uint32_t> s_jobs{0}, s_packed{0}, s_raw{0}, s_redirects{0}, s_missing{0}, s_busy{0};
static std::atomic<uint64_t> s_lookupUs{0}, s_packedUs{0};
static uint64_t nowUs() { return (uint64_t)esp_timer_get_time(); }

// xz clusters (archives from before 2021), whole, with XZ Embedded in single-call mode
static size_t xzDecode(const uint8_t *in, size_t inLen, uint8_t *out, size_t outCap, int *err) {
  struct xz_dec *d = xz_dec_init(XZ_SINGLE, 0);
  if (!d) { *err = 2; return 0; }
  struct xz_buf b = {in, 0, inLen, out, 0, outCap};
  enum xz_ret r = xz_dec_run(d, &b);
  xz_dec_end(d);
  // XZ_BUF_ERROR in single-call mode: the output didn't fit (or the input was cut short)
  *err = r == XZ_STREAM_END ? 0 : (r == XZ_BUF_ERROR && b.out_pos == outCap) ? 1 : 2;
  return b.out_pos;
}

// card and memory

// Card reads go through one small buffer the SD controller can DMA into. Reading straight
// into PSRAM (or a response's buffer) makes the driver borrow a DMA-capable block for every
// 512 bytes, and under load that borrow fails ("sdmmc_read_sectors: not enough mem"), so
// you get a short read and a body that stops part way. sdMutex, which every read here
// takes, keeps it to one reader at a time.
static const size_t DMA_BUF = 4096;
static uint8_t *g_dma = nullptr;   // only allocated during a read, holding internal RAM at rest
                                   // pushes the media handler under its floor

// one read under the card lock. The zim tasks can wait for the card, the network task
// (a body going out) can't, and asks again instead: -1 busy (or no memory just now),
// 0 error, 1 done
static int lockedRead(NomadFile64 *f, uint64_t off, uint8_t *buf, size_t n, uint32_t waitMs) {
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(waitMs)) != pdTRUE) return -1;
  g_dma = (uint8_t *)heap_caps_aligned_alloc(64, n > DMA_BUF ? DMA_BUF : ((n + 1023) & ~(size_t)511), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  bool ok = true;
  if (g_dma) {
    // whole 512-byte sectors, so SdFat reads straight into the DMA buffer instead of a
    // part sector through its cache (which needs the driver's borrowed block again)
    uint64_t pos = off & ~(uint64_t)511, end = off + n;
    uint64_t fsize = f->size();
    while (pos < end && ok) {
      size_t k = (size_t)((end - pos + 511) & ~(uint64_t)511);
      if (k > DMA_BUF) k = DMA_BUF;
      if (pos + k > fsize) k = (size_t)(fsize - pos);       // the file's last part sector
      ok = k > 0 && f->readAt(pos, g_dma, k) == k;
      if (ok) {
        uint64_t from = pos > off ? pos : off;
        uint64_t to = pos + k < end ? pos + k : end;
        memcpy(buf + (from - off), g_dma + (from - pos), (size_t)(to - from));
      }
      pos += k;
    }
  } else {
    ok = f->readAt(off, buf, n) == n;          // no DMA memory to spare: the driver's way
  }
  bool had = g_dma != nullptr;
  if (g_dma) { heap_caps_free(g_dma); g_dma = nullptr; }
  if (sdMutex) xSemaphoreGive(sdMutex);
  return ok ? 1 : had ? 0 : -1;
}
static bool hookRead(void *f, uint64_t off, uint8_t *buf, size_t n) {
  // a failed read is usually the card or memory being busy for a moment: try again
  for (int i = 0; i < 6; i++) {
    if (lockedRead((NomadFile64 *)f, off, buf, n, 3000) == 1) return true;
    vTaskDelay(pdMS_TO_TICKS(20 << (i < 4 ? i : 4)));
  }
  return false;
}
static void *hookAlloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void hookFree(void *p) { heap_caps_free(p); }
static size_t hookLargest() { return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

// the same read across split parts (.zimaa, .zimab), for bodies going out
static int partsRead(Arc *arc, uint64_t off, uint8_t *buf, size_t n, uint32_t waitMs) {
  if (streamDmaFree() < 4096) return -1;          // let WiFi drain first: ask again
  uint64_t base = 0;
  for (auto &p : arc->a.parts) {
    if (!n) break;
    if (off < base + p.size) {
      uint64_t local = off - base;
      size_t take = (size_t)((p.size - local) < n ? (p.size - local) : n);
      int r = lockedRead((NomadFile64 *)p.file, local, buf, take, waitMs);
      if (r != 1) return r;
      buf += take; off += take; n -= take;
    }
    base += p.size;
  }
  return n == 0 ? 1 : 0;
}

// the archive list

static std::string jsonStr(const char *s, size_t &i, size_t n) {   // at the opening quote
  std::string o;
  for (i++; i < n && s[i] != '"'; i++) {
    if (s[i] == '\\' && i + 1 < n) {
      char c = s[++i];
      if (c == 'u' && i + 4 < n) {
        unsigned v = strtoul(std::string(s + i + 1, 4).c_str(), nullptr, 16);
        i += 4;
        if (v < 0x80) o += (char)v;
        else if (v < 0x800) { o += (char)(0xC0 | (v >> 6)); o += (char)(0x80 | (v & 63)); }
        else { o += (char)(0xE0 | (v >> 12)); o += (char)(0x80 | ((v >> 6) & 63)); o += (char)(0x80 | (v & 63)); }
      } else o += c == 'n' ? '\n' : c == 't' ? '\t' : c;
    } else o += s[i];
  }
  return o;
}

// idx -> part paths, from the manifest the admin panel's indexer writes. Read again
// when its size changes (the card was re-indexed). Open archives from before are let go
static void checkManifest() {
  if (g_manifestSize != (uint64_t)-1 && millis() - g_lastCheck < 30000) return;
  g_lastCheck = millis();
  NomadFile64 f;
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) != pdTRUE) return;
  bool ok = f.open(MANIFEST);
  uint64_t size = ok ? f.size() : 0;
  if (sdMutex) xSemaphoreGive(sdMutex);
  if (!ok || size == g_manifestSize || size > 4 * 1024 * 1024) {
    if (ok && sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) == pdTRUE) { f.close(); xSemaphoreGive(sdMutex); }
    return;
  }
  char *buf = (char *)hookAlloc((size_t)size + 1);
  int r = buf ? lockedRead(&f, 0, (uint8_t *)buf, (size_t)size, 3000) : 0;
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) == pdTRUE) { f.close(); xSemaphoreGive(sdMutex); }
  if (r != 1) { if (buf) hookFree(buf); return; }
  buf[size] = 0;
  if (g_manBuf) hookFree(g_manBuf);
  g_manBuf = buf; g_manLen = (size_t)size;
  g_manifestSize = size;
  g_gen++;
}

// archive idx's part paths, from the manifest text
static std::vector<std::string> partsOf(int idx) {
  std::vector<std::string> out;
  if (!g_manBuf) return out;
  const char *s = g_manBuf; size_t n = g_manLen;
  const char *arr = strstr(s, "\"archives\"");
  for (const char *at = arr ? strstr(arr, "\"idx\"") : nullptr; at; ) {
    const char *next = strstr(at + 5, "\"idx\"");
    if (atoi(strchr(at, ':') + 1) == idx) {
      const char *parts = strstr(at, "\"parts\"");
      if (parts && (!next || parts < next)) {
        const char *end = strchr(parts, ']');
        for (const char *p = strstr(parts, "\"path\""); p && end && p < end; p = strstr(p + 6, "\"path\"")) {
          const char *q = strchr(p + 6, '"');
          if (!q || q > end) break;
          size_t i = q - s;
          out.push_back(jsonStr(s, i, n));
        }
      }
      break;
    }
    at = next;
  }
  return out;
}

// on the unzip task, which owns the unpacked clusters that may point into it
static void closeArcNow(Arc *arc) {
  if (g_clusters) g_clusters->forget(&arc->a);
  // closing has to happen, so wait as long as it takes: giving back a lock this task
  // doesn't hold would free it under whoever does
  if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
  for (auto *f : arc->files) { f->close(); delete f; }
  if (sdMutex) xSemaphoreGive(sdMutex);
  delete arc;
}
static void closeArc(Arc *arc) {
  Msg m = {2, arc};
  if (xQueueSend(g_uq, &m, pdMS_TO_TICKS(1000)) != pdTRUE) Serial.println("[zim] close lost");
}

static Arc *getArc(int idx) {
  checkManifest();
  for (size_t i = 0; i < g_open.size(); i++) {
    Arc *a = g_open[i];
    if (a->idx == idx && a->gen == g_gen) { a->used = ++g_tick; return a; }
  }
  // let go of archives from an older manifest, then the least used past MAX_OPEN
  for (size_t i = 0; i < g_open.size();) {
    if (g_open[i]->gen != g_gen && g_open[i]->refs == 0) { closeArc(g_open[i]); g_open.erase(g_open.begin() + i); }
    else i++;
  }
  while ((int)g_open.size() >= MAX_OPEN) {
    int lru = -1;
    for (size_t i = 0; i < g_open.size(); i++)
      if (g_open[i]->refs == 0 && (lru < 0 || g_open[i]->used < g_open[lru]->used)) lru = (int)i;
    if (lru < 0) break;
    closeArc(g_open[lru]); g_open.erase(g_open.begin() + lru);
  }
  std::vector<std::string> paths = partsOf(idx);
  if (paths.empty()) return nullptr;
  Arc *arc = new Arc();
  arc->idx = idx; arc->gen = g_gen; arc->used = ++g_tick;
  bool ok = true;
  const bool locked = !sdMutex || xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) == pdTRUE;
  if (!locked) ok = false;   // the card is busy: this request is answered "busy" and asked again
  for (auto &p : paths) {
    if (!ok) break;
    NomadFile64 *f = new NomadFile64();
    if (!f->open(p.c_str())) { delete f; ok = false; break; }
    arc->files.push_back(f);
    arc->a.parts.push_back({f, f->size()});
  }
  if (locked && sdMutex) xSemaphoreGive(sdMutex);
  if (!ok || arc->a.open() != nzim::OK) { closeArc(arc); return nullptr; }   // nothing unpacked from it yet
  g_open.push_back(arc);
  return arc;
}

// Answers.
//
// A request is only ever touched on the network task. ESPAsyncWebServer deletes a request
// when its phone disconnects, whatever another task is holding (pause()'s pointer doesn't
// keep it alive), so answering from the zim task ends up writing into freed requests and
// crashes under load. The zim and unzip tasks only work out a Job. The handler waits a
// moment for it, and if it isn't ready the answer goes out later from the network task's
// own polling (Deferred).

static const char FRAME_TAG[] = "<script src=\"/assets/zim-frame.js\"></script>";

static String esc(const std::string &s) {
  String o;
  for (char c : s) {
    if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;"; else if (c == '"') o += "&quot;"; else o += c;
  }
  return o;
}
static String notePage(const char *title, const String &text) {
  return String("<!DOCTYPE html><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                "<body style=\"font-family:system-ui,sans-serif;padding:24px;line-height:1.5;color:#333\"><h2 style=\"font-size:1.2rem\">") +
         title + "</h2><p>" + text + "</p></body>";
}

static String typeOf(const std::string &mime) {
  String t = mime.empty() ? String("application/octet-stream") : String(mime.c_str());
  if ((t.startsWith("text/") || t.indexOf("javascript") >= 0 || t.indexOf("json") >= 0 || t.indexOf("xml") >= 0 || t.indexOf("svg") >= 0) &&
      t.indexOf("charset=") < 0) t += "; charset=utf-8";
  return t;
}

// an archive page runs its own scripts at Nomad's address (as in Kiwix's reader). They
// may only talk to the archive itself, not Nomad's admin, upload or delete calls
static void pageHeaders(AsyncWebServerResponse *r, const String &host, bool isPage) {
  if (isPage) {
    String o = "http://" + host;
    r->addHeader("Content-Security-Policy", "connect-src " + o + "/zimview/ blob: data:; form-action " + o + "/zimview/; base-uri " + o + "/zimview/");
    r->addHeader("Cache-Control", "no-store");
  } else {
    // an archive's files never change under the same address while it is on the card
    r->addHeader("Cache-Control", "private, max-age=600");
  }
}

struct Job {
  // what was asked
  int idx = -1;
  std::string path, q, rawQuery;
  String range, host;
  // what the tasks worked out
  enum Kind : uint8_t { NOTE, REDIRECT, RAW, PACKED } kind = NOTE;
  int code = 503;
  String title, text, location;
  Arc *arc = nullptr;                 // RAW / PACKED: kept open while this lives
  nzim::Answer ans;
  const uint8_t *data = nullptr;      // PACKED: the unpacked bytes, in a pinned slot...
  size_t len = 0;
  nzim::Clusters::Slot *slot = nullptr;
  uint8_t *copy = nullptr;            // ...or a copy of its own (then nothing is pinned)
  uint32_t busySince = 0;            // first time every slot it could use was busy
  bool waited = false;
  // where it is, for the log when an answer is still waiting after a minute: 0 in the zim
  // task's line, 1 looking up, 2 in the unzip line, 3 unpacking, 4 done
  std::atomic<uint8_t> stage{0};
  uint32_t born = millis();
  std::atomic<bool> ready{false};
  // whoever lets go last (the answer finished, or a task after the phone left) releases
  // the slot and the archive
  ~Job() {
    if (slot) {
      if (xTaskGetCurrentTaskHandle() == g_utask) g_clusters->unpin(slot);
      else { Msg m = {1, slot}; if (xQueueSend(g_uq, &m, pdMS_TO_TICKS(100)) != pdTRUE) Serial.println("[zim] unpin lost"); }
    }
    if (copy) { heap_caps_free(copy); s_copyBytes -= len; }
    if (arc) arc->refs--;
  }
  void note(int c, const char *t, const String &x) { kind = NOTE; code = c; title = t; text = x; }
  void done() { stage = 4; ready.store(true, std::memory_order_release); }
};
using JobP = std::shared_ptr<Job>;

// the internal DMA memory the SD card and WiFi both need. Answers only start with some
// of it free (the media handler uses a floor for the same reason). Otherwise the answer
// waits on the network task's polling instead of blocking it
static const size_t ZIM_DMA_FLOOR = 8 * 1024;
static std::atomic<uint32_t> s_dmaWaits{0}, s_dmaMin{UINT32_MAX};
static bool dmaOk() {
  size_t f = streamDmaFree();
  if (f < s_dmaMin) s_dmaMin = f;
  return f >= ZIM_DMA_FLOOR;
}

// the answer for a finished job, on the network task only
static AsyncWebServerResponse *build(AsyncWebServerRequest *req, const JobP &j) {
  AsyncWebServerResponse *r = nullptr;
  switch (j->kind) {
    case Job::NOTE:
      r = req->beginResponse(j->code, "text/html; charset=utf-8", notePage(j->title.c_str(), j->text));
      r->addHeader("Cache-Control", "no-store");
      return r;
    case Job::REDIRECT:
      r = req->beginResponse(302, "text/plain", "");
      r->addHeader("Location", j->location);
      r->addHeader("Cache-Control", "no-store");
      return r;
    case Job::RAW: {
      uint64_t size = j->ans.rawSize, start = 0, end = size ? size - 1 : 0;
      bool partial = false;
      if (j->range.startsWith("bytes=") && size) {
        String rg = j->range.substring(6);
        int dash = rg.indexOf('-');
        String a = rg.substring(0, dash), b = rg.substring(dash + 1);
        int comma = b.indexOf(','); if (comma >= 0) b = b.substring(0, comma);
        if (dash >= 0 && (a.length() || b.length())) {
          if (!a.length()) { uint64_t tail = strtoull(b.c_str(), nullptr, 10); start = tail >= size ? 0 : size - tail; }
          else { start = strtoull(a.c_str(), nullptr, 10); if (b.length()) end = strtoull(b.c_str(), nullptr, 10); }
          if (end >= size) end = size - 1;
          if (start >= size || start > end) {
            r = req->beginResponse(416, "text/plain", "Range not satisfiable");
            r->addHeader("Content-Range", "bytes */" + String((unsigned long long)size));
            return r;
          }
          partial = true;
        }
      }
      size_t len = size ? (size_t)(end - start + 1) : 0;
      uint64_t from = j->ans.rawOff + start;
      JobP keep = j;
      auto stalledSince = std::make_shared<uint32_t>(0);
      r = req->beginResponse(typeOf(j->ans.mime), len, [keep, from, len, req, stalledSince](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
        if (index >= len) return 0;
        // room for the headers on the first piece: after a "try again" there the web library
        // writes headers plus a full socket of body, and what doesn't fit is lost from the reply
#if NOMAD_BENCH
        if (!g_benchNoFillGuard)
#endif
        if (index == 0 && maxLen > 4096) maxLen -= 2048;
        size_t n = len - index < maxLen ? len - index : maxLen;
        // A read that gives nothing is asked again (the card busy), until that has gone on
        // too long to be a busy card: then the connection is closed (nomadFillStalled).
#if NOMAD_BENCH
        const bool got = !g_benchBadRead && partsRead(keep->arc, from + index, buf, n, 50) == 1;
#else
        const bool got = partsRead(keep->arc, from + index, buf, n, 50) == 1;
#endif
        if (got) { *stalledSince = 0; return n; }
        return nomadFillStalled(*stalledSince, req, "an archive page") ? 0 : RESPONSE_TRY_AGAIN;
      });
      if (partial) {
        r->setCode(206);
        r->addHeader("Content-Range", "bytes " + String((unsigned long long)start) + "-" + String((unsigned long long)end) + "/" + String((unsigned long long)size));
      }
      r->addHeader("Accept-Ranges", "bytes");
      pageHeaders(r, j->host, false);
      return r;
    }
    case Job::PACKED: {
      String type = typeOf(j->ans.mime);
      bool isPage = type.startsWith("text/html") || type.indexOf("xhtml") >= 0;
      size_t at = isPage ? nzim::frameScriptAt(j->data, j->len) : (size_t)-1;
      size_t tag = at == (size_t)-1 ? 0 : sizeof(FRAME_TAG) - 1;
      JobP keep = j;
      r = req->beginResponse(type, j->len + tag, [keep, at, tag](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
        const uint8_t *data = keep->data;
        size_t len = keep->len, n = 0;
        while (n < maxLen && index < len + tag) {
          size_t take;
          if (tag && index >= at && index < at + tag) {          // the frame script
            take = at + tag - index; if (take > maxLen - n) take = maxLen - n;
            memcpy(buf + n, FRAME_TAG + (index - at), take);
          } else {
            size_t src = (tag && index >= at + tag) ? index - tag : index;
            size_t stop = (tag && index < at) ? at : len;          // up to the tag, or the end
            take = stop - src; if (take > maxLen - n) take = maxLen - n;
            memcpy(buf + n, data + src, take);
          }
          n += take; index += take;
        }
        return n;
      });
      pageHeaders(r, j->host, isPage);
      return r;
    }
  }
  return req->beginResponse(500, "text/plain", "");
}

// an answer not worked out yet: the request polls it (on the network task) until the job
// is done, and only then are its status and headers decided. ~60 s at most
class Deferred : public AsyncWebServerResponse {
  JobP _j;
  AsyncWebServerResponse *_inner = nullptr;
  AsyncWebServerRequest *_req = nullptr;
  uint32_t _since;
  void begin() {
    if (_inner || !_req) return;
    bool late = millis() - _since > 60000;
    if (!late && (!_j->ready.load(std::memory_order_acquire) || !dmaOk())) return;
    if (late) Serial.printf("[zim] late: ready %d stage %u kind %d dma %u path %.60s\n", (int)_j->ready.load(), (unsigned)_j->stage.load(),
                            (int)_j->kind, (unsigned)streamDmaFree(), _j->path.c_str());
    if (late && !_j->ready.load(std::memory_order_acquire)) {
      _j = std::make_shared<Job>();
      _j->note(503, "Couldn't open this page", "Nomad is busy. Go back and try again. (d)");
      _j->done();
    }
    _inner = build(_req, _j);
    for (const auto &h : _headers) _inner->addHeader(h.name(), h.value(), false);
    _inner->_respond(_req);
  }
public:
  explicit Deferred(JobP j) : _j(std::move(j)), _since(millis()) { _code = 200; }
  ~Deferred() override { delete _inner; }
  bool _sourceValid() const override { return true; }
  bool _started() const override { return _inner ? _inner->_started() : true; }
  bool _finished() const override { return _inner && _inner->_finished(); }
  bool _failed() const override { return _inner && _inner->_failed(); }
  void _respond(AsyncWebServerRequest *r) override { _req = r; begin(); }
  size_t _ack(AsyncWebServerRequest *r, size_t len, uint32_t time) override {
    if (!_inner) { begin(); return 0; }
    return _inner->_ack(r, len, time);
  }
};

// the tasks

// the zim task: which entry, and everything but unpacking. true: passed on to unzip
static bool lookUp(const JobP &j) {
  s_jobs++;
  j->stage = 1;
  uint64_t t0 = nowUs();
  Arc *arc = getArc(j->idx);
  if (!arc) { s_missing++; j->note(404, "Not in this archive", "That archive isn't on this card."); return false; }
  nzim::Answer ans = nzim::resolve(arc->a, j->idx, j->path, j->q, j->rawQuery);
  if (ans.kind == nzim::Answer::NOT_FOUND && j->path.find(' ') != std::string::npos) {
    // the web server reads "+" in a path as a space, but "C++" is a page name
    std::string plus = j->path;
    for (auto &c : plus) if (c == ' ') c = '+';
    nzim::Answer again = nzim::resolve(arc->a, j->idx, plus, j->q, j->rawQuery);
    if (again.kind != nzim::Answer::NOT_FOUND) ans = again;
  }
  s_lookupUs += nowUs() - t0;
  switch (ans.kind) {
    case nzim::Answer::NOT_FOUND: s_missing++; j->note(404, "Not in this archive", esc(ans.why)); return false;
    case nzim::Answer::ERROR:
      j->note(503, "Couldn't read this from the archive", "The card may be busy. Go back and try again. (" + esc(ans.why) + ")");
      return false;
    case nzim::Answer::REDIRECT:
      s_redirects++;
      j->kind = Job::REDIRECT; j->location = ans.location.c_str();
      return false;
    case nzim::Answer::RAW:
      s_raw++;
      j->kind = Job::RAW; j->ans = ans; j->arc = arc; arc->refs++;
      return false;
    case nzim::Answer::PACKED: {
      s_packed++;
      j->ans = ans; j->arc = arc; arc->refs++;
      Msg m = {0, new JobP(j)};
      j->stage = 2;
      if (xQueueSend(g_uq, &m, pdMS_TO_TICKS(2000)) == pdTRUE) return true;
      delete (JobP *)m.p;
      s_busy++;
      Serial.printf("[zim] busy q: unzip line full (%u)\n", (unsigned)uxQueueMessagesWaiting(g_uq));
      j->note(503, "Couldn't open this page", "Nomad is busy. Go back and try again. (q)");
      return false;
    }
  }
  return false;
}

static void task(void *) {
  uint32_t last = 0;
  for (;;) {
    Msg m;
    if (xQueueReceive(g_q, &m, pdMS_TO_TICKS(10000)) != pdTRUE) {
      // a quiet minute: let go of the open archives. Their file handles and bits sit in
      // internal RAM, and every KB of that counts against the media handler's 48 KB floor
      if (!g_open.empty() && millis() - last > 60000) {
        for (size_t i = 0; i < g_open.size();) {
          if (g_open[i]->refs == 0) { closeArc(g_open[i]); g_open.erase(g_open.begin() + i); }
          else i++;
        }
        if (g_open.empty()) std::vector<Arc *>().swap(g_open);
      }
      continue;
    }
    last = millis();
    JobP *jp = (JobP *)m.p;
    // nobody waiting for it any more (the phone left): skip the work
    if (jp->use_count() > 1 && !lookUp(*jp)) (*jp)->done();
    delete jp;
  }
}

// the unzip task: unpack. true: back in line (every slot it could use is still going out
// to a phone, their unpins are in the queue ahead of it)
static bool unpack(const JobP &j) {
  const uint8_t *data; size_t len; nzim::Clusters::Slot *slot;
  j->stage = 3;
  uint64_t t0 = nowUs();
  int r = g_clusters->blob(j->arc->a, j->ans.entry.cluster, j->ans.entry.blob, data, len, slot);
  s_packedUs += nowUs() - t0;
  // two ~2 MB clusters from an unprepared archive going out over a slow link can take
  // seconds, so wait its turn, up to 20 s (a slow picture beats a missing one). Timed by
  // the clock since a try count runs way too long with several jobs in the line. Only
  // pause when nothing else is waiting, a pause per turn holds up every job behind it
  if (r == nzim::BUSY && !j->waited) { j->waited = true; j->busySince = millis(); }
  if (r == nzim::BUSY && millis() - j->busySince < 20000) {
    if (uxQueueMessagesWaiting(g_uq) == 0) vTaskDelay(pdMS_TO_TICKS(10));
    Msg m = {0, new JobP(j)};
    if (xQueueSendToBack(g_uq, &m, pdMS_TO_TICKS(500)) == pdTRUE) return true;
    delete (JobP *)m.p;
  }
  if (r != nzim::OK) {
    if (r == nzim::BUSY || r == nzim::TOO_BIG) s_busy++;
    Serial.printf("[zim] busy u: r=%d waited %ums held %d line %u\n", r, (unsigned)(j->waited ? millis() - j->busySince : 0),
                  g_clusters->held(), (unsigned)uxQueueMessagesWaiting(g_uq));
    j->note(503, "Couldn't open this page", r == nzim::TOO_BIG
            ? "This part of the archive is too big for Nomad's memory."
            : r == nzim::BUSY ? "Nomad is busy. Go back and try again. (u)"
            : "The archive could not be read here (" + String(r) + ").");
    return false;
  }
  j->kind = Job::PACKED; j->data = data; j->len = len; j->slot = slot;   // ~Job unpins
  // a file out of an unprepared archive's ~2 MB cluster gets a copy of its own (up to 1 MB,
  // 1.5 MB in all) so the cluster is free again right away. Only two such clusters fit, and
  // a pinned one stays pinned until its last file reaches the phone, which takes seconds
  // over a slow link. Prepared archives don't need this
  if (slot->cap >= 1024 * 1024 && (int32_t)len <= COPY_MAX && s_copyBytes + (int32_t)len <= COPY_BUDGET) {
    uint8_t *c = (uint8_t *)heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c) {
      memcpy(c, data, len);
      s_copyBytes += len;
      j->copy = c; j->data = c; j->slot = nullptr;
      g_clusters->unpin(slot);
    }
  }
  return false;
}

static void unzipTask(void *) {
  uint32_t lastJob = 0;
  for (;;) {
    Msg m;
    if (xQueueReceive(g_uq, &m, pdMS_TO_TICKS(10000)) != pdTRUE) {
      continue;   // the arena stays: once given back, PSRAM fills in around where it was
                  // and a 4.6 MB block is never free again
    }
    if (m.kind == 1) { g_clusters->unpin((nzim::Clusters::Slot *)m.p); continue; }
    if (m.kind == 2) { closeArcNow((Arc *)m.p); continue; }
    lastJob = millis();
    JobP *jp = (JobP *)m.p;
    bool requeued = false;
    if (jp->use_count() > 1) requeued = unpack(*jp);
    if (!requeued) (*jp)->done();
    delete jp;
    // core 0's idle task is on the task watchdog: let it run between clusters
    vTaskDelay(1);
  }
}

static String encQ(const String &s) {
  static const char *hex = "0123456789ABCDEF";
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    unsigned char c = s[i];
    if (isalnum(c) || strchr("-_.~/:@!$'()*,;", c)) o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

static void onRequest(AsyncWebServerRequest *request) {
  const String &url = request->url();                   // already percent-decoded
  int n = -1; int slash = -1;
  if (url.length() > 9) {
    slash = url.indexOf('/', 9);
    String num = slash < 0 ? url.substring(9) : url.substring(9, slash);
    bool digits = num.length() > 0 && num.length() < 6;
    for (size_t i = 0; i < num.length(); i++) if (!isdigit((unsigned char)num[i])) digits = false;
    if (digits) n = num.toInt();
  }
  if (n < 0) { request->send(404, "text/plain", "Not an archive address"); return; }
  if (!g_q || uxQueueSpacesAvailable(g_q) < 2) { request->send(503, "text/plain", "Busy. Try again."); return; }
  JobP j = std::make_shared<Job>();
  j->idx = n;
  j->path = slash < 0 ? "" : url.substring(slash + 1).c_str();
  // the query, as the page wrote it (a Zimit copy keeps "page?x=1" under its whole address)
  bool first = true;
  for (size_t i = 0; i < request->params(); i++) {
    const AsyncWebParameter *p = request->getParam(i);
    if (!p || p->isPost() || p->isFile()) continue;
    j->q += first ? "?" : "&";
    j->rawQuery += first ? "?" : "&";
    j->q += p->name().c_str();
    j->rawQuery += encQ(p->name()).c_str();
    if (p->value().length()) {
      j->q += "="; j->q += p->value().c_str();
      j->rawQuery += "="; j->rawQuery += encQ(p->value()).c_str();
    }
    first = false;
  }
  if (request->hasHeader("Range")) j->range = request->header("Range");
  j->host = request->host();
  Msg m = {0, new JobP(j)};
  if (xQueueSend(g_q, &m, 0) != pdTRUE) {
    delete (JobP *)m.p;
    request->send(503, "text/plain", "Busy. Try again.");
    return;
  }
  // a moment for the usual case (a lookup, a small cluster already unpacked or quick to
  // unpack) so it goes out at once. Anything longer is answered from the network task's
  // polling. This blocks the network task, so it is kept short
  for (int i = 0; i < 30 && !j->ready.load(std::memory_order_acquire); i++) vTaskDelay(1);
  if (j->ready.load(std::memory_order_acquire) && dmaOk()) request->send(build(request, j));
  else { s_dmaWaits += j->ready ? 1 : 0; request->send(new Deferred(j)); }
}

}  // namespace zimsrv

// called once from setup(), before server.begin()
static void nomadZimRegister(AsyncWebServer &server) {
  nzim::hooks = {zimsrv::hookRead, zimsrv::hookAlloc, zimsrv::hookFree, zimsrv::hookLargest, zimsrv::nowUs, zimsrv::xzDecode};
  // a 4.6 MB arena of 64 KB pages on an S3: two unprepared ~2 MB clusters, or a few dozen
  // prepared 128 KB ones. A P4's 32 MB has room for three times that
  size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  bool big = psram > 16u * 1024 * 1024;
  zimsrv::g_slots = big ? 160 : 48;
  // in PSRAM, like everything here that lives long. Internal RAM held at rest pushes the
  // media handler under its floor
  zimsrv::g_clusters = new (heap_caps_malloc(sizeof(nzim::Clusters), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) nzim::Clusters(zimsrv::g_slots);
  if (big) zimsrv::g_clusters->arenaBytes = 3 * zimsrv::g_clusters->arenaBytes;
  // claimed now, while PSRAM is in one piece, and kept: an S3 keeps ~3.7 MB for the rest
  if (!zimsrv::g_clusters->claim()) Serial.println("[zim] no room for the archive arena");
  // long enough for 16 connections' worth of requests each waiting its turn (PSRAM: cheap)
  zimsrv::g_q = xQueueCreateWithCaps(96, sizeof(zimsrv::Msg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  zimsrv::g_uq = xQueueCreateWithCaps(192, sizeof(zimsrv::Msg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  // both below the network. Lookups are quick card reads, unpacking a 2 MB cluster is
  // CPU for a while, so it gets the other core from the lookups and the UI's loop()
  nomadCreateTask(zimsrv::task, "zim", 12288, nullptr, 3, &zimsrv::g_task, 1);
  nomadCreateTask(zimsrv::unzipTask, "unzip", 12288, nullptr, 2, &zimsrv::g_utask, 0);
  server.on("/zimview/*", HTTP_GET | HTTP_HEAD, zimsrv::onRequest);
  server.on("/api/zimserve", HTTP_GET, [](AsyncWebServerRequest *r) {
    using namespace zimsrv;
    auto *c = g_clusters;
    String j = "{\"v\":1,\"slots\":" + String(g_slots) +
               ",\"jobs\":" + String((unsigned)s_jobs) + ",\"packed\":" + String((unsigned)s_packed) +
               ",\"raw\":" + String((unsigned)s_raw) + ",\"redirects\":" + String((unsigned)s_redirects) +
               ",\"missing\":" + String((unsigned)s_missing) + ",\"busy\":" + String((unsigned)s_busy) +
               ",\"lookupMs\":" + String((unsigned)(s_lookupUs / 1000)) + ",\"packedMs\":" + String((unsigned)(s_packedUs / 1000)) +
               ",\"zstdMs\":" + String((unsigned)(c->zstdUs / 1000)) + ",\"readMs\":" + String((unsigned)(c->readUs / 1000)) +
               ",\"arenaKB\":" + String((unsigned)(c->arenaSize() / 1024)) + ",\"bigKB\":" + String((unsigned)(c->bigSize() / 1024)) +
               ",\"held\":" + String(c->held()) + ",\"evictions\":" + String((unsigned)c->evictions) +
               ",\"dmaFree\":" + String((unsigned)streamDmaFree()) + ",\"dmaMin\":" + String((unsigned)s_dmaMin) +
               ",\"dmaWaits\":" + String((unsigned)s_dmaWaits) +
               ",\"unpackedKB\":" + String((unsigned)(c->outBytes / 1024)) + ",\"cardKB\":" + String((unsigned)(c->cardBytes / 1024)) +
               ",\"psramFree\":" + String((unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) +
               ",\"psramLargest\":" + String((unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)) + "}";
    AsyncWebServerResponse *res = r->beginResponse(200, "application/json", j);
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });
}

#endif
