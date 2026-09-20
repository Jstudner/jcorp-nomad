#!/usr/bin/env bash
# Build and flash the Nomad S3 firmware, with no physical replug.
#
#   ./tools/flash-nomad.sh [--usbc] <sketch-dir> [build-dir] [port]
#
# --usbc builds for the Waveshare ESP32-S3-LCD-1.47B. That board is the 1.47 with
# the LCD backlight moved from GPIO 48 to GPIO 46, and that pin is the ONLY
# firmware-visible difference -- the .ino is byte-identical for both, verified
# against the flashing site's own build tree. So one source tree builds both,
# switched by -DBOARD_USB_C=1, which is what the site has always done; it just
# lived in nomadflash/build/usbc-patch.py instead of here. Without the flag you
# get the USB-A image, which is the default and what most units are.
#
# The replug used to be unavoidable: esptool's default hard reset drives RTS,
# and on a board whose console is the ESP32-S3's built-in USB-Serial-JTAG there
# is no real RTS line to drive. The chip stayed in the ROM bootloader printing
# "ESP-ROM:esp32s3-20210327" and nothing else, and only a power cycle cleared it.
#
# --after watchdog-reset is the answer: it resets through the RTC watchdog, which
# does not depend on a serial control line, so the board comes back up running
# the application and re-enumerates on its own. Verified end to end on 2026-09-20.
#
# Entering the bootloader was never the problem -- --before default-reset gets
# there by itself. /flash-mode on the running device does the same thing through
# the core's usb_persist_restart(RESTART_BOOTLOADER), which is worth knowing if
# the port is ever wedged, but it is not needed for a normal flash.
set -euo pipefail

USBC=0
if [ "${1:-}" = "--usbc" ]; then USBC=1; shift; fi

SKETCH="${1:?usage: flash-nomad.sh [--usbc] <sketch-dir> [build-dir] [port]}"
BUILD="${2:-$SKETCH/build}"
# The port is NOT stable. A watchdog reset re-enumerates the USB-Serial-JTAG
# device, and the kernel hands out the next free minor -- ttyACM0 becomes ttyACM1
# and a hard-coded path fails with "no device". Find it by its USB id instead.
find_port() {
  local dev id
  for dev in /dev/ttyACM* /dev/ttyUSB*; do
    [ -c "$dev" ] || continue
    id=$(udevadm info -q property -n "$dev" 2>/dev/null \
         | awk -F= '/^ID_VENDOR_ID=/{v=$2} /^ID_MODEL_ID=/{m=$2} END{print v":"m}')
    [ "$id" = "303a:1001" ] && { echo "$dev"; return 0; }
  done
  return 1
}
PORT="${3:-$(find_port || true)}"

FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB"
USBC_FLAG=""
[ "$USBC" = "1" ] && USBC_FLAG=" -DBOARD_USB_C=1"
CLI="${ARDUINO_CLI:-$HOME/nomadflash-build/bin/arduino-cli}"
CORE="$HOME/.arduino15/packages/esp32/hardware/esp32/3.3.3"
ESPTOOL="$HOME/.arduino15/packages/esp32/tools/esptool_py/5.1.0/esptool"

[ -x "$CLI" ] || { echo "arduino-cli not at $CLI (set ARDUINO_CLI=)" >&2; exit 1; }
[ -x "$ESPTOOL" ] || { echo "esptool not at $ESPTOOL" >&2; exit 1; }
[ -n "$PORT" ] && [ -c "$PORT" ] || { echo "no Espressif device found (looked for USB 303a:1001 on /dev/ttyACM* and /dev/ttyUSB*)" >&2; exit 1; }
echo "==> port $PORT"

echo "==> compile ($([ "$USBC" = 1 ] && echo 'USB-C / 1.47B, backlight GPIO 46' || echo 'USB-A / 1.47, backlight GPIO 48'))"
"$CLI" compile --fqbn "$FQBN" --build-path "$BUILD" \
  --build-property "compiler.cpp.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -I$SKETCH$USBC_FLAG" \
  --build-property "compiler.c.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -I$SKETCH$USBC_FLAG" \
  "$SKETCH"

NAME="$(basename "$SKETCH")"
echo "==> flash $PORT"
# Only the four app images. The FAT partition holding nothing of ours is left
# alone, and so is anything already on the card.
"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after watchdog-reset \
  write-flash -z --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0     "$BUILD/$NAME.ino.bootloader.bin" \
  0x8000  "$BUILD/$NAME.ino.partitions.bin" \
  0xe000  "$CORE/tools/partitions/boot_app0.bin" \
  0x10000 "$BUILD/$NAME.ino.bin"

echo "==> waiting for the AP (no replug needed)"
for i in $(seq 1 20); do
  sleep 5
  if nmcli -t -f SSID device wifi list ifname "${IFACE:-wlp3s0}" --rescan yes 2>/dev/null \
       | grep -q '^Jcorp_Nomad$'; then
    echo "    up after $((i*5))s"; exit 0
  fi
done
echo "!! AP did not return in 100s - check the board" >&2
exit 1
