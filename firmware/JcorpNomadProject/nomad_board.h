// nomad_board.h: everything that differs between the boards.
//
// Mk4  : ESP32-S3-LCD-1.47 (Waveshare). SDMMC slot 1, ST7789 soldered on, one USB OTG FS.
// P4   : ESP32-P4-WIFI6    (Waveshare). SDMMC slot 0 for the card, slot 1 owned by the
//                                        ESP32-C6 that carries the radio, USB OTG HS on a
//                                        header whose data lines are DNP, no screen.
// NANO : ESP32-P4-NANO     (Waveshare). Same P4 silicon, same card wiring and the same
//                                        C6 radio, plus wired Ethernet, a real USB-A
//                                        socket, and a switch in the card's supply.
//                                        Half the flash though, 16 MB instead of 32 MB.
//
// Nothing else in the firmware may hard-code a pin number. If a define is missing for a
// target the build breaks here instead of silently picking the other board's pins.

#ifndef NOMAD_BOARD_H
#define NOMAD_BOARD_H

// sdkconfig.h defines CONFIG_IDF_TARGET_*. Include it here rather than relying on
// Arduino.h having been pulled in first: several .cpp files include this header first.
#include "sdkconfig.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4)

  // Both P4 boards are the same chip and cannot be told apart at compile time by any
  // predefined macro, so build-p4.sh --nano passes this in. Everything the two share
  // stays written once, below.
  #if defined(NOMAD_BOARD_P4_NANO) && NOMAD_BOARD_P4_NANO
    #define NOMAD_BOARD_NAME "ESP32-P4-NANO"
  #else
    #define NOMAD_BOARD_NAME "ESP32-P4-WIFI6"
  #endif

  // SD card
  // Slot 0 is the UHS-I slot and its pins are IOMUX, so they are fixed in silicon and
  // the slot config must pass GPIO_NUM_NC for every line. Slot 1 is not available. The
  // Arduino core's ESP-Hosted build claims it for the C6 (CLK 18, CMD 19, D0-D3 14-17,
  // reset 54), and putting the card there kills the radio.
  // Verified identical on both boards, the NANO schematic wires SD1 to exactly these.
  #define NOMAD_SD_SLOT        0
  #define NOMAD_SD_IOMUX       1
  #define NOMAD_SD_CLK        43
  #define NOMAD_SD_CMD        44
  #define NOMAD_SD_D0         39
  #define NOMAD_SD_D1         40
  #define NOMAD_SD_D2         41
  #define NOMAD_SD_D3         42
  // P4 does not power the SDMMC IO rail itself (SOC_SDMMC_IO_POWER_EXTERNAL). On the
  // Espressif EV board and the Waveshare boards that copy it, VDDPST_5 hangs off on-chip
  // LDO channel 4. Set to -1 if a board turns out to feed that rail externally.
  #define NOMAD_SD_LDO_CHANNEL 4

  #if defined(NOMAD_BOARD_P4_NANO) && NOMAD_BOARD_P4_NANO
    // The NANO puts an AO3401 P-channel FET in the card's 3V3 feed, gate on GPIO45 with a
    // 10K pulldown, so the card is powered with the pin left alone and driving the pin
    // HIGH cuts it. It's the only way to power-cycle a wedged card without power-cycling
    // the board.
    #define NOMAD_SD_PWR_PIN        45
    #define NOMAD_SD_PWR_ON_LEVEL   0
  #else
    #define NOMAD_SD_PWR_PIN        (-1)
    #define NOMAD_SD_PWR_ON_LEVEL   0
  #endif

  // Ethernet
  // NANO only. IP101GRI on RMII, behind an RJ45 with PoE magnetics. The RMII data pins
  // are IOMUX-fixed on the P4 and so are the same on any P4 board. Only MDC/MDIO, the
  // PHY reset and the clock arrangement are board choices, and the NANO's match the
  // Arduino core's own esp32p4 variant exactly. A 25 MHz crystal clocks the PHY, which
  // drives 50 MHz back into GPIO50, so CLK_EXT_IN and not an output mode.
  // ETH_PHY_IP101 is an alias of ETH_PHY_TLK110 in the core (ETH.h), same driver.
  #if defined(NOMAD_BOARD_P4_NANO) && NOMAD_BOARD_P4_NANO
    #define NOMAD_HAS_ETHERNET   1
    #define NOMAD_ETH_PHY_ADDR   1
    #define NOMAD_ETH_MDC       31
    #define NOMAD_ETH_MDIO      52
    #define NOMAD_ETH_POWER     51   // PHY nRESET
  #else
    #define NOMAD_HAS_ETHERNET   0
    #define NOMAD_ETH_PHY_ADDR   (-1)
    #define NOMAD_ETH_MDC       (-1)
    #define NOMAD_ETH_MDIO      (-1)
    #define NOMAD_ETH_POWER     (-1)
  #endif

  // Display
  // Nothing is fitted on either board. LVGL still runs, into a sink, so every lv_*/ui_*
  // call site in the firmware keeps working unchanged and the boot messages go to serial
  // and the web log. Wire an ST7789 to the header, set these to match and flip the flag.
  #define NOMAD_HAS_DISPLAY    0
  #define NOMAD_LCD_MOSI      33
  #define NOMAD_LCD_SCLK      32
  #if defined(NOMAD_BOARD_P4_NANO) && NOMAD_BOARD_P4_NANO
    // 34 is RMII TXD0 on this board. Anything but a free pin here would take the wired
    // NIC down the moment a screen was fitted.
    #define NOMAD_LCD_CS      26
  #else
    #define NOMAD_LCD_CS      34
  #endif
  #define NOMAD_LCD_DC        22
  #define NOMAD_LCD_RST       23
  #define NOMAD_LCD_BL        20

  // Buttons / LED
  // GPIO 0 is a plain IO on P4, the strapping pins are 35/36. Neither board gives us a
  // button we can poll. The WIFI6 has none wired, and the NANO's BOOT key is on GPIO35,
  // which is also RMII TXD1 (a strap read at reset, then owned by the EMAC). So the
  // USB-mode exit is a web action on both.
  #define NOMAD_BOOT_BUTTON_PIN (-1)
  #define NOMAD_HAS_RGB_LED     0
  #define NOMAD_RGB_LED_PIN     (-1)

  // Storage / features
  // usbHost: the WIFI6 exposes OTG HS on a 4-pin header whose data lines are DNP, so it
  // has never been usable without rework. The NANO brings the same controller out to a
  // real USB-A socket, powered through a load switch that is hardwired on, so there is
  // no way to power-cycle an attached drive in software.
  #define NOMAD_HAS_USB_HOST    1
  #define NOMAD_HAS_USB_MSC_DEV 0   // the Type-C is a CH34x UART bridge, not the SoC's USB
  #define NOMAD_HAS_FLASH_UI    1   // LittleFS partition holds the web UI
  // Which partition that is, by label. The NANO has half the flash, and no stock 16 MB
  // layout has a big "spiffs" partition without also dragging in a speech model the upload
  // step insists on flashing. So the NANO uses the 9.9 MB "ffat" partition. LittleFS works
  // there because esp_littlefs finds a labelled partition with SUBTYPE_ANY and never checks
  // that the subtype says fat.
  #if defined(NOMAD_BOARD_P4_NANO) && NOMAD_BOARD_P4_NANO
    #define NOMAD_UI_PART_LABEL "ffat"
  #else
    #define NOMAD_UI_PART_LABEL "spiffs"
  #endif
  #define NOMAD_HAS_THUMBNAILER 1   // hardware JPEG codec + PPA scaler: cover thumbnails
  #define NOMAD_HAS_HOSTED_RADIO 1  // radio is a separate ESP32-C6 over SDIO (ESP-Hosted)
  #define NOMAD_HAS_NATIVE_BT   0   // BT lives on the C6, btStop() does not exist here

#elif defined(CONFIG_IDF_TARGET_ESP32S3)

  #define NOMAD_BOARD_NAME "ESP32-S3-LCD-1.47"

  #define NOMAD_SD_SLOT        1
  #define NOMAD_SD_IOMUX       0
  #define NOMAD_SD_CLK        14
  #define NOMAD_SD_CMD        15
  #define NOMAD_SD_D0         16
  #define NOMAD_SD_D1         18
  #define NOMAD_SD_D2         17
  #define NOMAD_SD_D3         21
  #define NOMAD_SD_LDO_CHANNEL (-1)
  #define NOMAD_SD_PWR_PIN     (-1)
  #define NOMAD_SD_PWR_ON_LEVEL 0

  #define NOMAD_HAS_ETHERNET   0
  #define NOMAD_ETH_PHY_ADDR   (-1)
  #define NOMAD_ETH_MDC       (-1)
  #define NOMAD_ETH_MDIO      (-1)
  #define NOMAD_ETH_POWER     (-1)

  #define NOMAD_HAS_DISPLAY    1
  #define NOMAD_LCD_MOSI      45
  #define NOMAD_LCD_SCLK      40
  #define NOMAD_LCD_CS        42
  #define NOMAD_LCD_DC        41
  #define NOMAD_LCD_RST       39
  // Backlight. The ESP32-S3-LCD-1.47 (USB-A) drives it from GPIO 48; the 1.47B (USB-C)
  // moves it to GPIO 46. That is the only firmware-visible difference between the two
  // boards. Build the USB-C image with -DBOARD_USB_C=1 (tools/flash-nomad.sh --usbc).
  // The switch has to live in a header every user of the pin includes, not in the .ino:
  // Display_ST7789.cpp is a separate translation unit, so a #define in the sketch never
  // reaches it and the board silently keeps GPIO 48 (dark screen, no error).
  #ifndef BOARD_USB_C
  #define BOARD_USB_C 0
  #endif
  #if BOARD_USB_C
  #define NOMAD_LCD_BL        46
  #else
  #define NOMAD_LCD_BL        48
  #endif

  #define NOMAD_BOOT_BUTTON_PIN 0
  #define NOMAD_HAS_RGB_LED     1
  #define NOMAD_RGB_LED_PIN     PIN_NEOPIXEL

  #define NOMAD_HAS_USB_HOST    0
  #define NOMAD_HAS_USB_MSC_DEV 1
  #define NOMAD_HAS_FLASH_UI    0
  #define NOMAD_UI_PART_LABEL   "spiffs"   // unused: no flash UI partition on the Mk4
  #define NOMAD_HAS_THUMBNAILER 0   // no JPEG codec and no PPA on the S3
  #define NOMAD_HAS_HOSTED_RADIO 0  // WiFi and BLE are on-die
  #define NOMAD_HAS_NATIVE_BT   1

#else
  #error "Jcorp Nomad: unsupported target. Build for esp32s3 (Mk4) or esp32p4 (P4/NANO)."
#endif

#endif  // NOMAD_BOARD_H
