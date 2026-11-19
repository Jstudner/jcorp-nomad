// NomadZimCore.h - reads pages out of a ZIM (Kiwix archive) on Nomad itself, so the
// archive reader works on every phone over plain http, scripts and all. Kiwix's own
// hotspots do the same with kiwix-serve. Without a server answering a page's requests,
// a browser can only do it through a service worker, and those need a secure page.
//
// No Arduino in here, so the same code builds on a PC and can be tested against real
// archives. The firmware side is NomadZimServe.h, it supplies the card reads and the
// big allocations through nzim::hooks.
//
//   Archive   header, mime list, URL lookup (binary search over the URL pointer list),
//             redirects, main page, where an uncompressed blob sits on the card
//   Clusters  zstd clusters unpacked into PSRAM only as far as the blob asked for, and
//             picked up from there for the next blob of the same cluster
//
// Memory: the archives' zstd frames all declare an 8 MB window, which a streaming decoder
// would allocate. With ZSTD_d_stableOutBuffer the output buffer is the window, so a
// cluster (~2 MB) costs its own size plus ~230 KB of decoder.
#ifndef NOMAD_ZIM_CORE_H
#define NOMAD_ZIM_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <string>
#include <vector>

#define ZSTD_STATIC_LINKING_ONLY
#if __has_include("src/zstd/zstd.h")
#include "src/zstd/zstd.h"
#else
#include <zstd.h>
#endif

namespace nzim {

struct Hooks {
  // read [off, off+n) of one part file, false on a card error or a busy card
  bool (*read)(void *file, uint64_t off, uint8_t *buf, size_t n);
  void *(*bigAlloc)(size_t n);   // PSRAM on Nomad
  void (*bigFree)(void *p);
  size_t (*largestFree)();       // biggest single allocation bigAlloc could make now
  uint64_t (*nowUs)();           // optional: a clock, for the time counters below
  // optional: one whole xz stream into out (archives from before 2021). Returns bytes
  // written; *err 0 done, 1 out too small, 2 broken. Without it xz clusters are BAD
  size_t (*xzDecode)(const uint8_t *in, size_t inLen, uint8_t *out, size_t outCap, int *err);
};
inline Hooks hooks = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};

enum { OK = 1, NOT_FOUND = 0, IO_ERROR = -1, TOO_BIG = -2, BAD = -3, BUSY = -4 };

static inline uint16_t u16(const uint8_t *b) { return b[0] | (b[1] << 8); }
static inline uint32_t u32(const uint8_t *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
static inline uint64_t u64(const uint8_t *b) { return u32(b) | ((uint64_t)u32(b + 4) << 32); }

struct Dirent {
  uint16_t mime = 0;
  char ns = 0;
  bool redirect = false;
  uint32_t target = 0;           // redirect: URL index
  uint32_t cluster = 0, blob = 0;
  std::string url;
};

struct Part { void *file; uint64_t size; };

class Archive {
public:
  std::vector<Part> parts;
  uint64_t total = 0;
  uint32_t entryCount = 0, clusterCount = 0, mainPage = 0xFFFFFFFF;
  uint64_t urlPtrPos = 0, clusterPtrPos = 0, mimeListPos = 0, checksumPos = 0;
  uint16_t major = 0;

  ~Archive() {
    if (_cache) hooks.bigFree(_cache);
    if (_mimes) hooks.bigFree(_mimes);
  }

  int open() {
    total = 0;
    for (auto &p : parts) total += p.size;
    uint8_t h[80];
    if (!read(0, h, 80)) return IO_ERROR;
    if (u32(h) != 0x044D495A) return BAD;
    major = u16(h + 4);
    entryCount = u32(h + 24); clusterCount = u32(h + 28);
    urlPtrPos = u64(h + 32); clusterPtrPos = u64(h + 48); mimeListPos = u64(h + 56);
    mainPage = u32(h + 64); checksumPos = u64(h + 72);
    // the mime list as it is in the file (NUL-separated), kept in PSRAM with an index
    // beside it. As separate strings it would sit in internal RAM, which SD and WiFi need
    size_t n = 2048;
    if (mimeListPos >= total) return BAD;
    if (mimeListPos + n > total) n = total - mimeListPos;
    if (_mimes) { hooks.bigFree(_mimes); _mimes = nullptr; }
    _mimeCount = 0;
    if (hooks.bigAlloc) _mimes = (char *)hooks.bigAlloc(n + 1 + sizeof(uint16_t) * 256);
    if (!_mimes) return TOO_BIG;
    if (!read(mimeListPos, (uint8_t *)_mimes, n)) return IO_ERROR;
    _mimes[n] = 0;
    uint16_t *at = (uint16_t *)(_mimes + ((n + 2) & ~(size_t)1));
    for (size_t p = 0; p < n && _mimes[p] && _mimeCount < 255;) {
      size_t e = p;
      while (e < n && _mimes[e]) e++;
      if (e >= n) break;
      at[_mimeCount++] = (uint16_t)p;
      p = e + 1;
    }
    _mimeAt = at;
    // the first steps of every binary search land on the same entries: keep them
    if (!_cache && hooks.bigAlloc) {
      _cache = (CacheSlot *)hooks.bigAlloc(sizeof(CacheSlot) * CACHE_SLOTS);
      if (_cache) for (int i = 0; i < CACHE_SLOTS; i++) _cache[i].idx = 0xFFFFFFFF;
    }
    return OK;
  }

  // a read that may cross from one part (.zimaa) into the next (.zimab)
  bool read(uint64_t off, uint8_t *buf, size_t n) {
    uint64_t base = 0;
    for (auto &p : parts) {
      if (n == 0) break;
      if (off < base + p.size) {
        uint64_t local = off - base;
        size_t take = (size_t)((p.size - local) < n ? (p.size - local) : n);
        if (!hooks.read(p.file, local, buf, take)) return false;
        buf += take; off += take; n -= take;
      }
      base += p.size;
    }
    return n == 0;
  }

  const char *mimeOf(const Dirent &d) const {
    return d.mime < _mimeCount ? _mimes + _mimeAt[d.mime] : "application/octet-stream";
  }

  int direntAt(uint64_t off, Dirent &d) {
    size_t want = 512;
    for (int attempt = 0; attempt < 3; attempt++, want *= 4) {
      if (off >= total) return BAD;   // a damaged pointer: total - off would wrap to about 4 GB
      size_t n = want;
      if (off + n > total) n = (size_t)(total - off);
      std::vector<uint8_t> b(n);
      if (!read(off, b.data(), n)) return IO_ERROR;
      if (n < 12) return BAD;
      d.mime = u16(b.data());
      d.redirect = d.mime == 0xFFFF;
      d.ns = (char)b[3];
      size_t at = d.redirect ? 12 : 16;
      if (at > n) return BAD;
      size_t e = at;
      while (e < n && b[e]) e++;
      if (e >= n) { if (n < want) return BAD; continue; }   // a long URL: read more
      d.url.assign((const char *)b.data() + at, e - at);
      if (d.redirect) d.target = u32(b.data() + 8);
      else { d.cluster = u32(b.data() + 8); d.blob = u32(b.data() + 12); }
      return OK;
    }
    return BAD;
  }

  int direntByIndex(uint32_t i, Dirent &d) {
    uint8_t p[8];
    if (!read(urlPtrPos + (uint64_t)i * 8, p, 8)) return IO_ERROR;
    return direntAt(u64(p), d);
  }

  int findByUrl(char ns, const std::string &url, Dirent &out) {
    int64_t lo = 0, hi = (int64_t)entryCount - 1;
    while (lo <= hi) {
      uint32_t mid = (uint32_t)((lo + hi) >> 1);
      char ens; std::string eurl;
      bool hit = cacheGet(mid, ens, eurl);
      Dirent d;
      if (!hit) {
        int r = direntByIndex(mid, d);
        if (r != OK) return r;
        ens = d.ns; eurl = d.url;
        cachePut(mid, ens, eurl);
      }
      int c = ens < ns ? -1 : ens > ns ? 1 : cmp(eurl, url);
      if (c == 0) {
        if (hit) { int r = direntByIndex(mid, d); if (r != OK) return r; }
        out = d;
        return OK;
      }
      if (c < 0) lo = mid + 1; else hi = (int64_t)mid - 1;
    }
    return NOT_FOUND;
  }

  int resolveRedirect(Dirent &d) {
    for (int depth = 0; d.redirect && depth < 8; depth++) {
      uint32_t t = d.target;
      int r = direntByIndex(t, d);
      if (r != OK) return r;
    }
    return d.redirect ? BAD : OK;
  }

  int mainPageEntry(Dirent &d) {
    int r;
    if (mainPage != 0xFFFFFFFF) r = direntByIndex(mainPage, d);
    else {
      r = findByUrl('W', "mainPage", d);
      if (r == NOT_FOUND) r = findByUrl('C', "index.html", d);
    }
    return r == OK ? resolveRedirect(d) : r;
  }

  // where cluster c starts on the card, how long it is, and its info byte
  int clusterExtent(uint32_t c, uint64_t &off, uint64_t &size, uint8_t &info) {
    if (c >= clusterCount) return BAD;
    uint8_t p[16];
    bool last = c + 1 >= clusterCount;
    if (!read(clusterPtrPos + (uint64_t)c * 8, p, last ? 8 : 16)) return IO_ERROR;
    off = u64(p);
    uint64_t next = last ? (checksumPos ? checksumPos : total) : u64(p + 8);
    if (next < off) return BAD;
    size = next - off;
    if (!read(off, &info, 1)) return IO_ERROR;
    return OK;
  }

  // an uncompressed cluster's blob is just bytes on the card: where they are.
  // NOT_FOUND means the cluster is compressed and has to go through Clusters
  int rawBlob(uint32_t c, uint32_t b, uint64_t &off, uint64_t &size) {
    uint64_t coff, csize; uint8_t info;
    int r = clusterExtent(c, coff, csize, info);
    if (r != OK) return r;
    int comp = info & 0x0F;
    if (comp != 0 && comp != 1) return NOT_FOUND;
    size_t os = (info & 0x10) ? 8 : 4;
    uint8_t t[16];
    if (!read(coff + 1 + (uint64_t)b * os, t, os * 2)) return IO_ERROR;
    uint64_t s = os == 8 ? u64(t) : u32(t), e = os == 8 ? u64(t + 8) : u32(t + os);
    if (e < s || 1 + e > csize) return BAD;
    off = coff + 1 + s; size = e - s;
    return OK;
  }

  static int cmp(const std::string &a, const std::string &b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    int c = memcmp(a.data(), b.data(), n);
    if (c) return c;
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
  }

private:
  char *_mimes = nullptr;
  uint16_t *_mimeAt = nullptr;
  unsigned _mimeCount = 0;
  // binary search steps already taken, by URL index: 512 x 128 bytes in PSRAM
  static const int CACHE_SLOTS = 512;
  struct CacheSlot { uint32_t idx; char ns; uint8_t len; char url[122]; };
  CacheSlot *_cache = nullptr;
  bool cacheGet(uint32_t idx, char &ns, std::string &url) {
    if (!_cache) return false;
    CacheSlot &s = _cache[idx % CACHE_SLOTS];
    if (s.idx != idx) return false;
    ns = s.ns; url.assign(s.url, s.len);
    return true;
  }
  void cachePut(uint32_t idx, char ns, const std::string &url) {
    if (!_cache || url.size() > sizeof(_cache[0].url)) return;
    CacheSlot &s = _cache[idx % CACHE_SLOTS];
    s.idx = idx; s.ns = ns; s.len = (uint8_t)url.size(); memcpy(s.url, url.data(), url.size());
  }
};

// Unpacked clusters. Not thread-safe: the firmware calls it from one task only.
//
// They live in an arena of 64 KB pages (claim() at boot, or on first use), a run of pages
// per cluster. PSRAM that has had 2 MB buffers come and go gets fragmented, and then a
// cluster has nowhere to go even with plenty free in total. Run size depends on the cluster:
//   - archives prepared by Nomad-Tools (zim_prepare.py) have ~128 KB clusters that say how
//     big they are unpacked: exactly that, unpacked whole in one go, and dozens fit
//   - any other zstd cluster (~2 MB, size unknown until unpacked): a 2.3 MB run, unpacked
//     only as far as asked and picked up from there, so it keeps its own decoder (two at most)
//   - xz clusters (archives from before 2021): unpacked whole, xz can't stop part way
// A cluster bigger than the arena (PhET's simulations) gets its own buffer while there's
// room, letting the arena go if it has to. release() hands everything back.
class Clusters {
public:
  struct Slot;
  struct Ctx {                                  // a zstd decoder and its input buffer
    ZSTD_DCtx *d = nullptr;
    uint8_t *in = nullptr;
    Slot *owner = nullptr;                      // a partly unpacked cluster still using it
  };
  struct Slot {
    Archive *a = nullptr;                       // nullptr: holds nothing
    uint32_t c = 0;
    uint8_t *buf = nullptr; size_t cap = 0;     // the output (and zstd's window)
    int page0 = -1, pages = 0;                  // where in the arena, page0 -1: its own buffer
    Ctx *ctx = nullptr;                         // set while partly unpacked
    ZSTD_outBuffer ob{};
    ZSTD_inBuffer ib{};
    uint64_t cardAt = 0, cardEnd = 0;           // the rest of the cluster on the card
    bool done = false;                          // the whole cluster is unpacked
    size_t os = 4;
    uint32_t used = 0;                          // for least-recently-used
    int pins = 0;                               // responses still sending from buf
  };

  static const size_t IN_CHUNK = 16384;
  static const size_t PAGE = 64 * 1024;
  static const size_t LEGACY = 2304 * 1024;     // libzim closes a cluster past 2 MB
  static const size_t WHOLE_MAX = 1024 * 1024;  // known size up to this: unpack it all at once
  static const int STREAMS = 2;
  size_t arenaBytes = 2 * LEGACY;               // 72 pages
  size_t leave = 1u << 20;                      // never take the last of PSRAM...
  size_t leaveBig = 384u << 10;                 // ...an oversize cluster's buffer is brief
  // where the time goes (with hooks.nowUs): bytes unpacked, card bytes read, and the
  // microseconds spent in each. evictions: clusters dropped to make room
  uint64_t outBytes = 0, cardBytes = 0, zstdUs = 0, readUs = 0, evictions = 0;

  explicit Clusters(int slots) : _slots(slots) {}
  ~Clusters() { release(true); }

  size_t arenaSize() const { return _arena ? _pages * PAGE : 0; }
  size_t bigSize() const {
    size_t n = 0;
    for (auto &s : _slots) if (s.a && s.page0 < 0) n += s.cap;
    return n;
  }
  int held() const { int n = 0; for (auto &s : _slots) if (s.a) n++; return n; }

  // take the arena now (at boot, while PSRAM is in one piece) rather than on first use
  bool claim() { return ensureArena(); }

  // blob b of cluster c: data points into a slot, valid while the slot is pinned (it
  // is, on OK. unpin() when the last byte is out)
  int blob(Archive &a, uint32_t c, uint32_t b, const uint8_t *&data, size_t &len, Slot *&slot) {
    slot = nullptr;
    Slot *s = find(a, c);
    size_t need = 0;                            // a bigger buffer than the first guess
    for (int attempt = 0; attempt < 3; attempt++) {
      if (!s) {
        int r = open(a, c, need, s);
        if (r != OK) return r;
      }
      s->used = ++_tick;
      // the offset table first, then as far as this blob ends
      int r = unpackTo(*s, (size_t)(b + 2) * s->os);
      if (r == TOO_BIG && !s->pins) { need = s->cap * 2; drop(*s); s = nullptr; continue; }
      if (r != OK) { drop(*s); return r; }
      const uint8_t *t = s->buf;
      uint64_t first = s->os == 8 ? u64(t) : u32(t);
      if (first % s->os || (uint64_t)(b + 2) * s->os > first) { drop(*s); return BAD; }
      uint64_t st = s->os == 8 ? u64(t + (size_t)b * 8) : u32(t + (size_t)b * 4);
      uint64_t en = s->os == 8 ? u64(t + (size_t)(b + 1) * 8) : u32(t + (size_t)(b + 1) * 4);
      if (en < st) { drop(*s); return BAD; }
      if (en > s->cap) {
        // past the buffer (one big script in an unprepared archive): again, with room for it
        if (s->pins) return BUSY;
        need = (size_t)en + 4096;
        drop(*s); s = nullptr;
        continue;
      }
      r = unpackTo(*s, (size_t)en);
      if (r != OK) { drop(*s); return r; }
      data = s->buf + st; len = (size_t)(en - st);
      s->pins++;
      slot = s;
      return OK;
    }
    return TOO_BIG;
  }

  void unpin(Slot *s) { if (s && s->pins > 0) s->pins--; }

  bool pinned() const { for (auto &s : _slots) if (s.pins) return true; return false; }

  // hand every buffer back. Not while anything is still sending, unless the whole thing
  // is going away
  bool release(bool force = false) {
    if (!force && pinned()) return false;
    for (auto &s : _slots) { s.pins = 0; drop(s); }
    for (Ctx *x : allCtx()) {
      if (x->d) ZSTD_freeDCtx(x->d);
      if (x->in) hooks.bigFree(x->in);
      *x = Ctx();
    }
    if (_arena) hooks.bigFree(_arena);
    if (_owner) hooks.bigFree(_owner);
    _arena = nullptr; _pages = 0; _owner = nullptr;
    return true;
  }
  bool holding() const { return _arena || _whole.d || bigSize(); }

  // let go of every cluster nothing is sending from
  void trim() { for (auto &s : _slots) drop(s); }

  // forget an archive's clusters (the card went away, the archive closed)
  void forget(Archive *a) { for (auto &s : _slots) if (s.a == a) drop(s); }

private:
  std::vector<Slot> _slots;
  uint32_t _tick = 0;
  uint8_t *_arena = nullptr;
  int _pages = 0;
  int16_t *_owner = nullptr;                    // per page: slot index, or -1 (PSRAM)
  Ctx _whole;                                   // for clusters unpacked in one go
  Ctx _stream[STREAMS];                         // for partly unpacked ones

  std::vector<Ctx *> allCtx() {
    std::vector<Ctx *> v{&_whole};
    for (auto &x : _stream) v.push_back(&x);
    return v;
  }

  static void *zalloc(void *, size_t n) { return hooks.bigAlloc(n); }
  static void zfree(void *, void *p) { if (p) hooks.bigFree(p); }

  size_t room(size_t keep) const {
    if (!hooks.largestFree) return (size_t)-1;
    size_t largest = hooks.largestFree();
    return largest > keep ? largest - keep : 0;
  }

  bool readyCtx(Ctx &x) {
    if (!x.in) x.in = (uint8_t *)hooks.bigAlloc(IN_CHUNK);
    if (!x.d) {
      ZSTD_customMem cm = {zalloc, zfree, nullptr};
      x.d = ZSTD_createDCtx_advanced(cm);
      if (x.d) {
        ZSTD_DCtx_setParameter(x.d, ZSTD_d_stableOutBuffer, 1);
        ZSTD_DCtx_setParameter(x.d, ZSTD_d_windowLogMax, 27);
      }
    }
    return x.in && x.d;
  }

  bool ensureArena() {
    if (_arena) return true;
    size_t want = arenaBytes - arenaBytes % PAGE;
    if (room(leave) < want) return false;
    _arena = (uint8_t *)hooks.bigAlloc(want);
    if (!_arena) return false;
    _pages = (int)(want / PAGE);
    _owner = (int16_t *)hooks.bigAlloc(sizeof(int16_t) * _pages);
    if (!_owner) { hooks.bigFree(_arena); _arena = nullptr; _pages = 0; return false; }
    for (int i = 0; i < _pages; i++) _owner[i] = -1;
    return true;
  }

  Slot *find(Archive &a, uint32_t c) {
    for (auto &s : _slots) if (s.a == &a && s.c == c) return &s;
    return nullptr;
  }

  // the least recently used cluster nothing is sending from (and not `keep`)
  Slot *victim(Slot *keep) {
    Slot *best = nullptr;
    for (auto &s : _slots)
      if (s.a && !s.pins && &s != keep && (!best || s.used < best->used)) best = &s;
    return best;
  }

  int freeRun(int n) {
    for (int i = 0; i + n <= _pages; i++) {
      int k = 0;
      while (k < n && _owner[i + k] < 0) k++;
      if (k == n) return i;
      i += k;
    }
    return -1;
  }

  // a buffer of `need` for slot s: a run of pages, or (bigger than the arena) its own
  int place(Slot &s, size_t need) {
    int npages = (int)((need + PAGE - 1) / PAGE);
    if (ensureArena() && npages <= _pages) {
      for (;;) {
        int at = freeRun(npages);
        if (at >= 0) {
          for (int k = 0; k < npages; k++) _owner[at + k] = (int16_t)(&s - &_slots[0]);
          s.page0 = at; s.pages = npages; s.buf = _arena + (size_t)at * PAGE; s.cap = (size_t)npages * PAGE;
          return OK;
        }
        Slot *v = victim(&s);
        if (!v) return BUSY;                    // everything is still sending: ask again
        drop(*v); evictions++;
      }
    }
    // its own buffer, the arena goes first if there's no room beside it
    size_t want = need + 4096;
    if (room(leaveBig) < want) {
      for (auto &o : _slots) if (o.pins && &o != &s) return BUSY;
      for (auto &o : _slots) if (&o != &s) drop(o);
      // the arena goes too, as a last resort: getting it back may not be possible later
      // (PSRAM fills in around it), so this is only for a cluster nothing else can hold
      if (_arena) { hooks.bigFree(_arena); _arena = nullptr; _pages = 0; }
      if (_owner) { hooks.bigFree(_owner); _owner = nullptr; }
      for (auto &x : _stream) {                 // their decoders too
        if (x.d) { ZSTD_freeDCtx(x.d); x.d = nullptr; }
        if (x.in) { hooks.bigFree(x.in); x.in = nullptr; }
      }
      if (room(leaveBig) < want) return TOO_BIG;
    }
    s.buf = (uint8_t *)hooks.bigAlloc(want);
    if (!s.buf) return TOO_BIG;
    s.page0 = -1; s.pages = 0; s.cap = want;
    return OK;
  }

  void drop(Slot &s) {
    if (s.pins || !s.a) { if (!s.a) s = Slot(); return; }
    if (s.page0 >= 0) { for (int k = 0; k < s.pages; k++) _owner[s.page0 + k] = -1; }
    else if (s.buf) hooks.bigFree(s.buf);
    if (s.ctx) s.ctx->owner = nullptr;
    s = Slot();
  }

  // a decoder for a cluster unpacked part way: a free one, or one taken from the least
  // recently used partly unpacked cluster (dropped, or finished if something sends from it)
  Ctx *streamCtx(Slot &s) {
    Ctx *pick = nullptr;
    for (auto &x : _stream) if (!x.owner) { pick = &x; break; }
    if (!pick) {
      Ctx *lru = nullptr;
      for (auto &x : _stream) if (x.owner != &s && (!lru || x.owner->used < lru->owner->used)) lru = &x;
      if (!lru) return nullptr;
      Slot &o = *lru->owner;
      if (o.pins) { if (unpackTo(o, o.cap) != OK) { o.ctx = nullptr; lru->owner = nullptr; } }
      else drop(o);
      pick = lru;
    }
    if (!readyCtx(*pick)) return nullptr;
    pick->owner = &s;
    return pick;
  }

  // take a slot for cluster c, place its buffer, and unpack it whole if it can be
  int open(Archive &a, uint32_t c, size_t need, Slot *&out) {
    out = nullptr;
    uint64_t off, size; uint8_t info;
    int r = a.clusterExtent(c, off, size, info);
    if (r != OK) return r;
    int comp = info & 0x0F;
    if (comp != 5 && comp != 4) return BAD;     // uncompressed goes by rawBlob, zlib/bzip2: pre-2010
    // a slot that holds nothing, or the least recently used one
    Slot *s = nullptr;
    for (auto &x : _slots) if (!x.a) { s = &x; break; }
    if (!s) { s = victim(nullptr); if (!s) return BUSY; drop(*s); evictions++; }
    s->a = &a; s->c = c; s->os = (info & 0x10) ? 8 : 4;
    s->cardAt = off + 1; s->cardEnd = off + size;
    s->used = ++_tick;
    size_t known = 0;
    if (comp == 5) {
      uint8_t hdr[18];
      size_t hn = size - 1 < sizeof(hdr) ? (size_t)(size - 1) : sizeof(hdr);
      if (!a.read(off + 1, hdr, hn)) { drop(*s); return IO_ERROR; }
      unsigned long long fcs = ZSTD_getFrameContentSize(hdr, hn);
      if (fcs != ZSTD_CONTENTSIZE_UNKNOWN && fcs != ZSTD_CONTENTSIZE_ERROR) known = (size_t)fcs;
    }
    size_t want = need ? need : known ? known : LEGACY;
    if (known && want < known) want = known;
    r = place(*s, want);
    if (r != OK) { drop(*s); return r; }
    if (comp == 4) {
      r = unpackXz(*s, size - 1);
      if (r == OK) { out = s; return OK; }
      drop(*s);
      // xz doesn't say how big it unpacks: twice the room, while that's sensible
      if (r == TOO_BIG && want < (32u << 20)) return open(a, c, want * 2, out);
      return r;
    }
    bool whole = known && known <= WHOLE_MAX;
    Ctx *x = whole ? &_whole : streamCtx(*s);
    if (!x || !readyCtx(*x)) { drop(*s); return TOO_BIG; }
    ZSTD_DCtx_reset(x->d, ZSTD_reset_session_only);
    s->ctx = x; x->owner = s;
    s->ob = {s->buf, s->cap, 0};
    s->ib = {x->in, 0, 0};
    s->done = false;
    if (whole) {
      r = unpackTo(*s, s->cap);
      s->ctx = nullptr; _whole.owner = nullptr;
      if (r != OK || !s->done) { drop(*s); return r != OK ? r : BAD; }
    }
    out = s;
    return OK;
  }

  // a whole xz cluster: its compressed bytes into a scratch buffer, then one call
  int unpackXz(Slot &s, uint64_t inLen) {
    if (!hooks.xzDecode) return BAD;
    uint8_t *in = (uint8_t *)hooks.bigAlloc((size_t)inLen);
    if (!in) return TOO_BIG;
    uint64_t t0 = hooks.nowUs ? hooks.nowUs() : 0;
    bool ok = s.a->read(s.cardAt, in, (size_t)inLen);
    if (hooks.nowUs) readUs += hooks.nowUs() - t0;
    cardBytes += inLen;
    if (!ok) { hooks.bigFree(in); return IO_ERROR; }
    int err = 2;
    t0 = hooks.nowUs ? hooks.nowUs() : 0;
    size_t n = hooks.xzDecode(in, (size_t)inLen, s.buf, s.cap, &err);
    if (hooks.nowUs) zstdUs += hooks.nowUs() - t0;
    hooks.bigFree(in);
    if (err == 1) return TOO_BIG;
    if (err != 0) return BAD;
    outBytes += n;
    s.ob = {s.buf, s.cap, n};
    s.done = true;
    s.cardAt = s.cardEnd;
    return OK;
  }

  // unpack until at least `need` bytes are out (or the cluster ends)
  int unpackTo(Slot &s, size_t need) {
    if (need > s.cap) need = s.cap;
    while (s.ob.pos < need && !s.done) {
      if (!s.ctx) return BAD;                   // finished early, or lost its decoder
      if (s.ib.pos >= s.ib.size) {
        if (s.cardAt >= s.cardEnd) return BAD;   // ran out of cluster before the frame ended
        size_t n = IN_CHUNK;
        if (s.cardAt + n > s.cardEnd) n = (size_t)(s.cardEnd - s.cardAt);
        uint64_t t0 = hooks.nowUs ? hooks.nowUs() : 0;
        if (!s.a->read(s.cardAt, s.ctx->in, n)) return IO_ERROR;
        if (hooks.nowUs) readUs += hooks.nowUs() - t0;
        cardBytes += n;
        s.cardAt += n;
        s.ib = {s.ctx->in, n, 0};
      }
      uint64_t t0 = hooks.nowUs ? hooks.nowUs() : 0;
      size_t before = s.ob.pos;
      size_t r = ZSTD_decompressStream(s.ctx->d, &s.ob, &s.ib);
      if (hooks.nowUs) zstdUs += hooks.nowUs() - t0;
      outBytes += s.ob.pos - before;
      if (ZSTD_isError(r)) return ZSTD_getErrorCode(r) == ZSTD_error_dstSize_tooSmall ? TOO_BIG : BAD;
      if (r == 0) {
        s.done = true;
        if (s.ctx != &_whole) { s.ctx->owner = nullptr; s.ctx = nullptr; }   // free for the next
      }
    }
    if (s.ob.pos >= need) return OK;
    return s.done ? OK : BAD;                   // asked past the end: the caller checks offsets
  }
};

// addresses
// /zimview/<n>/<namespace>/<path>, as archive.html and zim-sw.js use them

inline std::string urlDecode(const std::string &s) {
  std::string o; o.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
      o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else o += s[i];
  }
  return o;
}

inline std::string urlEncodePath(const std::string &s) {
  static const char *hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (isalnum(c) || strchr("/-_.~!$&'()*+,;=:@", c)) o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

struct Answer {
  enum Kind { NOT_FOUND, REDIRECT, RAW, PACKED, ERROR } kind = NOT_FOUND;
  std::string location;           // REDIRECT: full /zimview/... address
  std::string mime;
  Dirent entry;                   // RAW / PACKED
  uint64_t rawOff = 0, rawSize = 0;
  std::string why;
};

// what zim-sw.js's serve() does, minus the content: which entry an address means, or
// where to send the browser instead. `path` is everything after "/zimview/<n>/" and `q`
// the query with its "?" (or empty), both already percent-decoded. `rawQuery` is the
// query as it came, for the Location of a redirect
inline Answer resolve(Archive &a, int archiveIdx, const std::string &path, const std::string &q, const std::string &rawQuery) {
  Answer ans;
  std::string prefix = "/zimview/" + std::to_string(archiveIdx) + "/";
  Dirent d;
  int r;
  if (path.empty()) {
    r = a.mainPageEntry(d);
    if (r != OK) { ans.kind = r == NOT_FOUND ? Answer::NOT_FOUND : Answer::ERROR; ans.why = "This archive has no main page."; return ans; }
    ans.kind = Answer::REDIRECT;
    ans.location = prefix + urlEncodePath(std::string(1, d.ns) + "/" + d.url) + rawQuery;
    return ans;
  }
  size_t slash = path.find('/');
  std::string ns = slash != std::string::npos && slash > 0 ? path.substr(0, slash) : "";
  std::string rest = slash != std::string::npos && slash > 0 ? path.substr(slash + 1) : path;
  bool nsOk = ns.size() == 1 && ((ns[0] >= 'A' && ns[0] <= 'Z') || ns[0] == '-');
  struct Try { char ns; std::string p; bool withQuery; };
  std::vector<Try> tries;
  // a Zimit copy stores a page that had a query string under its whole address
  if (!q.empty() && nsOk) tries.push_back({ns[0], rest + q, true});
  if (!q.empty()) tries.push_back({'C', path + q, true});
  if (nsOk) tries.push_back({ns[0], rest, false});
  for (char n : {'C', 'A', 'I', '-'}) tries.push_back({n, path, false});
  std::string at; bool withQuery = false;
  bool found = false;
  for (auto &t : tries) {
    r = a.findByUrl(t.ns, t.p, d);
    if (r == NOT_FOUND && !t.p.empty() && t.p.back() == '/') r = a.findByUrl(t.ns, t.p + "index.html", d);
    if (r < 0) { ans.kind = Answer::ERROR; ans.why = "card read failed"; return ans; }
    if (r == OK) { at = std::string(1, t.ns) + "/" + t.p; withQuery = t.withQuery; found = true; break; }
  }
  if (!found) { ans.kind = Answer::NOT_FOUND; ans.why = "This page is not in the archive: " + path; return ans; }
  if (d.redirect) {
    Dirent t = d;
    r = a.resolveRedirect(t);
    if (r != OK) { ans.kind = Answer::ERROR; ans.why = "broken redirect"; return ans; }
    std::string tat = std::string(1, t.ns) + "/" + t.url;
    if (tat != at) {
      ans.kind = Answer::REDIRECT;
      ans.location = prefix + urlEncodePath(tat) + rawQuery;
      return ans;
    }
    d = t;
  }
  // a page answers at its own address, so its relative links resolve
  if (at != ns + "/" + rest + (withQuery ? q : "")) {
    ans.kind = Answer::REDIRECT;
    ans.location = prefix + urlEncodePath(at) + (withQuery ? "" : rawQuery);
    return ans;
  }
  ans.entry = d;
  ans.mime = a.mimeOf(d);
  r = a.rawBlob(d.cluster, d.blob, ans.rawOff, ans.rawSize);
  if (r == OK) ans.kind = Answer::RAW;
  else if (r == NOT_FOUND) ans.kind = Answer::PACKED;
  else { ans.kind = Answer::ERROR; ans.why = "card read failed"; }
  return ans;
}

// where zim-frame.js goes into a page: after <head>, else <html>, else the doctype, else
// the very start (zim-sw.js withFrameScript, same rules). Only the first 16 KB are looked at
inline size_t frameScriptAt(const uint8_t *p, size_t n) {
  if (n > 1 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) return (size_t)-1;   // UTF-16: leave it
  if (n > 16384) n = 16384;
  auto findTag = [&](const char *tag) -> size_t {
    size_t tl = strlen(tag);
    for (size_t i = 0; i + tl < n; i++) {
      if (p[i] != '<') continue;
      size_t k = 0;
      while (k < tl && tolower(p[i + 1 + k]) == tag[k]) k++;
      if (k < tl) continue;
      unsigned char after = p[i + 1 + tl];
      if (after != '>' && !isspace(after)) continue;
      for (size_t j = i + 1 + tl; j < n; j++) if (p[j] == '>') return j + 1;
      return (size_t)-1;
    }
    return (size_t)-1;
  };
  size_t at = findTag("head");
  if (at == (size_t)-1) at = findTag("html");
  if (at == (size_t)-1) at = findTag("!doctype");
  return at == (size_t)-1 ? 0 : at;
}

}  // namespace nzim
#endif
