"""Removable-disk discovery, partitioning, formatting and mounting.

Every destructive operation in here is gated behind an explicit disk choice and
a typed confirmation in the CLI layer - nothing auto-selects a target. Fixed
disks are filtered out by default and the disk holding the running OS is never
offered at all.

Native tooling does the work wherever it is trustworthy:

    Linux    wipefs + sfdisk + mkfs.vfat, mounted via udisksctl (no root
             needed for the mount) or plain mount as a fallback
    macOS    diskutil eraseDisk, which handles FAT32 at any card size
    Windows  diskpart, except that it refuses FAT32 above 32 GB - there the
             partition is made by diskpart and the filesystem is written by
             fat32.py directly to the locked volume

Only the Linux path has been exercised end to end during development; the macOS
and Windows paths follow each platform's documented behaviour but have not been
run on real hardware.
"""

from __future__ import annotations

import json
import os
import platform
import plistlib
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from typing import List, Optional

from . import console as c
from . import fat32

IS_LINUX = sys.platform.startswith("linux")
IS_MAC = sys.platform == "darwin"
IS_WINDOWS = sys.platform == "win32"

# Windows' own formatter tops out here; above it we write FAT32 ourselves.
WINDOWS_FAT32_LIMIT = 32 * 1024 * 1024 * 1024


class DiskError(RuntimeError):
    pass


@dataclass
class Disk:
    identifier: str                 # "/dev/sdb", "/dev/disk4", "2" on Windows
    raw_path: str                   # path for raw access
    description: str
    size: int
    removable: bool
    is_system: bool = False
    mountpoints: List[str] = field(default_factory=list)

    def summary(self) -> str:
        kind = "removable" if self.removable else c.yellow("FIXED")
        mounts = ", ".join(m for m in self.mountpoints if m)
        tail = f"  [{mounts}]" if mounts else ""
        return (
            f"{self.identifier:<16} {c.human_bytes(self.size):>9}  "
            f"{kind:<10} {self.description}{tail}"
        )


# ---------------------------------------------------------------- helpers --


def run(cmd, check=True, capture=True, dry_run=False, input_text=None, timeout=600):
    printable = " ".join(str(x) for x in cmd) if isinstance(cmd, (list, tuple)) else cmd
    if dry_run:
        c.info(c.dim(f"[dry-run] {printable}"))
        return subprocess.CompletedProcess(cmd, 0, "", "")
    c.debug(f"$ {printable}")
    proc = subprocess.run(
        cmd,
        check=False,
        capture_output=capture,
        text=True,
        input=input_text,
        shell=isinstance(cmd, str),
        timeout=timeout,
    )
    if check and proc.returncode != 0:
        raise DiskError(
            f"command failed ({proc.returncode}): {printable}\n"
            f"{(proc.stdout or '')}{(proc.stderr or '')}".strip()
        )
    return proc


def have(tool: str) -> bool:
    return shutil.which(tool) is not None


def is_privileged() -> bool:
    if IS_WINDOWS:
        try:
            import ctypes

            return bool(ctypes.windll.shell32.IsUserAnAdmin())
        except Exception:
            return False
    return os.geteuid() == 0


def privilege_hint() -> str:
    if IS_WINDOWS:
        return "Re-run this from an Administrator command prompt."
    if IS_MAC:
        return "Re-run with sudo."
    return "Re-run with sudo (or install udisks2 for the mount step)."


# ------------------------------------------------------------ enumeration --


def list_disks(include_fixed: bool = False) -> List[Disk]:
    if IS_LINUX:
        disks = _list_disks_linux()
    elif IS_MAC:
        disks = _list_disks_macos()
    elif IS_WINDOWS:
        disks = _list_disks_windows()
    else:
        raise DiskError(f"Unsupported platform: {platform.system()}")

    disks = [d for d in disks if not d.is_system]
    if not include_fixed:
        disks = [d for d in disks if d.removable]
    return disks


def _list_disks_linux() -> List[Disk]:
    if not have("lsblk"):
        raise DiskError("lsblk not found - install util-linux")
    proc = run(
        ["lsblk", "-J", "-b", "-o",
         "NAME,PATH,SIZE,TYPE,RM,HOTPLUG,MODEL,VENDOR,TRAN,MOUNTPOINTS"],
    )
    data = json.loads(proc.stdout)

    def mounts_of(node) -> List[str]:
        found = [m for m in (node.get("mountpoints") or []) if m]
        for child in node.get("children", []) or []:
            found += mounts_of(child)
        return found

    disks: List[Disk] = []
    for node in data.get("blockdevices", []):
        if node.get("type") != "disk":
            continue
        mounts = mounts_of(node)
        vendor = (node.get("vendor") or "").strip()
        model = (node.get("model") or "").strip()
        tran = (node.get("tran") or "").strip()
        desc = " ".join(x for x in (vendor, model) if x) or "unknown"
        if tran:
            desc += f" ({tran})"
        removable = bool(node.get("rm")) or bool(node.get("hotplug")) or tran in ("usb", "mmc")
        disks.append(
            Disk(
                identifier=node.get("path") or f"/dev/{node['name']}",
                raw_path=node.get("path") or f"/dev/{node['name']}",
                description=desc,
                size=int(node.get("size") or 0),
                removable=removable,
                is_system=any(m in ("/", "/boot", "/boot/efi") for m in mounts),
                mountpoints=mounts,
            )
        )
    return disks


def _list_disks_macos() -> List[Disk]:
    proc = run(["diskutil", "list", "-plist", "physical"])
    root = plistlib.loads(proc.stdout.encode())
    disks: List[Disk] = []
    for ident in root.get("WholeDisks", []):
        try:
            info_proc = run(["diskutil", "info", "-plist", f"/dev/{ident}"])
            info = plistlib.loads(info_proc.stdout.encode())
        except DiskError:
            continue
        mounts = []
        mp = info.get("MountPoint")
        if mp:
            mounts.append(mp)
        disks.append(
            Disk(
                identifier=f"/dev/{ident}",
                # rdiskN is the unbuffered node; far faster for bulk writes.
                raw_path=f"/dev/r{ident}",
                description=info.get("MediaName") or info.get("IORegistryEntryName") or "unknown",
                size=int(info.get("TotalSize") or info.get("Size") or 0),
                removable=bool(info.get("Removable") or info.get("RemovableMedia")
                               or info.get("Ejectable")),
                is_system=bool(info.get("SystemImage")) or info.get("MountPoint") == "/",
                mountpoints=mounts,
            )
        )
    return disks


_PS_LIST_DISKS = r"""
$ErrorActionPreference = 'Stop'
Get-Disk | ForEach-Object {
  $d = $_
  $mounts = @()
  try {
    $mounts = (Get-Partition -DiskNumber $d.Number -ErrorAction SilentlyContinue |
               Where-Object { $_.DriveLetter } |
               ForEach-Object { "$($_.DriveLetter):" })
  } catch {}
  [pscustomobject]@{
    Number       = $d.Number
    FriendlyName = $d.FriendlyName
    Size         = $d.Size
    BusType      = "$($d.BusType)"
    IsSystem     = [bool]$d.IsSystem
    IsBoot       = [bool]$d.IsBoot
    Mounts       = @($mounts)
  }
} | ConvertTo-Json -Depth 3
"""


def _powershell(script: str, dry_run=False, check=True):
    return run(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
        check=check,
        dry_run=dry_run,
    )


def _list_disks_windows() -> List[Disk]:
    proc = _powershell(_PS_LIST_DISKS)
    raw = json.loads(proc.stdout or "[]")
    if isinstance(raw, dict):
        raw = [raw]
    disks: List[Disk] = []
    for d in raw:
        bus = (d.get("BusType") or "").strip()
        disks.append(
            Disk(
                identifier=str(d.get("Number")),
                raw_path=rf"\\.\PhysicalDrive{d.get('Number')}",
                description=f"{(d.get('FriendlyName') or 'unknown').strip()} ({bus})",
                size=int(d.get("Size") or 0),
                removable=bus in ("USB", "SD", "MMC"),
                is_system=bool(d.get("IsSystem")) or bool(d.get("IsBoot")),
                mountpoints=[m for m in (d.get("Mounts") or []) if m],
            )
        )
    return disks


# ------------------------------------------------------- format and mount --


def prepare_card(disk: Disk, label: str = "NOMAD", dry_run: bool = False) -> str:
    """Wipe `disk`, lay down one FAT32 partition, mount it, return the path."""
    if IS_LINUX:
        return _prepare_linux(disk, label, dry_run)
    if IS_MAC:
        return _prepare_macos(disk, label, dry_run)
    if IS_WINDOWS:
        return _prepare_windows(disk, label, dry_run)
    raise DiskError(f"Unsupported platform: {platform.system()}")


# ------------------------------------------------------------------ Linux --


def _linux_partition_node(disk_path: str, index: int = 1) -> str:
    # /dev/sdb -> /dev/sdb1, but /dev/mmcblk0 -> /dev/mmcblk0p1
    base = os.path.basename(disk_path)
    if base and base[-1].isdigit():
        return f"{disk_path}p{index}"
    return f"{disk_path}{index}"


def _prepare_linux(disk: Disk, label: str, dry_run: bool) -> str:
    if not is_privileged() and not dry_run:
        raise DiskError(f"Partitioning needs root. {privilege_hint()}")

    for mp in disk.mountpoints:
        run(["umount", mp], check=False, dry_run=dry_run)
    run(["umount", _linux_partition_node(disk.raw_path)], check=False, dry_run=dry_run)

    c.step("Clearing existing partition signatures")
    run(["wipefs", "-a", disk.raw_path], dry_run=dry_run)

    c.step("Writing a single FAT32 partition")
    start, count = fat32.partition_plan(disk.size // fat32.SECTOR)
    script = f"label: dos\nstart={start}, size={count}, type=c, bootable\n"
    run(["sfdisk", "--wipe", "always", disk.raw_path], input_text=script, dry_run=dry_run)

    run(["udevadm", "settle"], check=False, dry_run=dry_run)
    run(["partprobe", disk.raw_path], check=False, dry_run=dry_run)
    if not dry_run:
        time.sleep(1.5)

    part = _linux_partition_node(disk.raw_path)
    c.step(f"Formatting {part} as FAT32")
    if have("mkfs.vfat"):
        run(["mkfs.vfat", "-F", "32", "-n", label[:11].upper(), part], dry_run=dry_run)
    else:
        c.warn("mkfs.vfat not found - using the built-in formatter")
        _format_with_builtin(part, label, dry_run=dry_run)

    return _mount_linux(part, dry_run)


def _mount_linux(part: str, dry_run: bool) -> str:
    if dry_run:
        c.info(c.dim(f"[dry-run] mount {part}"))
        return "/mnt/nomad-dry-run"

    if have("udisksctl"):
        proc = run(["udisksctl", "mount", "-b", part], check=False)
        out = (proc.stdout or "") + (proc.stderr or "")
        # "Mounted /dev/sdb1 at /media/user/NOMAD"
        if " at " in out:
            path = out.rsplit(" at ", 1)[1].strip().rstrip(".")
            if os.path.isdir(path):
                return path

    mount_point = tempfile.mkdtemp(prefix="nomad-sd-")
    run(["mount", "-t", "vfat", "-o", "rw,flush", part, mount_point])
    return mount_point


# ------------------------------------------------------------------ macOS --


def _prepare_macos(disk: Disk, label: str, dry_run: bool) -> str:
    run(["diskutil", "unmountDisk", disk.identifier], check=False, dry_run=dry_run)

    c.step("Erasing and formatting as FAT32 (MBR)")
    # diskutil's FAT32 goes through newfs_msdos, which is happy well past the
    # 32 GB ceiling Windows imposes.
    run(
        ["diskutil", "eraseDisk", "FAT32", label[:11].upper(), "MBRFormat", disk.identifier],
        dry_run=dry_run,
    )
    if dry_run:
        return f"/Volumes/{label[:11].upper()}"

    for _ in range(20):
        proc = run(["diskutil", "info", "-plist", f"{disk.identifier}s1"], check=False)
        if proc.returncode == 0:
            info = plistlib.loads((proc.stdout or "").encode())
            mp = info.get("MountPoint")
            if mp and os.path.isdir(mp):
                return mp
        time.sleep(1)

    guess = f"/Volumes/{label[:11].upper()}"
    if os.path.isdir(guess):
        return guess
    raise DiskError("Formatted the card but could not find where macOS mounted it")


# ---------------------------------------------------------------- Windows --


def _prepare_windows(disk: Disk, label: str, dry_run: bool) -> str:
    if not is_privileged() and not dry_run:
        raise DiskError(f"Partitioning needs Administrator. {privilege_hint()}")

    native_fat32 = disk.size <= WINDOWS_FAT32_LIMIT
    lines = [
        f"select disk {disk.identifier}",
        "clean",
        "create partition primary",
        "select partition 1",
        "active",
    ]
    if native_fat32:
        lines.append(f"format fs=fat32 quick label={label[:11].upper()}")
    lines.append("assign")
    lines.append("exit")
    script = "\r\n".join(lines) + "\r\n"

    c.step("Partitioning with diskpart")
    if not native_fat32:
        c.info("Card is over 32 GB, which Windows will not format as FAT32.")
        c.info("Creating the partition here and writing the filesystem directly.")

    if dry_run:
        c.info(c.dim("[dry-run] diskpart <<\n" + script + ">>"))
        return "N:\\"

    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, newline="") as fh:
        fh.write(script)
        script_path = fh.name
    try:
        run(["diskpart", "/s", script_path])
    finally:
        os.unlink(script_path)

    time.sleep(3)
    letter = _windows_drive_letter(disk.identifier)
    if not letter:
        raise DiskError("diskpart finished but Windows did not assign a drive letter")

    if not native_fat32:
        _windows_format_volume_builtin(letter, label)
        time.sleep(2)

    return f"{letter}\\"


def _windows_drive_letter(disk_number: str) -> Optional[str]:
    script = (
        f"(Get-Partition -DiskNumber {disk_number} | "
        "Where-Object DriveLetter | Select-Object -First 1).DriveLetter"
    )
    for _ in range(15):
        proc = _powershell(script, check=False)
        letter = (proc.stdout or "").strip()
        if letter:
            return f"{letter}:"
        time.sleep(1)
    return None


def _windows_format_volume_builtin(letter: str, label: str) -> None:
    """FAT32 a >32 GB volume Windows refuses to touch, via a locked handle."""
    import ctypes
    from ctypes import wintypes

    drive = letter.rstrip("\\").rstrip(":")
    script = (
        f"$p = Get-Partition -DriveLetter {drive}; "
        "[pscustomobject]@{Offset=$p.Offset; Size=$p.Size} | ConvertTo-Json"
    )
    info = json.loads(_powershell(script).stdout)
    offset = int(info["Offset"])
    size = int(info["Size"])

    GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
    FILE_SHARE_READ, FILE_SHARE_WRITE = 0x00000001, 0x00000002
    OPEN_EXISTING = 3
    FSCTL_LOCK_VOLUME = 0x00090018
    FSCTL_DISMOUNT_VOLUME = 0x00090020
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

    k32 = ctypes.windll.kernel32
    handle = k32.CreateFileW(
        f"\\\\.\\{drive}:", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None,
    )
    if handle == INVALID_HANDLE_VALUE:
        raise DiskError(f"Could not open volume {drive}: for raw access")

    returned = wintypes.DWORD(0)
    try:
        if not k32.DeviceIoControl(handle, FSCTL_LOCK_VOLUME, None, 0, None, 0,
                                   ctypes.byref(returned), None):
            raise DiskError(
                f"Could not lock volume {drive}: - close any Explorer windows "
                "or antivirus scanning the card and try again"
            )
        k32.DeviceIoControl(handle, FSCTL_DISMOUNT_VOLUME, None, 0, None, 0,
                            ctypes.byref(returned), None)

        c.step(f"Writing FAT32 to {drive}: ({c.human_bytes(size)})")
        with open(rf"\\.\{drive}:", "r+b", buffering=0) as fh:
            fat32.format_volume(
                fh,
                total_sectors=size // fat32.SECTOR,
                label=label,
                hidden_sectors=offset // fat32.SECTOR,
                base_offset=0,
            )
    finally:
        k32.CloseHandle(handle)


def _format_with_builtin(part_path: str, label: str, dry_run: bool) -> None:
    """Format an already-created partition with the bundled FAT32 writer."""
    if dry_run:
        c.info(c.dim(f"[dry-run] built-in FAT32 format of {part_path}"))
        return
    size = _block_device_size(part_path)
    with open(part_path, "r+b", buffering=0) as fh:
        fat32.format_volume(fh, total_sectors=size // fat32.SECTOR, label=label)


def _block_device_size(path: str) -> int:
    with open(path, "rb") as fh:
        return fh.seek(0, os.SEEK_END)


# -------------------------------------------------------------- finishing --


def flush_and_eject(mount_path: str, disk: Disk, dry_run: bool = False) -> None:
    """Make sure everything is on the card before the user pulls it out."""
    if dry_run:
        c.info(c.dim(f"[dry-run] sync + eject {disk.identifier}"))
        return

    if IS_WINDOWS:
        # Nothing reliable short of the eject API; the copy step already
        # flushed each file, so just tell the user.
        c.info("Use 'Safely Remove Hardware' before unplugging the card.")
        return

    subprocess.run(["sync"], check=False)
    if IS_MAC:
        subprocess.run(["diskutil", "eject", disk.identifier], check=False)
        c.ok("Card ejected")
        return

    unmounted = False
    if have("udisksctl"):
        proc = subprocess.run(
            ["udisksctl", "unmount", "-b", _linux_partition_node(disk.raw_path)],
            check=False, capture_output=True,
        )
        unmounted = proc.returncode == 0
    if not unmounted:
        # We may have mounted it ourselves into a temp dir, which udisks does
        # not know about.
        proc = subprocess.run(["umount", mount_path], check=False, capture_output=True)
        unmounted = proc.returncode == 0

    if unmounted:
        c.ok("Card unmounted - safe to remove")
    else:
        c.warn(f"Could not unmount {mount_path}; run 'sync' before removing the card")
