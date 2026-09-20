//<!-- Version 4.6 -->
// NomadSD.h - exFAT + FAT32 + FAT16 SD card support for Jcorp Nomad.
//
// Replaces SD_MMC with SdFat v2 on the ESP32-S3 SDMMC peripheral through a
// custom block device. The Arduino core links a precompiled IDF FATFS built
// with FF_FS_EXFAT=0, which no sketch setting can change, so exFAT is
// impossible through SD_MMC or FFat. SdFat brings its own implementation and
// auto-detects the format at mount, so one firmware handles any card.
//
// Exposes fs::FS, so every SD_MMC call site keeps working via
// `#define SD_MMC NomadSD`.
//
// Needs build_opt.h in the sketch folder (the Arduino IDE and arduino-cli both
// pick it up) and the SdFat library, tested with 2.3.0.

#ifndef NOMAD_SD_H
#define NOMAD_SD_H

#include <FS.h>
#include "sd_protocol_types.h"  // SDMMC_FREQ_DEFAULT / SDMMC_FREQ_HIGHSPEED

// same enumerators SD_MMC's sd_defines.h provided; the sketch tests CARD_NONE
typedef enum { CARD_NONE, CARD_MMC, CARD_SD, CARD_SDHC, CARD_UNKNOWN } sdcard_type_t;

// 64-bit read handle for files larger than 4 GB (exFAT only).
//
// fs::File is 32-bit end to end, so a 5 GB file reports its size modulo 4 GB and
// cannot seek past 4 GB. That API is fixed by the core, so the HTTP range handler
// uses this handle instead.
//
// The FsFile sits behind a void* deliberately: including SdFat's headers here would
// pull in its global `typedef FsFile File`, which collides with fs::File throughout
// the firmware. Move-only, like the FsFile it wraps.
class NomadFile64 {
public:
  NomadFile64();
  ~NomadFile64();
  NomadFile64(NomadFile64 &&other) noexcept;
  NomadFile64 &operator=(NomadFile64 &&other) noexcept;
  NomadFile64(const NomadFile64 &) = delete;
  NomadFile64 &operator=(const NomadFile64 &) = delete;

  bool open(const char *path);   // read-only
  void close();
  bool isOpen() const;
  uint64_t size() const;
  uint64_t position() const;
  bool seek(uint64_t pos);
  size_t read(uint8_t *buf, size_t len);
  // seek and read in one lock acquisition. The HTTP range handler shares one handle
  // between everyone reading the same file, so it has to position the handle itself,
  // and doing that as three separate calls took the SdFat lock three times per chunk.
  size_t readAt(uint64_t pos, uint8_t *buf, size_t len);
  explicit operator bool() const {
    return isOpen();
  }

private:
  void *_h;  // FsFile*
};

class NomadSDFS : public fs::FS {
public:
  NomadSDFS();

  // 64-bit size without opening a handle; 0 if missing or unreadable
  uint64_t fileSize64(const char *path);

  // must be called before begin(); same signature as SD_MMC.setPins
  bool setPins(int clk, int cmd, int d0, int d1, int d2, int d3);

  // SD_MMC.begin-compatible signature so call sites compile untouched. mountpoint,
  // format_if_mount_failed and maxOpenFiles are accepted but ignored: SdFat has no
  // global handle limit, and auto-format on mount failure risked wiping a card we
  // merely failed to read.
  bool begin(const char *mountpoint = "/sdcard", bool mode1bit = true, bool format_if_mount_failed = false,
             int sdmmc_frequency = SDMMC_FREQ_DEFAULT, uint8_t maxOpenFiles = 5);

  // USB MSC mode: card init only, raw sectors, no filesystem mount.
  // Works whatever format the card carries.
  bool beginRaw(bool mode1bit = false, int sdmmc_frequency = SDMMC_FREQ_HIGHSPEED);

  void end();

  sdcard_type_t cardType();
  uint64_t cardSize();
  uint64_t totalBytes();
  uint64_t usedBytes();
  int sectorSize();
  int numSectors();
  bool readRAW(uint8_t *buffer, uint32_t sector);
  bool writeRAW(uint8_t *buffer, uint32_t sector);
  // burst variants for USB MSC: one SD command per host transfer instead of
  // one per 512-byte sector - the per-command overhead is what made MSC slow
  bool readRAWMulti(uint8_t *buffer, uint32_t sector, size_t count);
  bool writeRAWMulti(const uint8_t *buffer, uint32_t sector, size_t count);

  // "exFAT" / "FAT32" / "FAT16" / "none" - for boot logging
  const char *fsTypeName();
  bool isExFat();
};

extern NomadSDFS NomadSD;

#endif  // NOMAD_SD_H
