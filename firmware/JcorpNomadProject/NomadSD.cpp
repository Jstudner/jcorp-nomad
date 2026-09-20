//<!-- Version 4.6 -->
// NomadSD.cpp - exFAT + FAT32 SD support: SdFat on the ESP32-S3 SDMMC host,
// wrapped in the Arduino fs::FS interface. See NomadSD.h for the why.
//
// Layering:
//   sdmmc_host driver (IDF, same peripheral + pins SD_MMC used)
//     -> SdMmcBlockDev (FsBlockDeviceInterface, raw sectors)
//       -> FsVolume (SdFat: auto-detects exFAT / FAT32 / FAT16)
//         -> NomadFSImpl / NomadFileImpl (fs::FSImpl / fs::FileImpl)
//           -> fs::FS "NomadSD" (drop-in for SD_MMC via #define)
//
// Thread safety: SdFat is not reentrant and the card is touched from async_tcp,
// the index workers and the streaming task, so every entry point takes one
// recursive mutex. This replaces the FATFS FF_FS_REENTRANT volume lock.

// SdFat first: its FILE_READ/FILE_WRITE macros clash with the ones in the core's
// FS.h. SdFat's are undef'd below; this file spells out oflags and mode strings.
#include <SdFatConfig.h>  // top-level include so the IDE/arduino-cli pull in the SdFat library
#include "FsLib/FsLib.h"
#include "common/FsBlockDeviceInterface.h"
#undef FILE_READ
#undef FILE_WRITE

#include "NomadSD.h"
#include <FSImpl.h>

#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp32-hal-periman.h"
#include "esp32-hal-log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

using namespace fs;

// ---------------------------------------------------------------- state

static SemaphoreHandle_t s_lock = nullptr;

struct FsLockGuard {
  FsLockGuard() {
    if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
  }
  ~FsLockGuard() {
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
  }
};

static sdmmc_card_t s_card;
static bool s_cardInited = false;
static bool s_mounted = false;
static bool s_mode1bit = true;
static int8_t s_pin_clk = -1, s_pin_cmd = -1, s_pin_d0 = -1, s_pin_d1 = -1, s_pin_d2 = -1, s_pin_d3 = -1;

// ---------------------------------------------------------------- block device

class SdMmcBlockDev : public FsBlockDeviceInterface {
public:
  bool isBusy() override {
    return false;  // sdmmc_read/write_sectors block until the card is done
  }
  bool readSector(uint32_t sector, uint8_t *dst) override {
    return readSectors(sector, dst, 1);
  }
  bool readSectors(uint32_t sector, uint8_t *dst, size_t ns) override {
    if (!s_cardInited) return false;
    return sdmmc_read_sectors(&s_card, dst, sector, ns) == ESP_OK;
  }
  bool writeSector(uint32_t sector, const uint8_t *src) override {
    return writeSectors(sector, src, 1);
  }
  bool writeSectors(uint32_t sector, const uint8_t *src, size_t ns) override {
    if (!s_cardInited) return false;
    return sdmmc_write_sectors(&s_card, src, sector, ns) == ESP_OK;
  }
  uint32_t sectorCount() override {
    return s_cardInited ? (uint32_t)s_card.csd.capacity : 0;
  }
  bool syncDevice() override {
    return true;  // writes are synchronous at the driver level
  }
};

static SdMmcBlockDev s_dev;
static FsVolume s_vol;

// ---------------------------------------------------------------- card init

static bool nomadDetachBus(void *) {
  NomadSD.end();
  return true;
}

static bool cardInit(bool mode1bit, int freq_khz) {
  if (s_cardInited) return true;
  if (s_pin_clk < 0 || s_pin_cmd < 0 || s_pin_d0 < 0 || (!mode1bit && (s_pin_d1 < 0 || s_pin_d2 < 0 || s_pin_d3 < 0))) {
    log_e("NomadSD: setPins must be called before begin");
    return false;
  }

  perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_CLK, nomadDetachBus);
  perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_CMD, nomadDetachBus);
  perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_D0, nomadDetachBus);
  perimanClearPinBus(s_pin_clk);
  perimanClearPinBus(s_pin_cmd);
  perimanClearPinBus(s_pin_d0);
  if (!mode1bit) {
    perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_D1, nomadDetachBus);
    perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_D2, nomadDetachBus);
    perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_D3, nomadDetachBus);
    perimanClearPinBus(s_pin_d1);
    perimanClearPinBus(s_pin_d2);
    perimanClearPinBus(s_pin_d3);
  }

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.clk = (gpio_num_t)s_pin_clk;
  slot_config.cmd = (gpio_num_t)s_pin_cmd;
  slot_config.d0 = (gpio_num_t)s_pin_d0;
  slot_config.d1 = (gpio_num_t)s_pin_d1;
  slot_config.d2 = (gpio_num_t)s_pin_d2;
  slot_config.d3 = (gpio_num_t)s_pin_d3;
  slot_config.width = mode1bit ? 1 : 4;

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_1;
  host.flags = mode1bit ? SDMMC_HOST_FLAG_1BIT : SDMMC_HOST_FLAG_4BIT;
  host.max_freq_khz = freq_khz;

  esp_err_t err = sdmmc_host_init();
  bool hostFresh = (err == ESP_OK);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    log_e("NomadSD: sdmmc_host_init failed (0x%x)", err);
    return false;
  }
  err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot_config);
  if (err != ESP_OK) {
    log_e("NomadSD: sdmmc_host_init_slot failed (0x%x)", err);
    if (hostFresh) sdmmc_host_deinit();
    return false;
  }
  memset(&s_card, 0, sizeof(s_card));
  err = sdmmc_card_init(&host, &s_card);
  if (err != ESP_OK) {
    log_e("NomadSD: sdmmc_card_init failed (0x%x) - check card and pull-ups", err);
    sdmmc_host_deinit();
    return false;
  }

  s_mode1bit = mode1bit;
  s_cardInited = true;

  // pin bookkeeping mirrors SD_MMC; failure here is non-fatal for us
  perimanSetPinBus(s_pin_cmd, ESP32_BUS_TYPE_SDMMC_CMD, (void *)&NomadSD, -1, -1);
  perimanSetPinBus(s_pin_clk, ESP32_BUS_TYPE_SDMMC_CLK, (void *)&NomadSD, -1, -1);
  perimanSetPinBus(s_pin_d0, ESP32_BUS_TYPE_SDMMC_D0, (void *)&NomadSD, -1, -1);
  if (!mode1bit) {
    perimanSetPinBus(s_pin_d1, ESP32_BUS_TYPE_SDMMC_D1, (void *)&NomadSD, -1, -1);
    perimanSetPinBus(s_pin_d2, ESP32_BUS_TYPE_SDMMC_D2, (void *)&NomadSD, -1, -1);
    perimanSetPinBus(s_pin_d3, ESP32_BUS_TYPE_SDMMC_D3, (void *)&NomadSD, -1, -1);
  }
  return true;
}

// ---------------------------------------------------------------- helpers

static String normalizePath(const char *path) {
  if (!path || !*path) return String("/");
  String p(path);
  if (p[0] != '/') p = "/" + p;
  while (p.length() > 1 && p.endsWith("/")) p.remove(p.length() - 1);
  return p;
}

// FS.h open modes ("r","w","a","r+","w+","a+") -> SdFat oflags
static oflag_t modeToOflag(const char *mode) {
  bool r = false, w = false, a = false, plus = false;
  for (const char *p = mode ? mode : "r"; *p; ++p) {
    if (*p == 'r') r = true;
    else if (*p == 'w') w = true;
    else if (*p == 'a') a = true;
    else if (*p == '+') plus = true;
  }
  (void)r;
  if (a) return plus ? (O_RDWR | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_APPEND);
  if (w) return plus ? (O_RDWR | O_CREAT | O_TRUNC) : (O_WRONLY | O_CREAT | O_TRUNC);
  return plus ? O_RDWR : O_RDONLY;
}

static time_t fatToUnixTime(uint16_t d, uint16_t t) {
  struct tm tv = {};
  tv.tm_year = ((d >> 9) & 0x7F) + 1980 - 1900;
  tv.tm_mon = ((d >> 5) & 0x0F) - 1;
  tv.tm_mday = d & 0x1F;
  tv.tm_hour = (t >> 11) & 0x1F;
  tv.tm_min = (t >> 5) & 0x3F;
  tv.tm_sec = (t & 0x1F) * 2;
  return mktime(&tv);
}

// ---------------------------------------------------------------- clock

// SdFat stamps new files through this callback, without one they all share a constant
// date. The Nomad has no RTC, so this reuses the fixed-epoch-plus-uptime approach
// already used for OPDS timestamps: not wall time, but files written later in a
// session sort after earlier ones.
static const time_t NOMAD_REFERENCE_EPOCH = 1752321600;  // 2025-07-12T12:00:00Z

static void nomadDateTimeCallback(uint16_t *date, uint16_t *time, uint8_t *ms10) {
  time_t now = NOMAD_REFERENCE_EPOCH + (time_t)(millis() / 1000UL);
  struct tm tm_now;
  gmtime_r(&now, &tm_now);
  *date = FS_DATE(tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
  *time = FS_TIME(tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
  if (ms10) *ms10 = 0;
}

// ---------------------------------------------------------------- NomadFile64

static inline FsFile *h2f(void *h) {
  return reinterpret_cast<FsFile *>(h);
}

NomadFile64::NomadFile64() : _h(new FsFile()) {}

NomadFile64::~NomadFile64() {
  if (_h) {
    close();
    delete h2f(_h);
    _h = nullptr;
  }
}

NomadFile64::NomadFile64(NomadFile64 &&other) noexcept : _h(other._h) {
  other._h = nullptr;
}

NomadFile64 &NomadFile64::operator=(NomadFile64 &&other) noexcept {
  if (this != &other) {
    if (_h) {
      close();
      delete h2f(_h);
    }
    _h = other._h;
    other._h = nullptr;
  }
  return *this;
}

bool NomadFile64::open(const char *path) {
  FsLockGuard lk;
  if (!_h || !s_mounted) return false;
  String p = normalizePath(path);
  return h2f(_h)->open(&s_vol, p.c_str(), O_RDONLY);
}

void NomadFile64::close() {
  FsLockGuard lk;
  if (_h && h2f(_h)->isOpen()) h2f(_h)->close();
}

bool NomadFile64::isOpen() const {
  FsLockGuard lk;
  return _h && h2f(_h)->isOpen();
}

uint64_t NomadFile64::size() const {
  FsLockGuard lk;
  if (!_h || !h2f(_h)->isOpen()) return 0;
  return h2f(_h)->fileSize();
}

uint64_t NomadFile64::position() const {
  FsLockGuard lk;
  if (!_h || !h2f(_h)->isOpen()) return 0;
  return h2f(_h)->curPosition();
}

bool NomadFile64::seek(uint64_t pos) {
  FsLockGuard lk;
  if (!_h || !h2f(_h)->isOpen()) return false;
  return h2f(_h)->seekSet(pos);
}

size_t NomadFile64::read(uint8_t *buf, size_t len) {
  FsLockGuard lk;
  if (!_h || !h2f(_h)->isOpen()) return 0;
  int n = h2f(_h)->read(buf, len);
  return n < 0 ? 0 : (size_t)n;
}

size_t NomadFile64::readAt(uint64_t pos, uint8_t *buf, size_t len) {
  FsLockGuard lk;
  if (!_h || !h2f(_h)->isOpen()) return 0;
  FsFile *f = h2f(_h);
  if (f->curPosition() != pos && !f->seekSet(pos)) return 0;
  int n = f->read(buf, len);
  return n < 0 ? 0 : (size_t)n;
}

// ---------------------------------------------------------------- FileImpl

class NomadFileImpl : public FileImpl {
public:
  NomadFileImpl(FsFile f, const String &fullPath) : _f(std::move(f)), _path(fullPath) {
    int idx = _path.lastIndexOf('/');
    _name = (idx >= 0 && idx + 1 < (int)_path.length()) ? _path.substring(idx + 1) : _path;
  }

  ~NomadFileImpl() override {
    close();
  }

  size_t write(const uint8_t *buf, size_t size) override {
    FsLockGuard lk;
    if (!_f.isOpen()) return 0;
    size_t n = _f.write(buf, size);
    return _f.getWriteError() ? 0 : n;
  }

  size_t read(uint8_t *buf, size_t size) override {
    FsLockGuard lk;
    if (!_f.isOpen()) return 0;
    int n = _f.read(buf, size);
    return n < 0 ? 0 : (size_t)n;
  }

  void flush() override {
    FsLockGuard lk;
    if (_f.isOpen()) _f.sync();
  }

  bool seek(uint32_t pos, SeekMode mode) override {
    FsLockGuard lk;
    if (!_f.isOpen()) return false;
    switch (mode) {
      case SeekSet: return _f.seekSet((uint64_t)pos);
      case SeekCur: return _f.seekCur((int64_t)pos);
      case SeekEnd: return _f.seekEnd(-(int64_t)pos);
      default: return false;
    }
  }

  size_t position() const override {
    FsLockGuard lk;
    return (size_t)_f.curPosition();
  }

  size_t size() const override {
    FsLockGuard lk;
    if (!_f.isOpen() || _f.isDir()) return 0;
    uint64_t sz = _f.fileSize();
    // Saturate, never wrap: a 5 GB file would otherwise report 0.88 GB and callers would
    // act on it as the whole file. SIZE_MAX also means "ask fileSize64()".
    return sz > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)sz;
  }

  bool setBufferSize(size_t) override {
    return false;  // SdFat reads through its own sector cache; no stdio buffer to size
  }

  void close() override {
    FsLockGuard lk;
    if (_f.isOpen()) _f.close();
  }

  time_t getLastWrite() override {
    FsLockGuard lk;
    uint16_t d = 0, t = 0;
    if (!_f.isOpen() || !_f.getModifyDateTime(&d, &t)) return 0;
    return fatToUnixTime(d, t);
  }

  const char *path() const override {
    return _path.c_str();
  }

  const char *name() const override {
    return _name.c_str();
  }

  boolean isDirectory(void) override {
    FsLockGuard lk;
    return _f.isOpen() && _f.isDir();
  }

  FileImplPtr openNextFile(const char *mode) override {
    FsLockGuard lk;
    (void)mode;  // directory iteration in this firmware is read-only
    if (!_f.isOpen() || !_f.isDir()) return FileImplPtr();
    FsFile child;
    if (!child.openNext(&_f, O_RDONLY)) return FileImplPtr();
    char nbuf[512];
    nbuf[0] = 0;
    child.getName(nbuf, sizeof(nbuf));
    String childPath = (_path == "/") ? String("/") + nbuf : _path + "/" + nbuf;
    return std::make_shared<NomadFileImpl>(std::move(child), childPath);
  }

  boolean seekDir(long position) override {
    FsLockGuard lk;
    if (!_f.isOpen() || !_f.isDir()) return false;
    return _f.seekSet((uint64_t)position);
  }

  String getNextFileName(void) override {
    return getNextFileName(nullptr);
  }

  String getNextFileName(bool *isDir) override {
    FsLockGuard lk;
    if (!_f.isOpen() || !_f.isDir()) return String("");
    FsFile child;
    if (!child.openNext(&_f, O_RDONLY)) return String("");
    char nbuf[512];
    nbuf[0] = 0;
    child.getName(nbuf, sizeof(nbuf));
    if (isDir) *isDir = child.isDir();
    child.close();
    return (_path == "/") ? String("/") + nbuf : _path + "/" + nbuf;
  }

  void rewindDirectory(void) override {
    FsLockGuard lk;
    if (_f.isOpen() && _f.isDir()) _f.rewind();
  }

  operator bool() override {
    FsLockGuard lk;
    return _f.isOpen();
  }

private:
  mutable FsFile _f;
  String _path;
  String _name;
};

// ---------------------------------------------------------------- FSImpl

class NomadFSImpl : public FSImpl {
public:
  FileImplPtr open(const char *path, const char *mode, const bool create) override {
    FsLockGuard lk;
    if (!s_mounted) return FileImplPtr();
    String p = normalizePath(path);
    oflag_t of = modeToOflag(mode);
    if ((of & O_CREAT) && create) {
      // fs::FS::open(path, mode, true) promises missing parent folders
      int idx = p.lastIndexOf('/');
      if (idx > 0) {
        String parent = p.substring(0, idx);
        if (!s_vol.exists(parent.c_str())) s_vol.mkdir(parent.c_str(), true);
      }
    }
    FsFile f = s_vol.open(p.c_str(), of);
    if (!f.isOpen()) return FileImplPtr();
    return std::make_shared<NomadFileImpl>(std::move(f), p);
  }

  bool exists(const char *path) override {
    FsLockGuard lk;
    if (!s_mounted) return false;
    String p = normalizePath(path);
    if (p == "/") return true;
    return s_vol.exists(p.c_str());
  }

  bool rename(const char *pathFrom, const char *pathTo) override {
    FsLockGuard lk;
    if (!s_mounted) return false;
    return s_vol.rename(normalizePath(pathFrom).c_str(), normalizePath(pathTo).c_str());
  }

  bool remove(const char *path) override {
    FsLockGuard lk;
    if (!s_mounted) return false;
    return s_vol.remove(normalizePath(path).c_str());
  }

  bool mkdir(const char *path) override {
    FsLockGuard lk;
    if (!s_mounted) return false;
    String p = normalizePath(path);
    if (s_vol.exists(p.c_str())) {
      FsFile f = s_vol.open(p.c_str(), O_RDONLY);
      bool isdir = f.isOpen() && f.isDir();
      f.close();
      return isdir;  // match VFS: mkdir on an existing dir succeeds
    }
    return s_vol.mkdir(p.c_str(), true);
  }

  bool rmdir(const char *path) override {
    FsLockGuard lk;
    if (!s_mounted) return false;
    return s_vol.rmdir(normalizePath(path).c_str());
  }
};

// ---------------------------------------------------------------- NomadSDFS

NomadSDFS::NomadSDFS() : FS(FSImplPtr(new NomadFSImpl())) {}

uint64_t NomadSDFS::fileSize64(const char *path) {
  FsLockGuard lk;
  if (!s_mounted) return 0;
  FsFile f;
  if (!f.open(&s_vol, normalizePath(path).c_str(), O_RDONLY)) return 0;
  uint64_t sz = f.isDir() ? 0 : f.fileSize();
  f.close();
  return sz;
}

bool NomadSDFS::setPins(int clk, int cmd, int d0, int d1, int d2, int d3) {
  if (s_cardInited) {
    log_e("NomadSD: setPins must be called before begin");
    return false;
  }
  s_pin_clk = (int8_t)clk;
  s_pin_cmd = (int8_t)cmd;
  s_pin_d0 = (int8_t)d0;
  s_pin_d1 = (int8_t)d1;
  s_pin_d2 = (int8_t)d2;
  s_pin_d3 = (int8_t)d3;
  return true;
}

bool NomadSDFS::begin(const char *mountpoint, bool mode1bit, bool format_if_mount_failed, int sdmmc_frequency, uint8_t maxOpenFiles) {
  (void)format_if_mount_failed;  // deliberately unsupported: auto-format destroyed unreadable-but-valid cards
  (void)maxOpenFiles;
  if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
  FsLockGuard lk;
  if (s_mounted) return true;
  if (!cardInit(mode1bit, sdmmc_frequency)) return false;
  FsDateTime::setCallback(nomadDateTimeCallback);
  // partition 1 (normal SD layout), then superfloppy (filesystem at sector 0)
  if (!s_vol.begin(&s_dev, true, 1) && !s_vol.begin(&s_dev, true, 0)) {
    log_e("NomadSD: card has no exFAT/FAT32/FAT16 filesystem I can read");
    return false;
  }
  s_mounted = true;
  _impl->mountpoint(mountpoint);
  log_i("NomadSD: mounted %s volume", fsTypeName());
  return true;
}

bool NomadSDFS::beginRaw(bool mode1bit, int sdmmc_frequency) {
  if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
  FsLockGuard lk;
  return cardInit(mode1bit, sdmmc_frequency);
}

void NomadSDFS::end() {
  FsLockGuard lk;
  s_mounted = false;
  if (s_cardInited) {
    s_cardInited = false;  // cleared first: perimanClearPinBus re-enters end() via detach
    sdmmc_host_deinit();
    perimanClearPinBus(s_pin_cmd);
    perimanClearPinBus(s_pin_clk);
    perimanClearPinBus(s_pin_d0);
    if (!s_mode1bit) {
      perimanClearPinBus(s_pin_d1);
      perimanClearPinBus(s_pin_d2);
      perimanClearPinBus(s_pin_d3);
    }
    _impl->mountpoint(NULL);
  }
}

sdcard_type_t NomadSDFS::cardType() {
  FsLockGuard lk;
  if (!s_cardInited) return CARD_NONE;
  return (s_card.ocr & SD_OCR_SDHC_CAP) ? CARD_SDHC : CARD_SD;
}

uint64_t NomadSDFS::cardSize() {
  FsLockGuard lk;
  if (!s_cardInited) return 0;
  return (uint64_t)s_card.csd.capacity * s_card.csd.sector_size;
}

uint64_t NomadSDFS::totalBytes() {
  FsLockGuard lk;
  if (!s_mounted) return 0;
  uint64_t clusterBytes = (uint64_t)s_vol.sectorsPerCluster() * 512ULL;
  return (uint64_t)s_vol.clusterCount() * clusterBytes;
}

// FAT32 FSInfo fast path. SdFat always walks the whole FAT, which costs tens of
// seconds on a big card inside setup(), so read the FSInfo free-cluster count directly
// and fall back to the walk only when it is absent or implausible. Known drift: SdFat
// does not update FSInfo on write, so the count goes stale until a PC mounts the card.
static bool looksLikeFat32BootSector(const uint8_t *s) {
  if (s[510] != 0x55 || s[511] != 0xAA) return false;
  if (s[0] != 0xEB && s[0] != 0xE9) return false;
  return memcmp(s + 82, "FAT32   ", 8) == 0;  // BS_FilSysType
}

static bool fat32FSInfoFreeClusters(uint32_t &freeOut) {
  uint8_t buf[512];
  if (!s_dev.readSector(0, buf)) return false;
  uint32_t volStart = 0;
  if (!looksLikeFat32BootSector(buf)) {
    // not superfloppy: sector 0 is an MBR, take partition 1
    if (buf[510] != 0x55 || buf[511] != 0xAA) return false;
    const uint8_t *pe = buf + 446;
    uint32_t lba = (uint32_t)pe[8] | ((uint32_t)pe[9] << 8) | ((uint32_t)pe[10] << 16) | ((uint32_t)pe[11] << 24);
    if (pe[4] == 0 || lba == 0) return false;
    volStart = lba;
    if (!s_dev.readSector(volStart, buf)) return false;
    if (!looksLikeFat32BootSector(buf)) return false;
  }
  uint16_t fsInfoSec = (uint16_t)buf[48] | ((uint16_t)buf[49] << 8);  // BPB_FSInfo
  if (fsInfoSec == 0 || fsInfoSec == 0xFFFF) return false;
  if (!s_dev.readSector(volStart + fsInfoSec, buf)) return false;
  uint32_t lead = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
  uint32_t sig = (uint32_t)buf[484] | ((uint32_t)buf[485] << 8) | ((uint32_t)buf[486] << 16) | ((uint32_t)buf[487] << 24);
  if (lead != 0x41615252 || sig != 0x61417272 || buf[510] != 0x55 || buf[511] != 0xAA) return false;
  uint32_t freeCount = (uint32_t)buf[488] | ((uint32_t)buf[489] << 8) | ((uint32_t)buf[490] << 16) | ((uint32_t)buf[491] << 24);
  if (freeCount == 0xFFFFFFFF || freeCount > s_vol.clusterCount()) return false;
  freeOut = freeCount;
  return true;
}

uint64_t NomadSDFS::usedBytes() {
  FsLockGuard lk;
  if (!s_mounted) return 0;
  uint64_t clusterBytes = (uint64_t)s_vol.sectorsPerCluster() * 512ULL;
  uint32_t freeFromFsInfo = 0;
  if (s_vol.fatType() == 32 && fat32FSInfoFreeClusters(freeFromFsInfo)) {
    return ((uint64_t)s_vol.clusterCount() - (uint64_t)freeFromFsInfo) * clusterBytes;
  }
  // exFAT allocation-bitmap scan is quick; a FAT32 card without valid FSInfo
  // pays the full FAT walk once, then SdFat maintains the count incrementally
  int32_t freeClusters = s_vol.freeClusterCount();
  if (freeClusters < 0) return 0;
  return ((uint64_t)s_vol.clusterCount() - (uint64_t)freeClusters) * clusterBytes;
}

int NomadSDFS::sectorSize() {
  FsLockGuard lk;
  if (!s_cardInited) return 0;
  return s_card.csd.sector_size;
}

int NomadSDFS::numSectors() {
  FsLockGuard lk;
  if (!s_cardInited) return 0;
  return s_card.csd.capacity;
}

bool NomadSDFS::readRAW(uint8_t *buffer, uint32_t sector) {
  FsLockGuard lk;
  return s_dev.readSector(sector, buffer);
}

bool NomadSDFS::writeRAW(uint8_t *buffer, uint32_t sector) {
  FsLockGuard lk;
  return s_dev.writeSector(sector, buffer);
}

bool NomadSDFS::readRAWMulti(uint8_t *buffer, uint32_t sector, size_t count) {
  FsLockGuard lk;
  return s_dev.readSectors(sector, buffer, count);
}

bool NomadSDFS::writeRAWMulti(const uint8_t *buffer, uint32_t sector, size_t count) {
  FsLockGuard lk;
  return s_dev.writeSectors(sector, buffer, count);
}

const char *NomadSDFS::fsTypeName() {
  FsLockGuard lk;
  if (!s_mounted) return "none";
  switch (s_vol.fatType()) {
    case FAT_TYPE_EXFAT: return "exFAT";
    case 32: return "FAT32";
    case 16: return "FAT16";
    default: return "FAT?";
  }
}

bool NomadSDFS::isExFat() {
  FsLockGuard lk;
  return s_mounted && s_vol.fatType() == FAT_TYPE_EXFAT;
}

NomadSDFS NomadSD;
