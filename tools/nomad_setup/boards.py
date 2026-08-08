"""Board profiles and flash-partition selection.

The partition scheme is the part people get wrong: the stock 4 MB Arduino
layout only gives the sketch 1.25 MB, and the Nomad firmware is about 1.4 MB.
Flashing it there produces a board that builds fine and then boots to nothing.
So the tool picks a scheme from the detected flash size and then *verifies* the
app actually fits before it writes anything.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional


@dataclass(frozen=True)
class Board:
    key: str
    name: str
    # Value of the NOMAD_BOARD macro in firmware/JcorpNomadProject/board_config.h.
    nomad_board: int
    fqbn: str
    # Fixed FQBN menu options. Flash size and partition scheme are added later,
    # once the chip has told us how much flash it actually has.
    options: Dict[str, str] = field(default_factory=dict)
    # Fallback if esptool cannot be run (no board attached, permissions, ...).
    default_flash_mb: int = 16
    notes: str = ""


BOARDS: Dict[str, Board] = {
    "pocket-dongle": Board(
        key="pocket-dongle",
        name="GNPE Pocket-Dongle-S3 0.96 (ESP32-S3 N16R8)",
        nomad_board=2,
        fqbn="esp32:esp32:esp32s3",
        options={
            "PSRAM": "opi",       # N16R8 is octal PSRAM; qspi will not detect it
            "CDCOnBoot": "cdc",
            "USBMode": "hwcdc",
            "CPUFreq": "240",
            "DebugLevel": "none",
        },
        default_flash_mb=16,
        notes="USB-A stick, 0.96\" ST7735 160x80, microSD in the plug.",
    ),
    "t-dongle": Board(
        key="t-dongle",
        name="LilyGO T-Dongle-S3",
        nomad_board=3,
        fqbn="esp32:esp32:esp32s3",
        options={
            "PSRAM": "opi",
            "CDCOnBoot": "cdc",
            "USBMode": "hwcdc",
            "CPUFreq": "240",
            "DebugLevel": "none",
        },
        default_flash_mb=16,
        notes="Same stick shape as the GNPE board, different wiring, APA102 LED.",
    ),
    "waveshare-1.47": Board(
        key="waveshare-1.47",
        name="Waveshare ESP32-S3-LCD-1.47",
        nomad_board=1,
        fqbn="esp32:esp32:esp32s3",
        options={
            "PSRAM": "opi",
            "CDCOnBoot": "cdc",
            "USBMode": "hwcdc",
            "CPUFreq": "240",
            "DebugLevel": "none",
        },
        default_flash_mb=16,
        notes="1.47\" ST7789 172x320, the original Nomad board.",
    ),
}

DEFAULT_BOARD = "pocket-dongle"


# Preferred partition scheme per flash size. Every one of these gives the app
# at least 3 MB, which is comfortably more than the ~1.45 MB the firmware needs
# and leaves headroom for future growth.
#
#   flash MB -> (arduino PartitionScheme id, FlashSize menu id, app0 bytes)
PARTITION_CHOICES: Dict[int, tuple] = {
    4: ("huge_app", "4M", 0x300000),
    8: ("default_8MB", "8M", 0x330000),
    16: ("app3M_fat9M_16MB", "16M", 0x300000),
    32: ("default_32MB", "32M", 0xC80000),
}

# Smallest flash we will flash onto at all. 2 MB cannot hold the firmware.
MIN_FLASH_MB = 4


def partition_choice(flash_mb: int) -> tuple:
    """Return (scheme_id, flash_size_id, app0_bytes) for a flash size in MB."""
    if flash_mb in PARTITION_CHOICES:
        return PARTITION_CHOICES[flash_mb]
    # Unknown size: fall back to the largest scheme that certainly fits.
    known = sorted(k for k in PARTITION_CHOICES if k <= flash_mb)
    if known:
        return PARTITION_CHOICES[known[-1]]
    raise ValueError(
        f"{flash_mb} MB of flash is too small for the Nomad firmware "
        f"(need at least {MIN_FLASH_MB} MB)."
    )


def build_fqbn(board: Board, flash_mb: int) -> str:
    """Full FQBN including flash size and the matching partition scheme."""
    scheme, flash_id, _ = partition_choice(flash_mb)
    opts = dict(board.options)
    opts["FlashSize"] = flash_id
    opts["PartitionScheme"] = scheme
    joined = ",".join(f"{k}={v}" for k, v in sorted(opts.items()))
    return f"{board.fqbn}:{joined}"


# --------------------------------------------------------------------------
# esp32 partition-table binary parsing
#
# The table written at 0x8000 is a flat array of 32-byte entries:
#   magic u16 (0x50AA) | type u8 | subtype u8 | offset u32 | size u32 |
#   label char[16] | flags u32
# It ends at the first entry whose magic does not match.
# --------------------------------------------------------------------------

PARTITION_MAGIC = b"\xaa\x50"
PART_TYPE_APP = 0x00


@dataclass
class Partition:
    type: int
    subtype: int
    offset: int
    size: int
    label: str

    @property
    def end(self) -> int:
        return self.offset + self.size


def parse_partition_table(blob: bytes) -> List[Partition]:
    parts: List[Partition] = []
    import struct

    for pos in range(0, len(blob) - 31, 32):
        entry = blob[pos:pos + 32]
        if entry[:2] != PARTITION_MAGIC:
            break
        _, ptype, subtype, offset, size, label, flags = struct.unpack(
            "<2sBBLL16sL", entry
        )
        parts.append(
            Partition(
                type=ptype,
                subtype=subtype,
                offset=offset,
                size=size,
                label=label.split(b"\x00", 1)[0].decode("utf-8", "replace"),
            )
        )
    return parts


def first_app_partition(parts: List[Partition]) -> Optional[Partition]:
    for p in parts:
        if p.type == PART_TYPE_APP:
            return p
    return None
