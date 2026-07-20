# exFAT + FAT32 SD card support

Nomad firmware 4.6 replaces the Arduino `SD_MMC` filesystem stack with
**NomadSD** (`firmware/JcorpNomadProject/NomadSD.h/.cpp`), which mounts
**exFAT, FAT32 and FAT16** cards interchangeably, auto-detected at boot.

Cards larger than 32 GB work exactly as they come out of the box, since SDXC
cards ship exFAT. Existing FAT32 cards keep working unchanged: format support
is additive, not a migration.

## Why SD_MMC could never do exFAT

The Arduino-ESP32 core links a **precompiled** ESP-IDF FATFS library built with
`FF_FS_EXFAT = 0`. No sketch-level setting can re-enable it, so `SD_MMC`,
`FFat` and anything else on `esp_vfs_fat` is FAT32-only, permanently. Years of
attempts through that stack went nowhere because the limit lives in a `.a` file.

## How NomadSD works

```
sdmmc_host driver (same SDMMC peripheral + pins as before)
  -> SdMmcBlockDev        raw-sector block device (FsBlockDeviceInterface)
    -> SdFat FsVolume     auto-detects exFAT / FAT32 / FAT16 at mount
      -> fs::FSImpl wrap  standard Arduino fs::FS + fs::File
        -> NomadSD        drop-in for SD_MMC (`#define SD_MMC NomadSD`)
```

SdFat carries its own MIT-licensed FAT/exFAT implementation, so the locked IDF
FATFS is no longer used. The web server, indexer and every existing `SD_MMC.*`
call site work unchanged through `fs::FS`.

## Build requirements

- **SdFat library** >= 2.3.0 (Library Manager: "SdFat" by Bill Greiman).
- `build_opt.h` in the sketch folder, already in the repo. The ESP32 core picks
  it up automatically in both the IDE and arduino-cli. It sets
  `USE_BLOCK_DEVICE_INTERFACE=1`, UTF-8 long filenames, and incremental
  free-space accounting.
- Board settings and partition scheme are unchanged.

## What this buys

### Files larger than 4 GB stream

`fs::File` is 32-bit end to end: `size()`, `position()` and `seek()` all use
`size_t`, so a 5 GB file reported 0.88 GB and could not be seeked past 4 GB.
That API is fixed by the core.

`NomadFile64` (in `NomadSD.h`) is a 64-bit read handle used by the HTTP range
handler instead. Sizes come from `NomadSD.fileSize64()`, offsets parse with
`strtoull`, and the stream table stores `uint64_t`. Two related fixes:

- A range starting beyond EOF returns **416** with a `Content-Range`, per
  RFC 9110, instead of being silently clamped.
- Any single response body is capped at 256 MB so it fits `size_t`. A plain GET
  of a larger file degrades to **206** with an honest `Content-Range` rather
  than a truncated 200; clients fetch the next slice, which media players and
  the ZIM reader already do.

### `size()` saturates instead of wrapping

`NomadFileImpl::size()` returns `SIZE_MAX` past 4 GB rather than wrapping to a
small number. `SIZE_MAX` doubles as a sentinel: the indexer, file browser and
usage scanner check for it and only then pay for a `fileSize64()` lookup.
Normal files, and every file on a FAT32 card, cost nothing extra.

### The 2 GB stdio guard is gone

Mainline had to close and reopen a streaming handle crossing 2 GB, because
`fs::File` sat on newlib stdio whose signed 32-bit offset corrupted the FILE
buffer on a reused handle. SdFat does not use stdio and seeks 64-bit natively,
so deep seeks no longer pay a close/reopen, on exFAT and FAT32 alike.

### Fewer stalls on full cards

Creating a file no longer scans a huge FAT for free clusters, which was the
main aggravator behind `/save` and `/upload` watchdog resets on nearly-full
cards. Contiguous-file seeks are O(1) instead of a cluster-chain walk.

### File timestamps

An `FsDateTime` callback is installed at mount. Without one every file the
device writes gets an identical constant date. The device is offline with no
RTC, so this uses the fixed-epoch-plus-uptime approach already used for OPDS
timestamps: not wall time, but files written later in a session sort after
earlier ones.

## Behaviour notes

- The boot log and admin console report the detected filesystem and bus mode,
  e.g. `Mounted 4-bit @ 40MHz (exFAT)`.
- Mount tries 4-bit/40 MHz, then 4-bit/20 MHz, then 1-bit/20 MHz. The last rung
  matches the old behaviour, so any board or card that cannot do 4-bit still
  comes up.
- **USB mode no longer mounts a filesystem.** MSC passes raw sectors and the
  host PC handles the format. The old `format_if_mount_failed=true` in USB mode,
  which could silently reformat an exFAT card as FAT32, is gone.
- FAT32 free space is read from the FSInfo sector; exFAT uses the allocation
  bitmap. A card with broken FSInfo falls back to a one-time full FAT walk.
- All SD I/O is serialized by one recursive mutex inside NomadSD, replacing the
  FATFS `FF_FS_REENTRANT` volume lock.

## Verification

`arduino-cli` build: 50% flash, 42% RAM, 720 bytes below mainline.

Filesystem behaviour is covered by a host harness linking the same SdFat version
the firmware uses, through the same block-device interface, against real disk
images. All 30 checks pass:

- **FAT32**: mounts through the same part1-then-superfloppy order, finds root
  and nested files written by an external tool, reads correct content, reports
  exact sizes, seeks, iterates directories, and round-trips a new file.
- **exFAT**: mounts, reports correct volume geometry, creates a 5 GiB file,
  reports the full 64-bit size, seeks past 4 GiB, writes and reads back a marker
  there, and survives a reused handle crossing the 2 GiB and 4 GiB lines, the
  exact pattern that used to corrupt newlib stdio.

`fsck.exfat` reports the SdFat-written volume clean.

## Known issue: FAT32 free-space counter drifts

`fsck.vfat` on a FAT32 image after SdFat wrote to it reports:

```
Free cluster summary wrong (129815 vs. really 129814)
```

SdFat does not update the FAT32 **FSInfo** sector when it allocates clusters.
The old FATFS did, so this is a small regression on FAT32 cards only.

- **Scope**: drifts by exactly the clusters the device itself writes (index
  files, config, uploads). File data is unaffected and nothing risks corruption.
- **Visibility**: FSInfo is advisory. Windows and Linux recompute free space
  rather than trusting it; only `chkdsk`/`fsck` remarks on it, and any PC write
  resets it. The dirty bit is not set, so there is no "repair this drive" prompt.
- **Effect on us**: `usedBytes()` seeds from FSInfo for a fast boot and inherits
  the drift. The firmware will not trust that fast path until
  `reconcileStatTrust()` agrees with a real walk to within 2%.
- **Not applicable to exFAT**, which has no FSInfo.

Fixes if it ever matters: write a corrected FSInfo at unmount, or mark FSInfo
unknown (`0xFFFFFFFF`, explicitly legal) on first write and accept one full FAT
walk on the next boot. Neither is implemented; the current behaviour is bounded
and self-correcting.
