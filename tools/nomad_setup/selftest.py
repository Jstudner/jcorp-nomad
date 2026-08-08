"""Offline self-checks - no hardware, no SD card, nothing destructive.

Runs the two pieces of this tool that are worth not getting wrong: the FAT32
formatter (against a throwaway image file, checked with fsck.vfat and mtools
when they are installed) and the pre-flash partition checks.

    nomad-setup selftest
"""

from __future__ import annotations

import shutil
import struct
import subprocess
import tempfile
from pathlib import Path
from typing import List, Tuple

from . import console as c
from . import esp, fat32, sdcard

# Card sizes worth covering: each one lands in a different cluster-size band,
# and the >32 GB ones are exactly what Windows refuses to format.
IMAGE_SIZES = [
    ("64 MB", 64 << 20),
    ("512 MB", 512 << 20),
    ("2 GB", 2 << 30),
    ("8 GB", 8 << 30),
    ("32 GB", 32 << 30),
    ("64 GB", 64 << 30),
    ("128 GB", 128 << 30),
]


def _check_boot_sector(path: Path, start_lba: int, geo: fat32.Fat32Geometry) -> List[str]:
    """Read the boot sector back and confirm it describes what we planned."""
    problems: List[str] = []
    with open(path, "rb") as fh:
        fh.seek(start_lba * fat32.SECTOR)
        bs = fh.read(fat32.SECTOR)

    if bs[510:512] != b"\x55\xaa":
        problems.append("boot sector signature missing")
    if bs[82:90] != b"FAT32   ":
        problems.append("filesystem type is not FAT32")
    if struct.unpack_from("<H", bs, 11)[0] != fat32.SECTOR:
        problems.append("bytes-per-sector mismatch")
    if bs[13] != geo.sectors_per_cluster:
        problems.append("sectors-per-cluster mismatch")
    if struct.unpack_from("<L", bs, 32)[0] != geo.total_sectors:
        problems.append("total sector count mismatch")
    if struct.unpack_from("<L", bs, 36)[0] != geo.fat_sectors:
        problems.append("FAT size mismatch")
    if struct.unpack_from("<L", bs, 28)[0] != start_lba:
        problems.append("hidden sector count does not match the partition start")

    # The FAT must be big enough to address every cluster.
    if (geo.cluster_count + 2) * 4 > geo.fat_sectors * fat32.SECTOR:
        problems.append("FAT is too small for the cluster count")
    if geo.cluster_count < fat32.MIN_FAT32_CLUSTERS:
        problems.append("cluster count is below the FAT32 minimum")
    return problems


def _fsck(image: Path, start_lba: int, sectors: int,
          geo: fat32.Fat32Geometry) -> Tuple[bool, str]:
    """fsck.vfat only looks at a bare volume, so slice the partition out.

    Copy everything up to and including the root directory cluster - on a
    128 GB card the two FATs alone are 32 MB, and a short slice would make
    fsck report a missing volume label."""
    if not shutil.which("fsck.vfat"):
        return True, "skipped (fsck.vfat not installed)"

    metadata_sectors = geo.first_data_sector + geo.sectors_per_cluster
    with tempfile.NamedTemporaryFile(suffix=".part", delete=False) as tmp:
        part_path = Path(tmp.name)
    try:
        with open(image, "rb") as src, open(part_path, "r+b") as dst:
            src.seek(start_lba * fat32.SECTOR)
            remaining = min(metadata_sectors, sectors) * fat32.SECTOR
            while remaining > 0:
                chunk = src.read(min(remaining, 8 << 20))
                if not chunk:
                    break
                dst.write(chunk)
                remaining -= len(chunk)
            dst.truncate(sectors * fat32.SECTOR)
        proc = subprocess.run(["fsck.vfat", "-n", str(part_path)],
                              capture_output=True, text=True, timeout=300)
        detail = ((proc.stdout or "") + (proc.stderr or "")).strip().splitlines()
        return proc.returncode == 0, (detail[-1] if detail else "clean")
    finally:
        part_path.unlink(missing_ok=True)


def _mtools_roundtrip(image: Path, start_lba: int) -> Tuple[bool, str]:
    """Write a file through an independent FAT implementation and read it back."""
    if not (shutil.which("mcopy") and shutil.which("mdir")):
        return True, "skipped (mtools not installed)"

    target = f"{image}@@{start_lba * fat32.SECTOR}"
    env = {"MTOOLS_SKIP_CHECK": "1"}
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as tmp:
        tmp.write("jcorp nomad selftest\n")
        payload = Path(tmp.name)
    try:
        proc = subprocess.run(["mcopy", "-i", target, str(payload), "::/HELLO.TXT"],
                              capture_output=True, text=True, env=env, timeout=120)
        if proc.returncode != 0:
            return False, (proc.stderr or proc.stdout or "mcopy failed").strip()[:120]
        proc = subprocess.run(["mdir", "-i", target, "::"],
                              capture_output=True, text=True, env=env, timeout=120)
        if proc.returncode != 0 or "HELLO" not in (proc.stdout or ""):
            return False, "file written but not listed back"
        return True, "wrote and read back a file"
    finally:
        payload.unlink(missing_ok=True)


def run_fat32_tests() -> int:
    c.heading("FAT32 formatter")
    failures = 0
    rows = []

    for label, size in IMAGE_SIZES:
        with tempfile.NamedTemporaryFile(suffix=".img", delete=False) as tmp:
            image = Path(tmp.name)
        try:
            # Sparse: only the structures we write consume real space.
            with open(image, "r+b") as fh:
                fh.truncate(size)
                start, sectors, geo = fat32.format_disk(fh, size // fat32.SECTOR, "NOMAD")

            problems = _check_boot_sector(image, start, geo)
            fsck_ok, fsck_note = _fsck(image, start, sectors, geo)
            mt_ok, mt_note = _mtools_roundtrip(image, start)

            ok = not problems and fsck_ok and mt_ok
            failures += 0 if ok else 1
            note = "; ".join(problems) if problems else f"{fsck_note}; {mt_note}"
            rows.append([
                label,
                "PASS" if ok else "FAIL",
                f"{geo.cluster_bytes // 1024} KB",
                f"{geo.cluster_count:,}",
                note[:52],
            ])
        except fat32.Fat32Error as exc:
            failures += 1
            rows.append([label, "FAIL", "-", "-", str(exc)[:52]])
        finally:
            image.unlink(missing_ok=True)

    c.table(["card", "result", "cluster", "clusters", "notes"], rows)
    return failures


def _fake_build(directory: Path, app_bytes: int, app_part: int, flash_mb: int) -> None:
    def entry(ptype, subtype, offset, size, label):
        return struct.pack("<2sBBLL16sL", b"\xaa\x50", ptype, subtype, offset, size,
                           label.encode().ljust(16, b"\x00"), 0)

    table = (entry(1, 2, 0x9000, 0x5000, "nvs")
             + entry(1, 0, 0xE000, 0x2000, "otadata")
             + entry(0, 0x10, 0x10000, app_part, "app0"))
    (directory / "boot.bin").write_bytes(b"\x00" * 1024)
    (directory / "parts.bin").write_bytes(table.ljust(3072, b"\xff"))
    (directory / "app.ino.bin").write_bytes(b"\x00" * app_bytes)
    (directory / "flash_args").write_text(
        f"--flash-mode dio --flash-freq 80m --flash-size {flash_mb}MB\n"
        "0x0 boot.bin\n0x8000 parts.bin\n0x10000 app.ino.bin\n"
    )


def run_preflight_tests() -> int:
    c.heading("Pre-flash checks")
    cases = [
        # name,            app,       app0 size, built-for, chip flash, chip,      expect ok
        ("healthy 16 MB",  1_450_000, 0x300000, 16, 16, "ESP32-S3", True),
        ("app > partition", 1_450_000, 0x140000, 4, 4, "ESP32-S3", False),
        ("built for bigger flash", 1_450_000, 0x300000, 16, 4, "ESP32-S3", False),
        ("smaller build on big chip", 1_450_000, 0x300000, 4, 16, "ESP32-S3", True),
        ("wrong chip family", 1_450_000, 0x300000, 16, 16, "ESP32-C3", False),
    ]

    failures = 0
    rows = []
    for name, app, part, built_mb, chip_mb, chip_name, expect_ok in cases:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            _fake_build(directory, app, part, built_mb)
            plan = esp.load_flash_plan(directory)
            result = esp.preflight(plan, esp.ChipInfo(chip=chip_name, flash_mb=chip_mb,
                                                      psram_mb=8))
            passed = result.ok == expect_ok
            failures += 0 if passed else 1
            verdict = "allowed" if result.ok else "refused"
            rows.append([name, "PASS" if passed else "FAIL", verdict,
                         (result.errors[0].split(".")[0] if result.errors else "")[:44]])

    c.table(["case", "result", "verdict", "reason"], rows)
    return failures


def run_template_tests() -> int:
    """The required-files manifest is only useful if it matches the template
    actually shipped in the repo, so check the two against each other."""
    c.heading("SD card template")
    try:
        template = sdcard.find_template()
    except sdcard.SdCardError as exc:
        c.warn(str(exc).splitlines()[0])
        c.info("Skipped (run this from inside the repository to include it)")
        return 0

    plan = sdcard.plan_copy(template, include_placeholders=True)
    copied = {item.relative for item in plan.items}

    failures = 0
    rows = []

    for name in sdcard.REQUIRED_FILES:
        present = name in copied
        failures += 0 if present else 1
        rows.append([name, "PASS" if present else "FAIL",
                     "in template and copied" if present else "MISSING from the template"])

    for name in sdcard.REQUIRED_DIRS:
        has_content = any(rel.startswith(f"{name}/") for rel in copied)
        failures += 0 if has_content else 1
        rows.append([f"{name}/", "PASS" if has_content else "FAIL",
                     f"{sum(1 for r in copied if r.startswith(name + '/'))} files"])

    on_disk = {str(p.relative_to(template)).replace("\\", "/")
               for p in template.rglob("*") if p.is_file()}
    skipped = sorted(on_disk - copied)
    unexpected = [s for s in skipped if Path(s).name not in sdcard.SKIP_NAMES]
    failures += len(unexpected)
    rows.append([
        "template coverage",
        "PASS" if not unexpected else "FAIL",
        f"{len(copied)}/{len(on_disk)} copied, skipped: "
        + (", ".join(Path(s).name for s in skipped) or "nothing"),
    ])

    c.table(["item", "result", "notes"], rows)
    if unexpected:
        for name in unexpected:
            c.error(f"template file not covered by the copy plan: {name}")
    return failures


def cmd_selftest(args) -> int:
    c.heading("nomad-setup self-test")
    c.info("Nothing here touches a disk or a board.")

    failures = run_fat32_tests() + run_preflight_tests() + run_template_tests()

    c.heading("Result")
    if failures:
        c.error(f"{failures} check(s) failed")
        return 1
    c.ok("All checks passed")
    if not shutil.which("fsck.vfat"):
        c.info("Install dosfstools for a stronger filesystem check.")
    if not shutil.which("mcopy"):
        c.info("Install mtools to also verify a file round-trip.")
    return 0
