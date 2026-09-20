#!/usr/bin/env python3
"""Mirror SD_Card_Template/ onto a mounted Nomad SD card.

Hash-based: only files whose content differs are copied, each via an atomic
.nmupd.tmp + rename so a yanked card never holds a half-written page. Never
deletes card content except the explicit CLEANUP list below (dead files that
were removed from the template but linger on provisioned cards).

Usage:
  ./sync_card.py            # auto-detect the card under /media/$USER
  ./sync_card.py /media/jstudner/NOMAD
"""

import hashlib
import os
import shutil
import subprocess
import sys

TEMPLATE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "SD_Card_Template")

# stale files removed from the template that should also leave the card
CLEANUP = [
    "zimtest.html",
    "assets/nomad-zimfs.js",
    "assets/zip.min.js",
]

# never push local dev leftovers
SKIP_NAMES = {".DS_Store", "Thumbs.db"}

# updates the APP on a live card - user content and device-owned state never
# come from the template: content buckets hold the real library (placeholders
# are provisioning's job), .system-ui.json is written by the firmware, and
# config/ holds settings.json (wifi + admin passwords)
SKIP_DIRS = {"Movies", "Shows", "Music", "Books", "Gallery", "Files",
             "Cookbook", "Workshop", "Documents", "Archive", "Maps", "config"}
SKIP_ROOT_FILES = {".system-ui.json", ".system-theme.json", "media.json"}


def sha256(path, bufsize=1 << 20):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(bufsize)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def find_card():
    base = os.path.join("/media", os.environ.get("USER", "jstudner"))
    if not os.path.isdir(base):
        return None
    for name in sorted(os.listdir(base)):
        root = os.path.join(base, name)
        # a provisioned Nomad card has the menu page and a config dir at its root
        if os.path.ismount(root) and os.path.isfile(os.path.join(root, "menu.html")) \
                and os.path.isdir(os.path.join(root, "config")):
            return root
    return None


def main():
    card = sys.argv[1] if len(sys.argv) > 1 else find_card()
    if not card or not os.path.ismount(card):
        print("No mounted Nomad card found (looked for a mount under /media with menu.html + config/).")
        print("Insert the card, or pass the mount point explicitly.")
        sys.exit(1)
    print(f"Template: {TEMPLATE}")
    print(f"Card:     {card}")

    copied, same, errors = 0, 0, 0
    for dirpath, dirnames, filenames in os.walk(TEMPLATE):
        rel_dir = os.path.relpath(dirpath, TEMPLATE)
        if rel_dir == ".":
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            if name in SKIP_NAMES or name.endswith(".nmupd.tmp"):
                continue
            if rel_dir == "." and name in SKIP_ROOT_FILES:
                continue
            rel = name if rel_dir == "." else os.path.join(rel_dir, name)
            src = os.path.join(dirpath, name)
            dst = os.path.join(card, rel)
            try:
                if os.path.isfile(dst) and os.path.getsize(dst) == os.path.getsize(src) \
                        and sha256(dst) == sha256(src):
                    same += 1
                    continue
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                tmp = dst + ".nmupd.tmp"
                shutil.copyfile(src, tmp)
                os.replace(tmp, dst)
                copied += 1
                print(f"  updated {rel}")
            except OSError as e:
                errors += 1
                print(f"  ERROR   {rel}: {e}")

    removed = 0
    for rel in CLEANUP:
        dst = os.path.join(card, rel)
        if os.path.isfile(dst):
            try:
                os.remove(dst)
                removed += 1
                print(f"  removed {rel}")
            except OSError as e:
                errors += 1
                print(f"  ERROR   removing {rel}: {e}")

    print("Flushing writes (sync)...")
    subprocess.run(["sync"], check=False)
    print(f"Done: {copied} updated, {removed} removed, {same} unchanged, {errors} errors.")
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
