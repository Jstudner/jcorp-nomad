// board_config.h - Jcorp Nomad hardware profiles
//
// Everything that differs between supported boards lives here. Pick a board by
// changing NOMAD_BOARD below (or by passing -DNOMAD_BOARD=... from the build),
// and nothing else in the firmware needs to be touched.
//
// Supported boards
//   NOMAD_BOARD_POCKET_DONGLE_S3   Pocket-Dongle-S3-0.96 / LilyGO T-Dongle-S3 class
//                                  USB-A stick, ESP32-S3 N16R8, 0.96" ST7735 160x80,
//                                  microSD in the USB shell, APA102 RGB LED.
//   NOMAD_BOARD_WAVESHARE_LCD147   Waveshare ESP32-S3-LCD-1.47 (the original Nomad
//                                  target), 1.47" ST7789 172x320, WS2812 RGB LED.
//
// If your dongle turns out to be wired differently from the profile below, the
// only file you need to edit is this one. Flash firmware/NomadHardwareTest to
// probe the real wiring - it brute-forces the SD pinout and sweeps the LCD.

#ifndef NOMAD_BOARD_CONFIG_H
#define NOMAD_BOARD_CONFIG_H

// ---------------------------------------------------------------- board ids
#define NOMAD_BOARD_WAVESHARE_LCD147 1
#define NOMAD_BOARD_POCKET_DONGLE_S3 2

#ifndef NOMAD_BOARD
#define NOMAD_BOARD NOMAD_BOARD_POCKET_DONGLE_S3
#endif

// ------------------------------------------------------- capability tokens
#define NOMAD_LCD_ST7789 1
#define NOMAD_LCD_ST7735 2

#define NOMAD_LED_NONE   0
#define NOMAD_LED_WS2812 1
#define NOMAD_LED_APA102 2

#define NOMAD_UI_PORTRAIT_TALL  1  // 172x320, the original SquareLine layout
#define NOMAD_UI_LANDSCAPE_MINI 2  // 160x80, the dongle layout

// =========================================================================
#if NOMAD_BOARD == NOMAD_BOARD_POCKET_DONGLE_S3
// =========================================================================
#define NOMAD_BOARD_NAME "Pocket-Dongle-S3 0.96"

// ---- display: 0.96" IPS, ST7735(S) controller, 160x80 landscape ----------
#define NOMAD_LCD_CONTROLLER NOMAD_LCD_ST7735
#define LCD_WIDTH  160
#define LCD_HEIGHT 80

// The panel is a 160x80 window inside the controller's 132x162 GRAM. In
// landscape (MADCTL MV|MY) the column window starts at 1 and the row window at
// 26. Verified against both LilyGO's esp_lcd config (set_gap(1, 26)) and
// Adafruit_ST7735's INITR_MINI160x80 rotation 1.
#define LCD_OFFSET_X 1
#define LCD_OFFSET_Y 26
#define LCD_MADCTL   0xA8  // MY | MV | BGR
#define LCD_INVERT_COLORS 1

#define LCD_PIN_MISO -1
#define LCD_PIN_MOSI 3
#define LCD_PIN_SCLK 5
#define LCD_PIN_CS   4
#define LCD_PIN_DC   2
#define LCD_PIN_RST  1
#define LCD_PIN_BL   38

// ST7735S is only specced to ~15 MHz but happily runs faster on these short
// traces. 27 MHz redraws the whole 160x80 frame in ~10 ms with plenty of
// margin. Raise to 40000000 if you want, drop to 20000000 if you see speckle.
#define LCD_SPI_FREQ 27000000

// Backlight is driven through a PNP/low-side inverter on this board family:
// pulling the pin LOW lights it up. Flip to 1 if your screen is brightest at
// 0% and dark at 100%.
#define LCD_BL_ACTIVE_LEVEL 0

// ---- microSD (SDMMC, 4-bit) ---------------------------------------------
#define SD_CLK_PIN 12
#define SD_CMD_PIN 16
#define SD_D0_PIN  14
#define SD_D1_PIN  17
#define SD_D2_PIN  21
#define SD_D3_PIN  18

// ---- RGB LED: APA102 (2-wire, clock + data) ------------------------------
#define NOMAD_LED_TYPE   NOMAD_LED_APA102
#define LED_PIN_DATA     40
#define LED_PIN_CLOCK    39
#define LED_APA102_LEVEL 12  // APA102 global-current field, 0..31

// ---- controls ------------------------------------------------------------
#define BOOT_BUTTON_PIN 0

// ---- UI ------------------------------------------------------------------
#define NOMAD_UI_LAYOUT   NOMAD_UI_LANDSCAPE_MINI
#define LVGL_BUF_DIVISOR  4
#define LVGL_FULL_REFRESH 0

// =========================================================================
#elif NOMAD_BOARD == NOMAD_BOARD_WAVESHARE_LCD147
// =========================================================================
#define NOMAD_BOARD_NAME "Waveshare ESP32-S3-LCD-1.47"

#define NOMAD_LCD_CONTROLLER NOMAD_LCD_ST7789
#define LCD_WIDTH  172
#define LCD_HEIGHT 320

#define LCD_OFFSET_X 34
#define LCD_OFFSET_Y 0
#define LCD_MADCTL   0x00
#define LCD_INVERT_COLORS 1

#define LCD_PIN_MISO -1
#define LCD_PIN_MOSI 45
#define LCD_PIN_SCLK 40
#define LCD_PIN_CS   42
#define LCD_PIN_DC   41
#define LCD_PIN_RST  39
#define LCD_PIN_BL   48

#define LCD_SPI_FREQ 80000000
#define LCD_BL_ACTIVE_LEVEL 1

#define SD_CLK_PIN 14
#define SD_CMD_PIN 15
#define SD_D0_PIN  16
#define SD_D1_PIN  18
#define SD_D2_PIN  17
#define SD_D3_PIN  21

#define NOMAD_LED_TYPE NOMAD_LED_WS2812
#define LED_PIN_DATA   38
#define LED_PIN_CLOCK  -1

#define BOOT_BUTTON_PIN 0

#define NOMAD_UI_LAYOUT   NOMAD_UI_PORTRAIT_TALL
#define LVGL_BUF_DIVISOR  20
#define LVGL_FULL_REFRESH 0   // upstream moved to partial refresh; far less SPI traffic

#else
#error "Unknown NOMAD_BOARD - see board_config.h for the supported profiles"
#endif

// ------------------------------------------------- shared derived settings
// Backlight PWM. 10-bit resolution gives 0..1023 duty steps.
#define LCD_BL_PWM_FREQ_HZ 1000
#define LCD_BL_PWM_BITS    10
#define LCD_BL_PWM_MAX     ((1 << LCD_BL_PWM_BITS) - 1)

// Boot button: hold this long to fall into USB mass-storage mode. A shorter
// press just cycles the on-screen page.
#define NOMAD_BTN_LONGPRESS_MS 1200
#define NOMAD_BTN_DEBOUNCE_MS  40

#endif  // NOMAD_BOARD_CONFIG_H
