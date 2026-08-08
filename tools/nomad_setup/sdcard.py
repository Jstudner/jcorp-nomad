"""Populating a prepared card with the Nomad web UI and folder structure."""

from __future__ import annotations

import os
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple

from . import console as c

# Created whether or not the placeholder media is copied, because the firmware's
# indexer expects them to exist.
MEDIA_DIRS = ["Movies", "Shows", "Books", "Music", "Gallery", "Files", "config"]

# Directories in the template whose contents are demo media rather than part of
# the web interface.
PLACEHOLDER_DIRS = {"Movies", "Shows", "Books", "Music", "Gallery", "Files"}

# Never copied onto the card: helper scripts that only matter in the repo.
SKIP_NAMES = {"img.py", ".DS_Store", "Thumbs.db", "desktop.ini"}


class SdCardError(RuntimeError):
    pass


@dataclass
class CopyItem:
    source: Path
    relative: str
    size: int


@dataclass
class CopyPlan:
    items: List[CopyItem]
    total_bytes: int
    skipped_placeholders: int

    @property
    def count(self) -> int:
        return len(self.items)


def find_template(repo_root: Optional[Path] = None) -> Path:
    """Locate SD_Card_Template, whether run from the repo or a copy of tools/."""
    candidates = []
    if repo_root:
        candidates.append(Path(repo_root) / "SD_Card_Template")
    here = Path(__file__).resolve()
    for parent in here.parents:
        candidates.append(parent / "SD_Card_Template")
    candidates.append(Path.cwd() / "SD_Card_Template")

    for path in candidates:
        if path.is_dir() and (path / "index.html").is_file():
            return path
    raise SdCardError(
        "Could not find SD_Card_Template. Run this from inside the jcorp-nomad "
        "checkout, or pass --template /path/to/SD_Card_Template."
    )


def plan_copy(template: Path, include_placeholders: bool = True) -> CopyPlan:
    items: List[CopyItem] = []
    skipped = 0

    for root, dirnames, filenames in os.walk(template):
        dirnames[:] = [d for d in dirnames if d not in SKIP_NAMES and not d.startswith(".")]
        rel_root = Path(root).relative_to(template)
        top = rel_root.parts[0] if rel_root.parts else ""

        if not include_placeholders and top in PLACEHOLDER_DIRS:
            skipped += len([f for f in filenames if f not in SKIP_NAMES])
            continue

        for name in filenames:
            if name in SKIP_NAMES or name.startswith("."):
                continue
            src = Path(root) / name
            try:
                size = src.stat().st_size
            except OSError:
                continue
            items.append(CopyItem(src, str(rel_root / name).replace("\\", "/"), size))

    return CopyPlan(items, sum(i.size for i in items), skipped)


def free_space(path: str) -> int:
    usage = shutil.disk_usage(path)
    return usage.free


def copy_to_card(
    plan: CopyPlan,
    mount_path: str,
    dry_run: bool = False,
    verify: bool = True,
) -> Tuple[int, List[str]]:
    """Copy the planned files, then create the media folders. Returns
    (bytes written, list of problems)."""
    dest_root = Path(mount_path)
    problems: List[str] = []

    if not dry_run:
        if not dest_root.is_dir():
            raise SdCardError(f"{mount_path} is not a directory")
        available = free_space(mount_path)
        # The web UI plus a little slack for directory entries.
        if available < plan.total_bytes + (4 << 20):
            raise SdCardError(
                f"Not enough space on the card: need "
                f"{c.human_bytes(plan.total_bytes)}, {c.human_bytes(available)} free"
            )

    bar = c.ProgressBar(max(plan.total_bytes, 1), "copying")
    written = 0

    for item in plan.items:
        target = dest_root / item.relative
        if dry_run:
            c.debug(f"[dry-run] copy {item.relative} ({c.human_bytes(item.size)})")
            written += item.size
            continue

        target.parent.mkdir(parents=True, exist_ok=True)
        bar.label = item.relative[-28:]
        try:
            with open(item.source, "rb") as src, open(target, "wb") as dst:
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    dst.write(chunk)
                    written += len(chunk)
                    bar.advance(len(chunk))
                dst.flush()
                os.fsync(dst.fileno())
        except OSError as exc:
            problems.append(f"{item.relative}: {exc}")

    bar.finish()

    for name in MEDIA_DIRS:
        path = dest_root / name
        if dry_run:
            c.debug(f"[dry-run] mkdir {name}")
            continue
        try:
            path.mkdir(exist_ok=True)
        except OSError as exc:
            problems.append(f"mkdir {name}: {exc}")

    if verify and not dry_run:
        problems += _verify(plan, dest_root)

    return written, problems


def _verify(plan: CopyPlan, dest_root: Path) -> List[str]:
    """Size-compare every copied file. Catches the classic silent failure where
    a card reports success and drops the tail of a write."""
    problems: List[str] = []
    c.step("Verifying copied files")
    for item in plan.items:
        target = dest_root / item.relative
        try:
            actual = target.stat().st_size
        except OSError:
            problems.append(f"{item.relative}: missing after copy")
            continue
        if actual != item.size:
            problems.append(
                f"{item.relative}: {actual} bytes on card, expected {item.size}"
            )
    return problems


def looks_like_nomad_card(mount_path: str) -> bool:
    root = Path(mount_path)
    return (root / "index.html").is_file() and (root / "menu.html").is_file()
