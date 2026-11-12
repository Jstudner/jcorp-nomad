// NomadThumbs.cpp - see NomadThumbs.h for what this is and why.

#include "NomadThumbs.h"
#include "nomad_board.h"
#include "NomadSD.h"

#define SD_MMC NomadSD

extern void webLog(const String &message, const String &type);
extern void webLogf(const String &type, const char *format, ...);
extern SemaphoreHandle_t sdMutex;
extern volatile bool indexingTasksActive;
extern volatile bool indexingInProgress;
extern volatile bool sdScanInProgress;
extern volatile bool requestIndexing;

// The directory generated thumbnails live in. A dot directory, so
// shouldSkipIndexingPath() already keeps it out of the media index and the listings.
static const char *THUMB_ROOT = "/.thumbs";

// The name comes from the source path alone, so serving a thumbnail is one existence
// check with no stat of the original. The optimiser checks freshness, the request path
// doesn't.

static uint64_t fnv1a(const String &s) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c >= 'A' && c <= 'Z') c += 32;   // case-fold: FAT is case-insensitive
    h ^= (uint8_t)c;
    h *= 1099511628211ULL;
  }
  return h;
}

static bool isJpegPath(const String &p) {
  String l = p;
  l.toLowerCase();
  return l.endsWith(".jpg") || l.endsWith(".jpeg");
}

bool nomadThumbsSupported() {
#if NOMAD_HAS_THUMBNAILER
  return true;
#else
  return false;
#endif
}

String nomadThumbPathFor(const String &srcPath) {
#if !NOMAD_HAS_THUMBNAILER
  (void)srcPath;
  return String();
#else
  if (!isJpegPath(srcPath)) return String();
  if (srcPath.startsWith(THUMB_ROOT)) return String();   // never thumbnail a thumbnail
  uint64_t h = fnv1a(srcPath);
  char buf[64];
  // Sharded by the top byte: a few thousand entries in one FAT directory turns every
  // open into a linear scan, and the serve path does one of those per grid card.
  snprintf(buf, sizeof(buf), "%s/%02x/%014llx.jpg", THUMB_ROOT,
           (unsigned)(h >> 56), (unsigned long long)(h & 0x00FFFFFFFFFFFFFFULL));
  return String(buf);
#endif
}

#if !NOMAD_HAS_THUMBNAILER

String nomadThumbLookup(const String &) { return String(); }
bool nomadThumbsStart(bool) { return false; }
void nomadThumbsStop() {}
bool nomadThumbsClear() { return false; }
bool nomadThumbsBusy() { return false; }
bool nomadThumbsAllOptimised() { return false; }
void nomadThumbsStatus(NomadThumbStats &out) {
  memset(&out, 0, sizeof(out));
  out.phase = NOMAD_THUMB_IDLE;
}

#else  // NOMAD_HAS_THUMBNAILER

#include "driver/jpeg_decode.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static NomadThumbStats s_st;
static volatile bool s_running = false;
static volatile bool s_stopReq = false;
static volatile bool s_clearing = false;
static bool s_everScanned = false;
static uint32_t s_startedAt = 0;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

String nomadThumbLookup(const String &srcPath) {
  String t = nomadThumbPathFor(srcPath);
  if (!t.length()) return String();
  // This runs on async_tcp, once per grid card, and NomadSD's per-volume lock waits
  // forever. Without a bounded wait a grid burst during a pass or a reindex parks the whole
  // web server on the card before the handler gets to its own timed lock. If the card is
  // busy, say there's no thumbnail and the caller serves the original.
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(250)) != pdTRUE) return String();
  bool have = SD_MMC.exists(t);
  if (sdMutex) xSemaphoreGive(sdMutex);
  return have ? t : String();
}

void nomadThumbsStatus(NomadThumbStats &out) {
  portENTER_CRITICAL(&s_lock);
  out = s_st;
  portEXIT_CRITICAL(&s_lock);
  if (s_running) out.elapsedMs = millis() - s_startedAt;
}

bool nomadThumbsAllOptimised() {
  // Only a pass that ran to completion can say the library is done. A stopped pass only
  // counted `covers` as far as it got.
  return s_everScanned && !s_running && s_st.phase == NOMAD_THUMB_DONE &&
         s_st.covers > 0 && s_st.failed == 0 &&
         (s_st.upToDate + s_st.skipped) >= s_st.covers;
}

static void setCurrent(const char *p) {
  portENTER_CRITICAL(&s_lock);
  strncpy(s_st.current, p ? p : "", sizeof(s_st.current) - 1);
  s_st.current[sizeof(s_st.current) - 1] = 0;
  portEXIT_CRITICAL(&s_lock);
}

static void setError(const char *p) {
  portENTER_CRITICAL(&s_lock);
  strncpy(s_st.lastError, p ? p : "", sizeof(s_st.lastError) - 1);
  s_st.lastError[sizeof(s_st.lastError) - 1] = 0;
  portEXIT_CRITICAL(&s_lock);
}

static jpeg_decoder_handle_t s_dec = NULL;
static jpeg_encoder_handle_t s_enc = NULL;
static ppa_client_handle_t s_ppa = NULL;

static bool hwAcquire() {
  jpeg_decode_engine_cfg_t dcfg = {};
  dcfg.timeout_ms = 5000;
  if (jpeg_new_decoder_engine(&dcfg, &s_dec) != ESP_OK) {
    setError("JPEG decoder unavailable");
    return false;
  }
  jpeg_encode_engine_cfg_t ecfg = {};
  ecfg.timeout_ms = 5000;
  if (jpeg_new_encoder_engine(&ecfg, &s_enc) != ESP_OK) {
    setError("JPEG encoder unavailable");
    return false;
  }
  ppa_client_config_t pcfg = {};
  pcfg.oper_type = PPA_OPERATION_SRM;
  pcfg.max_pending_trans_num = 1;
  if (ppa_register_client(&pcfg, &s_ppa) != ESP_OK) {
    setError("PPA unavailable");
    return false;
  }
  return true;
}

static void hwRelease() {
  if (s_ppa) { ppa_unregister_client(s_ppa); s_ppa = NULL; }
  if (s_enc) { jpeg_del_encoder_engine(s_enc); s_enc = NULL; }
  if (s_dec) { jpeg_del_decoder_engine(s_dec); s_dec = NULL; }
}

static inline uint32_t align16(uint32_t v) { return (v + 15u) & ~15u; }

// Both L1 and L2 are 64-byte lines on the P4 (CONFIG_CACHE_L*_CACHE_LINE_SIZE). The PPA
// wants its output buffer address AND its declared size aligned to that, for external
// memory, and rejects the transaction with ESP_ERR_INVALID_ARG otherwise.
static const size_t CACHE_LINE = 64;
static inline size_t alignCache(size_t v) { return (v + CACHE_LINE - 1) & ~(CACHE_LINE - 1); }

// Skip reasons, so the pass can count each kind.
static const char *SKIP_SMALL       = "already small";
static const char *SKIP_PROGRESSIVE = "progressive JPEG";
static const char *SKIP_NO_SAVING   = "no saving";
static const char *SKIP_TOO_BIG     = "too large for free memory";
static const char *SKIP_NOT_JPEG    = "not a JPEG";

struct Buf {
  void *p = NULL;
  size_t n = 0;
  void free_() { if (p) { heap_caps_free(p); p = NULL; n = 0; } }
  ~Buf() { free_(); }
};

// Read a whole file into a decoder-aligned buffer. Held under the card lock only for
// the read itself. The codec work afterwards must not block someone streaming.
static bool readAll(const String &path, Buf &out, uint32_t &fileSize, time_t &mtime) {
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
    setError("card busy");
    return false;
  }
  File f = SD_MMC.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    if (sdMutex) xSemaphoreGive(sdMutex);
    setError("cover unreadable");
    return false;
  }
  fileSize = f.size();
  mtime = f.getLastWrite();
  jpeg_decode_memory_alloc_cfg_t mcfg = {};
  mcfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
  out.p = jpeg_alloc_decoder_mem(fileSize, &mcfg, &out.n);
  if (!out.p) {
    f.close();
    if (sdMutex) xSemaphoreGive(sdMutex);
    setError("out of memory reading cover");
    return false;
  }
  size_t got = f.read((uint8_t *)out.p, fileSize);
  f.close();
  if (sdMutex) xSemaphoreGive(sdMutex);
  if (got != fileSize) {
    setError("short read on cover");
    return false;
  }
  return true;
}

static const uint8_t THUMB_TAG[4] = { 'N', 'T', 'H', '1' };
static const size_t THUMB_TRAILER = sizeof(THUMB_TAG) + 4 + 4;

static void packTrailer(uint8_t *out, uint32_t srcSize, uint32_t srcStamp) {
  memcpy(out, THUMB_TAG, sizeof(THUMB_TAG));
  memcpy(out + 4, &srcSize, 4);
  memcpy(out + 8, &srcStamp, 4);
}

static bool writeAll(const String &path, const uint8_t *data, size_t len,
                    uint32_t srcSize, uint32_t srcStamp) {
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
    setError("card busy");
    return false;
  }
  // LittleFS-style mkdir -p: the shard directory, then the root above it.
  int slash = path.lastIndexOf('/');
  if (slash > 0) {
    String parent = path.substring(0, slash);
    if (!SD_MMC.exists(parent)) {
      if (!SD_MMC.exists(THUMB_ROOT)) SD_MMC.mkdir(THUMB_ROOT);
      SD_MMC.mkdir(parent);
    }
  }
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    if (sdMutex) xSemaphoreGive(sdMutex);
    setError("could not create thumbnail");
    return false;
  }
  size_t put = f.write(data, len);
  uint8_t tail[12];
  packTrailer(tail, srcSize, srcStamp);
  if (put == len && f.write(tail, THUMB_TRAILER) != THUMB_TRAILER) put = 0;
  f.close();
  if (put != len) SD_MMC.remove(path);
  if (sdMutex) xSemaphoreGive(sdMutex);
  if (put != len) {
    setError("card full? thumbnail write short");
    return false;
  }
  return true;
}

// Quantise a scale factor to what the PPA can actually do: an integer plus a 4-bit
// fraction, so steps of 1/16. Always rounds down, so a thumbnail never overshoots the
// target edge, and never below 1/16 which is the hardware's floor.
static float quantiseScale(float want) {
  int steps = (int)(want * 16.0f);
  if (steps < 1) steps = 1;
  if (steps > 255 * 16) steps = 255 * 16;
  return (float)steps / 16.0f;
}

// Read width, height and the SOF marker straight out of the JPEG. jpeg_decoder_get_info()
// answers ESP_OK with a zeroed struct for a frame header it doesn't recognise, which looks
// like a real 0x0 image. Walking the markers gives a definite answer and the frame type,
// which matters since the hardware decoder is baseline-only.
static bool sofIsBaseline(uint8_t m) {
  // C0 baseline, C1 extended sequential: both are sequential DCT and decodable.
  return m == 0xC0 || m == 0xC1;
}

static bool jpegHeader(const uint8_t *d, uint32_t n, uint32_t &w, uint32_t &h, uint8_t &sof) {
  if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;   // no SOI
  uint32_t i = 2;
  while (i + 3 < n) {
    if (d[i] != 0xFF) { i++; continue; }                     // resync over padding
    uint8_t m = d[i + 1];
    if (m == 0xFF) { i++; continue; }                        // fill byte
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD9)) { i += 2; continue; }   // no payload
    uint32_t len = ((uint32_t)d[i + 2] << 8) | d[i + 3];
    if (len < 2 || i + 2 + len > n) return false;
    // Every SOFn carries the frame size. DHT (C4), JPG (C8) and DAC (CC) share the
    // range but are not frame headers.
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      if (len < 8) return false;
      h = ((uint32_t)d[i + 5] << 8) | d[i + 6];
      w = ((uint32_t)d[i + 7] << 8) | d[i + 8];
      sof = m;
      return w > 0 && h > 0;
    }
    i += 2 + len;
  }
  return false;
}

// One cover. Returns true when a thumbnail was written, sets *why when it skips one.
static bool convertOne(const String &src, const String &dst,
                       uint32_t &inBytes, uint32_t &outBytes, const char **why) {
  *why = NULL;

  Buf jin;
  uint32_t srcSize = 0;
  time_t srcMtime = 0;
  if (!readAll(src, jin, srcSize, srcMtime)) return false;
  inBytes = srcSize;

  if (srcSize < NOMAD_THUMB_MIN_SRC_BYTES) { *why = SKIP_SMALL; return false; }

  uint32_t w = 0, h = 0;
  uint8_t sof = 0;
  if (!jpegHeader((const uint8_t *)jin.p, srcSize, w, h, sof)) {
    // A .jpg that is really a PNG is common in scraped artwork, say which it is.
    const uint8_t *b = (const uint8_t *)jin.p;
    if (srcSize > 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
      setError("PNG file named .jpg");
      *why = SKIP_NOT_JPEG;
    } else {
      setError("not a readable JPEG");
      *why = SKIP_NOT_JPEG;
    }
    return false;
  }
  // The P4's engine is documented as baseline-only, and progressive JPEG (SOF2/6/10/14)
  // is common in scraped artwork. Try the hardware anyway and sort by the result: a failed
  // non-baseline frame is a capability limit, a failed baseline frame is a real fault.
  if (w <= NOMAD_THUMB_MAX_PX && h <= NOMAD_THUMB_MAX_PX) { *why = SKIP_SMALL; return false; }

  // The decoder always writes a whole number of 16-pixel MCUs, so the buffer is the
  // aligned size even though only w x h of it is the picture.
  // Bound the dimensions before any arithmetic. w and h come straight from the frame
  // header, so a corrupt or hostile file can claim 65535x65535, and (size_t)aw*ah*2 is a
  // 32-bit multiply that would wrap to a small number and get past the memory guard below.
  if (w > 8192 || h > 8192) { *why = SKIP_TOO_BIG; return false; }
  const uint32_t aw = align16(w), ah = align16(h);
  const uint64_t rawSize64 = (uint64_t)aw * ah * 2;   // RGB565
  if (rawSize64 > (uint64_t)24 * 1024 * 1024) { *why = SKIP_TOO_BIG; return false; }
  const size_t rawSize = (size_t)rawSize64;

  // Refuse rather than fragment PSRAM into uselessness for whoever is streaming.
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  if (rawSize + (rawSize / 4) > largest) { *why = SKIP_TOO_BIG; return false; }

  Buf raw;
  jpeg_decode_memory_alloc_cfg_t rcfg = {};
  rcfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
  raw.p = jpeg_alloc_decoder_mem(rawSize, &rcfg, &raw.n);
  if (!raw.p) { setError("out of memory decoding cover"); return false; }

  jpeg_decode_cfg_t dcfg = {};
  dcfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  dcfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
  dcfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
  uint32_t decoded = 0;
  esp_err_t de = jpeg_decoder_process(s_dec, &dcfg, (const uint8_t *)jin.p, srcSize,
                                      (uint8_t *)raw.p, raw.n, &decoded);
  if (de != ESP_OK) {
    static char m[96];
    if (!sofIsBaseline(sof)) {
      snprintf(m, sizeof(m), "progressive JPEG (SOF%u): %s", (unsigned)(sof & 0x0F), esp_err_to_name(de));
      setError(m);
      *why = SKIP_PROGRESSIVE;
    } else {
      snprintf(m, sizeof(m), "hardware JPEG decode failed (%s) on %ux%u", esp_err_to_name(de),
               (unsigned)w, (unsigned)h);
      setError(m);
    }
    return false;
  }
  jin.free_();   // the compressed source is dead weight from here on

  // Scale, and the geometry that goes with it. The PPA refuses (ESP_ERR_INVALID_ARG)
  // when the block it would produce doesn't fit the output picture, and block * k/16 is
  // only whole pixels when the block is a multiple of 16 (640x360 at 9/16 wants 360 x
  // 202.5). So crop the input to a multiple of 16, centred, at most 15 pixels a side.
  const uint32_t k = (uint32_t)(quantiseScale((float)NOMAD_THUMB_MAX_PX / (float)(w > h ? w : h)) * 16.0f + 0.5f);
  const float q = (float)k / 16.0f;
  uint32_t bw = w & ~15u, bh = h & ~15u;
  if (bw < 16 || bh < 16) { *why = SKIP_SMALL; return false; }
  const uint32_t offx = (w - bw) / 2, offy = (h - bh) / 2;
  uint32_t ow = (bw / 16) * k, oh = (bh / 16) * k;
  if (ow < 16 || oh < 16) { *why = SKIP_SMALL; return false; }

  // This buffer is the PPA's output and the encoder's input, so it has to suit both.
  // jpeg_alloc_encoder_mem() doesn't always align to a cache line (360x198x3 is a multiple
  // of 16 but not 64) and the PPA refuses that. Allocate it the PPA's way, which also
  // covers what the encoder wants.
  const size_t scaledSize = (size_t)ow * oh * 3;   // RGB888
  const size_t scaledAlloc = alignCache(scaledSize);
  Buf scaled;
  scaled.p = heap_caps_aligned_calloc(CACHE_LINE, 1, scaledAlloc, MALLOC_CAP_SPIRAM);
  scaled.n = scaledAlloc;
  if (!scaled.p) { scaled.n = 0; setError("out of memory scaling cover"); return false; }

  ppa_srm_oper_config_t srm = {};
  srm.in.buffer = raw.p;
  srm.in.pic_w = aw;
  srm.in.pic_h = ah;
  srm.in.block_w = bw;
  srm.in.block_h = bh;
  srm.in.block_offset_x = offx;
  srm.in.block_offset_y = offy;
  srm.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  srm.out.buffer = scaled.p;
  srm.out.buffer_size = scaled.n;
  srm.out.pic_w = ow;
  srm.out.pic_h = oh;
  srm.out.block_offset_x = 0;
  srm.out.block_offset_y = 0;
  srm.out.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
  srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  srm.scale_x = q;
  srm.scale_y = q;
  srm.mode = PPA_TRANS_MODE_BLOCKING;
  esp_err_t pe = ppa_do_scale_rotate_mirror(s_ppa, &srm);
  if (pe != ESP_OK) {
    char m[96];
    snprintf(m, sizeof(m), "PPA scale failed (%s) %ux%u blk %ux%u+%u+%u -> %ux%u buf %u",
             esp_err_to_name(pe), (unsigned)w, (unsigned)h, (unsigned)bw, (unsigned)bh,
             (unsigned)offx, (unsigned)offy, (unsigned)ow, (unsigned)oh, (unsigned)scaledAlloc);
    setError(m);
    return false;
  }
  raw.free_();

  // A thumbnail is a fraction of its source, half the raw size is plenty.
  size_t encCap = scaledSize / 2;
  if (encCap < 16384) encCap = 16384;
  Buf jout;
  jpeg_encode_memory_alloc_cfg_t ocfg = {};
  ocfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  jout.p = jpeg_alloc_encoder_mem(encCap, &ocfg, &jout.n);
  if (!jout.p) { setError("out of memory encoding thumbnail"); return false; }

  jpeg_encode_cfg_t ecfg = {};
  ecfg.width = ow;
  ecfg.height = oh;
  ecfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB888;
  ecfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
  ecfg.image_quality = NOMAD_THUMB_QUALITY;
  uint32_t jlen = 0;
  esp_err_t ee = jpeg_encoder_process(s_enc, &ecfg, (const uint8_t *)scaled.p, scaledSize,
                                      (uint8_t *)jout.p, jout.n, &jlen);
  if (ee != ESP_OK || jlen == 0) {
    char m[96];
    snprintf(m, sizeof(m), "JPEG encode failed (%s) at %ux%u", esp_err_to_name(ee),
             (unsigned)ow, (unsigned)oh);
    setError(m);
    return false;
  }
  scaled.free_();

  if (jlen >= srcSize) { *why = SKIP_NO_SAVING; return false; }
  if (!writeAll(dst, (const uint8_t *)jout.p, jlen, srcSize, (uint32_t)srcMtime)) return false;
  outBytes = jlen;
  return true;
}

struct PassCfg { bool rebuildAll; };
static PassCfg s_cfg;

// Cover art lives beside the media it belongs to. Gallery and Files are left out, they
// hold the user's own pictures.
static const char *THUMB_ROOTS[] = { "/Movies", "/Shows", "/Books", "/Games",
                                     "/Music", "/Cookbook", "/Workshop", NULL };

// Is this thumbnail still the one for this cover?
//
// Timestamps can't tell. The device stamps files with a fixed epoch plus uptime (no RTC,
// no NTP), while cover art has the real date from the PC that wrote the card, so a fresh
// thumbnail can look older than its cover.
//
// So the source's size and stamp go into the thumbnail as a short trailer after the
// JPEG's end-of-image marker, where decoders ignore it, and get compared to the cover.
static bool thumbIsCurrent(const String &src, const String &dst) {
  if (s_cfg.rebuildAll) return false;
  if (!SD_MMC.exists(dst)) return false;

  File t = SD_MMC.open(dst, FILE_READ);
  if (!t) return false;
  uint32_t tsize = t.size();
  if (tsize < THUMB_TRAILER) { t.close(); return false; }
  uint8_t tail[12] = {0};
  t.seek(tsize - THUMB_TRAILER);
  size_t got = t.read(tail, THUMB_TRAILER);
  t.close();
  if (got != THUMB_TRAILER || memcmp(tail, THUMB_TAG, sizeof(THUMB_TAG)) != 0) {
    return false;   // written before the trailer existed, or truncated
  }
  uint32_t wantSize = 0, wantStamp = 0;
  memcpy(&wantSize, tail + 4, 4);
  memcpy(&wantStamp, tail + 8, 4);

  File c = SD_MMC.open(src, FILE_READ);
  if (!c) return true;              // cover vanished, leave what we have
  uint32_t csize = c.size();
  uint32_t cstamp = (uint32_t)c.getLastWrite();
  c.close();
  return csize == wantSize && cstamp == wantStamp;
}

static void walk(const String &dir, int depth) {
  if (s_stopReq || depth > 6) return;
  File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) { if (d) d.close(); return; }

  File e;
  while (!s_stopReq && (e = d.openNextFile())) {
    String p = String(e.path());
    if (!p.startsWith("/")) p = "/" + p;
    bool isDir = e.isDirectory();
    e.close();

    int slash = p.lastIndexOf('/');
    String base = slash >= 0 ? p.substring(slash + 1) : p;
    if (base.startsWith(".")) continue;

    if (isDir) {
      walk(p, depth + 1);
      continue;
    }
    if (!isJpegPath(p)) continue;

    portENTER_CRITICAL(&s_lock);
    s_st.covers++;
    portEXIT_CRITICAL(&s_lock);

    String dst = nomadThumbPathFor(p);
    if (!dst.length()) continue;

    if (thumbIsCurrent(p, dst)) {
      portENTER_CRITICAL(&s_lock);
      s_st.upToDate++;
      portEXIT_CRITICAL(&s_lock);
      continue;
    }

    portENTER_CRITICAL(&s_lock);
    s_st.phase = NOMAD_THUMB_WORKING;
    portEXIT_CRITICAL(&s_lock);
    setCurrent(p.c_str());

    uint32_t in = 0, out = 0;
    const char *why = NULL;
    if (convertOne(p, dst, in, out, &why)) {
      portENTER_CRITICAL(&s_lock);
      s_st.converted++;
      s_st.srcBytes += in;
      s_st.thumbBytes += out;
      portEXIT_CRITICAL(&s_lock);
    } else if (why) {
      portENTER_CRITICAL(&s_lock);
      s_st.skipped++;
      if (why == SKIP_PROGRESSIVE)    { s_st.progressive++; s_st.progressiveBytes += in; }
      else if (why == SKIP_SMALL)     s_st.alreadySmall++;
      else if (why == SKIP_NO_SAVING) s_st.noSaving++;
      else if (why == SKIP_TOO_BIG)   s_st.tooBig++;
      else if (why == SKIP_NOT_JPEG)  s_st.notJpeg++;
      portEXIT_CRITICAL(&s_lock);
    } else {
      portENTER_CRITICAL(&s_lock);
      s_st.failed++;
      portEXIT_CRITICAL(&s_lock);
      webLogf("warning", "[THUMBS] %s: %s", p.c_str(), s_st.lastError);
    }
    // Breathing room. The codecs are fast enough that a tight loop would hold the card
    // lock most of the time and make playback stutter.
    vTaskDelay(pdMS_TO_TICKS(15));
  }
  d.close();
}

static void thumbTask(void *) {
  if (!hwAcquire()) {
    hwRelease();
    portENTER_CRITICAL(&s_lock);
    s_st.phase = NOMAD_THUMB_FAILED;
    portEXIT_CRITICAL(&s_lock);
    webLogf("error", "[THUMBS] could not start: %s", s_st.lastError);
    s_running = false;
    vTaskDelete(NULL);
    return;
  }

  webLog(s_cfg.rebuildAll ? "[THUMBS] rebuilding every cover thumbnail"
                          : "[THUMBS] optimising covers that have no current thumbnail", "info");

  for (int i = 0; THUMB_ROOTS[i] && !s_stopReq; i++) {
    if (!SD_MMC.exists(THUMB_ROOTS[i])) continue;
    walk(String(THUMB_ROOTS[i]), 0);
  }

  hwRelease();
  setCurrent("");
  s_everScanned = true;

  portENTER_CRITICAL(&s_lock);
  s_st.phase = s_stopReq ? NOMAD_THUMB_STOPPED : NOMAD_THUMB_DONE;
  s_st.elapsedMs = millis() - s_startedAt;
  uint32_t conv = s_st.converted, skip = s_st.skipped, fail = s_st.failed, ok = s_st.upToDate;
  uint32_t prog = s_st.progressive, small_ = s_st.alreadySmall;
  uint32_t nosave = s_st.noSaving, big = s_st.tooBig;
  uint64_t progB = s_st.progressiveBytes;
  uint64_t sb = s_st.srcBytes, tb = s_st.thumbBytes;
  portEXIT_CRITICAL(&s_lock);

  // Always print the skip breakdown. "already small" and "progressive" tell nothing-to-do
  // apart from the hardware being unable to do it.
  webLogf("info", "[THUMBS] skips: %u already small, %u progressive (%.1f MB of art), %u no saving, %u too large",
          (unsigned)small_, (unsigned)prog, (double)progB / (1024.0 * 1024.0),
          (unsigned)nosave, (unsigned)big);

  if (conv) {
    webLogf("success", "[THUMBS] %u converted, %u already current, %u skipped, %u failed. %.1f MB of cover art now serves as %.1f MB",
            (unsigned)conv, (unsigned)ok, (unsigned)skip, (unsigned)fail,
            (double)sb / (1024.0 * 1024.0), (double)tb / (1024.0 * 1024.0));
  } else if (prog && prog >= (ok + skip) / 2) {
    webLogf("warning", "[THUMBS] %u of %u covers are progressive JPEG, which this hardware decoder cannot read. Nothing to convert",
            (unsigned)prog, (unsigned)(ok + skip));
  } else if (fail) {
    webLogf("warning", "[THUMBS] nothing converted: %u already current, %u skipped, %u failed. Last: %s",
            (unsigned)ok, (unsigned)skip, (unsigned)fail, s_st.lastError);
  } else {
    webLogf("success", "[THUMBS] nothing to do: %u covers, all already optimised", (unsigned)(ok + skip));
  }

  s_running = false;
  vTaskDelete(NULL);
}

bool nomadThumbsStart(bool rebuildAll) {
  // Check-and-claim in one critical section. HTTP handlers all share async_tcp and can't
  // race each other, but the --selftest hook starts a pass from loopTask, and two winners
  // here would share one set of codec handles.
  bool taken = false;
  portENTER_CRITICAL(&s_lock);
  if (!s_running && !s_clearing) { s_running = true; taken = true; }
  portEXIT_CRITICAL(&s_lock);
  if (!taken) return false;
  // Same set the scan endpoint checks. A reindex that is queued or walking is as much of
  // a problem as one whose tasks are live.
  if (indexingTasksActive || indexingInProgress || sdScanInProgress || requestIndexing) {
    s_running = false;
    setError("indexing or a card scan is running; try again when it finishes");
    return false;
  }
  s_cfg.rebuildAll = rebuildAll;
  s_stopReq = false;

  portENTER_CRITICAL(&s_lock);
  memset(&s_st, 0, sizeof(s_st));
  s_st.phase = NOMAD_THUMB_SCANNING;
  portEXIT_CRITICAL(&s_lock);
  s_startedAt = millis();

  // Its own task instead of the PSRAM-stack pool: the JPEG and PPA drivers want a real
  // stack, and it's short-lived.
  if (xTaskCreatePinnedToCore(thumbTask, "Thumbs", 8192, NULL, 1, NULL, 1) != pdPASS) {
    s_running = false;
    setError("could not start the worker task");
    portENTER_CRITICAL(&s_lock);
    s_st.phase = NOMAD_THUMB_FAILED;
    portEXIT_CRITICAL(&s_lock);
    return false;
  }
  return true;
}

void nomadThumbsStop() { s_stopReq = true; }

bool nomadThumbsBusy() { return s_running || s_clearing; }

static void wipeDir(const String &dir) {
  File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) { if (d) d.close(); return; }
  // Collect first: removing entries while iterating the same handle is not safe.
  String victims[64];
  int n = 0;
  File e;
  while ((e = d.openNextFile()) && n < 64) {
    victims[n++] = String(e.path());
    e.close();
  }
  bool more = (n == 64);
  d.close();
  for (int i = 0; i < n; i++) {
    String p = victims[i];
    if (!p.startsWith("/")) p = "/" + p;
    File probe = SD_MMC.open(p);
    bool isDir = probe && probe.isDirectory();
    if (probe) probe.close();
    if (isDir) { wipeDir(p); SD_MMC.rmdir(p); }
    else SD_MMC.remove(p);
  }
  if (more) wipeDir(dir);   // directory had more than one batch of entries
}

// Deleting an optimised library is thousands of FAT removes. Like every heavy card
// endpoint it runs on its own task. Inline on async_tcp it would stall playback and the
// whole HTTP server, and risk the task watchdog.
static void thumbClearTask(void *) {
  if (!sdMutex || xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
    wipeDir(String(THUMB_ROOT));
    SD_MMC.rmdir(THUMB_ROOT);
    if (sdMutex) xSemaphoreGive(sdMutex);
    portENTER_CRITICAL(&s_lock);
    memset(&s_st, 0, sizeof(s_st));
    s_st.phase = NOMAD_THUMB_IDLE;
    portEXIT_CRITICAL(&s_lock);
    s_everScanned = false;
    webLog("[THUMBS] generated thumbnails cleared; the grids get full-size art again", "info");
  } else {
    webLog("[THUMBS] could not clear: the card stayed busy", "warning");
  }
  s_clearing = false;
  vTaskDelete(NULL);
}

bool nomadThumbsClear() {
  bool taken = false;
  portENTER_CRITICAL(&s_lock);
  if (!s_running && !s_clearing) { s_clearing = true; taken = true; }
  portEXIT_CRITICAL(&s_lock);
  if (!taken) return false;
  if (xTaskCreatePinnedToCore(thumbClearTask, "ThumbClr", 4096, NULL, 1, NULL, 1) != pdPASS) {
    s_clearing = false;
    return false;
  }
  return true;
}

#endif  // NOMAD_HAS_THUMBNAILER
