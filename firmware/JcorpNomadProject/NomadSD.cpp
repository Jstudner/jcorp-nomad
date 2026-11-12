//<!-- Version 5.0 -->
// NomadSD.cpp - exFAT + FAT32 storage: SdFat over one or two block devices, wrapped in
// the Arduino fs::FS interface and unioned into a single namespace. See NomadSD.h.
//
// Layering:
//   sdmmc_host driver (IDF)  -> SdMmcBlockDev  \
//                                               >- NomadVol (FsVolume: exFAT/FAT32/FAT16)
//   usb_host_msc (NomadUSB)  -> UsbMscBlockDev /        |
//                                                       v
//                              NomadFSImpl (resolves a path to a volume, unions dirs)
//                                                       |
//                                                       v
//                                         fs::FS "NomadSD"  (drop-in for SD_MMC)
//
// Thread safety: SdFat is not reentrant and the volumes are touched from async_tcp, the
// index workers and the streaming task, so every entry point takes that volume's
// recursive mutex. This replaces the FATFS FF_FS_REENTRANT volume lock.

// SdFat first: its FILE_READ/FILE_WRITE macros clash with the ones in the core's
// FS.h. SdFat's are undef'd below, this file spells out oflags and mode strings.
#include <SdFatConfig.h>  // top-level include so the IDE/arduino-cli pull in the SdFat library
#include "FsLib/FsLib.h"
#include "common/FsBlockDeviceInterface.h"
#undef FILE_READ
#undef FILE_WRITE

#include "NomadSD.h"
#include "nomad_board.h"
#include "NomadUSB.h"
#include <FSImpl.h>

#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp32-hal-periman.h"
#include "esp32-hal-log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include <set>

#if NOMAD_SD_LDO_CHANNEL >= 0
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

using namespace fs;

struct NomadVol {
  const char *label = "?";
  FsBlockDeviceInterface *dev = nullptr;
  FsVolume vol;
  bool mounted = false;
  SemaphoreHandle_t lock = nullptr;
  // Bumped on every mount and every unmount. A handle records the generation it was
  // opened on, because FsVolume::begin() placement-news the next drive's volume into
  // the SAME memory: without this a handle held across a hot-swap keeps a cluster
  // chain describing the old drive and applies it to the new one. If the two are
  // formatted differently it is worse than wrong data, the handle's m_vol is a
  // FatVolume* pointing at an ExFatVolume and the first call through it is undefined.
  uint32_t gen = 1;
};

static NomadVol s_vols[NOMAD_VOL_COUNT];

static inline NomadVol &V(int i) {
  return s_vols[i];
}

// True while a handle opened on `vol` at generation `gen` still describes real media.
static inline bool volLive(int vol, uint32_t gen) {
  return vol >= 0 && vol < NOMAD_VOL_COUNT && s_vols[vol].mounted && s_vols[vol].gen == gen;
}

// Cluster size in bytes, without going through FsVolume::bytesPerCluster().
//
// That accessor is uint16_t on the FAT side (FatPartition.h: `uint16_t
// bytesPerCluster() { return m_bytesPerSector << m_sectorsPerClusterShift; }`), so a
// 64 KB cluster is 65536 truncated to ZERO. FsVolume widens the result to uint32_t
// afterwards, which hides it. Windows formats FAT32 over ~32 GB with 64 KB clusters, so
// this is the common case on a big drive (it reads as 0.0 GB total). exFAT's own field
// is uint32_t and unaffected.
static inline uint64_t volClusterBytes(FsVolume &vol) {
  return (uint64_t)vol.sectorsPerCluster() * 512ULL;   // 512 is enforced at mount
}

struct VolLock {
  NomadVol *v;
  explicit VolLock(NomadVol &vv) : v(&vv) {
    if (v->lock) xSemaphoreTakeRecursive(v->lock, portMAX_DELAY);
  }
  ~VolLock() {
    if (v->lock) xSemaphoreGiveRecursive(v->lock);
  }
};

// SD card state

static sdmmc_card_t s_card;
static bool s_cardInited = false;
// written only under the card volume's lock (every sector access holds it), read as a copy
static NomadSDStats s_stats = {};
static bool s_mode1bit = true;
static int8_t s_pin_clk = -1, s_pin_cmd = -1, s_pin_d0 = -1, s_pin_d1 = -1, s_pin_d2 = -1, s_pin_d3 = -1;
#if NOMAD_SD_LDO_CHANNEL >= 0
static sd_pwr_ctrl_handle_t s_pwrCtrl = nullptr;
#endif

// Block devices

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
    const int64_t t0 = esp_timer_get_time();
    const bool ok = sdmmc_read_sectors(&s_card, dst, sector, ns) == ESP_OK;
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    s_stats.reads++;
    s_stats.sectorsRead += ns;
    if (us > s_stats.maxReadUs) s_stats.maxReadUs = us;
    if (us > NOMAD_SD_SLOW_MS * 1000) s_stats.slowReads++;
    if (!ok) { s_stats.readErrors++; s_stats.lastErrorMs = millis(); }
    return ok;
  }
  bool writeSector(uint32_t sector, const uint8_t *src) override {
    return writeSectors(sector, src, 1);
  }
  bool writeSectors(uint32_t sector, const uint8_t *src, size_t ns) override {
    if (!s_cardInited) return false;
    const bool ok = sdmmc_write_sectors(&s_card, src, sector, ns) == ESP_OK;
    s_stats.writes++;
    s_stats.sectorsWritten += ns;
    if (!ok) { s_stats.writeErrors++; s_stats.lastErrorMs = millis(); }
    return ok;
  }
  uint32_t sectorCount() override {
    return s_cardInited ? (uint32_t)s_card.csd.capacity : 0;
  }
  bool syncDevice() override {
    return true;  // writes are synchronous at the driver level
  }
};

// The USB side lives in NomadUSB.cpp behind plain functions, because pulling
// SdFat's headers into that translation unit would drag in its global
// `typedef FsFile File` and collide with fs::File across the firmware.
class UsbMscBlockDev : public FsBlockDeviceInterface {
public:
  bool isBusy() override {
    return false;
  }
  bool readSector(uint32_t sector, uint8_t *dst) override {
    return readSectors(sector, dst, 1);
  }
  bool readSectors(uint32_t sector, uint8_t *dst, size_t ns) override {
    return nomadUsbReadSectors(sector, dst, ns);
  }
  bool writeSector(uint32_t sector, const uint8_t *src) override {
    return writeSectors(sector, src, 1);
  }
  bool writeSectors(uint32_t sector, const uint8_t *src, size_t ns) override {
    return nomadUsbWriteSectors(sector, src, ns);
  }
  uint32_t sectorCount() override {
    return nomadUsbSectorCount();
  }
  bool syncDevice() override {
    return true;
  }
};

static SdMmcBlockDev s_sdDev;
static UsbMscBlockDev s_usbDev;

// card init

static bool nomadDetachBus(void *) {
  NomadSD.end();
  return true;
}

static bool cardInit(bool mode1bit, int freq_khz) {
  if (s_cardInited) return true;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

#if NOMAD_SD_IOMUX
  // Slot 0 on the P4 is IO MUX only, but the driver still validates the pin numbers and
  // rejects GPIO_NUM_NC ("Invalid GPIO number -1", 0x102 ESP_ERR_INVALID_ARG). It wants
  // the real IOMUX numbers, recognises them as the fixed set and routes through the MUX,
  // which is also what the IDF sd_card example passes on this target.
  slot_config.clk = (gpio_num_t)NOMAD_SD_CLK;
  slot_config.cmd = (gpio_num_t)NOMAD_SD_CMD;
  slot_config.d0 = (gpio_num_t)NOMAD_SD_D0;
  slot_config.d1 = (gpio_num_t)NOMAD_SD_D1;
  slot_config.d2 = (gpio_num_t)NOMAD_SD_D2;
  slot_config.d3 = (gpio_num_t)NOMAD_SD_D3;
  slot_config.cd = SDMMC_SLOT_NO_CD;
  slot_config.wp = SDMMC_SLOT_NO_WP;
  slot_config.width = mode1bit ? 1 : 4;
  const int8_t p_clk = NOMAD_SD_CLK, p_cmd = NOMAD_SD_CMD, p_d0 = NOMAD_SD_D0;
  const int8_t p_d1 = NOMAD_SD_D1, p_d2 = NOMAD_SD_D2, p_d3 = NOMAD_SD_D3;
#else
  if (s_pin_clk < 0 || s_pin_cmd < 0 || s_pin_d0 < 0 || (!mode1bit && (s_pin_d1 < 0 || s_pin_d2 < 0 || s_pin_d3 < 0))) {
    log_e("NomadSD: setPins must be called before begin");
    return false;
  }
  slot_config.clk = (gpio_num_t)s_pin_clk;
  slot_config.cmd = (gpio_num_t)s_pin_cmd;
  slot_config.d0 = (gpio_num_t)s_pin_d0;
  slot_config.d1 = (gpio_num_t)s_pin_d1;
  slot_config.d2 = (gpio_num_t)s_pin_d2;
  slot_config.d3 = (gpio_num_t)s_pin_d3;
  slot_config.width = mode1bit ? 1 : 4;
  const int8_t p_clk = s_pin_clk, p_cmd = s_pin_cmd, p_d0 = s_pin_d0;
  const int8_t p_d1 = s_pin_d1, p_d2 = s_pin_d2, p_d3 = s_pin_d3;
#endif

  perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_CLK, nomadDetachBus);
  perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_CMD, nomadDetachBus);
  perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_D0, nomadDetachBus);
  perimanClearPinBus(p_clk);
  perimanClearPinBus(p_cmd);
  perimanClearPinBus(p_d0);
  if (!mode1bit) {
    perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_D1, nomadDetachBus);
    perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_D2, nomadDetachBus);
    perimanSetBusDeinit(ESP32_BUS_TYPE_SDMMC_D3, nomadDetachBus);
    perimanClearPinBus(p_d1);
    perimanClearPinBus(p_d2);
    perimanClearPinBus(p_d3);
  }

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
#if NOMAD_SD_SLOT == 0
  host.slot = SDMMC_HOST_SLOT_0;
#else
  host.slot = SDMMC_HOST_SLOT_1;
#endif
  host.flags = mode1bit ? SDMMC_HOST_FLAG_1BIT : SDMMC_HOST_FLAG_4BIT;
  host.max_freq_khz = freq_khz;

#if NOMAD_SD_PWR_PIN >= 0
  // The NANO puts a P-channel FET in the card's 3V3 feed. A 10K pulldown already holds it
  // on, but driving it gives the pin a known state in case something gets wired to the
  // header it's also brought out on.
  pinMode(NOMAD_SD_PWR_PIN, OUTPUT);
  digitalWrite(NOMAD_SD_PWR_PIN, NOMAD_SD_PWR_ON_LEVEL);
#endif

#if NOMAD_SD_LDO_CHANNEL >= 0
  // The P4 doesn't power the SDMMC IO rail from the chip's own supply. VDDPST_5 comes off
  // an on-chip LDO that has to be up before the card sees a valid bus. A board that feeds
  // the rail externally would set NOMAD_SD_LDO_CHANNEL to -1.
  if (!s_pwrCtrl) {
    sd_pwr_ctrl_ldo_config_t ldo_config = {};
    ldo_config.ldo_chan_id = NOMAD_SD_LDO_CHANNEL;
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_pwrCtrl) != ESP_OK) {
      log_e("NomadSD: on-chip LDO channel %d would not start", NOMAD_SD_LDO_CHANNEL);
      s_pwrCtrl = nullptr;
    }
  }
  host.pwr_ctrl_handle = s_pwrCtrl;
#endif

  esp_err_t err = sdmmc_host_init();
  bool hostFresh = (err == ESP_OK);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    log_e("NomadSD: sdmmc_host_init failed (0x%x)", err);
    return false;
  }
  err = sdmmc_host_init_slot((int)host.slot, &slot_config);
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

  // pin bookkeeping like SD_MMC does, a failure here doesn't matter
  perimanSetPinBus(p_cmd, ESP32_BUS_TYPE_SDMMC_CMD, (void *)&NomadSD, -1, -1);
  perimanSetPinBus(p_clk, ESP32_BUS_TYPE_SDMMC_CLK, (void *)&NomadSD, -1, -1);
  perimanSetPinBus(p_d0, ESP32_BUS_TYPE_SDMMC_D0, (void *)&NomadSD, -1, -1);
  if (!mode1bit) {
    perimanSetPinBus(p_d1, ESP32_BUS_TYPE_SDMMC_D1, (void *)&NomadSD, -1, -1);
    perimanSetPinBus(p_d2, ESP32_BUS_TYPE_SDMMC_D2, (void *)&NomadSD, -1, -1);
    perimanSetPinBus(p_d3, ESP32_BUS_TYPE_SDMMC_D3, (void *)&NomadSD, -1, -1);
  }
  return true;
}

// helpers

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

// Volume resolution. If a path already exists on some mounted volume, that volume
// owns it. Only a new path goes to the write volume, otherwise editing a file on USB
// would make a shadow copy on the card.

static bool volHas(NomadVol &v, const String &p) {
  if (!v.mounted) return false;
  VolLock lk(v);
  return v.vol.exists(p.c_str());
}

static int resolveVolume(const String &p) {
  if (p == "/") {
    for (int i = 0; i < NOMAD_VOL_COUNT; ++i)
      if (s_vols[i].mounted) return i;
    return NOMAD_VOL_NONE;
  }
  for (int i = 0; i < NOMAD_VOL_COUNT; ++i)
    if (volHas(s_vols[i], p)) return i;
  return NOMAD_VOL_NONE;
}

static int writeVolume() {
  for (int i = 0; i < NOMAD_VOL_COUNT; ++i)
    if (s_vols[i].mounted) return i;
  return NOMAD_VOL_NONE;
}

// Volume that should service a write to this path: wherever it already is, else primary.
static int resolveForWrite(const String &p) {
  int v = resolveVolume(p);
  return (v == NOMAD_VOL_NONE) ? writeVolume() : v;
}

static bool isDirOn(NomadVol &v, const String &p) {
  if (!v.mounted) return false;
  VolLock lk(v);
  if (p == "/") return true;
  FsFile f = v.vol.open(p.c_str(), O_RDONLY);
  bool d = f.isOpen() && f.isDir();
  f.close();
  return d;
}

// How many mounted volumes carry this path as a directory. Two means the listing has
// to be unioned, one is the fast path.
static int dirVolumeCount(const String &p) {
  int n = 0;
  for (int i = 0; i < NOMAD_VOL_COUNT; ++i)
    if (isDirOn(s_vols[i], p)) n++;
  return n;
}

// ---------------------------------------------------------------- clock

// SdFat stamps new files through this callback, without one they all share a constant
// date. Nomad has no RTC, so this uses the same fixed epoch plus uptime as the
// OPDS timestamps. Not wall time, but files written later in a session sort after
// earlier ones.
static const time_t NOMAD_REFERENCE_EPOCH = 1752321600;  // 2025-07-12T12:00:00Z

static void nomadDateTimeCallback(uint16_t *date, uint16_t *time, uint8_t *ms10) {
  time_t now = NOMAD_REFERENCE_EPOCH + (time_t)(millis() / 1000UL);
  struct tm tm_now;
  gmtime_r(&now, &tm_now);
  *date = FS_DATE(tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
  *time = FS_TIME(tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
  if (ms10) *ms10 = 0;
}

// NomadFile64

static inline FsFile *h2f(void *h) {
  return reinterpret_cast<FsFile *>(h);
}

// Abandon a handle whose volume has gone: FsFile's move-assignment nulls its internals
// without calling into the volume, which close() would. SdFat is built here with
// DESTRUCTOR_CLOSES_FILE = 0, so nothing else reaches back for it either.
static inline void abandonFsFile(FsFile &f) {
  f = FsFile();
}

NomadFile64::NomadFile64() : _h(new FsFile()), _vol(NOMAD_VOL_NONE), _gen(0) {}

NomadFile64::~NomadFile64() {
  if (_h) {
    close();
    delete h2f(_h);
    _h = nullptr;
  }
}

NomadFile64::NomadFile64(NomadFile64 &&other) noexcept : _h(other._h), _vol(other._vol), _gen(other._gen) {
  other._h = nullptr;
  other._vol = NOMAD_VOL_NONE;
  other._gen = 0;
}

NomadFile64 &NomadFile64::operator=(NomadFile64 &&other) noexcept {
  if (this != &other) {
    if (_h) {
      close();
      delete h2f(_h);
    }
    _h = other._h;
    _vol = other._vol;
    _gen = other._gen;
    other._h = nullptr;
    other._vol = NOMAD_VOL_NONE;
    other._gen = 0;
  }
  return *this;
}

bool NomadFile64::open(const char *path) {
  if (!_h) return false;
  String p = normalizePath(path);
  int v = resolveVolume(p);
  if (v == NOMAD_VOL_NONE) return false;
  VolLock lk(V(v));
  if (!h2f(_h)->open(&V(v).vol, p.c_str(), O_RDONLY)) return false;
  _vol = v;
  _gen = V(v).gen;
  return true;
}

// Under the volume lock: still the media this handle was opened on? A stale handle is
// dropped here rather than reported, so a caller that ignores the return value cannot
// then read the next drive's sectors through it.
bool NomadFile64::alive() const {
  if (_vol == NOMAD_VOL_NONE || !_h) return false;
  if (volLive(_vol, _gen)) return true;
  if (h2f(_h)->isOpen()) abandonFsFile(*h2f(_h));
  return false;
}

void NomadFile64::close() {
  if (!_h || _vol == NOMAD_VOL_NONE) return;
  VolLock lk(V(_vol));
  if (!alive()) return;   // already abandoned, closing would touch the next drive
  if (h2f(_h)->isOpen()) h2f(_h)->close();
}

bool NomadFile64::isOpen() const {
  if (!_h || _vol == NOMAD_VOL_NONE) return false;
  VolLock lk(V(_vol));
  if (!alive()) return false;
  return h2f(_h)->isOpen();
}

uint64_t NomadFile64::size() const {
  if (!_h || _vol == NOMAD_VOL_NONE) return 0;
  VolLock lk(V(_vol));
  if (!alive() || !h2f(_h)->isOpen()) return 0;
  return h2f(_h)->fileSize();
}

uint64_t NomadFile64::position() const {
  if (!_h || _vol == NOMAD_VOL_NONE) return 0;
  VolLock lk(V(_vol));
  if (!alive() || !h2f(_h)->isOpen()) return 0;
  return h2f(_h)->curPosition();
}

bool NomadFile64::seek(uint64_t pos) {
  if (!_h || _vol == NOMAD_VOL_NONE) return false;
  VolLock lk(V(_vol));
  if (!alive() || !h2f(_h)->isOpen()) return false;
  return h2f(_h)->seekSet(pos);
}

size_t NomadFile64::read(uint8_t *buf, size_t len) {
  if (!_h || _vol == NOMAD_VOL_NONE) return 0;
  VolLock lk(V(_vol));
  if (!alive() || !h2f(_h)->isOpen()) return 0;
  int n = h2f(_h)->read(buf, len);
  return n < 0 ? 0 : (size_t)n;
}

size_t NomadFile64::readAt(uint64_t pos, uint8_t *buf, size_t len) {
  if (!_h || _vol == NOMAD_VOL_NONE) return 0;
  VolLock lk(V(_vol));
  if (!alive() || !h2f(_h)->isOpen()) return 0;
  FsFile *f = h2f(_h);
  if (f->curPosition() != pos && !f->seekSet(pos)) return 0;
  int n = f->read(buf, len);
  return n < 0 ? 0 : (size_t)n;
}

// FileImpl

class NomadFileImpl : public FileImpl {
public:
  NomadFileImpl(FsFile f, const String &fullPath, int vol)
    : _f(std::move(f)), _path(fullPath), _vol(vol), _gen(V(vol).gen) {
    int idx = _path.lastIndexOf('/');
    _name = (idx >= 0 && idx + 1 < (int)_path.length()) ? _path.substring(idx + 1) : _path;
  }

  ~NomadFileImpl() override {
    close();
  }

  // Under the volume lock: is this still the media the handle was opened on? See the
  // generation comment on NomadVol. A stale handle is abandoned here rather than
  // closed, because close() would sync through a volume pointer that now describes a
  // different drive, and on a differently formatted one that is undefined behaviour.
  bool alive() const {
    if (volLive(_vol, _gen)) return true;
    if (_f.isOpen()) abandonFsFile(_f);
    return false;
  }

  size_t write(const uint8_t *buf, size_t size) override {
    VolLock lk(V(_vol));
    if (!alive() || !_f.isOpen()) return 0;
    size_t n = _f.write(buf, size);
    return _f.getWriteError() ? 0 : n;
  }

  size_t read(uint8_t *buf, size_t size) override {
    VolLock lk(V(_vol));
    if (!alive() || !_f.isOpen()) return 0;
    int n = _f.read(buf, size);
    return n < 0 ? 0 : (size_t)n;
  }

  void flush() override {
    VolLock lk(V(_vol));
    if (alive() && _f.isOpen()) _f.sync();
  }

  bool seek(uint32_t pos, SeekMode mode) override {
    VolLock lk(V(_vol));
    if (!alive() || !_f.isOpen()) return false;
    switch (mode) {
      case SeekSet: return _f.seekSet((uint64_t)pos);
      case SeekCur: return _f.seekCur((int64_t)pos);
      case SeekEnd: return _f.seekEnd(-(int64_t)pos);
      default: return false;
    }
  }

  size_t position() const override {
    VolLock lk(V(_vol));
    if (!alive()) return 0;
    return (size_t)_f.curPosition();
  }

  size_t size() const override {
    VolLock lk(V(_vol));
    if (!alive() || !_f.isOpen() || _f.isDir()) return 0;
    uint64_t sz = _f.fileSize();
    // Saturate, never wrap: a 5 GB file would otherwise report 0.88 GB and callers would
    // act on it as the whole file. SIZE_MAX also means "ask fileSize64()".
    return sz > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)sz;
  }

  bool setBufferSize(size_t) override {
    return false;  // SdFat reads through its own sector cache, no stdio buffer to size
  }

  void close() override {
    VolLock lk(V(_vol));
    if (!alive()) return;   // already abandoned, closing would touch the next drive
    if (_f.isOpen()) _f.close();
  }

  time_t getLastWrite() override {
    VolLock lk(V(_vol));
    uint16_t d = 0, t = 0;
    if (!alive() || !_f.isOpen() || !_f.getModifyDateTime(&d, &t)) return 0;
    return fatToUnixTime(d, t);
  }

  const char *path() const override {
    return _path.c_str();
  }

  const char *name() const override {
    return _name.c_str();
  }

  boolean isDirectory(void) override {
    VolLock lk(V(_vol));
    return alive() && _f.isOpen() && _f.isDir();
  }

  FileImplPtr openNextFile(const char *mode) override;

  boolean seekDir(long position) override {
    VolLock lk(V(_vol));
    if (!alive() || !_f.isOpen() || !_f.isDir()) return false;
    return _f.seekSet((uint64_t)position);
  }

  String getNextFileName(void) override {
    return getNextFileName(nullptr);
  }

  String getNextFileName(bool *isDir) override {
    VolLock lk(V(_vol));
    if (!alive() || !_f.isOpen() || !_f.isDir()) return String("");
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
    VolLock lk(V(_vol));
    if (alive() && _f.isOpen() && _f.isDir()) _f.rewind();
  }

  operator bool() override {
    VolLock lk(V(_vol));
    return alive() && _f.isOpen();
  }

private:
  mutable FsFile _f;
  String _path;
  String _name;
  int _vol;
  uint32_t _gen;
};

// Union directory. Only built when a directory exists on both volumes. It walks each
// volume's handle in priority order and skips a name the higher-priority volume already
// returned, so a file present on both appears once, from the card.

class NomadUnionDirImpl : public FileImpl {
public:
  explicit NomadUnionDirImpl(const String &fullPath) : _path(fullPath) {
    int idx = _path.lastIndexOf('/');
    _name = (idx >= 0 && idx + 1 < (int)_path.length()) ? _path.substring(idx + 1) : _path;
    openAll();
  }

  ~NomadUnionDirImpl() override {
    close();
  }

  size_t write(const uint8_t *, size_t) override { return 0; }
  size_t read(uint8_t *, size_t) override { return 0; }
  void flush() override {}
  bool seek(uint32_t, SeekMode) override { return false; }
  size_t position() const override { return 0; }
  size_t size() const override { return 0; }
  bool setBufferSize(size_t) override { return false; }
  time_t getLastWrite() override { return 0; }
  boolean seekDir(long) override { return false; }
  boolean isDirectory(void) override { return true; }
  const char *path() const override { return _path.c_str(); }
  const char *name() const override { return _name.c_str(); }
  operator bool() override { return _open; }

  void close() override {
    for (int i = 0; i < NOMAD_VOL_COUNT; ++i) {
      if (!_have[i]) continue;
      VolLock lk(V(i));
      if (!aliveOn(i)) continue;
      if (_d[i].isOpen()) _d[i].close();
    }
    _open = false;
  }

  void rewindDirectory(void) override {
    for (int i = 0; i < NOMAD_VOL_COUNT; ++i) {
      if (!_have[i]) continue;
      VolLock lk(V(i));
      if (!aliveOn(i)) continue;
      if (_d[i].isOpen()) _d[i].rewind();
    }
    _cur = 0;
    _seen.clear();
  }

  FileImplPtr openNextFile(const char *mode) override;

  String getNextFileName(void) override { return getNextFileName(nullptr); }

  String getNextFileName(bool *isDir) override {
    String childPath;
    bool dir = false;
    if (!nextName(childPath, dir)) return String("");
    if (isDir) *isDir = dir;
    return childPath;
  }

private:
  // Same generation rule as the single-volume handle, per side of the union: half of a
  // union listing lives on a drive that can be pulled while the listing is being walked.
  bool aliveOn(int i) const {
    if (volLive(i, _gen[i])) return true;
    if (_have[i] && _d[i].isOpen()) abandonFsFile(_d[i]);
    return false;
  }

  void openAll() {
    for (int i = 0; i < NOMAD_VOL_COUNT; ++i) {
      _have[i] = false;
      _gen[i] = 0;
      if (!s_vols[i].mounted) continue;
      VolLock lk(V(i));
      if (!s_vols[i].vol.exists(_path.c_str())) continue;
      if (_d[i].open(&s_vols[i].vol, _path.c_str(), O_RDONLY) && _d[i].isDir()) {
        _have[i] = true;
        _gen[i] = s_vols[i].gen;
        _open = true;
      } else if (_d[i].isOpen()) {
        _d[i].close();
      }
    }
  }

  // Advance to the next not-yet-seen entry. Leaves the child handle open in `out`
  // when `wantHandle`, otherwise closes it and only reports the name.
  bool advance(FsFile *out, String &childPath, bool &isDir, int &volOut) {
    while (_cur < NOMAD_VOL_COUNT) {
      if (!_have[_cur]) {
        _cur++;
        continue;
      }
      VolLock lk(V(_cur));
      if (!aliveOn(_cur)) {   // this half went away mid-listing
        _cur++;
        continue;
      }
      FsFile child;
      if (!child.openNext(&_d[_cur], O_RDONLY)) {
        _cur++;
        continue;
      }
      char nbuf[512];
      nbuf[0] = 0;
      child.getName(nbuf, sizeof(nbuf));
      std::string key(nbuf);
      if (_seen.count(key)) {  // the card already offered this name
        child.close();
        continue;
      }
      _seen.insert(key);
      isDir = child.isDir();
      childPath = (_path == "/") ? String("/") + nbuf : _path + "/" + nbuf;
      volOut = _cur;
      if (out) {
        *out = std::move(child);
      } else {
        child.close();
      }
      return true;
    }
    return false;
  }

  bool nextName(String &childPath, bool &isDir) {
    int v = 0;
    return advance(nullptr, childPath, isDir, v);
  }

  String _path;
  String _name;
  mutable FsFile _d[NOMAD_VOL_COUNT];
  bool _have[NOMAD_VOL_COUNT] = {false};
  uint32_t _gen[NOMAD_VOL_COUNT] = {0};
  bool _open = false;
  int _cur = 0;
  std::set<std::string> _seen;

  friend class NomadFSImpl;
};

// A child directory reached through a union listing may itself exist on both volumes.
// Handing back a single-volume handle there would drop half of /Movies/Foo.
static FileImplPtr makeDirImpl(FsFile f, const String &path, int vol) {
  if (dirVolumeCount(path) > 1) {
    if (f.isOpen()) {
      VolLock lk(V(vol));
      f.close();
    }
    return std::make_shared<NomadUnionDirImpl>(path);
  }
  return std::make_shared<NomadFileImpl>(std::move(f), path, vol);
}

FileImplPtr NomadFileImpl::openNextFile(const char *mode) {
  (void)mode;  // directory iteration in this firmware is read-only
  FsFile child;
  String childPath;
  bool isDir = false;
  {
    VolLock lk(V(_vol));
    if (!alive() || !_f.isOpen() || !_f.isDir()) return FileImplPtr();
    if (!child.openNext(&_f, O_RDONLY)) return FileImplPtr();
    char nbuf[512];
    nbuf[0] = 0;
    child.getName(nbuf, sizeof(nbuf));
    isDir = child.isDir();
    childPath = (_path == "/") ? String("/") + nbuf : _path + "/" + nbuf;
  }
  if (isDir) return makeDirImpl(std::move(child), childPath, _vol);
  return std::make_shared<NomadFileImpl>(std::move(child), childPath, _vol);
}

FileImplPtr NomadUnionDirImpl::openNextFile(const char *mode) {
  (void)mode;
  FsFile child;
  String childPath;
  bool isDir = false;
  int vol = 0;
  if (!advance(&child, childPath, isDir, vol)) return FileImplPtr();
  if (isDir) return makeDirImpl(std::move(child), childPath, vol);
  return std::make_shared<NomadFileImpl>(std::move(child), childPath, vol);
}

// ---------------------------------------------------------------- FSImpl

class NomadFSImpl : public FSImpl {
public:
  FileImplPtr open(const char *path, const char *mode, const bool create) override {
    String p = normalizePath(path);
    oflag_t of = modeToOflag(mode);
    const bool writing = (of & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) != 0;

    int v = writing ? resolveForWrite(p) : resolveVolume(p);
    if (v == NOMAD_VOL_NONE) {
      // A read of a path on no volume, or any access with nothing mounted.
      if (!writing) return FileImplPtr();
      return FileImplPtr();
    }

    // A directory carried by both volumes has to be listed as one.
    if (!writing && dirVolumeCount(p) > 1) {
      auto d = std::make_shared<NomadUnionDirImpl>(p);
      if (!(bool)*d) return FileImplPtr();
      return d;
    }

    VolLock lk(V(v));
    if ((of & O_CREAT) && create) {
      // fs::FS::open(path, mode, true) promises missing parent folders
      int idx = p.lastIndexOf('/');
      if (idx > 0) {
        String parent = p.substring(0, idx);
        if (!V(v).vol.exists(parent.c_str())) V(v).vol.mkdir(parent.c_str(), true);
      }
    }
    FsFile f = V(v).vol.open(p.c_str(), of);
    if (!f.isOpen()) return FileImplPtr();
    return std::make_shared<NomadFileImpl>(std::move(f), p, v);
  }

  bool exists(const char *path) override {
    String p = normalizePath(path);
    if (p == "/") return writeVolume() != NOMAD_VOL_NONE;
    return resolveVolume(p) != NOMAD_VOL_NONE;
  }

  bool rename(const char *pathFrom, const char *pathTo) override {
    String from = normalizePath(pathFrom);
    int v = resolveVolume(from);
    if (v == NOMAD_VOL_NONE) return false;
    // Renaming across volumes would be a copy. SdFat can't, and the callers (index swap,
    // admin rename) never need it. Keep it on one volume.
    VolLock lk(V(v));
    return V(v).vol.rename(from.c_str(), normalizePath(pathTo).c_str());
  }

  bool remove(const char *path) override {
    String p = normalizePath(path);
    int v = resolveVolume(p);
    if (v == NOMAD_VOL_NONE) return false;
    VolLock lk(V(v));
    return V(v).vol.remove(p.c_str());
  }

  bool mkdir(const char *path) override {
    String p = normalizePath(path);
    int v = resolveForWrite(p);
    if (v == NOMAD_VOL_NONE) return false;
    VolLock lk(V(v));
    if (V(v).vol.exists(p.c_str())) {
      FsFile f = V(v).vol.open(p.c_str(), O_RDONLY);
      bool isdir = f.isOpen() && f.isDir();
      f.close();
      return isdir;  // match VFS: mkdir on an existing dir succeeds
    }
    return V(v).vol.mkdir(p.c_str(), true);
  }

  bool rmdir(const char *path) override {
    String p = normalizePath(path);
    int v = resolveVolume(p);
    if (v == NOMAD_VOL_NONE) return false;
    VolLock lk(V(v));
    return V(v).vol.rmdir(p.c_str());
  }
};

// NomadSDFS

NomadSDFS::NomadSDFS() : FS(FSImplPtr(new NomadFSImpl())) {
  s_vols[NOMAD_VOL_SD].label = "SD";
  s_vols[NOMAD_VOL_SD].dev = &s_sdDev;
  s_vols[NOMAD_VOL_USB].label = "USB";
  s_vols[NOMAD_VOL_USB].dev = &s_usbDev;
}

uint64_t NomadSDFS::fileSize64(const char *path) {
  String p = normalizePath(path);
  int v = resolveVolume(p);
  if (v == NOMAD_VOL_NONE) return 0;
  VolLock lk(V(v));
  FsFile f;
  if (!f.open(&V(v).vol, p.c_str(), O_RDONLY)) return 0;
  uint64_t sz = f.isDir() ? 0 : f.fileSize();
  f.close();
  return sz;
}

bool NomadSDFS::fileStat64(const char *path, uint64_t &size, time_t &mtime) {
  const String p = normalizePath(path);
  const int v = resolveVolume(p);
  if (v == NOMAD_VOL_NONE) return false;
  VolLock lk(V(v));
  if (!V(v).mounted) return false;
  FsFile f;
  if (!f.open(&V(v).vol, p.c_str(), O_RDONLY)) return false;
  const bool file = !f.isDir();
  uint16_t d = 0, t = 0;
  size = file ? f.fileSize() : 0;
  mtime = (file && f.getModifyDateTime(&d, &t)) ? fatToUnixTime(d, t) : 0;
  f.close();
  return file;
}

int NomadSDFS::volumeOf(const char *path) {
  return resolveVolume(normalizePath(path));
}

bool NomadSDFS::existsOn(int vol, const char *path) {
  if (!volumeMounted(vol)) return false;
  return volHas(V(vol), normalizePath(path));
}

// Both of these keep going after a failure on one volume: a half-deleted
// pair is worse than either outcome, and the count is what the caller reports.
int NomadSDFS::removeAll(const char *path) {
  const String p = normalizePath(path);
  int n = 0;
  for (int i = 0; i < NOMAD_VOL_COUNT; ++i) {
    if (!volHas(s_vols[i], p)) continue;
    VolLock lk(V(i));
    if (s_vols[i].vol.remove(p.c_str())) n++;
  }
  return n;
}

int NomadSDFS::rmdirAll(const char *path) {
  const String p = normalizePath(path);
  int n = 0;
  for (int i = 0; i < NOMAD_VOL_COUNT; ++i) {
    if (!isDirOn(s_vols[i], p)) continue;
    VolLock lk(V(i));
    if (s_vols[i].vol.rmdir(p.c_str())) n++;
  }
  return n;
}

int NomadSDFS::soleVolumeOfDir(const char *path) {
  const String p = normalizePath(path);
  int found = NOMAD_VOL_NONE, n = 0;
  for (int i = 0; i < NOMAD_VOL_COUNT; ++i) {
    if (!isDirOn(s_vols[i], p)) continue;
    if (n == 0) found = i;
    n++;
  }
  return (n == 1) ? found : NOMAD_VOL_NONE;
}

bool NomadSDFS::setPins(int clk, int cmd, int d0, int d1, int d2, int d3) {
#if NOMAD_SD_IOMUX
  // Slot 0's pins are fixed in silicon. Accept the call so the sketch is board-agnostic,
  // but say so loudly if somebody passes pins that are not the ones the SoC will use.
  (void)d1; (void)d2; (void)d3;
  if (clk != NOMAD_SD_CLK || cmd != NOMAD_SD_CMD || d0 != NOMAD_SD_D0) {
    log_w("NomadSD: this board's SD slot is IOMUX-fixed; ignoring requested pins");
  }
  return true;
#else
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
#endif
}

bool NomadSDFS::begin(const char *mountpoint, bool mode1bit, bool format_if_mount_failed, int sdmmc_frequency, uint8_t maxOpenFiles) {
  (void)format_if_mount_failed;  // not supported: auto-format can wipe a valid card it just failed to read
  (void)maxOpenFiles;
  NomadVol &v = V(NOMAD_VOL_SD);
  if (!v.lock) v.lock = xSemaphoreCreateRecursiveMutex();
  VolLock lk(v);
  if (v.mounted) return true;
  if (!cardInit(mode1bit, sdmmc_frequency)) return false;
  FsDateTime::setCallback(nomadDateTimeCallback);
  // partition 1 (normal SD layout), then superfloppy (filesystem at sector 0)
  if (!v.vol.begin(&s_sdDev, true, 1) && !v.vol.begin(&s_sdDev, true, 0)) {
    log_e("NomadSD: card has no exFAT/FAT32/FAT16 filesystem I can read");
    return false;
  }
  v.mounted = true;
  v.gen++;
  _impl->mountpoint(mountpoint);
  log_i("NomadSD: mounted %s volume on the card - %u clusters of %u bytes",
        volumeFsTypeName(NOMAD_VOL_SD), (unsigned)v.vol.clusterCount(),
        (unsigned)volClusterBytes(v.vol));
  return true;
}

bool NomadSDFS::beginRaw(bool mode1bit, int sdmmc_frequency) {
  NomadVol &v = V(NOMAD_VOL_SD);
  if (!v.lock) v.lock = xSemaphoreCreateRecursiveMutex();
  VolLock lk(v);
  return cardInit(mode1bit, sdmmc_frequency);
}

void NomadSDFS::end() {
  NomadVol &v = V(NOMAD_VOL_SD);
  if (!v.lock) return;
  VolLock lk(v);
  if (v.mounted) {
    v.mounted = false;
    v.gen++;      // invalidate every handle still holding this volume's clusters
    v.vol.end();  // and make the volume itself refuse anything that gets past that
  }
  if (s_cardInited) {
    s_cardInited = false;  // cleared first: perimanClearPinBus re-enters end() via detach
    sdmmc_host_deinit();
    perimanClearPinBus(NOMAD_SD_CMD);
    perimanClearPinBus(NOMAD_SD_CLK);
    perimanClearPinBus(NOMAD_SD_D0);
    if (!s_mode1bit) {
      perimanClearPinBus(NOMAD_SD_D1);
      perimanClearPinBus(NOMAD_SD_D2);
      perimanClearPinBus(NOMAD_SD_D3);
    }
    if (!V(NOMAD_VOL_USB).mounted) _impl->mountpoint(NULL);
  }
}

// USB volume

bool NomadSDFS::beginUSB() {
#if NOMAD_HAS_USB_HOST
  NomadVol &v = V(NOMAD_VOL_USB);
  if (!v.lock) v.lock = xSemaphoreCreateRecursiveMutex();
  VolLock lk(v);
  if (v.mounted) return true;
  if (!nomadUsbReady() || nomadUsbSectorCount() == 0) return false;
  // Everything above SdFat here (the FAT32 FSInfo probe below, SdFat's own cache)
  // works in 512-byte sectors, into 512-byte buffers. A 4Kn drive would transfer 4096
  // bytes into them. Refuse it in one place rather than smash a stack buffer later.
  if (nomadUsbSectorSize() != 512) {
    log_e("NomadSD: the USB device uses %u-byte sectors; only 512 is supported",
          (unsigned)nomadUsbSectorSize());
    return false;
  }
  FsDateTime::setCallback(nomadDateTimeCallback);
  if (!v.vol.begin(&s_usbDev, true, 1) && !v.vol.begin(&s_usbDev, true, 0)) {
    log_e("NomadSD: the USB device has no exFAT/FAT32/FAT16 filesystem I can read");
    return false;
  }
  v.mounted = true;
  v.gen++;
  _impl->mountpoint("/sdcard");
  log_i("NomadSD: mounted %s volume over USB, %u clusters of %u bytes",
        volumeFsTypeName(NOMAD_VOL_USB), (unsigned)v.vol.clusterCount(),
        (unsigned)volClusterBytes(v.vol));
  return true;
#else
  return false;
#endif
}

void NomadSDFS::endUSB() {
  NomadVol &v = V(NOMAD_VOL_USB);
  if (!v.lock) return;
  VolLock lk(v);
  if (!v.mounted) return;
  v.mounted = false;
  v.gen++;      // handles held across the unplug stop describing anything
  v.vol.end();  // and beginUSB() starts the next drive from a clean volume
  if (!V(NOMAD_VOL_SD).mounted) _impl->mountpoint(NULL);
}

bool NomadSDFS::usbMounted() {
  return V(NOMAD_VOL_USB).mounted;
}

// Reporting

sdcard_type_t NomadSDFS::cardType() {
  if (!s_cardInited) return CARD_NONE;
  return (s_card.ocr & SD_OCR_SDHC_CAP) ? CARD_SDHC : CARD_SD;
}

uint64_t NomadSDFS::cardSize() {
  if (!s_cardInited) return 0;
  return (uint64_t)s_card.csd.capacity * s_card.csd.sector_size;
}

bool NomadSDFS::volumeMounted(int vol) {
  return vol >= 0 && vol < NOMAD_VOL_COUNT && s_vols[vol].mounted;
}

const char *NomadSDFS::volumeLabel(int vol) {
  return (vol >= 0 && vol < NOMAD_VOL_COUNT) ? s_vols[vol].label : "?";
}

uint64_t NomadSDFS::volumeTotalBytes(int vol) {
  if (!volumeMounted(vol)) return 0;
  NomadVol &v = V(vol);
  VolLock lk(v);
  return (uint64_t)v.vol.clusterCount() * volClusterBytes(v.vol);
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

static bool fat32FSInfoFreeClusters(NomadVol &v, uint32_t &freeOut) {
  uint8_t buf[512];
  if (!v.dev->readSector(0, buf)) return false;
  uint32_t volStart = 0;
  if (!looksLikeFat32BootSector(buf)) {
    // not superfloppy: sector 0 is an MBR, take partition 1
    if (buf[510] != 0x55 || buf[511] != 0xAA) return false;
    const uint8_t *pe = buf + 446;
    uint32_t lba = (uint32_t)pe[8] | ((uint32_t)pe[9] << 8) | ((uint32_t)pe[10] << 16) | ((uint32_t)pe[11] << 24);
    if (pe[4] == 0 || lba == 0) return false;
    volStart = lba;
    if (!v.dev->readSector(volStart, buf)) return false;
    if (!looksLikeFat32BootSector(buf)) return false;
  }
  uint16_t fsInfoSec = (uint16_t)buf[48] | ((uint16_t)buf[49] << 8);  // BPB_FSInfo
  if (fsInfoSec == 0 || fsInfoSec == 0xFFFF) return false;
  if (!v.dev->readSector(volStart + fsInfoSec, buf)) return false;
  uint32_t lead = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
  uint32_t sig = (uint32_t)buf[484] | ((uint32_t)buf[485] << 8) | ((uint32_t)buf[486] << 16) | ((uint32_t)buf[487] << 24);
  if (lead != 0x41615252 || sig != 0x61417272 || buf[510] != 0x55 || buf[511] != 0xAA) return false;
  uint32_t freeCount = (uint32_t)buf[488] | ((uint32_t)buf[489] << 8) | ((uint32_t)buf[490] << 16) | ((uint32_t)buf[491] << 24);
  if (freeCount == 0xFFFFFFFF || freeCount > v.vol.clusterCount()) return false;
  freeOut = freeCount;
  return true;
}

uint64_t NomadSDFS::volumeUsedBytes(int vol) {
  if (!volumeMounted(vol)) return 0;
  NomadVol &v = V(vol);
  VolLock lk(v);
  const uint64_t clusterBytes = volClusterBytes(v.vol);
  uint32_t freeFromFsInfo = 0;
  if (v.vol.fatType() == 32 && fat32FSInfoFreeClusters(v, freeFromFsInfo)) {
    return ((uint64_t)v.vol.clusterCount() - (uint64_t)freeFromFsInfo) * clusterBytes;
  }
  // exFAT allocation-bitmap scan is quick. A FAT32 card without valid FSInfo
  // pays the full FAT walk once, then SdFat maintains the count incrementally
  int32_t freeClusters = v.vol.freeClusterCount();
  if (freeClusters < 0) return 0;
  return ((uint64_t)v.vol.clusterCount() - (uint64_t)freeClusters) * clusterBytes;
}

// The storage bar and the admin panel describe one library, so the bare calls report
// the union. Per-volume figures are still available for the breakdown.
uint64_t NomadSDFS::totalBytes() {
  uint64_t t = 0;
  for (int i = 0; i < NOMAD_VOL_COUNT; ++i) t += volumeTotalBytes(i);
  return t;
}

uint64_t NomadSDFS::usedBytes() {
  uint64_t u = 0;
  for (int i = 0; i < NOMAD_VOL_COUNT; ++i) u += volumeUsedBytes(i);
  return u;
}

int NomadSDFS::sectorSize() {
  if (!s_cardInited) return 0;
  return s_card.csd.sector_size;
}

int NomadSDFS::numSectors() {
  if (!s_cardInited) return 0;
  return s_card.csd.capacity;
}

bool NomadSDFS::readRAW(uint8_t *buffer, uint32_t sector) {
  return s_sdDev.readSector(sector, buffer);
}

bool NomadSDFS::writeRAW(uint8_t *buffer, uint32_t sector) {
  return s_sdDev.writeSector(sector, buffer);
}

bool NomadSDFS::readRAWMulti(uint8_t *buffer, uint32_t sector, size_t count) {
  return s_sdDev.readSectors(sector, buffer, count);
}

bool NomadSDFS::writeRAWMulti(const uint8_t *buffer, uint32_t sector, size_t count) {
  return s_sdDev.writeSectors(sector, buffer, count);
}

// Volume-explicit writes

// The parent directory's volume, which is the one a new file must be created on.
// resolveVolume() on the file itself answers NONE while the file does not exist yet,
// which is when a write needs the answer.
int NomadSDFS::volumeOfParent(const char *path) {
  String p = normalizePath(path);
  int idx = p.lastIndexOf('/');
  String parent = (idx > 0) ? p.substring(0, idx) : String("/");
  if (parent.length() == 0) parent = "/";
  for (int i = 0; i < NOMAD_VOL_COUNT; ++i)
    if (isDirOn(s_vols[i], parent)) return i;
  return NOMAD_VOL_NONE;
}

int NomadSDFS::volumeForNewPath(const char *path) {
  String p = normalizePath(path);
  int idx = p.lastIndexOf('/');
  while (idx > 0) {
    String anc = p.substring(0, idx);
    for (int i = 0; i < NOMAD_VOL_COUNT; ++i)
      if (isDirOn(s_vols[i], anc)) return i;
    idx = anc.lastIndexOf('/');
  }
  for (int i = 0; i < NOMAD_VOL_COUNT; ++i)
    if (s_vols[i].mounted) return i;   // nothing above it exists: the root's volume
  return NOMAD_VOL_NONE;
}

bool NomadSDFS::mkdirOn(int vol, const char *path) {
  if (!volumeMounted(vol)) return false;
  String p = normalizePath(path);
  VolLock lk(V(vol));
  if (V(vol).vol.exists(p.c_str())) {
    FsFile f = V(vol).vol.open(p.c_str(), O_RDONLY);
    bool isdir = f.isOpen() && f.isDir();
    f.close();
    return isdir;   // match mkdir(): an existing directory is success
  }
  return V(vol).vol.mkdir(p.c_str(), true);
}

bool NomadSDFS::removeOn(int vol, const char *path) {
  if (!volumeMounted(vol)) return false;
  String p = normalizePath(path);
  VolLock lk(V(vol));
  return V(vol).vol.remove(p.c_str());
}

bool NomadSDFS::rmdirOn(int vol, const char *path) {
  if (!volumeMounted(vol)) return false;
  String p = normalizePath(path);
  VolLock lk(V(vol));
  return V(vol).vol.rmdir(p.c_str());
}

bool NomadSDFS::renameOn(int vol, const char *from, const char *to) {
  if (!volumeMounted(vol)) return false;
  String f = normalizePath(from), t = normalizePath(to);
  VolLock lk(V(vol));
  return V(vol).vol.rename(f.c_str(), t.c_str());
}

fs::File NomadSDFS::openOn(int vol, const char *path, const char *mode, bool createParents) {
  if (!volumeMounted(vol)) return fs::File();
  String p = normalizePath(path);
  oflag_t of = modeToOflag(mode);
  VolLock lk(V(vol));
  if ((of & O_CREAT) && createParents) {
    int idx = p.lastIndexOf('/');
    if (idx > 0) {
      String parent = p.substring(0, idx);
      if (!V(vol).vol.exists(parent.c_str())) V(vol).vol.mkdir(parent.c_str(), true);
    }
  }
  FsFile f = V(vol).vol.open(p.c_str(), of);
  if (!f.isOpen()) return fs::File();
  return fs::File(std::make_shared<NomadFileImpl>(std::move(f), p, vol));
}

const char *NomadSDFS::volumeFsTypeName(int vol) {
  if (!volumeMounted(vol)) return "none";
  NomadVol &v = V(vol);
  VolLock lk(v);
  switch (v.vol.fatType()) {
    case FAT_TYPE_EXFAT: return "exFAT";
    case 32: return "FAT32";
    case 16: return "FAT16";
    default: return "FAT?";
  }
}

const char *NomadSDFS::fsTypeName() {
  return volumeFsTypeName(NOMAD_VOL_SD);
}

bool NomadSDFS::isExFat() {
  NomadVol &v = V(NOMAD_VOL_SD);
  if (!v.mounted) return false;
  VolLock lk(v);
  return v.vol.fatType() == FAT_TYPE_EXFAT;
}

NomadSDFS NomadSD;

// storage health

void nomadSDStats(NomadSDStats &out) {
  VolLock lk(V(NOMAD_VOL_SD));
  out = s_stats;
}

bool nomadSDCardInfo(NomadSDCardInfo &out) {
  VolLock lk(V(NOMAD_VOL_SD));
  memset(&out, 0, sizeof(out));
  if (!s_cardInited) return false;
  out.ok = true;
  out.mid = s_card.cid.mfg_id;
  out.oid = s_card.cid.oem_id;
  out.rev = s_card.cid.revision;
  out.serial = (uint32_t)s_card.cid.serial;
  memcpy(out.name, s_card.cid.name, sizeof(out.name) - 1);
  // SD CID MDT: year since 2000 in the high 8 bits, month in the low 4
  out.year = 2000 + ((s_card.cid.date >> 4) & 0xFF);
  out.month = s_card.cid.date & 0xF;
  out.bytes = (uint64_t)s_card.csd.capacity * s_card.csd.sector_size;
  out.freqKhz = s_card.real_freq_khz;
  out.busWidth = s_mode1bit ? 1 : 4;   // how the host drives it, not what the card could do
  out.sdhc = (s_card.ocr & SD_OCR_SDHC_CAP) != 0;
  out.uhs1 = s_card.is_uhs1;
  out.allocUnitKb = s_card.ssr.alloc_unit_kb;
  return true;
}
