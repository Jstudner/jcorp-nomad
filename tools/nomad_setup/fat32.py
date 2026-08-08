"""A minimal, dependency-free FAT32 formatter.

Why this exists: Nomad mounts the card through ESP-IDF's FATFS, which is built
without exFAT support, so the card *must* be FAT32. Every OS can make a FAT32
volume except the one most people are using - Windows refuses to format
anything over 32 GB as FAT32, and 64 GB cards are the common case. Rather than
sending people off to download a random fat32format.exe, the tool writes the
filesystem itself when the OS declines.

On Linux and macOS the native tools are used instead; this module is the
fallback path. It writes structures only, never file data: once the volume
exists the OS mounts it and the template is copied with ordinary file I/O.

Reference: Microsoft FAT32 File System Specification (fatgen103).
"""

from __future__ import annotations

import os
import struct
from dataclasses import dataclass
from typing import BinaryIO

SECTOR = 512
RESERVED_SECTORS = 32
NUM_FATS = 2
ROOT_CLUSTER = 2
FSINFO_SECTOR = 1
BACKUP_BOOT_SECTOR = 6

# Below this many clusters a volume is FAT16 by definition, and drivers will
# read it as FAT16 no matter what the boot sector claims.
MIN_FAT32_CLUSTERS = 65525
MAX_FAT32_CLUSTERS = 0x0FFFFFF5


class Fat32Error(RuntimeError):
    pass


def cluster_sectors_for(total_sectors: int) -> int:
    """Sectors per cluster, using the conventional Microsoft size table."""
    mb = (total_sectors * SECTOR) / (1024 * 1024)
    if mb < 32:
        raise Fat32Error("FAT32 needs a volume of at least 32 MB")
    if mb <= 260:
        return 1  # 512 B
    if mb <= 8192:
        return 8  # 4 KB
    if mb <= 16384:
        return 16  # 8 KB
    if mb <= 32768:
        return 32  # 16 KB
    return 64  # 32 KB


@dataclass
class Fat32Geometry:
    total_sectors: int
    sectors_per_cluster: int
    fat_sectors: int
    data_sectors: int
    cluster_count: int

    @property
    def cluster_bytes(self) -> int:
        return self.sectors_per_cluster * SECTOR

    @property
    def first_data_sector(self) -> int:
        return RESERVED_SECTORS + NUM_FATS * self.fat_sectors


def plan(total_sectors: int, sectors_per_cluster: int = 0) -> Fat32Geometry:
    """Work out FAT size and cluster count for a volume of this many sectors."""
    if total_sectors * SECTOR < 32 * 1024 * 1024:
        raise Fat32Error("Volume is too small for FAT32 (minimum 32 MB)")

    spc = sectors_per_cluster or cluster_sectors_for(total_sectors)

    while True:
        # fatgen103's FATSz32 estimate, then refined below.
        tmp1 = total_sectors - RESERVED_SECTORS
        tmp2 = ((256 * spc) + NUM_FATS) // 2
        fat_sectors = (tmp1 + tmp2 - 1) // tmp2

        data_sectors = total_sectors - RESERVED_SECTORS - NUM_FATS * fat_sectors
        cluster_count = data_sectors // spc

        # The estimate can be a sector or two generous. Shrink the FAT while it
        # still describes every cluster, so no space is wasted.
        while fat_sectors > 1:
            smaller = fat_sectors - 1
            d = total_sectors - RESERVED_SECTORS - NUM_FATS * smaller
            c = d // spc
            # +2 for the two reserved FAT entries, 4 bytes per entry.
            if (c + 2) * 4 > smaller * SECTOR:
                break
            fat_sectors = smaller
            data_sectors, cluster_count = d, c

        if cluster_count < MIN_FAT32_CLUSTERS:
            if spc == 1:
                raise Fat32Error(
                    "Volume is too small to hold a valid FAT32 filesystem "
                    "(fewer than 65525 clusters even at 512 B each)"
                )
            spc //= 2
            continue

        if cluster_count > MAX_FAT32_CLUSTERS:
            if spc >= 128:
                raise Fat32Error("Volume is too large for FAT32")
            spc *= 2
            continue

        return Fat32Geometry(
            total_sectors=total_sectors,
            sectors_per_cluster=spc,
            fat_sectors=fat_sectors,
            data_sectors=data_sectors,
            cluster_count=cluster_count,
        )


def _boot_sector(geo: Fat32Geometry, label: str, hidden_sectors: int, volume_id: int) -> bytes:
    lbl = (label.upper()[:11]).ljust(11).encode("ascii", "replace")

    bs = bytearray(SECTOR)
    bs[0:3] = b"\xeb\x58\x90"          # jump over the BPB
    bs[3:11] = b"MSWIN4.1"             # OEM name; the value everything expects
    struct.pack_into("<H", bs, 11, SECTOR)
    bs[13] = geo.sectors_per_cluster
    struct.pack_into("<H", bs, 14, RESERVED_SECTORS)
    bs[16] = NUM_FATS
    struct.pack_into("<H", bs, 17, 0)   # root entries: always 0 on FAT32
    struct.pack_into("<H", bs, 19, 0)   # total sectors 16-bit: unused
    bs[21] = 0xF8                       # media descriptor: fixed disk
    struct.pack_into("<H", bs, 22, 0)   # FATSz16: unused
    struct.pack_into("<H", bs, 24, 63)  # sectors per track (legacy CHS)
    struct.pack_into("<H", bs, 26, 255)  # heads (legacy CHS)
    struct.pack_into("<L", bs, 28, hidden_sectors)
    struct.pack_into("<L", bs, 32, geo.total_sectors)
    struct.pack_into("<L", bs, 36, geo.fat_sectors)
    struct.pack_into("<H", bs, 40, 0)   # ExtFlags: both FATs mirrored
    struct.pack_into("<H", bs, 42, 0)   # filesystem version
    struct.pack_into("<L", bs, 44, ROOT_CLUSTER)
    struct.pack_into("<H", bs, 48, FSINFO_SECTOR)
    struct.pack_into("<H", bs, 50, BACKUP_BOOT_SECTOR)
    bs[64] = 0x80                       # drive number
    bs[66] = 0x29                       # extended boot signature
    struct.pack_into("<L", bs, 67, volume_id)
    bs[71:82] = lbl
    bs[82:90] = b"FAT32   "
    struct.pack_into("<H", bs, 510, 0xAA55)
    return bytes(bs)


def _fsinfo_sector(free_clusters: int, next_free: int) -> bytes:
    fs = bytearray(SECTOR)
    struct.pack_into("<L", fs, 0, 0x41615252)
    struct.pack_into("<L", fs, 484, 0x61417272)
    struct.pack_into("<L", fs, 488, free_clusters)
    struct.pack_into("<L", fs, 492, next_free)
    struct.pack_into("<L", fs, 508, 0xAA550000)
    return bytes(fs)


ATTR_VOLUME_ID = 0x08


def _fat_timestamp() -> tuple:
    """(time, date) packed the way FAT directory entries want them."""
    import time as _time

    t = _time.localtime()
    year = max(t.tm_year, 1980)
    fat_date = ((year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    fat_time = (t.tm_hour << 11) | (t.tm_min << 5) | (min(t.tm_sec, 58) // 2)
    return fat_time, fat_date


def _volume_label_entry(label: str) -> bytes:
    """A root-directory entry carrying the volume label, then an end marker."""
    entry = bytearray(SECTOR)
    entry[0:11] = (label.upper()[:11]).ljust(11).encode("ascii", "replace")
    entry[11] = ATTR_VOLUME_ID
    fat_time, fat_date = _fat_timestamp()
    struct.pack_into("<H", entry, 14, fat_time)   # creation time
    struct.pack_into("<H", entry, 16, fat_date)   # creation date
    struct.pack_into("<H", entry, 18, fat_date)   # last access date
    struct.pack_into("<H", entry, 22, fat_time)   # write time
    struct.pack_into("<H", entry, 24, fat_date)   # write date
    # The rest of the sector stays zero, which is the end-of-directory marker.
    return bytes(entry)


def format_volume(
    fh: BinaryIO,
    total_sectors: int,
    label: str = "NOMAD",
    hidden_sectors: int = 0,
    base_offset: int = 0,
    volume_id: int | None = None,
    progress=None,
) -> Fat32Geometry:
    """Write a fresh FAT32 filesystem into an open volume.

    `fh` must be opened for binary read/write. `base_offset` is where the
    volume begins inside `fh` (0 when the handle is the partition itself,
    partition_start * 512 when it is the whole disk).
    """
    geo = plan(total_sectors)
    if volume_id is None:
        volume_id = int.from_bytes(os.urandom(4), "little") or 0x4E4F4D41

    boot = _boot_sector(geo, label, hidden_sectors, volume_id)
    # Root directory occupies one cluster and is the only thing allocated.
    free_clusters = geo.cluster_count - 1
    fsinfo = _fsinfo_sector(free_clusters, ROOT_CLUSTER + 1)

    def write_at(sector: int, data: bytes) -> None:
        fh.seek(base_offset + sector * SECTOR)
        fh.write(data)

    # Boot sector, FSInfo, and their backups at sector 6/7.
    write_at(0, boot)
    write_at(FSINFO_SECTOR, fsinfo)
    write_at(BACKUP_BOOT_SECTOR, boot)
    write_at(BACKUP_BOOT_SECTOR + FSINFO_SECTOR, fsinfo)

    # Remaining reserved sectors must be zeroed so stale data is not mistaken
    # for a filesystem.
    blank = b"\x00" * SECTOR
    for s in range(2, RESERVED_SECTORS):
        if s in (BACKUP_BOOT_SECTOR, BACKUP_BOOT_SECTOR + FSINFO_SECTOR):
            continue
        write_at(s, blank)

    # Both FATs: entry 0 = media descriptor, entry 1 = end marker,
    # entry 2 = end of the root directory chain. Everything else is free.
    fat_head = bytearray(SECTOR)
    struct.pack_into("<L", fat_head, 0, 0x0FFFFFF8)
    struct.pack_into("<L", fat_head, 4, 0x0FFFFFFF)
    struct.pack_into("<L", fat_head, 8, 0x0FFFFFFF)

    zero_chunk = b"\x00" * (SECTOR * 256)
    total_work = NUM_FATS * geo.fat_sectors + geo.sectors_per_cluster
    for fat_index in range(NUM_FATS):
        start = RESERVED_SECTORS + fat_index * geo.fat_sectors
        write_at(start, bytes(fat_head))
        remaining = geo.fat_sectors - 1
        sector = start + 1
        while remaining > 0:
            n = min(remaining, 256)
            fh.seek(base_offset + sector * SECTOR)
            fh.write(zero_chunk[: n * SECTOR])
            sector += n
            remaining -= n
            if progress:
                progress(n, total_work)

    # Zero the root directory cluster so it contains no stale entries.
    root_sector = geo.first_data_sector
    remaining = geo.sectors_per_cluster
    sector = root_sector
    while remaining > 0:
        n = min(remaining, 256)
        fh.seek(base_offset + sector * SECTOR)
        fh.write(zero_chunk[: n * SECTOR])
        sector += n
        remaining -= n
        if progress:
            progress(n, total_work)

    # The label lives in two places: the boot sector, and a volume-label entry
    # at the top of the root directory. Tools warn about a volume that only has
    # the first one, so write both.
    write_at(root_sector, _volume_label_entry(label))

    fh.flush()
    try:
        os.fsync(fh.fileno())
    except (OSError, AttributeError):
        pass
    return geo


# ----------------------------------------------------------------- MBR ----

MBR_ALIGN_SECTORS = 2048  # 1 MiB alignment, what every card formatter uses
PART_TYPE_FAT32_LBA = 0x0C


def _chs(lba: int) -> bytes:
    """Legacy CHS triple, saturated at the classic 1023/254/63 maximum."""
    heads, sectors = 255, 63
    c = lba // (heads * sectors)
    if c > 1023:
        return bytes((0xFE, 0xFF, 0xFF))
    h = (lba // sectors) % heads
    s = (lba % sectors) + 1
    return bytes((h, ((c >> 2) & 0xC0) | (s & 0x3F), c & 0xFF))


def write_mbr(fh: BinaryIO, part_start: int, part_sectors: int) -> None:
    """Write a single-partition MBR describing a FAT32 LBA partition."""
    mbr = bytearray(SECTOR)
    entry = bytearray(16)
    entry[0] = 0x00                    # not bootable
    entry[1:4] = _chs(part_start)
    entry[4] = PART_TYPE_FAT32_LBA
    entry[5:8] = _chs(part_start + part_sectors - 1)
    struct.pack_into("<L", entry, 8, part_start)
    struct.pack_into("<L", entry, 12, part_sectors)

    mbr[446:462] = entry
    struct.pack_into("<H", mbr, 510, 0xAA55)

    fh.seek(0)
    fh.write(bytes(mbr))
    fh.flush()
    try:
        os.fsync(fh.fileno())
    except (OSError, AttributeError):
        pass


def partition_plan(disk_sectors: int) -> tuple:
    """(start_lba, sector_count) for a single aligned partition filling a disk."""
    start = MBR_ALIGN_SECTORS
    if disk_sectors <= start:
        raise Fat32Error("Disk is too small to partition")
    count = disk_sectors - start
    # Trim to a whole multiple of the alignment so the end is tidy too.
    count -= count % MBR_ALIGN_SECTORS
    if count <= 0:
        raise Fat32Error("Disk is too small to partition")
    return start, count


def format_disk(fh: BinaryIO, disk_sectors: int, label: str = "NOMAD", progress=None):
    """Partition a whole raw disk with one FAT32 partition and format it."""
    start, count = partition_plan(disk_sectors)
    write_mbr(fh, start, count)
    geo = format_volume(
        fh,
        total_sectors=count,
        label=label,
        hidden_sectors=start,
        base_offset=start * SECTOR,
        progress=progress,
    )
    return start, count, geo
