"""Terminal output and prompts. No third-party dependencies."""

from __future__ import annotations

import os
import shutil
import sys
from typing import Iterable, List, Optional, Sequence


def _supports_colour() -> bool:
    if os.environ.get("NO_COLOR"):
        return False
    if not sys.stdout.isatty():
        return False
    if sys.platform == "win32":
        # Windows 10+ consoles understand ANSI once VT processing is on;
        # colorama-free enablement via the kernel32 call below.
        try:
            import ctypes

            kernel32 = ctypes.windll.kernel32
            kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), 7)
            return True
        except Exception:
            return False
    return True


_COLOUR = _supports_colour()


def _c(code: str, text: str) -> str:
    return f"\033[{code}m{text}\033[0m" if _COLOUR else text


def bold(t: str) -> str:
    return _c("1", t)


def dim(t: str) -> str:
    return _c("2", t)


def red(t: str) -> str:
    return _c("31", t)


def green(t: str) -> str:
    return _c("32", t)


def yellow(t: str) -> str:
    return _c("33", t)


def cyan(t: str) -> str:
    return _c("36", t)


VERBOSE = False


def set_verbose(on: bool) -> None:
    global VERBOSE
    VERBOSE = on


def heading(text: str) -> None:
    width = min(shutil.get_terminal_size((80, 24)).columns, 78)
    print()
    print(bold(text))
    print(dim("-" * min(len(text), width)))


def info(text: str) -> None:
    print(f"  {text}")


def step(text: str) -> None:
    print(f"{cyan('  ->')} {text}")


def ok(text: str) -> None:
    print(f"{green('  ok')} {text}")


def warn(text: str) -> None:
    print(f"{yellow(' warn')} {text}")


def error(text: str) -> None:
    print(f"{red('  !!')} {text}", file=sys.stderr)


def debug(text: str) -> None:
    if VERBOSE:
        print(dim(f"     {text}"))


def table(headers: Sequence[str], rows: Iterable[Sequence[str]]) -> None:
    rows = [[str(c) for c in r] for r in rows]
    widths = [len(h) for h in headers]
    for r in rows:
        for i, cell in enumerate(r):
            if i < len(widths):
                widths[i] = max(widths[i], len(cell))

    line = "  " + "  ".join(h.ljust(widths[i]) for i, h in enumerate(headers))
    print(bold(line.rstrip()))
    for r in rows:
        print("  " + "  ".join(c.ljust(widths[i]) for i, c in enumerate(r)).rstrip())


def human_bytes(n: int) -> str:
    if n <= 0:
        return "0 B"
    for unit, size in (("TB", 1 << 40), ("GB", 1 << 30), ("MB", 1 << 20), ("KB", 1 << 10)):
        if n >= size:
            return f"{n / size:.1f} {unit}"
    return f"{n} B"


# ---------------------------------------------------------------- prompts --

ASSUME_YES = False


def set_assume_yes(on: bool) -> None:
    global ASSUME_YES
    ASSUME_YES = on


def confirm(question: str, default: bool = False) -> bool:
    if ASSUME_YES:
        print(f"  {question} {dim('[auto-yes]')}")
        return True
    suffix = "[Y/n]" if default else "[y/N]"
    while True:
        try:
            answer = input(f"  {question} {suffix} ").strip().lower()
        except EOFError:
            return default
        if not answer:
            return default
        if answer in ("y", "yes"):
            return True
        if answer in ("n", "no"):
            return False


def confirm_destructive(question: str, phrase: str) -> bool:
    """Type-the-phrase confirmation, used before anything that erases data.

    Deliberately not covered by --yes: erasing the wrong disk is not the kind
    of mistake a scripted run should be able to make silently. Use
    --i-know-what-im-doing if you really want it unattended.
    """
    if ASSUME_YES and FORCE_DESTRUCTIVE:
        print(f"  {question} {dim('[auto-confirmed]')}")
        return True
    print()
    print(f"  {yellow(question)}")
    try:
        answer = input(f"  Type {bold(phrase)} to continue: ").strip()
    except EOFError:
        return False
    return answer == phrase


FORCE_DESTRUCTIVE = False


def set_force_destructive(on: bool) -> None:
    global FORCE_DESTRUCTIVE
    FORCE_DESTRUCTIVE = on


def choose(prompt: str, options: List[tuple], default_index: Optional[int] = None):
    """options: list of (value, label). Returns the chosen value."""
    if not options:
        return None
    if len(options) == 1:
        print(f"  {prompt}: {options[0][1]}")
        return options[0][0]

    print()
    print(f"  {bold(prompt)}")
    for i, (_, label) in enumerate(options, 1):
        marker = "*" if default_index is not None and i - 1 == default_index else " "
        print(f"   {marker} {i}) {label}")

    while True:
        hint = f" [{default_index + 1}]" if default_index is not None else ""
        try:
            raw = input(f"  Choice{hint}: ").strip()
        except EOFError:
            raw = ""
        if not raw and default_index is not None:
            return options[default_index][0]
        if raw.isdigit() and 1 <= int(raw) <= len(options):
            return options[int(raw) - 1][0]
        error("Enter one of the numbers listed above.")


class ProgressBar:
    """Single-line byte progress. Falls back to periodic prints when piped."""

    def __init__(self, total: int, label: str = ""):
        self.total = max(total, 1)
        self.label = label
        self.done = 0
        self._last_render = -1.0
        self._tty = sys.stdout.isatty()

    def advance(self, n: int) -> None:
        self.done += n
        pct = min(self.done / self.total, 1.0)
        if pct - self._last_render < 0.01 and pct < 1.0:
            return
        self._last_render = pct
        if self._tty:
            width = 28
            filled = int(width * pct)
            bar = "#" * filled + "-" * (width - filled)
            sys.stdout.write(
                f"\r     [{bar}] {pct * 100:5.1f}%  {self.label[:28]:<28}"
            )
            sys.stdout.flush()

    def finish(self) -> None:
        if self._tty:
            sys.stdout.write("\r" + " " * 78 + "\r")
            sys.stdout.flush()
