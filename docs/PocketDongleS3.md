# Jcorp Nomad on the Pocket-Dongle-S3 (0.96")

This is the build guide for running Nomad on the USB-stick style ESP32-S3
boards — the **GNPE Pocket-Dongle-S3-0.96**, the **LilyGO T-Dongle-S3** and the
various clones of that design. They all share the same recipe: an ESP32-S3
N16R8 module, a 0.96" ST7735 IPS panel at 160x80, a microSD slot hidden inside
the USB-A shell, an APA102 status LED and a boot button.

Nomad still builds for the original Waveshare ESP32-S3-LCD-1.47 board — see
[Switching boards](#switching-boards).

---

## 1. Before you flash: run the self-test

These dongles are made by several factories and the pin maps are *usually*
identical to LilyGO's reference design, but not guaranteed. Flash
`firmware/NomadHardwareTest` first — it is a single self-contained sketch that
reuses the same `board_config.h` the firmware does.

It checks, and tells you exactly what to change if something is off:

| Step | Verifies | Fix if wrong |
| --- | --- | --- |
| Chip report | 16 MB flash + 8 MB OPI PSRAM detected | Board menu settings (below) |
| Backlight ramp | `LCD_BL_ACTIVE_LEVEL` polarity | Flip it in `board_config.h` |
| Colour bars | Controller, colour order, inversion | `LCD_MADCTL` BGR bit `0x08`, `LCD_INVERT_COLORS` |
| Edge frame | Window offsets | `LCD_OFFSET_X` / `LCD_OFFSET_Y` |
| LED sweep | LED type and channel order | `NOMAD_LED_TYPE`, `led_write()` in `RGB_lamp.cpp` |
| SD sweep | SD pin map — **tries known alternatives automatically** | Copy the pins it reports into `board_config.h` |
| Button | `BOOT_BUTTON_PIN` | Change it in `board_config.h` |

Everything the port needs to know about your board lives in one file:
`firmware/JcorpNomadProject/board_config.h`.

---

## 2. Arduino IDE settings

Install **esp32 by Espressif Systems, version 3.x** (the firmware uses the 3.x
`ledcAttach()` API). Then, under *Tools*:

| Setting | Value |
| --- | --- |
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | Enabled |
| CPU Frequency | 240 MHz |
| Core Debug Level | None |
| USB DFU On Boot | Disabled |
| Erase All Flash Before Sketch Upload | Disabled |
| Events Run On | Core 1 |
| Flash Mode | QIO 80 MHz |
| Flash Size | **16MB (128Mb)** |
| JTAG Adapter | Disabled |
| Arduino Runs On | Core 1 |
| USB Firmware MSC On Boot | Disabled |
| Partition Scheme | **16M Flash (3MB APP/9.9MB FATFS)** |
| PSRAM | **OPI PSRAM** |
| Upload Mode | UART0 / Hardware CDC |
| Upload Speed | 921600 |
| USB Mode | Hardware CDC and JTAG |

> **PSRAM must be OPI**, not QSPI. The N16R8 module has octal PSRAM, and
> selecting the wrong mode either fails to detect it or hangs at boot. Octal
> PSRAM also permanently occupies GPIO 33–37, which is why nothing in the pin
> map uses those.

Required libraries (same as the original board):

* `lvgl` 8.3.x — copy `firmware/JcorpNomadProject/lv_conf.h` next to the
  library folder as usual
* `ESPAsyncWebServer` + `AsyncTCP`
* `ArduinoJson`
* `SdFat`

### Getting into the bootloader

The stick has no reset button. To force download mode: hold the boot button
while plugging it in. From a running Nomad you can also POST to `/flash-mode`
from the admin page.

---

## 3. Pin map

Taken from LilyGO's T-Dongle-S3 reference design, which the Pocket-Dongle
follows. Confirm with the self-test if in doubt.

### Display — ST7735, 160x80 landscape

| Signal | GPIO |
| --- | --- |
| MOSI / SDA | 3 |
| SCLK | 5 |
| CS | 4 |
| DC | 2 |
| RST | 1 |
| Backlight | 38 (active **LOW**) |

The 160x80 panel sits inside the ST7735's 132x162 GRAM, so the window is offset
by `X=1, Y=26` with `MADCTL = 0xA8` (MY | MV | BGR) and inversion on. SPI runs
at 27 MHz, which redraws the whole frame in about 10 ms; raise `LCD_SPI_FREQ`
to 40 MHz if you want, or drop to 20 MHz if you see speckle.

### microSD — SDMMC, 4-bit

| Signal | GPIO |
| --- | --- |
| CLK | 12 |
| CMD | 16 |
| D0 | 14 |
| D1 | 17 |
| D2 | 21 |
| D3 | 18 |

The firmware mounts in tiers — 4-bit/40 MHz, then 4-bit/20 MHz, then
1-bit/20 MHz, then 1-bit/400 kHz — and logs which one took. It **never**
formats the card on a failed mount.

### Status LED — APA102

| Signal | GPIO |
| --- | --- |
| Data | 40 |
| Clock | 39 |

### Button

Boot button on GPIO 0.

---

## 4. Using it

### On-screen pages

The 160x80 screen carries three pages. **Tap** the boot button to cycle:

1. **Connection** — SSID in the title bar, AP IP address, connected user count,
   SD usage bar
2. **System** — free heap, free PSRAM, die temperature, uptime
3. **Storage** — used / free / card size

The title bar always shows the SSID plus a Wi-Fi and an SD icon, which are grey
when the subsystem is down and green when it is up.

![Layout of the three dongle pages plus the message overlay](dongle-ui-layout.png)

*Layout mock-up rendered from the same geometry as `ui_screen_mini.c` — not a
photo of a panel. Substitute fonts and icons; on the device these are LVGL's
Montserrat 12/14 and the `LV_SYMBOL_WIFI` / `LV_SYMBOL_SD_CARD` glyphs.*

### USB mass storage

**Hold** the boot button for ~1.2 s and the stick reboots as a plain USB drive
so you can drag media straight onto the card. Eject it from the host (or press
the button again) and it reboots back into media-server mode. `/enterUsb` from
the admin page does the same thing.

> On the original firmware the boot button was wired to a `FALLING` interrupt
> that rebooted into USB mode immediately, so contact bounce or a static
> discharge could knock the server offline mid-stream. It is now debounced and
> polled, and a tap does something harmless.

---

## 5. Switching boards

One line in `firmware/JcorpNomadProject/board_config.h`:

```c
#define NOMAD_BOARD NOMAD_BOARD_POCKET_DONGLE_S3   // 0.96" USB stick
// #define NOMAD_BOARD NOMAD_BOARD_WAVESHARE_LCD147  // 1.47" Waveshare board
```

or pass `-DNOMAD_BOARD=...` from the build. That single switch selects the LCD
controller and geometry, the SPI/SD/LED pins, the backlight polarity, the LED
backend, the LVGL buffer size and which of the two screen layouts gets
compiled. Nothing else in the firmware is board-aware.

To add a third board, copy one of the `#elif` blocks in `board_config.h` and
fill in the numbers.

---

## 6. Troubleshooting

**Screen stays black, serial looks healthy.**
Backlight polarity. Set `LCD_BL_ACTIVE_LEVEL` to `1` and reflash. The
self-test's ramp step makes this obvious.

**Screen shows a shifted or wrapped image, with a band of noise at one edge.**
Window offsets. Run the self-test and adjust `LCD_OFFSET_X` / `LCD_OFFSET_Y`
until the 1-pixel white frame sits exactly on the physical edge.

**Colours are inverted (a photo negative).**
Toggle `LCD_INVERT_COLORS`.

**Red and blue are swapped.**
Clear the BGR bit: change `LCD_MADCTL` from `0xA8` to `0xA0`. If red/blue are
swapped in the LVGL UI but correct in the self-test's colour bars, it is the
framebuffer byte order instead — set `LV_COLOR_16_SWAP` to `1` in `lv_conf.h`.

**"SDMMC Card initialization failed" at every speed.**
Run the self-test's SD sweep; it tries the other known dongle pin maps and
prints any that mount. Also confirm the card is FAT32 or exFAT — cards over
32 GB formatted by Windows default to exFAT, which works, but NTFS does not.

**PSRAM reports "not detected".**
The board menu is set to QSPI PSRAM or PSRAM is disabled. Set it to OPI PSRAM.

**Boots straight into USB drive mode every time.**
A stuck boot button, or a `USB_MODE` flag left in NVS. Let it enumerate and
eject it once; the eject handler clears the flag.

**Rainbow LED works but solid colours look wrong.**
Fixed in this port — the old code passed `(g, r, b)` to a driver that already
handled channel order, so every colour picked in the admin page came out with
red and green swapped.
