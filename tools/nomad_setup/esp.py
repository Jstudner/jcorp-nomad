"""esptool discovery, chip interrogation, and flashing.

The flash step never invents offsets. arduino-cli writes a `flash_args` file
next to the build output listing the exact flash options and every
offset/binary pair, and that file is what gets executed. The only thing this
module adds on top is a set of checks that refuse to write a firmware the chip
cannot actually run - wrong flash size, or an app bigger than the partition it
is being written into.
"""

from __future__ import annotations

import glob
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Tuple

from . import boards
from . import console as c

DEFAULT_BAUD = 921600


class EspError(RuntimeError):
    pass


# ------------------------------------------------------------- discovery --


@dataclass
class EspTool:
    command: List[str]
    version: str

    @property
    def major(self) -> int:
        m = re.match(r"v?(\d+)", self.version)
        return int(m.group(1)) if m else 4

    def sub(self, name: str) -> str:
        """esptool v5 renamed every subcommand from snake_case to kebab-case."""
        return name.replace("_", "-") if self.major >= 5 else name

    def run(self, args: List[str], dry_run=False, stream=False, timeout=600):
        cmd = self.command + args
        if dry_run:
            c.info(c.dim("[dry-run] " + " ".join(cmd)))
            return subprocess.CompletedProcess(cmd, 0, "", "")
        c.debug("$ " + " ".join(cmd))
        if stream:
            proc = subprocess.run(cmd, text=True, timeout=timeout)
            return proc
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def find_esptool(explicit: Optional[str] = None,
                 arduino_data_dir: Optional[Path] = None) -> EspTool:
    candidates: List[List[str]] = []

    if explicit:
        candidates.append([explicit])

    # The copy bundled with the installed ESP32 core is always the right
    # version for the core that produced the binaries.
    search_roots = []
    if arduino_data_dir:
        search_roots.append(Path(arduino_data_dir))
    search_roots += [
        Path.home() / ".arduino15",
        Path.home() / "Library" / "Arduino15",
        Path(os.environ.get("LOCALAPPDATA", "")) / "Arduino15",
    ]
    for root in search_roots:
        pattern = str(root / "packages" / "esp32" / "tools" / "esptool_py" / "*" / "esptool*")
        for path in sorted(glob.glob(pattern), reverse=True):
            if os.path.isfile(path) and os.access(path, os.X_OK):
                candidates.append([path])

    candidates.append([sys.executable, "-m", "esptool"])
    for name in ("esptool", "esptool.py"):
        found = shutil.which(name)
        if found:
            candidates.append([found])

    for cmd in candidates:
        try:
            proc = subprocess.run(cmd + ["version"], capture_output=True, text=True, timeout=60)
            if proc.returncode != 0:
                proc = subprocess.run(cmd + ["--version"], capture_output=True, text=True,
                                      timeout=60)
            if proc.returncode == 0:
                out = (proc.stdout or proc.stderr or "").strip().splitlines()
                version = ""
                for line in out:
                    m = re.search(r"v?(\d+\.\d+[\.\w]*)", line)
                    if m:
                        version = m.group(1)
                        break
                return EspTool(cmd, version or "unknown")
        except (OSError, subprocess.SubprocessError):
            continue

    raise EspError(
        "esptool not found. Install it with:  pip install esptool\n"
        "(or install the ESP32 board package in the Arduino IDE, which bundles it)"
    )


def list_serial_ports() -> List[Tuple[str, str]]:
    """[(device, description)] - pyserial when available, OS fallbacks otherwise."""
    try:
        from serial.tools import list_ports  # type: ignore

        return [(p.device, (p.description or "").strip()) for p in list_ports.comports()]
    except Exception:
        pass

    if sys.platform == "win32":
        try:
            proc = subprocess.run(
                ["powershell", "-NoProfile", "-Command",
                 "[System.IO.Ports.SerialPort]::GetPortNames()"],
                capture_output=True, text=True, timeout=60,
            )
            return [(p.strip(), "") for p in (proc.stdout or "").split() if p.strip()]
        except Exception:
            return []

    patterns = (["/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.wchusbserial*"]
                if sys.platform == "darwin"
                else ["/dev/ttyACM*", "/dev/ttyUSB*"])
    found: List[Tuple[str, str]] = []
    for pattern in patterns:
        found += [(p, "") for p in sorted(glob.glob(pattern))]
    return found


# ------------------------------------------------------------ chip probe --


@dataclass
class ChipInfo:
    chip: str = "unknown"
    flash_mb: int = 0
    psram_mb: int = 0
    mac: str = ""
    features: str = ""
    raw: str = ""


_FLASH_RE = re.compile(r"[Dd]etected flash size:\s*(\d+)\s*MB")
_CHIP_RE = re.compile(r"Chip is\s+(\S+)")
_MAC_RE = re.compile(r"MAC:\s*([0-9a-fA-F:]+)")
_FEATURES_RE = re.compile(r"Features:\s*(.+)")
_PSRAM_RE = re.compile(r"PSRAM\s*(\d+)\s*MB", re.IGNORECASE)


def probe_chip(tool: EspTool, port: str, baud: int = 115200) -> ChipInfo:
    proc = tool.run(["--port", port, "--baud", str(baud), tool.sub("flash_id")], timeout=120)
    out = (proc.stdout or "") + (proc.stderr or "")
    info = ChipInfo(raw=out)

    if proc.returncode != 0:
        raise EspError(
            f"Could not talk to the board on {port}.\n"
            "  - Is it plugged in and not held open by a serial monitor?\n"
            "  - These sticks have no reset button: hold the boot button while\n"
            "    plugging it in to force download mode, then try again.\n\n"
            + out.strip()
        )

    m = _CHIP_RE.search(out)
    if m:
        info.chip = m.group(1).rstrip(",")
    m = _FLASH_RE.search(out)
    if m:
        info.flash_mb = int(m.group(1))
    m = _MAC_RE.search(out)
    if m:
        info.mac = m.group(1)
    m = _FEATURES_RE.search(out)
    if m:
        info.features = m.group(1).strip()
        p = _PSRAM_RE.search(info.features)
        if p:
            info.psram_mb = int(p.group(1))
    return info


# ------------------------------------------------------------ flash plan --


@dataclass
class FlashPlan:
    directory: Path
    options: List[str]                       # --flash-mode dio --flash-freq 80m ...
    entries: List[Tuple[int, Path]] = field(default_factory=list)  # (offset, file)
    flash_size_mb: int = 0

    def total_bytes(self) -> int:
        return sum(p.stat().st_size for _, p in self.entries if p.is_file())

    def highest_end(self) -> int:
        return max((off + p.stat().st_size for off, p in self.entries if p.is_file()),
                   default=0)


def load_flash_plan(directory: Path) -> FlashPlan:
    """Parse the `flash_args` file arduino-cli leaves beside the binaries."""
    directory = Path(directory)
    args_file = directory / "flash_args"
    if not args_file.is_file():
        raise EspError(
            f"No flash_args in {directory}.\n"
            "Point --firmware at an arduino-cli build output directory "
            "(build/esp32.esp32.esp32s3), or let the tool build the firmware."
        )

    lines = [ln.strip() for ln in args_file.read_text().splitlines() if ln.strip()]
    if not lines:
        raise EspError(f"{args_file} is empty")

    options = lines[0].split()
    plan = FlashPlan(directory=directory, options=options)

    m = re.search(r"--flash[-_]size\s+(\d+)MB", lines[0])
    if m:
        plan.flash_size_mb = int(m.group(1))

    for line in lines[1:]:
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        offset_text, filename = parts
        filename = filename.strip().strip('"')
        path = directory / filename
        if not path.is_file():
            raise EspError(f"{filename} listed in flash_args but missing from {directory}")
        plan.entries.append((int(offset_text, 0), path))

    if not plan.entries:
        raise EspError(f"{args_file} lists no binaries to write")
    return plan


@dataclass
class PreflightResult:
    errors: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)
    app_size: int = 0
    app_partition: Optional[boards.Partition] = None

    @property
    def ok(self) -> bool:
        return not self.errors


def preflight(plan: FlashPlan, chip: Optional[ChipInfo]) -> PreflightResult:
    """Refuse combinations that would produce a board that does not boot."""
    result = PreflightResult()

    # Which entry is the partition table, and which is the app?
    part_blob: Optional[bytes] = None
    app_path: Optional[Path] = None
    for offset, path in plan.entries:
        if offset == 0x8000:
            part_blob = path.read_bytes()
        elif offset == 0x10000 or path.name.endswith(".ino.bin"):
            app_path = path

    if app_path:
        result.app_size = app_path.stat().st_size

    if part_blob:
        table = boards.parse_partition_table(part_blob)
        app_part = boards.first_app_partition(table)
        result.app_partition = app_part
        if app_part and result.app_size:
            if result.app_size > app_part.size:
                result.errors.append(
                    f"Firmware is {c.human_bytes(result.app_size)} but the "
                    f"'{app_part.label}' partition is only "
                    f"{c.human_bytes(app_part.size)}. Rebuild with a larger "
                    "partition scheme - the tool picks one automatically from "
                    "the detected flash size."
                )
            elif result.app_size > app_part.size * 0.9:
                result.warnings.append(
                    f"Firmware fills {result.app_size * 100 // app_part.size}% of "
                    f"the '{app_part.label}' partition."
                )
        if table:
            table_end = max(p.end for p in table)
            if chip and chip.flash_mb and table_end > chip.flash_mb * 1024 * 1024:
                result.errors.append(
                    f"The partition table needs {c.human_bytes(table_end)} of flash "
                    f"but the chip reports {chip.flash_mb} MB."
                )
    else:
        result.warnings.append("No partition table at 0x8000 in this build - not verified.")

    if chip and chip.flash_mb and plan.flash_size_mb:
        if plan.flash_size_mb > chip.flash_mb:
            result.errors.append(
                f"Firmware was built for {plan.flash_size_mb} MB of flash but the "
                f"chip has {chip.flash_mb} MB."
            )
        elif plan.flash_size_mb < chip.flash_mb:
            result.warnings.append(
                f"Firmware was built for {plan.flash_size_mb} MB of flash; the chip "
                f"has {chip.flash_mb} MB, so the extra space will go unused."
            )

    if chip and chip.chip and "S3" not in chip.chip.upper():
        result.errors.append(
            f"This firmware is for the ESP32-S3; the board reports '{chip.chip}'."
        )

    if chip and chip.flash_mb and chip.psram_mb == 0:
        result.warnings.append(
            "The chip did not report any PSRAM. On an N16R8 that usually means "
            "the module is a different variant - the firmware still runs, but "
            "check the board menu if the display misbehaves."
        )

    return result


def flash(tool: EspTool, plan: FlashPlan, port: str, baud: int = DEFAULT_BAUD,
          erase_all: bool = False, dry_run: bool = False) -> None:
    args = [
        "--chip", "esp32s3",
        "--port", port,
        "--baud", str(baud),
        "--before", tool.sub("default_reset"),
        "--after", tool.sub("hard_reset"),
        tool.sub("write_flash"),
    ]
    if erase_all:
        args.append("--erase-all")
    args += ["-z"]
    args += plan.options
    for offset, path in plan.entries:
        args += [hex(offset), str(path)]

    proc = tool.run(args, dry_run=dry_run, stream=True, timeout=1800)
    if not dry_run and proc.returncode != 0:
        raise EspError(
            "esptool failed to write the firmware. If it could not enter the "
            "bootloader, unplug the stick, hold the boot button, plug it back "
            "in, and run the flash step again."
        )


def erase_flash(tool: EspTool, port: str, baud: int = DEFAULT_BAUD, dry_run: bool = False) -> None:
    proc = tool.run(
        ["--chip", "esp32s3", "--port", port, "--baud", str(baud), tool.sub("erase_flash")],
        dry_run=dry_run, stream=True, timeout=900,
    )
    if not dry_run and proc.returncode != 0:
        raise EspError("esptool could not erase the flash")
