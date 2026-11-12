#!/usr/bin/env bash
# build-p4.sh - compile (and optionally flash) the Jcorp Nomad P4 firmware.
#
# Uses the arduino-cli bundled inside the flatpak Arduino IDE2, run directly on the host.
# It is a static Go binary, so no sandbox is involved. Only the path differs between a
# user flatpak install and a system one.
#
#   ./build-p4.sh              compile firmware + build the interface image
#   ./build-p4.sh --upload     compile, then flash BOTH the firmware and the interface
#   ./build-p4.sh --ui-only    rebuild and flash only the interface image (no compile)
#   ./build-p4.sh --ui-image   build only the interface image and say what is in it (no
#                              compile, no flashing; --nano for the NANO's partition)
#   ./build-p4.sh --no-ui      skip the interface image entirely
#   ./build-p4.sh --upload -p /dev/ttyACM1
#   ./build-p4.sh --selftest --upload   # run a thumbnail pass at boot, for testing
#   ./build-p4.sh --fstest --upload     # run the storage self-test at boot, likewise
#   ./build-p4.sh --s3         build the Mk4 (ESP32-S3) firmware from this same tree
#   ./build-p4.sh --nano       build for the ESP32-P4-NANO (wired NIC, USB-A, 16MB flash)
#
# The interface ships WITH the firmware. It is built into a LittleFS image straight from
# this repo's SD_Card_Template/, so a board always serves what is in the tree and never
# whatever is on the inserted card.
#
# Requires esp32 core 3.3.3 and the pinned libraries: ArduinoJson 7.3.0,
# Async TCP (ESP32Async) 3.4.7, ESP Async WebServer (ESP32Async) 3.7.1, lvgl 8.3.10,
# SdFat 2.3.0.

set -euo pipefail

SKETCHDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/JcorpNomadProject" && pwd)"
BUILDROOT="${NOMAD_BUILD_DIR:-/tmp/nomad-build}"

TARGET=p4
P4BOARD=wifi6
UPLOAD=0
UIIMAGE=1
UIONLY=0
UIIMAGEONLY=0
PORT=""
SELFTEST=0
SELFTEST_DEF=""
FSTEST=0
RADIO_OFF=0
while [ $# -gt 0 ]; do
  case "$1" in
    --upload)  UPLOAD=1 ;;
    --s3)      TARGET=s3 ;;
    --p4)      TARGET=p4 ;;
    --nano)    TARGET=p4; P4BOARD=nano ;;
    --no-ui)   UIIMAGE=0 ;;
    --ui-only) UIONLY=1; UPLOAD=1 ;;
    --ui-image) UIIMAGEONLY=1 ;;
    -p)        PORT="$2"; shift ;;
    --selftest) SELFTEST=1 ;;
    --fstest)   FSTEST=1 ;;
    --radio-off) RADIO_OFF=1 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

# A selftest build runs one thumbnail pass at boot, so the codec path can be exercised
# from a serial console with no client on the hotspot. Never in a normal build.
[ "$SELFTEST" = "1" ] && SELFTEST_DEF="-DNOMAD_THUMB_SELFTEST=1"
# The storage layer is only reachable over HTTP, which needs a client on the hotspot.
# This runs the same checks from a serial console. Never in a normal build.
[ "$FSTEST" = "1" ] && SELFTEST_DEF="$SELFTEST_DEF -DNOMAD_FS_SELFTEST=1"
# Bring the board up with the radio untouched, so a co-processor that is not answering
# cannot force ESP-Hosted to restart the host in a loop.
[ "$RADIO_OFF" = "1" ] && SELFTEST_DEF="$SELFTEST_DEF -DNOMAD_RADIO_OFF=1"
# Both P4 boards are the same silicon and no predefined macro separates them, so the
# board identity has to be passed in. nomad_board.h keys every difference off this.
[ "$P4BOARD" = "nano" ] && SELFTEST_DEF="$SELFTEST_DEF -DNOMAD_BOARD_P4_NANO=1"
# NOMAD_EXTRA_DEFS="-DFOO=1 ..." adds test defines without a flag of their own
SELFTEST_DEF="$SELFTEST_DEF ${NOMAD_EXTRA_DEFS:-}"

# The prebuilt IDF libraries the core links against. Also where Espressif ships the
# ESP-Hosted slave firmware for the co-processor, at the version the host expects.
LIBS=$(ls -d "$HOME"/.arduino15/packages/esp32/tools/esp32-arduino-libs/idf-release_*/ 2>/dev/null | head -1 || true)
LIBS="${LIBS%/}"

# locate the flatpak's arduino-cli
ACLI=""
for base in "$HOME/.local/share/flatpak" /var/lib/flatpak; do
  cand=$(ls -d "$base"/app/cc.arduino.IDE2/x86_64/stable/*/files/arduino-ide/resources/app/lib/backend/resources/arduino-cli 2>/dev/null | head -1 || true)
  [ -n "$cand" ] && ACLI="$cand" && break
done
if [ -z "$ACLI" ]; then
  echo "arduino-cli not found inside the Arduino IDE2 flatpak." >&2
  echo "The snap arduino-cli cannot build this: its home interface cannot read ~/.arduino15" >&2
  echo "and its glibc is too old for esp32 3.3.3's PyInstaller tools." >&2
  exit 1
fi

export ARDUINO_DIRECTORIES_DATA="$HOME/.arduino15"
export ARDUINO_DIRECTORIES_USER="$HOME/Arduino"

# per-target settings
if [ "$TARGET" = p4 ]; then
  # Both P4 boards take their Type-C to a CH34x USB-UART bridge, not to the SoC's own USB,
  # so Serial is plain UART0 (GPIO 37/38) and CDCOnBoot stays off. USBMode=hwcdc keeps
  # TinyUSB from claiming an OTG controller: the high-speed one is wanted for storage.
  SIZETOOL=riscv32-esp-elf-size
  if [ "$P4BOARD" = nano ]; then
    # The NANO carries 16 MB of flash (GD25Q128), not the WIFI6's 32 MB, so the 32 MB
    # scheme's UI partition would sit at 26 MB, past the end of the chip. This layout
    # gives 9.9 MB for the interface, more than the 32 MB board's 6.75 MB. Its data
    # partition is labelled "ffat". NomadUI mounts NOMAD_UI_PART_LABEL, and LittleFS is
    # fine in a partition whose subtype says fat. esp_sr_16 is the only stock 16 MB
    # scheme with a "spiffs" label, but it carries upload.extra_flags for a speech model,
    # so every flash fails on a missing srmodels.bin. Don't switch to it without dealing
    # with that.
    FQBN="esp32:esp32:esp32p4:USBMode=hwcdc,FlashMode=qio,FlashSize=16M,PSRAM=enabled,PartitionScheme=app3M_fat9M_16MB,DebugLevel=info"
    BUILDPATH="$BUILDROOT/p4nano"
    UI_PART_OFFSET=0x610000
    UI_PART_SIZE=0x9E0000
  else
    FQBN="esp32:esp32:esp32p4:USBMode=hwcdc,FlashMode=qio,FlashSize=32M,PSRAM=enabled,PartitionScheme=app13M_data7M_32MB,DebugLevel=info"
    BUILDPATH="$BUILDROOT/p4"
    UI_PART_OFFSET=0x1910000
    UI_PART_SIZE=0x6C0000
  fi
else
  FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB"
  BUILDPATH="$BUILDROOT/s3"
  SIZETOOL=xtensa-esp32s3-elf-size
fi

# lvgl needs LV_CONF_INCLUDE_SIMPLE plus the sketch dir on the include path, and both the
# C and C++ flags need it because lvgl ships .c files. The usb_msc include dirs mirror the
# INCLUDE_DIRS/PRIV_INCLUDE_DIRS of the upstream component's CMakeLists.
INCLUDES="-DLV_CONF_INCLUDE_SIMPLE -I$SKETCHDIR"
INCLUDES="$INCLUDES -I$SKETCHDIR/src/usb_msc/include"
INCLUDES="$INCLUDES -I$SKETCHDIR/src/usb_msc/include/usb"
INCLUDES="$INCLUDES -I$SKETCHDIR/src/usb_msc/include/esp_private"
INCLUDES="$INCLUDES -I$SKETCHDIR/src/usb_msc/private_include"

mkdir -p "$BUILDPATH"

# interface image
# The set here MUST match nomadUiBelongsInFlash() in NomadUI.cpp: root-level
# .html/.css/.js, four named files and /assets, except the maps stack (assets/maps/) and
# any single file over NOMAD_UI_FLASH_MAX_FILE (200 KB), which stay on the card and are
# served from there. Anything else on a card is media and has no business in flash.
#
# The whole interface is ~10 MB, against 6.75 MB on the WIFI6 and 9.9 MB on the NANO.
# What stays on the card is the map engine and fonts, and the PDF, comic, e-book and chess
# engines, each only used to open something that's on the card anyway. The rest is ~3 MB,
# which leaves room on both boards for the interface updater to stage a new copy.
#
# Partition geometry is set per board above, from the scheme's own CSV (the "spiffs"
# line of default_32MB.csv for WIFI6, the "ffat" line of app3M_fat9M_16MB.csv for NANO).
# It must match the scheme in the FQBN. The image is written to a raw offset, so a
# mismatch flashes a perfectly good filesystem into the middle of something else.
# LittleFS geometry comes from the P4 sdkconfig: CONFIG_LITTLEFS_PAGE_SIZE=256, and the
# block size is the flash sector, 4096. Getting either wrong produces an image that
# builds fine and then will not mount.
UI_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../SD_Card_Template" && pwd)"
# Beside the build folder, not in it: arduino-cli empties the build folder whenever the
# build options change (a new -D does it), which would delete the image before --upload.
UI_DIR="$BUILDPATH-ui"
mkdir -p "$UI_DIR"
UI_IMAGE="$UI_DIR/ui.littlefs.bin"
UI_FLASH_MAX_FILE=$((200 * 1024))   # NOMAD_UI_FLASH_MAX_FILE in NomadUI.h

build_ui_image() {
  local MKL
  MKL=$(ls -d "$HOME"/.arduino15/packages/esp32/tools/mklittlefs/*/mklittlefs 2>/dev/null | head -1 || true)
  [ -n "$MKL" ] || { echo "mklittlefs not found under ~/.arduino15" >&2; return 1; }

  local STAGE="$UI_DIR/ui-stage"
  rm -rf "$STAGE"
  mkdir -p "$STAGE/assets"

  ( cd "$UI_SRC" && for f in *.html *.css *.js; do [ -f "$f" ] && cp "$f" "$STAGE/"; done ) || true
  for f in default-themes.json favicon.ico Logo.png placeholder.jpg; do
    [ -f "$UI_SRC/$f" ] && cp "$UI_SRC/$f" "$STAGE/"
  done
  # -R, and copy the directory's contents rather than a glob: assets/ has subdirectories
  # (leaflet, and its images/), and a plain `cp assets/*` drops every one of them.
  cp -R "$UI_SRC"/assets/. "$STAGE/assets/" 2>/dev/null || true
  # what stays on the card (see above): listed, then left out of the stage
  local LEFT="$UI_DIR/ui-on-card.txt"
  ( cd "$STAGE" && { find assets/maps -type f 2>/dev/null; find . -type f -size +$((UI_FLASH_MAX_FILE / 1024))k ! -path './assets/maps/*' | sed 's|^\./||'; } | sort ) > "$LEFT"
  # an Emscripten engine's memory file goes where its .js goes (libunrar.js.mem): the two
  # must come from the same build, and only the .js is big enough to stay on the card
  ( cd "$STAGE" && while IFS= read -r f; do case "$f" in *.js) if [ -f "$f.mem" ]; then echo "$f.mem"; fi;; esac; done < "$LEFT" ) > "$LEFT.mem"
  sort -u "$LEFT" "$LEFT.mem" -o "$LEFT"; rm -f "$LEFT.mem"
  ( cd "$STAGE" && while IFS= read -r f; do rm -f -- "$STAGE/$f"; done < "$LEFT" )
  # what those card copies must be for this build (fonts aside: data, not code), so the admin
  # page can tell, offline, when the card's maps code or reader engines are from another build
  ( cd "$UI_SRC" && grep -v '^assets/maps/fonts/' "$LEFT" | while IFS= read -r f; do
      printf '%s %s %s\n' "$(sha256sum -- "$f" | cut -c1-64)" "$(stat -c %s -- "$f")" "$f"; done ) |
    python3 -c 'import json,sys; print(json.dumps({"format":1,"files":{"/"+l.split(" ",2)[2].rstrip("\n"):[int(l.split(" ")[1]),l.split(" ")[0]] for l in sys.stdin if l.strip()}}, separators=(",",":")))' \
    > "$STAGE/assets/uicard.json"
  find "$STAGE" -depth -type d -empty -delete
  mkdir -p "$STAGE"

  # A build id the firmware can report, so you can see which interface a board is serving
  # without trusting the flashing procedure. Content-derived, so rebuilding an unchanged
  # tree gives the same id.
  local ID
  ID=$(cd "$STAGE" && find . -type f ! -name .uiversion -print0 | sort -z | xargs -0 cat | sha256sum | cut -c1-12)
  local COUNT BYTES
  COUNT=$(find "$STAGE" -type f | wc -l)
  BYTES=$(du -sb "$STAGE" | cut -f1)
  printf '%s\n' "$ID" > "$STAGE/.uiversion"

  if [ "$BYTES" -ge $((UI_PART_SIZE)) ]; then
    echo "interface is ${BYTES} bytes but the partition is $((UI_PART_SIZE)); it will not fit" >&2
    return 1
  fi

  # mklittlefs exits 0 even when it runs out of room part way ("No more free space",
  # "error adding file"), leaving an image with files missing. Its output is the only sign
  local MKOUT
  MKOUT=$("$MKL" -c "$STAGE" -b 4096 -p 256 -s "$UI_PART_SIZE" "$UI_IMAGE" 2>&1) || { echo "$MKOUT" >&2; return 1; }
  if printf '%s' "$MKOUT" | grep -qi 'error'; then
    printf '%s\n' "$MKOUT" | grep -i 'error' | head -5 >&2
    echo "interface does not fit the $((UI_PART_SIZE)) byte partition" >&2
    rm -f "$UI_IMAGE"
    return 1
  fi
  local BLOCKS
  BLOCKS=$(find "$STAGE" -type f -printf '%s\n' | awk '{b += int(($1 + 4095) / 4096)} END {print b * 4096}')
  echo "=== interface image: $COUNT files, $BYTES bytes (~$BLOCKS in 4 KB blocks) of $((UI_PART_SIZE)), id $ID ==="
  echo "    $UI_IMAGE"
  echo "    left on the card: $(wc -l < "$LEFT") files, $(cd "$UI_SRC" && tr '\n' '\0' < "$LEFT" | xargs -0 -r du -cb 2>/dev/null | tail -1 | cut -f1) bytes (list: $LEFT)"
}

if [ "$TARGET" = p4 ] && [ "$UIIMAGE" = 1 ]; then
  build_ui_image
fi
[ "$UIIMAGEONLY" = 1 ] && exit 0

if [ "$UIONLY" = 1 ]; then
  ESPTOOL=$(ls -d "$HOME"/.arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | head -1 || true)
  [ -n "$ESPTOOL" ] || { echo "esptool not found under ~/.arduino15" >&2; exit 1; }
  if [ -z "$PORT" ]; then PORT=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1 || true); fi
  [ -n "$PORT" ] || { echo "no serial port found; pass -p /dev/ttyACMx" >&2; exit 1; }
  [ -w "$PORT" ] || sudo chmod a+rw "$PORT"
  echo "=== flashing interface image only to $UI_PART_OFFSET on $PORT ==="
  "$ESPTOOL" --chip esp32p4 --port "$PORT" --baud 921600 write-flash "$UI_PART_OFFSET" "$UI_IMAGE"
  exit 0
fi

echo "=== compiling Nomad ($TARGET) ==="
echo "    sketch: $SKETCHDIR"
echo "    build:  $BUILDPATH"
"$ACLI" compile \
  --fqbn "$FQBN" \
  --build-property "compiler.cpp.extra_flags=$INCLUDES $SELFTEST_DEF" \
  --build-property "compiler.c.extra_flags=$INCLUDES $SELFTEST_DEF" \
  --build-path "$BUILDPATH" \
  "$SKETCHDIR"

ELF="$BUILDPATH/JcorpNomadProject.ino.elf"
if [ -f "$ELF" ]; then
  TOOL=$(find "$HOME/.arduino15/packages/esp32/tools" -name "$SIZETOOL" -type f 2>/dev/null | head -1 || true)
  if [ -n "$TOOL" ]; then
    echo "=== static RAM (-fdata-sections + --gc-sections already applied) ==="
    "$TOOL" -A "$ELF" | awk '/\.data|\.bss|\.dram/ {print}'
  fi
  # keep the elf: a coredump only decodes against the exact build it came from
  cp -f "$ELF" "$BUILDPATH/last-good.elf" 2>/dev/null || true
fi

if [ "$UPLOAD" = 1 ]; then
  if [ -z "$PORT" ]; then
    PORT=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1 || true)
  fi
  [ -n "$PORT" ] || { echo "no serial port found; pass -p /dev/ttyACMx" >&2; exit 1; }
  # A user who isn't in the dialout group can't open the port (root:dialout).
  if [ ! -w "$PORT" ]; then
    echo "=== $PORT is not writable, taking it for this session ==="
    sudo chmod a+rw "$PORT"
  fi
  if [ "$TARGET" = p4 ] && [ "$UIIMAGE" = 1 ] && [ ! -f "$UI_IMAGE" ]; then
    echo "the interface image is missing ($UI_IMAGE); nothing was flashed" >&2
    exit 1
  fi
  echo "=== flashing $PORT ==="
  "$ACLI" upload -p "$PORT" --fqbn "$FQBN" --input-dir "$BUILDPATH" "$SKETCHDIR"

  # Firmware and interface always go on together so the two can't drift.
  if [ "$TARGET" = p4 ] && [ "$UIIMAGE" = 1 ] && [ -f "$UI_IMAGE" ]; then
    ESPTOOL=$(ls -d "$HOME"/.arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | head -1 || true)
    if [ -n "$ESPTOOL" ]; then
      echo "=== flashing interface image to $UI_PART_OFFSET ==="
      "$ESPTOOL" --chip esp32p4 --port "$PORT" --baud 921600 write-flash "$UI_PART_OFFSET" "$UI_IMAGE"
    else
      echo "esptool not found; the interface image was NOT flashed" >&2
    fi
  fi
fi
