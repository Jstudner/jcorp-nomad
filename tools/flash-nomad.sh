#!/usr/bin/env bash
# Build and flash Nomad S3 firmware, with no physical replug.
#
#   ./tools/flash-nomad.sh [--usbc] <sketch-dir> [build-dir] [port]
#
# --usbc builds for the Waveshare ESP32-S3-LCD-1.47B: the 1.47 with the LCD
# backlight on GPIO 46 instead of 48. That pin is the only firmware-visible
# difference, so -DBOARD_USB_C=1 switches it. Default is the USB-A image, which
# most units are.
#
# esptool's default hard reset drives RTS, and the S3's built-in USB-Serial-JTAG
# has no real RTS line, so the chip stays in the ROM bootloader printing
# "ESP-ROM:esp32s3-20210327" until a power cycle. --after watchdog-reset resets
# through the RTC watchdog instead, so the board boots the app and re-enumerates
# by itself. --before default-reset enters the bootloader fine. If the port is
# ever wedged, /flash-mode on the running device does the same through
# usb_persist_restart(RESTART_BOOTLOADER).
set -euo pipefail

USBC=0
if [ "${1:-}" = "--usbc" ]; then USBC=1; shift; fi

SKETCH="${1:?usage: flash-nomad.sh [--usbc] <sketch-dir> [build-dir] [port]}"
BUILD="${2:-$SKETCH/build}"
# The port moves: a watchdog reset re-enumerates the USB-Serial-JTAG device and
# the kernel hands out the next free minor (ttyACM0 becomes ttyACM1), so a
# hard-coded path fails with "no device". Find it by USB id.
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
CLI="${ARDUINO_CLI:-$(command -v arduino-cli || true)}"
CORE="$HOME/.arduino15/packages/esp32/hardware/esp32/3.3.3"
ESPTOOL="$HOME/.arduino15/packages/esp32/tools/esptool_py/5.1.0/esptool"

[ -x "$CLI" ] || { echo "arduino-cli not found (put it on PATH or set ARDUINO_CLI=)" >&2; exit 1; }
[ -x "$ESPTOOL" ] || { echo "esptool not at $ESPTOOL" >&2; exit 1; }
[ -n "$PORT" ] && [ -c "$PORT" ] || { echo "no Espressif device found (looked for USB 303a:1001 on /dev/ttyACM* and /dev/ttyUSB*)" >&2; exit 1; }
echo "==> port $PORT"

echo "==> compile ($([ "$USBC" = 1 ] && echo 'USB-C / 1.47B, backlight GPIO 46' || echo 'USB-A / 1.47, backlight GPIO 48'))"
# NOMAD_LIBS=dir: use the libraries in dir ahead of the installed ones
LIBARG=(); [ -n "${NOMAD_LIBS:-}" ] && LIBARG=(--libraries "$NOMAD_LIBS")
"$CLI" compile "${LIBARG[@]}" --fqbn "$FQBN" --build-path "$BUILD" \
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
  if nmcli -t -f SSID device wifi list ${IFACE:+ifname "$IFACE"} --rescan yes 2>/dev/null \
       | grep -q '^Jcorp_Nomad$'; then
    echo "    up after $((i*5))s"; exit 0
  fi
done
echo "!! AP did not return in 100s - check the board" >&2
exit 1
