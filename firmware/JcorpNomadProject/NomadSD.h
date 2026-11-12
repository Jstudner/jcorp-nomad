//<!-- Version 5.0 -->
// NomadSD.h - exFAT + FAT32 + FAT16 storage for Jcorp Nomad, across one or two volumes.
//
// Replaces SD_MMC with SdFat v2 on a custom block device. The Arduino core links a
// precompiled IDF FATFS built with FF_FS_EXFAT=0, which no sketch setting can change, so
// exFAT is impossible through SD_MMC or FFat. SdFat brings its own implementation and
// auto-detects the format at mount, so one firmware handles any card.
//
// It's a union of up to two volumes:
//
//   NOMAD_VOL_SD   the microSD card, on the SDMMC host        (primary when present)
//   NOMAD_VOL_USB  a USB mass-storage device, on the USB host (P4 only)
//
// Reads resolve against the primary first and fall through to the secondary, so
// /Movies/Foo.mkv is found wherever it lives. Listing a directory that exists on both
// volumes returns the union of their entries, deduplicated by name with the primary
// winning, so the browser sees one merged library. Writes always land on the primary.
// With only one volume mounted every path takes the fast path.
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

// same enumerators SD_MMC's sd_defines.h provided, the sketch tests CARD_NONE
typedef enum { CARD_NONE, CARD_MMC, CARD_SD, CARD_SDHC, CARD_UNKNOWN } sdcard_type_t;

// Which physical volume a path resolved to. Ordering is read-priority order.
enum NomadVolId { NOMAD_VOL_SD = 0, NOMAD_VOL_USB = 1, NOMAD_VOL_COUNT = 2, NOMAD_VOL_NONE = -1 };

// 64-bit read handle for files larger than 4 GB (exFAT only).
//
// fs::File is 32-bit end to end, so a 5 GB file reports its size modulo 4 GB and
// cannot seek past 4 GB. That API is fixed by the core, so the HTTP range handler
// uses this handle instead.
//
// The FsFile sits behind a void* because including SdFat's headers here would
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

  bool open(const char *path);   // read-only; resolves across volumes
  void close();
  bool isOpen() const;
  uint64_t size() const;
  uint64_t position() const;
  bool seek(uint64_t pos);
  size_t read(uint8_t *buf, size_t len);
  // seek and read in one lock acquisition. The HTTP range handler shares one handle
  // between everyone reading the same file, so it has to position the handle itself.
  // One call takes the SdFat lock once per chunk instead of three times.
  size_t readAt(uint64_t pos, uint8_t *buf, size_t len);
  int volume() const { return _vol; }  // NomadVolId this handle opened on
  explicit operator bool() const {
    return isOpen();
  }

private:
  // A USB drive can be pulled with this handle open, and the next drive's volume is
  // built in the same memory. Every entry point re-checks that the volume is still the
  // one it opened, and abandons the handle if it is not.
  bool alive() const;

  void *_h;  // FsFile*
  int _vol;
  uint32_t _gen;
};

class NomadSDFS : public fs::FS {
public:
  NomadSDFS();

  // 64-bit size without opening a handle, 0 if missing or unreadable
  uint64_t fileSize64(const char *path);

  // size and last-write time in one open, for ETags. false if missing, unreadable or a folder
  bool fileStat64(const char *path, uint64_t &size, time_t &mtime);

  // Which volume a path lives on, or NOMAD_VOL_NONE. Read-priority order.
  int volumeOf(const char *path);

  // Does this specific volume carry the path? A union hides which one does, and a
  // caller about to write, rename or refuse a name needs to ask about the volume it
  // is actually going to touch.
  bool existsOn(int vol, const char *path);

  // Remove from EVERY volume that carries the path, and answer how many that was.
  // fs::FS::remove() resolves one volume, and the union shows one entry for a name on
  // both the card and the drive, so remove() alone leaves a copy nobody can see to delete.
  int removeAll(const char *path);
  int rmdirAll(const char *path);

  // The one volume that services this directory, or NOMAD_VOL_NONE when more than one
  // does (a unioned directory) or none does. A listing of a directory that lives on a
  // single volume has every entry on that volume, so a caller can stamp the whole list
  // at once. Resolving each entry costs an exists() per volume per file, and a big
  // folder is a thousand of them.
  int soleVolumeOfDir(const char *path);

  // Volume-explicit writes.
  //
  // Everything above finds a volume by looking the path UP, which is wrong for a write.
  // A path that doesn't exist yet resolves to nothing, so resolveForWrite falls back to
  // the first mounted volume and a new file in a folder that only exists on USB would
  // land on the card.
  //
  // These take the volume as an argument and never guess. A cross-volume MOVE ends with
  // source and destination sharing one path on two volumes, so it needs removeOn:
  // removeAll() would delete BOTH copies.
  int volumeOfParent(const char *path);   // volume carrying this path's parent dir
  // Which volume a path that does NOT exist yet belongs on: the one carrying its nearest
  // existing ancestor (volumeOfParent answers NONE for a new nested path). A new sub-folder
  // of a USB folder made on the card would shadow the real one, reads go in volume order.
  // Walks up to the root, first match wins on a unioned ancestor.
  int volumeForNewPath(const char *path);
  bool mkdirOn(int vol, const char *path);
  bool removeOn(int vol, const char *path);
  bool rmdirOn(int vol, const char *path);
  bool renameOn(int vol, const char *from, const char *to);
  // Open on ONE named volume. `mode` is an fs::FS mode string, createParents works like
  // fs::FS::open(path, mode, true).
  fs::File openOn(int vol, const char *path, const char *mode, bool createParents = false);

  // microSD volume.

  // must be called before begin(); ignored on a board whose slot is IOMUX-fixed
  bool setPins(int clk, int cmd, int d0, int d1, int d2, int d3);

  // SD_MMC.begin-compatible signature so call sites compile untouched. mountpoint,
  // format_if_mount_failed and maxOpenFiles are accepted but ignored: SdFat has no
  // global handle limit, and auto-format on mount failure risked wiping a card we
  // merely failed to read.
  bool begin(const char *mountpoint = "/sdcard", bool mode1bit = true, bool format_if_mount_failed = false,
             int sdmmc_frequency = SDMMC_FREQ_DEFAULT, uint8_t maxOpenFiles = 5);

  // USB MSC device mode: card init only, raw sectors, no filesystem mount.
  // Works whatever format the card carries.
  bool beginRaw(bool mode1bit = false, int sdmmc_frequency = SDMMC_FREQ_HIGHSPEED);

  void end();

  // USB volume (P4).

  // Mount a USB mass-storage device that NomadUSB has already enumerated. With no device
  // attached it returns false and leaves the union with one volume.
  bool beginUSB();
  void endUSB();
  bool usbMounted();

  // Per-volume reporting. Bare calls report the primary, so existing admin/status code
  // is unchanged.

  sdcard_type_t cardType();
  uint64_t cardSize();
  uint64_t totalBytes();
  uint64_t usedBytes();
  bool volumeMounted(int vol);
  uint64_t volumeTotalBytes(int vol);
  uint64_t volumeUsedBytes(int vol);
  const char *volumeLabel(int vol);      // "SD" / "USB"
  const char *volumeFsTypeName(int vol);

  // raw sector access, microSD only (feeds USB MSC device mode)
  int sectorSize();
  int numSectors();
  bool readRAW(uint8_t *buffer, uint32_t sector);
  bool writeRAW(uint8_t *buffer, uint32_t sector);
  // burst variants for USB MSC: one SD command per host transfer instead of
  // one per 512-byte sector. The per-command overhead is what makes MSC slow
  bool readRAWMulti(uint8_t *buffer, uint32_t sector, size_t count);
  bool writeRAWMulti(const uint8_t *buffer, uint32_t sector, size_t count);

  // "exFAT" / "FAT32" / "FAT16" / "none", for boot logging
  const char *fsTypeName();
  bool isExFat();
};

extern NomadSDFS NomadSD;

// Storage health: what the card driver has seen since boot. Every sector read and write
// already passes through one place, so this costs a timer read and a few adds per call.
struct NomadSDStats {
  uint32_t reads, readErrors, writes, writeErrors;
  uint32_t slowReads;      // a read call that took over NOMAD_SD_SLOW_MS
  uint32_t maxReadUs;      // the slowest read call
  uint64_t sectorsRead, sectorsWritten;
  uint32_t lastErrorMs;    // millis() of the last failed read or write, 0 for none
};
static const uint32_t NOMAD_SD_SLOW_MS = 250;
void nomadSDStats(NomadSDStats &out);

// the card's own registers: who made it, when, and how the bus is running
struct NomadSDCardInfo {
  bool ok;
  int mid, oid, rev, year, month;
  uint32_t serial;
  char name[8];
  uint64_t bytes;
  int freqKhz, busWidth;
  bool sdhc, uhs1;
  uint32_t allocUnitKb;
};
bool nomadSDCardInfo(NomadSDCardInfo &out);

#endif  // NOMAD_SD_H
