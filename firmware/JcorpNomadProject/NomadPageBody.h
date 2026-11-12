// NomadPageBody.h - page files and shared assets off the card, sent late rather than short
// when Nomad is busy.
//
// The library's own file response can cut a body short under a 200 with the full
// Content-Length, which leaves the browser with a broken script. It reads the card without
// sdMutex into a PSRAM send buffer, so the SD driver borrows a DMA block for every sector
// and that fails with films playing. And _ack counts what it gave client()->write() as
// sent, but write() returns 0 when lwIP is out of memory and those bytes are lost.
//
// Here a body reads under sdMutex, in whole sectors, through a DMA buffer borrowed for that
// one read, into a 4 KB block in PSRAM, and only what the socket takes is counted. Anything
// that can't happen right now waits for the next ack or poll (about every half second) and
// carries on from where it stopped. The archive server reads the card the same way.

#ifndef NOMAD_PAGE_BODY_H
#define NOMAD_PAGE_BODY_H

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <memory>
#include <new>
#include <ESPAsyncWebServer.h>
#include "NomadSD.h"

static const size_t NOMAD_PAGE_BLOCK = 4096;
static const uint32_t NOMAD_PAGE_LOCK_MS = 250;   // runs on the network task: wait briefly, then on the next ack or poll
#ifndef NOMAD_PAGE_ACK_MS
#define NOMAD_PAGE_ACK_MS 5000   // AsyncTCP's own default (CONFIG_ASYNC_TCP_MAX_ACK_TIME)
#endif

struct NomadPageBody {
  NomadFile64 f;
  String path;
  uint64_t size = 0;
  uint8_t *blk = nullptr;     // PSRAM, NOMAD_PAGE_BLOCK
  uint64_t blkStart = 0;      // file offset of blk[0], a multiple of 512
  size_t blkLen = 0;
  uint32_t stuckAt = 0;       // millis() of the first refill that couldn't happen
  size_t sent = 0;            // what the socket has taken, for the timeout's note
  bool stuck = false, stuckLogged = false;
  ~NomadPageBody() {
    // a read-only close does no card I/O, but SdFat's handle state is shared with the
    // reads under the lock
    const bool locked = sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) == pdTRUE;
    f.close();
    if (locked) xSemaphoreGive(sdMutex);
    if (blk) heap_caps_free(blk);
  }
};

// Put the 4 KB around pos in b.blk. false: not now, try again on the next ack or poll.
static bool nomadPageRefill(NomadPageBody &b, uint64_t pos) {
  if (!b.blk) {
    b.blk = (uint8_t *)heap_caps_malloc(NOMAD_PAGE_BLOCK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b.blk) return false;
  }
  const uint64_t aligned = pos & ~(uint64_t)511;
  size_t k = NOMAD_PAGE_BLOCK;
  if (aligned + k > b.size) k = (size_t)(b.size - aligned);
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(NOMAD_PAGE_LOCK_MS)) != pdTRUE) return false;
  // whole sectors (the file's last part sector rounded up), so SdFat reads straight into it
  uint8_t *dma = (uint8_t *)heap_caps_aligned_alloc(64, (k + 511) & ~(size_t)511, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  size_t got = 0;
  if (dma) {
    got = b.f.readAt(aligned, dma, k);
    if (got == k) memcpy(b.blk, dma, k);
    heap_caps_free(dma);
  }
  if (sdMutex) xSemaphoreGive(sdMutex);
  if (got != k) return false;
  b.blkStart = aligned;
  b.blkLen = k;
  return true;
}

static uint32_t g_pageAddFails = 0;   // socket had room but lwIP refused the segment
static uint32_t g_pageAddFailLogMs = 0;

class NomadBodyResponse : public AsyncWebServerResponse {
  std::shared_ptr<NomadPageBody> _b;
  String _head;   // what the socket hasn't taken of the headers yet

public:
  NomadBodyResponse(std::shared_ptr<NomadPageBody> b, const String &mime) : _b(b) {
    _code = 200;
    _contentType = mime;
    _contentLength = (size_t)b->size;
    _sendContentLength = true;
    _chunked = false;
  }
  bool _sourceValid() const override { return true; }

  void _respond(AsyncWebServerRequest *request) override {
    addHeader("Connection", "close", false);
    _assembleHead(_head, request->version());
    _state = RESPONSE_HEADERS;
    _ack(request, 0, 0);
  }

  size_t _ack(AsyncWebServerRequest *request, size_t len, uint32_t time) override {
    (void)time;
    _ackedLength += len;
    AsyncClient *c = request->client();
    if (!c) return 0;
    size_t added = 0;
    if (_state == RESPONSE_HEADERS) {
      while (_head.length()) {
        const size_t room = c->space();
        if (!room) break;
        const size_t w = c->add(_head.c_str(), _head.length() < room ? _head.length() : room);
        if (!w) { notePageAddFail(); break; }
        _head.remove(0, w);
        _writtenLength += w;
        added += w;
      }
      if (!_head.length()) _state = RESPONSE_CONTENT;
    }
    NomadPageBody &b = *_b;
    while (_state == RESPONSE_CONTENT) {
      if (_sentLength >= _contentLength) { _state = RESPONSE_WAIT_ACK; break; }
      const uint64_t pos = _sentLength;
      if (!(b.blkLen && pos >= b.blkStart && pos < b.blkStart + b.blkLen) && !nomadPageRefill(b, pos)) {
        if (!b.stuck) { b.stuck = true; b.stuckAt = millis(); }
        else if (!b.stuckLogged && millis() - b.stuckAt > 5000) {
          b.stuckLogged = true;
          Serial.printf("[Page] %s waiting on the card at %u of %llu\n", b.path.c_str(), (unsigned)pos, (unsigned long long)b.size);
        }
        break;
      }
      b.stuck = false;
      size_t take = (size_t)(b.blkStart + b.blkLen - pos);
      const size_t room = c->space();
      if (take > room) take = room;
      if (!take) break;
      const size_t w = c->add((const char *)b.blk + (pos - b.blkStart), take);
      if (!w) { notePageAddFail(); break; }
      _sentLength += w;
      _writtenLength += w;
      added += w;
      b.sent = _sentLength;
    }
    if (added) c->send();
    if (_state == RESPONSE_WAIT_ACK && _ackedLength >= _writtenLength) _state = RESPONSE_END;
    return added;
  }

private:
  static void notePageAddFail() {
    g_pageAddFails++;
    if (millis() - g_pageAddFailLogMs > 10000) {
      g_pageAddFailLogMs = millis();
      Serial.printf("[Page] socket refused a segment (lwIP memory), %u times since boot; carrying on\n", (unsigned)g_pageAddFails);
    }
  }
};

// a path as one query value (/media?file=...): only unreserved characters and '/' stay
static String nomadQueryEncode(const String &s) {
  static const char *hex = "0123456789ABCDEF";
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    const unsigned char c = (unsigned char)s[i];
    if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

// The response for a card file the caller has already found (size from fileStat64).
// nullptr: it didn't open. Takes sdMutex for the open.
static AsyncWebServerResponse *nomadPageResponse(AsyncWebServerRequest *request, const String &cardPath,
                                                 const String &mime, uint64_t size) {
  auto b = std::make_shared<NomadPageBody>();
  b->path = cardPath;
  b->size = size;
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return nullptr;
  const bool ok = b->f.open(cardPath.c_str());
  if (sdMutex) xSemaphoreGive(sdMutex);
  if (!ok) return nullptr;
  AsyncWebServerResponse *r = new (std::nothrow) NomadBodyResponse(b, mime);
  if (r && request->client()) {
    AsyncClient *c = request->client();
    c->setAckTimeout(NOMAD_PAGE_ACK_MS);
    // the library's handler, which closes, with a note of where the body was
    c->onTimeout([b](void *, AsyncClient *cl, uint32_t t) {
      Serial.printf("[Page] no ack for %u ms, closing %s at %u of %llu\n", (unsigned)t, b->path.c_str(),
                    (unsigned)b->sent, (unsigned long long)b->size);
      cl->close();
    }, nullptr);
  }
  return r;
}

// A card file by path, for the routes that don't need sendSdFile's checks (the portal
// pages, the folder indexes). Sends the file, or a 503 to retry, or a 404 if it's missing.
static void nomadSendCardFile(AsyncWebServerRequest *request, const String &path, const String &mime,
                              const char *cacheControl, const String &etag = String()) {
  uint64_t size = 0;
  time_t mtime = 0;
  bool ok = false, locked = true;
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) != pdTRUE) locked = false;
  else {
    ok = SD_MMC.fileStat64(path.c_str(), size, mtime);
    if (sdMutex) xSemaphoreGive(sdMutex);
  }
  if (locked && !ok) { request->send(404, "text/plain", "File not found"); return; }
  AsyncWebServerResponse *r = (ok && size < 0xFFFFFFFFull) ? nomadPageResponse(request, path, mime, size) : nullptr;
  if (!r) {
    AsyncWebServerResponse *busy = request->beginResponse(503, "text/plain", "Server busy - retry");
    if (busy) { busy->addHeader("Retry-After", "1"); request->send(busy); }
    return;
  }
  r->addHeader("Cache-Control", cacheControl);
  if (etag.length()) r->addHeader("ETag", etag);
  request->send(r);
}

#endif  // NOMAD_PAGE_BODY_H
