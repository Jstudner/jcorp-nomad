"""Building the firmware with arduino-cli.

Everything the Arduino IDE guide tells you to click through by hand - the board
package, the five libraries, dropping lv_conf.h next to the LVGL folder, and
picking the flash size and partition scheme - happens here instead.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

from . import boards
from . import console as c

ESP32_CORE = "esp32:esp32"
ESP32_MIN_MAJOR = 3
ESP32_INDEX_URL = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"

# Pinned to the versions this firmware is known to build against. The
# ESPAsyncWebServer/AsyncTCP forks are the ESP32Async ones - the older
# me-no-dev releases do not compile on core 3.x.
# These are the exact versions upstream pins in the README. ArduinoJson in
# particular must be 7.x - the firmware uses the v7 JsonDocument API, which is
# not source compatible with v6.
REQUIRED_LIBRARIES = [
    ("lvgl", "8.3.10"),
    ("ArduinoJson", "7.3.0"),
    ("Async TCP", "3.4.7"),
    ("ESP Async WebServer", "3.7.1"),
    ("SdFat", "2.3.0"),
]


class BuildError(RuntimeError):
    pass


@dataclass
class ArduinoCli:
    path: str
    version: str

    def run(self, args: List[str], check=True, stream=False, dry_run=False, timeout=3600):
        cmd = [self.path] + args
        if dry_run:
            c.info(c.dim("[dry-run] " + " ".join(cmd)))
            return subprocess.CompletedProcess(cmd, 0, "", "")
        c.debug("$ " + " ".join(cmd))
        if stream:
            proc = subprocess.run(cmd, text=True, timeout=timeout)
        else:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        if check and proc.returncode != 0:
            detail = ""
            if not stream:
                detail = "\n" + ((proc.stdout or "") + (proc.stderr or "")).strip()
            raise BuildError(f"arduino-cli {' '.join(args[:2])} failed{detail}")
        return proc

    def data_dir(self) -> Optional[Path]:
        try:
            proc = self.run(["config", "dump", "--format", "json"], check=False)
            cfg = json.loads(proc.stdout or "{}")
            # Key moved between arduino-cli releases.
            for path in (("directories", "data"), ("config", "directories", "data")):
                node = cfg
                for key in path:
                    node = node.get(key, {}) if isinstance(node, dict) else {}
                if isinstance(node, str):
                    return Path(node)
        except Exception:
            pass
        for guess in (Path.home() / ".arduino15",
                      Path.home() / "Library" / "Arduino15",
                      Path(os.environ.get("LOCALAPPDATA", "")) / "Arduino15"):
            if guess.is_dir():
                return guess
        return None


def find_arduino_cli(explicit: Optional[str] = None) -> Optional[ArduinoCli]:
    candidates = []
    if explicit:
        candidates.append(explicit)
    found = shutil.which("arduino-cli")
    if found:
        candidates.append(found)
    # Where the Arduino IDE 2.x keeps its bundled copy.
    candidates += [
        str(Path.home() / ".local" / "bin" / "arduino-cli"),
        "/usr/local/bin/arduino-cli",
        "/opt/homebrew/bin/arduino-cli",
        str(Path(os.environ.get("LOCALAPPDATA", "")) / "Programs" / "arduino-cli"
            / "arduino-cli.exe"),
    ]

    for path in candidates:
        if not path or not os.path.isfile(path):
            continue
        try:
            proc = subprocess.run([path, "version", "--format", "json"],
                                  capture_output=True, text=True, timeout=60)
            if proc.returncode == 0:
                data = json.loads(proc.stdout or "{}")
                return ArduinoCli(path, data.get("VersionString") or data.get("version") or "?")
        except Exception:
            continue
    return None


def install_hint() -> str:
    if sys.platform == "win32":
        return ("Install arduino-cli from https://arduino.github.io/arduino-cli/latest/installation/ "
                "(or 'winget install ArduinoSA.CLI') and re-run.")
    if sys.platform == "darwin":
        return "Install it with:  brew install arduino-cli"
    return ("Install it with:\n"
            "    curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/"
            "install.sh | BINDIR=~/.local/bin sh")


# ------------------------------------------------------------ core / libs --


def core_installed(cli: ArduinoCli) -> Optional[str]:
    proc = cli.run(["core", "list", "--format", "json"], check=False)
    try:
        data = json.loads(proc.stdout or "[]")
    except json.JSONDecodeError:
        return None
    entries = data.get("platforms", data) if isinstance(data, dict) else data
    for entry in entries or []:
        ident = entry.get("id") or (entry.get("platform") or {}).get("id")
        if ident == ESP32_CORE:
            return entry.get("installed_version") or entry.get("installed")
    return None


def ensure_core(cli: ArduinoCli, install: bool, dry_run: bool = False) -> str:
    version = core_installed(cli)
    if version:
        major = int(re.match(r"(\d+)", version).group(1)) if re.match(r"(\d+)", version) else 0
        if major >= ESP32_MIN_MAJOR:
            c.ok(f"ESP32 board package {version}")
            return version
        c.warn(f"ESP32 board package {version} is too old (need {ESP32_MIN_MAJOR}.x)")
        if not install:
            raise BuildError(
                f"Upgrade it in the Arduino IDE, or re-run with --install-deps"
            )

    if not install and not version:
        raise BuildError(
            "The ESP32 board package is not installed.\n"
            "  Re-run with --install-deps to have the tool fetch it "
            "(about 1 GB), or install 'esp32 by Espressif Systems' in the "
            "Arduino IDE's Boards Manager."
        )

    c.step("Installing the ESP32 board package (this takes a while)")
    cli.run(["core", "update-index", "--additional-urls", ESP32_INDEX_URL],
            stream=True, dry_run=dry_run)
    cli.run(["core", "install", ESP32_CORE, "--additional-urls", ESP32_INDEX_URL],
            stream=True, dry_run=dry_run, timeout=5400)
    return core_installed(cli) or "installed"


def installed_libraries(cli: ArduinoCli) -> dict:
    proc = cli.run(["lib", "list", "--format", "json"], check=False)
    try:
        data = json.loads(proc.stdout or "[]")
    except json.JSONDecodeError:
        return {}
    entries = data.get("installed_libraries", data) if isinstance(data, dict) else data
    result = {}
    for entry in entries or []:
        lib = entry.get("library", entry)
        name = lib.get("name")
        if name:
            result[name] = lib.get("version", "?")
    return result


def ensure_libraries(cli: ArduinoCli, install: bool, dry_run: bool = False) -> List[str]:
    have = installed_libraries(cli)
    missing = [(name, ver) for name, ver in REQUIRED_LIBRARIES if name not in have]

    for name, _ in REQUIRED_LIBRARIES:
        if name in have:
            c.ok(f"{name} {have[name]}")

    if not missing:
        return []

    names = ", ".join(n for n, _ in missing)
    if not install:
        raise BuildError(
            f"Missing libraries: {names}.\n"
            "  Re-run with --install-deps, or install them from the Arduino "
            "IDE's Library Manager."
        )

    cli.run(["lib", "update-index"], check=False, dry_run=dry_run)
    for name, version in missing:
        c.step(f"Installing {name} {version}")
        proc = cli.run(["lib", "install", f"{name}@{version}"], check=False,
                       stream=True, dry_run=dry_run, timeout=1200)
        if not dry_run and proc.returncode != 0:
            # A pinned version may age out of the index; fall back to latest.
            c.warn(f"{name}@{version} unavailable, installing the latest instead")
            cli.run(["lib", "install", name], stream=True, dry_run=dry_run, timeout=1200)
    return [n for n, _ in missing]


def ensure_lv_conf(cli: ArduinoCli, sketch_dir: Path, dry_run: bool = False) -> None:
    """LVGL looks for lv_conf.h one level above its own folder.

    This is the single most common reason a first Nomad build fails, so the
    tool places the project's copy for you rather than making it a README step.
    """
    proc = cli.run(["config", "dump", "--format", "json"], check=False)
    lib_dir: Optional[Path] = None
    try:
        cfg = json.loads(proc.stdout or "{}")
        node = cfg.get("directories") or (cfg.get("config") or {}).get("directories") or {}
        user = node.get("user")
        if user:
            lib_dir = Path(user) / "libraries"
    except Exception:
        pass
    if lib_dir is None:
        for guess in (Path.home() / "Arduino" / "libraries",
                      Path.home() / "Documents" / "Arduino" / "libraries"):
            if guess.is_dir():
                lib_dir = guess
                break
    if lib_dir is None or not lib_dir.is_dir():
        c.warn("Could not locate the Arduino libraries folder; skipping lv_conf.h check")
        return

    source = sketch_dir / "lv_conf.h"
    target = lib_dir / "lv_conf.h"
    if not source.is_file():
        c.warn(f"{source} is missing from the sketch")
        return

    if target.is_file() and target.read_bytes() == source.read_bytes():
        c.ok("lv_conf.h is in place")
        return

    if dry_run:
        c.info(c.dim(f"[dry-run] copy {source} -> {target}"))
        return

    if target.is_file():
        backup = target.with_suffix(".h.nomad-backup")
        shutil.copy2(target, backup)
        c.warn(f"Existing lv_conf.h backed up to {backup.name}")
    shutil.copy2(source, target)
    c.ok(f"Installed lv_conf.h into {lib_dir}")


# ----------------------------------------------------------------- build --


def find_sketch(repo_root: Optional[Path] = None) -> Path:
    candidates = []
    if repo_root:
        candidates.append(Path(repo_root) / "firmware" / "JcorpNomadProject")
    here = Path(__file__).resolve()
    for parent in here.parents:
        candidates.append(parent / "firmware" / "JcorpNomadProject")
    candidates.append(Path.cwd() / "firmware" / "JcorpNomadProject")

    for path in candidates:
        if (path / "JcorpNomadProject.ino").is_file():
            return path
    raise BuildError(
        "Could not find firmware/JcorpNomadProject. Run this from inside the "
        "jcorp-nomad checkout, or pass --sketch /path/to/JcorpNomadProject."
    )


def compile_firmware(
    cli: ArduinoCli,
    board: boards.Board,
    flash_mb: int,
    sketch_dir: Path,
    dry_run: bool = False,
    clean: bool = False,
    extra_properties: Optional[List[str]] = None,
) -> Path:
    fqbn = boards.build_fqbn(board, flash_mb)
    scheme, _, app_bytes = boards.partition_choice(flash_mb)

    c.info(f"Board  : {board.name}")
    c.info(f"FQBN   : {fqbn}")
    c.info(f"Layout : {scheme} ({c.human_bytes(app_bytes)} for the app)")

    args = ["compile", "--fqbn", fqbn, "--export-binaries"]
    if clean:
        args.append("--clean")
    args += [
        "--build-property", f"compiler.cpp.extra_flags=-DNOMAD_BOARD={board.nomad_board}",
        "--build-property", f"compiler.c.extra_flags=-DNOMAD_BOARD={board.nomad_board}",
    ]
    for prop in extra_properties or []:
        args += ["--build-property", prop]
    args.append(str(sketch_dir))

    c.step("Compiling (a few minutes on a first run)")
    cli.run(args, stream=True, dry_run=dry_run, timeout=5400)

    out_dir = sketch_dir / "build" / _fqbn_dirname(fqbn)
    if dry_run:
        return out_dir
    if not (out_dir / "flash_args").is_file():
        # arduino-cli sanitises the FQBN differently across versions; find it.
        found = sorted((sketch_dir / "build").glob("*/flash_args"))
        if not found:
            raise BuildError(f"Build finished but no output found under {sketch_dir / 'build'}")
        out_dir = found[0].parent
    return out_dir


def _fqbn_dirname(fqbn: str) -> str:
    """arduino-cli names the export directory after the FQBN without options."""
    return ".".join(fqbn.split(":")[:3])
