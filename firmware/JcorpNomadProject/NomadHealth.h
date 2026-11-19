// NomadHealth.h - "Check videos": finds the videos that will play badly, before anyone
// presses play.
//
// Only reads headers. For each MP4/M4V/MOV the library index lists: the top-level box
// headers, then only the small boxes inside the index (moov: mvhd, tkhd, hdlr, stsd),
// skipping the sample tables that make an index megabytes long. A few sector reads a file.
// MKV and AVI are judged by container alone.
//
//   POST /api/video-check            start (admin). ?fresh=1 rechecks everything.
//   GET  /api/video-check            {"state":"running","done":n,"total":m} | the last summary
//
// Results: /.system-index/video-check.ndjson, a summary line then one line per video
// (f is 0 when there is nothing to report): {"p":path,"sz":bytes,"f":flags,"v":"avc1","a":"mp4a","w":1920,"h":1080,
// "kb":kbps,"mk":index KB}. A re-run reuses every line whose file size has not changed, so
// only new or replaced videos are read again. The file browser reads the same file.
#ifndef NOMAD_HEALTH_H
#define NOMAD_HEALTH_H
#include <Arduino.h>
#include "src/AsyncWebServer/ESPAsyncWebServer.h"   // carried here, with Nomad's limits (src/AsyncWebServer/README.md)
#include <esp_heap_caps.h>
#include "NomadSD.h"

// what can be wrong with a video, the file browser uses the same bits
enum : uint16_t {
  VH_INDEX_END   = 1 << 0,   // index (moov) after the media: the browser must fetch the end first
  VH_BIG_INDEX   = 1 << 1,   // index over 2 MB with a chunk a frame: Nomad-Tools can shrink it
  VH_HEVC        = 1 << 2,   // not Firefox, many PCs
  VH_AV1         = 1 << 3,   // not older iPhones
  VH_OLD_VIDEO   = 1 << 4,   // MPEG-4 part 2 and friends: no browser
  VH_HIGH_PROF   = 1 << 5,   // H.264 High 10 / 4:2:2 / 4:4:4: browsers will not decode
  VH_DOLBY       = 1 << 6,   // AC-3 / E-AC-3 audio: silent in Chrome and Firefox
  VH_HEAVY       = 1 << 7,   // over 2.5 Mbps: stalls once a few people watch
  VH_UHD         = 1 << 8,   // wider than 1920: wasted on a phone
  VH_MKV         = 1 << 9,   // no iPhone
  VH_AVI         = 1 << 10,  // no browser
  VH_UNREADABLE  = 1 << 11,  // no index found, or not a video at all
  VH_FRAGMENTED  = 1 << 12,  // fragmented MP4: Firefox reads from the start to seek
};
static const int VH_BITS = 13;
static const char *const VH_KEYS[VH_BITS] = {
  "indexEnd", "bigIndex", "hevc", "av1", "oldVideo", "highProfile",
  "dolby", "heavy", "uhd", "mkv", "avi", "unreadable", "fragmented",
};
static const char *VH_RESULTS = "/.system-index/video-check.ndjson";
static const char *VH_RESULTS_TMP = "/.system-index/video-check.tmp";
static const uint64_t VH_BIG_INDEX_BYTES = 2ULL * 1024 * 1024;
static const uint32_t VH_HEAVY_KBPS = 2500;
// bump when a check changes: results saved under other rules are read again, not reused
static const int VH_RULES = 2;

// reading headers

// Small reads through one sector-aligned internal window: PSRAM isn't DMA-capable on the
// S3, and a misaligned read costs a command per sector. Header reads are close together,
// so most land in the window already loaded.
struct VhReader {
  NomadFile64 *f = nullptr;
  uint8_t *win = nullptr;          // 1 KB, internal DMA
  uint64_t winPos = UINT64_MAX;
  size_t winLen = 0;
  uint32_t cardReads = 0;
  bool get(uint64_t pos, uint8_t *out, size_t n) {
    if (n > 512) return false;
    if (winPos == UINT64_MAX || pos < winPos || pos + n > winPos + winLen) {
      winPos = pos & ~511ULL;
      if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
      winLen = f->readAt(winPos, win, 1024);
      if (sdMutex) xSemaphoreGive(sdMutex);
      cardReads++;
      if (pos + n > winPos + winLen) return false;
    }
    memcpy(out, win + (pos - winPos), n);
    return true;
  }
};

static inline uint32_t vhBe32(const uint8_t *b) { return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static inline uint64_t vhBe64(const uint8_t *b) { return (uint64_t)vhBe32(b) << 32 | vhBe32(b + 4); }

struct VhResult {
  uint16_t flags = 0;
  char v[5] = {0}, a[5] = {0};
  uint16_t w = 0, h = 0;
  uint32_t kbps = 0;
  uint32_t moovKB = 0;
  uint32_t vSamples = 0, vChunks = 0;   // video track: a chunk a frame is an index twice its size
};

// one box header at pos: its type, its total size, and where its payload starts
static bool vhBox(VhReader &r, uint64_t pos, uint64_t limit, uint32_t &type, uint64_t &size, uint64_t &body) {
  uint8_t h[16];
  if (pos + 8 > limit || !r.get(pos, h, 8)) return false;
  size = vhBe32(h);
  type = vhBe32(h + 4);
  body = pos + 8;
  if (size == 1) {
    if (pos + 16 > limit || !r.get(pos + 8, h + 8, 8)) return false;
    size = vhBe64(h + 8);
    body = pos + 16;
  } else if (size == 0) {
    size = limit - pos;
  }
  return size >= (body - pos) && pos + size <= limit;
}

#define VH_FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static void vhFourcc(uint32_t t, char out[5]) {
  for (int i = 0; i < 4; i++) {
    char c = (char)(t >> (24 - 8 * i));
    out[i] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? c : '?';
  }
  out[4] = 0;
}

// walk one trak: handler type, track size, first sample entry and (for H.264) its profile
static void vhTrak(VhReader &r, uint64_t pos, uint64_t end, VhResult &res) {
  uint32_t handler = 0, entry = 0;
  uint16_t w = 0, h = 0;
  uint8_t profile = 0;
  uint32_t samples = 0, chunks = 0;
  // depth-first through trak > mdia > minf > stbl, reading only the leaves we need
  uint64_t stackEnd[5];
  uint64_t p = pos;
  int depth = 0;
  stackEnd[0] = end;
  int guard = 0;
  while (guard++ < 200) {
    if (p >= stackEnd[depth]) {
      if (depth == 0) break;
      p = stackEnd[depth--];
      continue;
    }
    uint32_t t; uint64_t sz, body;
    if (!vhBox(r, p, stackEnd[depth], t, sz, body)) break;
    if ((t == VH_FOURCC('m','d','i','a') || t == VH_FOURCC('m','i','n','f') || t == VH_FOURCC('s','t','b','l')) && depth < 4) {
      stackEnd[++depth] = p + sz;
      p = body;
      continue;
    }
    uint8_t b[96];
    if (t == VH_FOURCC('t','k','h','d') && r.get(body, b, 1)) {
      // width and height are the last 8 bytes of tkhd, 16.16 fixed point
      uint8_t wh[8];
      if (sz >= 8 + 8 && r.get(p + sz - 8, wh, 8)) { w = vhBe32(wh) >> 16; h = vhBe32(wh + 4) >> 16; }
    } else if (t == VH_FOURCC('h','d','l','r') && !handler && r.get(body, b, 12)) {
      handler = vhBe32(b + 8);                     // the mdia one, a MOV has a second in minf ('alis')
    } else if (t == VH_FOURCC('s','t','s','d') && r.get(body, b, 16)) {
      entry = vhBe32(b + 12);                      // version/flags, count, entry size, format
      if (entry == VH_FOURCC('a','v','c','1') || entry == VH_FOURCC('a','v','c','3')) {
        // VisualSampleEntry is 78 bytes after its header, its child boxes follow (avcC first)
        uint64_t child = body + 8 + 8 + 78;
        uint32_t ct; uint64_t csz, cbody;
        uint8_t c[2];
        for (int k = 0; k < 4 && vhBox(r, child, p + sz, ct, csz, cbody); k++) {
          if (ct == VH_FOURCC('a','v','c','C')) { if (r.get(cbody, c, 2)) profile = c[1]; break; }
          child += csz;
        }
      }
    } else if ((t == VH_FOURCC('s','t','s','z') || t == VH_FOURCC('s','t','z','2')) && r.get(body, b, 12)) {
      samples = vhBe32(b + 8);
    } else if ((t == VH_FOURCC('s','t','c','o') || t == VH_FOURCC('c','o','6','4')) && r.get(body, b, 8)) {
      chunks = vhBe32(b + 4);
    }
    p += sz;
  }
  if (handler == VH_FOURCC('v','i','d','e') && !res.v[0]) {
    vhFourcc(entry, res.v);
    res.w = w; res.h = h;
    res.vSamples = samples; res.vChunks = chunks;
    if (entry == VH_FOURCC('h','v','c','1') || entry == VH_FOURCC('h','e','v','1')) res.flags |= VH_HEVC;
    else if (entry == VH_FOURCC('a','v','0','1')) res.flags |= VH_AV1;
    else if (entry != VH_FOURCC('a','v','c','1') && entry != VH_FOURCC('a','v','c','3')) res.flags |= VH_OLD_VIDEO;
    if (profile == 110 || profile == 122 || profile == 244) res.flags |= VH_HIGH_PROF;
    if (w > 1920) res.flags |= VH_UHD;
  } else if (handler == VH_FOURCC('s','o','u','n') && !res.a[0]) {
    vhFourcc(entry, res.a);
    if (entry == VH_FOURCC('a','c','-','3') || entry == VH_FOURCC('e','c','-','3')) res.flags |= VH_DOLBY;
  }
}

static VhResult vhCheckMp4(VhReader &r, uint64_t fileSize) {
  VhResult res;
  uint64_t p = 0, moov = 0, moovSize = 0, moovBody = 0;
  bool mdatFirst = false, haveMoov = false;
  for (int i = 0; i < 64 && p < fileSize; i++) {
    uint32_t t; uint64_t sz, body;
    if (!vhBox(r, p, fileSize, t, sz, body)) break;
    if (t == VH_FOURCC('m','d','a','t') && !haveMoov) mdatFirst = true;
    if (t == VH_FOURCC('m','o','o','v')) { haveMoov = true; moov = p; moovSize = sz; moovBody = body; break; }
    p += sz;
  }
  if (!haveMoov) { res.flags |= VH_UNREADABLE; return res; }
  if (mdatFirst) res.flags |= VH_INDEX_END;
  res.moovKB = (uint32_t)(moovSize / 1024);
  bool fragmented = false;
  uint32_t timescale = 0;
  uint64_t duration = 0;
  uint64_t q = moovBody, end = moov + moovSize;
  for (int i = 0; i < 64 && q < end; i++) {
    uint32_t t; uint64_t sz, body;
    if (!vhBox(r, q, end, t, sz, body)) break;
    uint8_t b[32];
    if (t == VH_FOURCC('m','v','h','d') && r.get(body, b, 32)) {
      if (b[0] == 1) { timescale = vhBe32(b + 20); duration = vhBe64(b + 24); }
      else { timescale = vhBe32(b + 12); duration = vhBe32(b + 16); }
    } else if (t == VH_FOURCC('t','r','a','k')) {
      vhTrak(r, body, q + sz, res);
    } else if (t == VH_FOURCC('m','v','e','x')) {
      fragmented = true;
    }
    q += sz;
  }
  // A long film's index is big however it's laid out. Only mark one written a chunk a
  // frame, Nomad-Tools' Fast start can make that one ~40% smaller.
  if (moovSize > VH_BIG_INDEX_BYTES && res.vChunks * 2 > res.vSamples) res.flags |= VH_BIG_INDEX;
  // A fragmented file's mvhd covers only the samples in moov, not the film: no bitrate.
  if (fragmented) { res.flags |= VH_FRAGMENTED; duration = 0; }
  if (timescale && duration) {
    uint64_t secs = duration / timescale;
    if (secs) res.kbps = (uint32_t)(fileSize * 8 / 1000 / secs);
    if (res.kbps > VH_HEAVY_KBPS) res.flags |= VH_HEAVY;
  }
  if (!res.v[0]) res.flags |= VH_UNREADABLE;
  return res;
}

// the job

static volatile bool g_vhRunning = false;
static volatile int g_vhDone = 0, g_vhTotal = 0;
static String g_vhSummary;                       // last summary line, or empty

static String vhJsonField(const String &line, const char *key) {
  String k = String("\"") + key + "\":";
  int i = line.indexOf(k);
  if (i < 0) return "";
  i += k.length();
  if (line[i] == '"') {
    int e = i + 1;
    while (e < (int)line.length() && !(line[e] == '"' && line[e - 1] != '\\')) e++;
    return line.substring(i + 1, e);
  }
  int e = i;
  while (e < (int)line.length() && line[e] != ',' && line[e] != '}') e++;
  return line.substring(i, e);
}

static bool vhIsVideo(const String &lower, uint16_t &containerFlag, bool &mp4) {
  containerFlag = 0; mp4 = false;
  if (lower.endsWith(".mp4") || lower.endsWith(".m4v") || lower.endsWith(".mov")) { mp4 = true; return true; }
  if (lower.endsWith(".mkv")) { containerFlag = VH_MKV; return true; }
  if (lower.endsWith(".avi")) { containerFlag = VH_AVI; return true; }
  if (lower.endsWith(".webm")) return true;
  return false;
}

static uint32_t vhHash(const String &s) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  return h;
}

// Everything big lives in PSRAM. Streaming and the card share the internal heap, and a
// big library's paths as Strings there (~100 KB) starve it until the card can't even be
// listed.
struct VhList {
  char *arena = nullptr;          // paths, NUL-separated
  uint32_t *off = nullptr;        // where each path starts in the arena
  uint64_t *size = nullptr;       // the size the index gave it
  size_t used = 0, count = 0, cap = 0, arenaCap = 0;
  bool init(size_t maxFiles, size_t arenaBytes) {
    arena = (char *)heap_caps_malloc(arenaBytes, MALLOC_CAP_SPIRAM);
    off = (uint32_t *)heap_caps_malloc(sizeof(uint32_t) * maxFiles, MALLOC_CAP_SPIRAM);
    size = (uint64_t *)heap_caps_malloc(sizeof(uint64_t) * maxFiles, MALLOC_CAP_SPIRAM);
    cap = maxFiles; arenaCap = arenaBytes;
    return arena && off && size;
  }
  bool add(const String &p, uint64_t sz) {
    if (count >= cap || used + p.length() + 1 > arenaCap) return false;
    memcpy(arena + used, p.c_str(), p.length() + 1);
    off[count] = used; size[count] = sz; count++;
    used += p.length() + 1;
    return true;
  }
  const char *path(size_t i) const { return arena + off[i]; }
  ~VhList() { heap_caps_free(arena); heap_caps_free(off); heap_caps_free(size); }
};

// read a card file through the 1 KB window, a line at a time
template <typename F>
static bool vhEachLine(const char *path, uint8_t *win, F onLine) {
  NomadFile64 f;
  bool opened = false;
  if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) { opened = f.open(path); xSemaphoreGive(sdMutex); }
  if (!opened) return false;
  uint64_t pos = 0;
  String line;
  while (true) {
    size_t n = 0;
    if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) break;
    n = f.readAt(pos, win, 1024);
    if (sdMutex) xSemaphoreGive(sdMutex);
    pos += n;
    for (size_t i = 0; i < n; i++) {
      if (win[i] != '\n') { line += (char)win[i]; continue; }
      if (line.length()) onLine(line);
      line = "";
    }
    if (n == 0) { if (line.length()) onLine(line); break; }
    vTaskDelay(0);
  }
  if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
  f.close();
  if (sdMutex) xSemaphoreGive(sdMutex);
  return true;
}

// The whole job, in its own function so everything in it is destroyed before the task exits:
// nomadTaskExit() suspends and the stack is never unwound.
static void vhRun(bool fresh) {
  const uint32_t t0 = millis();
  uint8_t *win = (uint8_t *)heap_caps_aligned_alloc(64, 1024, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!win) { g_vhSummary = "{\"_type\":\"summary\",\"error\":\"no memory\"}"; return; }

  // previous results: path hash + size -> the old line, so a re-run only reads what changed
  VhList prev;
  if (!fresh && prev.init(8192, 1536 * 1024)) {
    bool sameRules = false;            // the summary comes first and says which rules wrote it
    vhEachLine(VH_RESULTS, win, [&](const String &line) {
      if (line.indexOf("\"_type\":\"summary\"") >= 0) { sameRules = vhJsonField(line, "rules").toInt() == VH_RULES; return; }
      if (!sameRules) return;
      String p = vhJsonField(line, "p");
      if (p.length()) prev.add(line, (uint64_t)vhHash(p) << 32 | (uint32_t)vhJsonField(line, "sz").toDouble());
    });
  }

  // the videos the library index lists: no directory walk, the indexes already have them
  VhList videos;
  String bucketNote;
  if (videos.init(8192, 1024 * 1024)) {
    const char *buckets[] = { "Movies", "Shows", "Files", "Gallery" };
    for (const char *b : buckets) {
      int lines = 0, found = 0;
      String path = String(INDEX_DIR) + "/" + b + ".index.ndjson";
      const bool opened = vhEachLine(path.c_str(), win, [&](const String &line) {
        lines++;
        if (line.indexOf("\"t\":\"f\"") < 0) return;
        String p = vhJsonField(line, "p"), lower = p;
        lower.toLowerCase();
        uint16_t cf; bool mp4;
        if (vhIsVideo(lower, cf, mp4) && videos.add(p, (uint64_t)vhJsonField(line, "sz").toDouble())) found++;
      });
      bucketNote += String(bucketNote.length() ? "," : "") + "\"" + b + "\":[" + String(opened ? 1 : 0) + "," + String(lines) + "," + String(found) + "]";
    }
  }
  g_vhTotal = videos.count;

  File out;
  if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
  SD_MMC.remove(VH_RESULTS_TMP);
  out = SD_MMC.open(VH_RESULTS_TMP, FILE_WRITE);
  if (sdMutex) xSemaphoreGive(sdMutex);

  // output collects in PSRAM and goes to the card 12 KB at a time
  const size_t OUT_CAP = 16 * 1024;
  char *outBuf = (char *)heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM);
  size_t outLen = 0;
  auto flush = [&]() {
    if (!outLen) return;
    if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
    out.write((const uint8_t *)outBuf, outLen);
    if (sdMutex) xSemaphoreGive(sdMutex);
    outLen = 0;
  };

  uint32_t counts[VH_BITS] = {0};
  uint32_t checked = 0, reused = 0, flagged = 0, cardReads = 0, slowest = 0;
  for (size_t i = 0; out && outBuf && i < videos.count; i++) {
    const String p = videos.path(i);
    const uint64_t sz = videos.size[i];
    String line;
    const uint64_t key = (uint64_t)vhHash(p) << 32 | (uint32_t)sz;
    for (size_t k = 0; k < prev.count; k++) {
      if (prev.size[k] != key) continue;
      String old = prev.path(k);
      if (vhJsonField(old, "p") == p && (uint64_t)vhJsonField(old, "sz").toDouble() == sz) { line = old; reused++; }
      break;
    }
    if (!line.length()) {
      String lower = p; lower.toLowerCase();
      uint16_t cf; bool mp4;
      vhIsVideo(lower, cf, mp4);
      VhResult res;
      res.flags = cf;
      if (mp4) {
        const uint32_t s0 = millis();
        NomadFile64 f;
        bool opened = false;
        if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) == pdTRUE) { opened = f.open(p.c_str()); xSemaphoreGive(sdMutex); }
        if (opened) {
          VhReader r; r.f = &f; r.win = win;
          res = vhCheckMp4(r, f.size());
          res.flags |= cf;
          cardReads += r.cardReads;
          if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
          f.close();
          if (sdMutex) xSemaphoreGive(sdMutex);
        } else {
          res.flags |= VH_UNREADABLE;
        }
        const uint32_t took = millis() - s0;
        if (took > slowest) slowest = took;
      }
      checked++;
      line = "{\"p\":\"" + escapeJsonString(p) + "\",\"sz\":" + String((unsigned long long)sz) + ",\"f\":" + String(res.flags);
      if (res.v[0]) line += String(",\"v\":\"") + res.v + "\"";
      if (res.a[0]) line += String(",\"a\":\"") + res.a + "\"";
      if (res.w) line += ",\"w\":" + String(res.w) + ",\"h\":" + String(res.h);
      if (res.kbps) line += ",\"kb\":" + String(res.kbps);
      if (res.moovKB) line += ",\"mk\":" + String(res.moovKB);
      line += "}";
    }
    const uint16_t flags = (uint16_t)vhJsonField(line, "f").toInt();
    if (flags) {
      flagged++;
      for (int b = 0; b < VH_BITS; b++) if (flags & (1 << b)) counts[b]++;
    }
    if (outLen + line.length() + 1 > OUT_CAP) flush();
    memcpy(outBuf + outLen, line.c_str(), line.length());
    outLen += line.length();
    outBuf[outLen++] = '\n';
    g_vhDone = i + 1;
    // someone watching gets the card first
    vTaskDelay(mediaStreamingActive ? pdMS_TO_TICKS(50) : 1);
  }
  flush();
  heap_caps_free(outBuf);

  const uint32_t ms = millis() - t0;
  String sum = "{\"_type\":\"summary\",\"rules\":" + String(VH_RULES) + ",\"videos\":" + String(videos.count) + ",\"checked\":" + String(checked) +
               ",\"reused\":" + String(reused) + ",\"flagged\":" + String(flagged) + ",\"ms\":" + String(ms) +
               ",\"cardReads\":" + String(cardReads) + ",\"slowestMs\":" + String(slowest) +
               ",\"prev\":" + String(prev.count) + ",\"buckets\":{" + bucketNote + "},\"counts\":{";
  for (int b = 0; b < VH_BITS; b++) sum += String(b ? "," : "") + "\"" + VH_KEYS[b] + "\":" + String(counts[b]);
  sum += "}}";
  if (out) {
    if (sdMutex) xSemaphoreTake(sdMutex, portMAX_DELAY);
    out.close();
    // summary first: the real file is the summary line then the body
    File src = SD_MMC.open(VH_RESULTS_TMP, FILE_READ);
    SD_MMC.remove(VH_RESULTS);
    File dst = SD_MMC.open(VH_RESULTS, FILE_WRITE);
    if (src && dst) {
      dst.print(sum + "\n");
      while (true) { size_t n = src.read(win, 1024); if (!n) break; dst.write(win, n); }
    }
    if (src) src.close();
    if (dst) dst.close();
    SD_MMC.remove(VH_RESULTS_TMP);
    if (sdMutex) xSemaphoreGive(sdMutex);
  }
  heap_caps_free(win);
  g_vhSummary = sum;
  webLogf("success", "Video scan: %u videos, %u with issues (%lus)", (unsigned)videos.count, (unsigned)flagged, (unsigned long)(ms / 1000));
}

static void vhTask(void *arg) {
  vhRun((bool)(intptr_t)arg);
  g_vhRunning = false;
  nomadTaskExit();
}

static void nomadHealthRegister(AsyncWebServer &server) {
  server.on("/api/video-check", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (g_vhRunning) {
      r->send(200, "application/json", "{\"state\":\"running\",\"done\":" + String(g_vhDone) + ",\"total\":" + String(g_vhTotal) + "}");
      return;
    }
    if (!g_vhSummary.length()) {
      // after a restart: the summary is the results file's first line
      File f;
      if (sdMutex && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        f = SD_MMC.open(VH_RESULTS, FILE_READ);
        if (f) { g_vhSummary = f.readStringUntil('\n'); f.close(); }
        xSemaphoreGive(sdMutex);
      }
    }
    String s = g_vhSummary.length() ? g_vhSummary : String("{\"_type\":\"summary\",\"videos\":0}");
    r->send(200, "application/json", "{\"state\":\"" + String(g_vhSummary.length() ? "done" : "never") + "\"," + s.substring(1));
  });
  server.on("/api/video-check", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!checkAdminAuth(r)) { r->send(401, "application/json", "{\"error\":\"Unauthorized\"}"); return; }
    if (g_vhRunning) { r->send(409, "application/json", "{\"error\":\"Already scanning\"}"); return; }
    if (indexingInProgress || indexingTasksActive || sdScanInProgress) {
      r->send(409, "application/json", "{\"error\":\"Wait for the scan to finish\"}");
      return;
    }
    const bool fresh = r->hasParam("fresh") || r->hasParam("fresh", true);
    g_vhRunning = true; g_vhDone = 0; g_vhTotal = 0;
    if (nomadCreateTask(vhTask, "VideoCheck", 12 * 1024, (void *)(intptr_t)fresh, 1, NULL, 1) != pdPASS) {
      g_vhRunning = false;
      r->send(503, "application/json", "{\"error\":\"no task\"}");
      return;
    }
    r->send(202, "application/json", "{\"state\":\"running\"}");
  });
}

#endif
