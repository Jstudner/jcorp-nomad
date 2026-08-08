/*
 * NomadHardwareTest - bring-up / pin verification for the Jcorp Nomad port.
 *
 * Flash this BEFORE the full firmware when you put Nomad on a new board (or on
 * a dongle clone whose wiring you are not sure about). It is a single file with
 * no library dependencies beyond the ESP32 core, and it reuses the exact same
 * board_config.h the firmware uses - so whatever passes here will work there.
 *
 * What it checks, in order:
 *   1. Chip / flash / PSRAM report            (is OPI PSRAM actually enabled?)
 *   2. Backlight ramp                         (is LCD_BL_ACTIVE_LEVEL right?)
 *   3. Colour + geometry test pattern         (controller, offsets, MADCTL)
 *   4. RGB status LED sweep                   (LED type and channel order)
 *   5. microSD mount, with a fallback sweep   (SD pin map)
 *   6. Live boot-button readout
 *
 * Open the serial monitor at 115200 and follow along - every step prints what
 * you should be seeing and what to change in board_config.h if you are not.
 *
 * Arduino IDE board settings: see docs/PocketDongleS3.md
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD_MMC.h>
#include <SD.h>

#include "../JcorpNomadProject/board_config.h"

// ============================================================== LCD =========
static SPIClass lcdSpi(FSPI);

static void lcdCmd(uint8_t c) {
  lcdSpi.beginTransaction(SPISettings(LCD_SPI_FREQ, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_PIN_CS, LOW);
  digitalWrite(LCD_PIN_DC, LOW);
  lcdSpi.transfer(c);
  digitalWrite(LCD_PIN_CS, HIGH);
  lcdSpi.endTransaction();
}

static void lcdData(uint8_t d) {
  lcdSpi.beginTransaction(SPISettings(LCD_SPI_FREQ, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_PIN_CS, LOW);
  digitalWrite(LCD_PIN_DC, HIGH);
  lcdSpi.transfer(d);
  digitalWrite(LCD_PIN_CS, HIGH);
  lcdSpi.endTransaction();
}

static void lcdCmdData(uint8_t cmd, const uint8_t *d, uint8_t n) {
  lcdCmd(cmd);
  for (uint8_t i = 0; i < n; ++i) lcdData(d[i]);
}

static void lcdWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  x0 += LCD_OFFSET_X; x1 += LCD_OFFSET_X;
  y0 += LCD_OFFSET_Y; y1 += LCD_OFFSET_Y;
  lcdCmd(0x2A);
  lcdData(x0 >> 8); lcdData(x0 & 0xFF); lcdData(x1 >> 8); lcdData(x1 & 0xFF);
  lcdCmd(0x2B);
  lcdData(y0 >> 8); lcdData(y0 & 0xFF); lcdData(y1 >> 8); lcdData(y1 & 0xFF);
  lcdCmd(0x2C);
}

static void lcdBlit(const uint16_t *px, uint32_t count) {
  lcdSpi.beginTransaction(SPISettings(LCD_SPI_FREQ, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_PIN_CS, LOW);
  digitalWrite(LCD_PIN_DC, HIGH);
  lcdSpi.transferBytes((uint8_t *)px, NULL, count * 2);
  digitalWrite(LCD_PIN_CS, HIGH);
  lcdSpi.endTransaction();
}

static uint16_t rowbuf[LCD_WIDTH];

static void lcdFillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t colour) {
  if (w == 0 || h == 0) return;
  for (uint16_t i = 0; i < w && i < LCD_WIDTH; ++i) rowbuf[i] = colour;
  for (uint16_t r = 0; r < h; ++r) {
    lcdWindow(x, y + r, x + w - 1, y + r);
    lcdBlit(rowbuf, w);
  }
}

static void lcdFill(uint16_t colour) {
  lcdFillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, colour);
}

static void backlight(uint8_t percent) {
#if LCD_PIN_BL >= 0
  uint32_t duty = ((uint32_t)percent * LCD_BL_PWM_MAX) / 100u;
#if !LCD_BL_ACTIVE_LEVEL
  duty = LCD_BL_PWM_MAX - duty;
#endif
  ledcWrite(LCD_PIN_BL, duty);
#else
  (void)percent;   // no backlight GPIO on this board; panel is always lit
#endif
}

static void lcdInit() {
  pinMode(LCD_PIN_CS, OUTPUT);
  pinMode(LCD_PIN_DC, OUTPUT);
  if (LCD_PIN_RST >= 0) pinMode(LCD_PIN_RST, OUTPUT);
  digitalWrite(LCD_PIN_CS, HIGH);

#if LCD_PIN_BL >= 0
  ledcAttach(LCD_PIN_BL, LCD_BL_PWM_FREQ_HZ, LCD_BL_PWM_BITS);
#endif
  backlight(0);

  lcdSpi.begin(LCD_PIN_SCLK, LCD_PIN_MISO, LCD_PIN_MOSI);

  digitalWrite(LCD_PIN_CS, LOW);
  delay(20);
  if (LCD_PIN_RST >= 0) {
    digitalWrite(LCD_PIN_RST, LOW);  delay(50);
    digitalWrite(LCD_PIN_RST, HIGH); delay(120);
  }
  digitalWrite(LCD_PIN_CS, HIGH);

#if NOMAD_LCD_CONTROLLER == NOMAD_LCD_ST7735
  static const uint8_t frmctr1[] = {0x01, 0x2C, 0x2D};
  static const uint8_t frmctr3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
  static const uint8_t pwctr1[]  = {0xA2, 0x02, 0x84};
  static const uint8_t pwctr3[]  = {0x0A, 0x00};
  static const uint8_t pwctr4[]  = {0x8A, 0x2A};
  static const uint8_t pwctr5[]  = {0x8A, 0xEE};

  lcdCmd(0x01); delay(150);
  lcdCmd(0x11); delay(150);
  lcdCmdData(0xB1, frmctr1, sizeof(frmctr1));
  lcdCmdData(0xB2, frmctr1, sizeof(frmctr1));
  lcdCmdData(0xB3, frmctr3, sizeof(frmctr3));
  lcdCmd(0xB4); lcdData(0x07);
  lcdCmdData(0xC0, pwctr1, sizeof(pwctr1));
  lcdCmd(0xC1); lcdData(0xC5);
  lcdCmdData(0xC2, pwctr3, sizeof(pwctr3));
  lcdCmdData(0xC3, pwctr4, sizeof(pwctr4));
  lcdCmdData(0xC4, pwctr5, sizeof(pwctr5));
  lcdCmd(0xC5); lcdData(0x0E);
#if LCD_INVERT_COLORS
  lcdCmd(0x21);
#else
  lcdCmd(0x20);
#endif
  lcdCmd(0x36); lcdData(LCD_MADCTL);
  lcdCmd(0x3A); lcdData(0x05);
  lcdCmd(0x13); delay(10);
  lcdCmd(0x29); delay(100);
#else  // ST7789
  lcdCmd(0x11); delay(120);
  lcdCmd(0x36); lcdData(LCD_MADCTL);
  lcdCmd(0x3A); lcdData(0x05);
  lcdCmd(0xB0); lcdData(0x00); lcdData(0xE8);
#if LCD_INVERT_COLORS
  lcdCmd(0x21);
#else
  lcdCmd(0x20);
#endif
  lcdCmd(0x11); delay(120);
  lcdCmd(0x29);
#endif
}

// ============================================================== LED =========
#if NOMAD_LED_TYPE == NOMAD_LED_APA102
static void apaByte(uint8_t v) {
  for (int8_t b = 7; b >= 0; --b) {
    digitalWrite(LED_PIN_DATA, (v >> b) & 1);
    digitalWrite(LED_PIN_CLOCK, HIGH);
    digitalWrite(LED_PIN_CLOCK, LOW);
  }
}
static void ledInit() {
  pinMode(LED_PIN_DATA, OUTPUT);
  pinMode(LED_PIN_CLOCK, OUTPUT);
  digitalWrite(LED_PIN_DATA, LOW);
  digitalWrite(LED_PIN_CLOCK, LOW);
}
static void ledSet(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < 4; ++i) apaByte(0x00);
  apaByte(0xE0 | (LED_APA102_LEVEL & 0x1F));
  apaByte(b); apaByte(g); apaByte(r);
  for (int i = 0; i < 4; ++i) apaByte(0xFF);
}
#elif NOMAD_LED_TYPE == NOMAD_LED_WS2812
static void ledInit() {}
static void ledSet(uint8_t r, uint8_t g, uint8_t b) { neopixelWrite(LED_PIN_DATA, r, g, b); }
#else
static void ledInit() {}
static void ledSet(uint8_t, uint8_t, uint8_t) {}
#endif

// =============================================================== SD =========
#if NOMAD_SD_BUS == NOMAD_SD_BUS_SPI

// This board wires the card as a plain 4-wire SPI device on its own host.
static SPIClass sdSpi(HSPI);

struct SdSpiPinSet {
  const char *name;
  int sclk, mosi, miso, cs;
};

static const SdSpiPinSet kSdSpiCandidates[] = {
  {"board_config.h",        SD_SCLK_PIN, SD_MOSI_PIN, SD_MISO_PIN, SD_CS_PIN},
  {"GNPE Pocket-Dongle-S3", 17, 18, 16, 47},
};

static bool trySdSpi(const SdSpiPinSet &p, uint32_t freq) {
  SD.end();
  sdSpi.end();
  delay(20);
  sdSpi.begin(p.sclk, p.miso, p.mosi, p.cs);
  if (!SD.begin(p.cs, sdSpi, freq, "/sdcard", 5, false /* never format */)) return false;
  if (SD.cardType() == CARD_NONE) return false;
  return true;
}

static void sdSweep() {
  Serial.println("\n--- 5. microSD (SPI) ---");

  static const uint32_t freqs[] = {SD_SPI_FREQ, 10000000, 4000000, 1000000};

  for (size_t i = 0; i < sizeof(kSdSpiCandidates) / sizeof(kSdSpiCandidates[0]); ++i) {
    const SdSpiPinSet &p = kSdSpiCandidates[i];
    Serial.printf("  trying %-24s SCLK %d MOSI %d MISO %d CS %d ... ",
                  p.name, p.sclk, p.mosi, p.miso, p.cs);

    for (size_t f = 0; f < sizeof(freqs) / sizeof(freqs[0]); ++f) {
      if (!trySdSpi(p, freqs[f])) continue;

      Serial.printf("MOUNTED @ %lu MHz\n", (unsigned long)(freqs[f] / 1000000UL));
      Serial.printf("    card size : %.2f GB\n",
                    (double)SD.cardSize() / (1024.0 * 1024.0 * 1024.0));
      Serial.printf("    used      : %.2f GB of %.2f GB\n",
                    (double)SD.usedBytes() / (1024.0 * 1024.0 * 1024.0),
                    (double)SD.totalBytes() / (1024.0 * 1024.0 * 1024.0));

      Serial.println("    root listing:");
      File root = SD.open("/");
      if (root) {
        int n = 0;
        for (File e = root.openNextFile(); e && n < 15; e = root.openNextFile(), ++n) {
          Serial.printf("      %-30s %s\n", e.name(), e.isDirectory() ? "<dir>" : "");
          e.close();
        }
        root.close();
      }
      if (i != 0) {
        Serial.println("    *** These pins are NOT what board_config.h says.");
        Serial.println("    *** Copy them into the SD section of board_config.h.");
      }
      return;
    }
    Serial.println("no");
  }

  Serial.println("  No SPI pin set worked. Is a FAT32 card actually inserted?");
}

#else  // NOMAD_SD_BUS_SDMMC

struct SdPinSet {
  const char *name;
  int clk, cmd, d0, d1, d2, d3;
};

// board_config.h first, then the other layouts we know about. If one of the
// fallbacks mounts, copy those numbers into board_config.h.
static const SdPinSet kSdCandidates[] = {
  {"board_config.h",              SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN},
  {"LilyGO T-Dongle-S3",          12, 16, 14, 17, 21, 18},
  {"Waveshare ESP32-S3-LCD-1.47", 14, 15, 16, 18, 17, 21},
  {"ESP32-S3 devkit common",      36, 35, 37, 38, 33, 34},
};

static bool trySd(const SdPinSet &p, bool oneBit, int freq) {
  SD_MMC.end();
  delay(20);
  if (!SD_MMC.setPins(p.clk, p.cmd, p.d0, p.d1, p.d2, p.d3)) return false;
  if (!SD_MMC.begin("/sdcard", oneBit, false /* never format */, freq, 5)) return false;
  if (SD_MMC.cardType() == CARD_NONE) return false;
  return true;
}

// Some boards wire the card as plain SPI instead. Probe that as a fallback so a
// total SDMMC failure can be told apart from a wrong pin map.
static bool trySdSpiFallback(int sck, int mosi, int miso, int cs) {
  static SPIClass sdSpi(HSPI);
  SD.end();
  sdSpi.end();
  delay(20);
  sdSpi.begin(sck, miso, mosi, cs);
  if (!SD.begin(cs, sdSpi, 20000000, "/sdcard", 5, false)) return false;
  if (SD.cardType() == CARD_NONE) return false;
  Serial.printf("    card size : %.2f GB\n",
                (double)SD.cardSize() / (1024.0 * 1024.0 * 1024.0));
  return true;
}

static void sdSweep() {
  Serial.println("\n--- 5. microSD (SDMMC) ---");

  for (size_t i = 0; i < sizeof(kSdCandidates) / sizeof(kSdCandidates[0]); ++i) {
    const SdPinSet &p = kSdCandidates[i];
    Serial.printf("  trying %-28s CLK %d CMD %d D0 %d D1 %d D2 %d D3 %d ... ",
                  p.name, p.clk, p.cmd, p.d0, p.d1, p.d2, p.d3);

    bool ok = trySd(p, false, SDMMC_FREQ_HIGHSPEED);
    const char *how = "4-bit 40 MHz";
    if (!ok) { ok = trySd(p, false, SDMMC_FREQ_DEFAULT); how = "4-bit 20 MHz"; }
    if (!ok) { ok = trySd(p, true,  SDMMC_FREQ_DEFAULT); how = "1-bit 20 MHz"; }

    if (!ok) {
      Serial.println("no");
      continue;
    }

    Serial.printf("MOUNTED (%s)\n", how);
    Serial.printf("    card size : %.2f GB\n",
                  (double)SD_MMC.cardSize() / (1024.0 * 1024.0 * 1024.0));
    Serial.printf("    used      : %.2f GB of %.2f GB\n",
                  (double)SD_MMC.usedBytes() / (1024.0 * 1024.0 * 1024.0),
                  (double)SD_MMC.totalBytes() / (1024.0 * 1024.0 * 1024.0));

    Serial.println("    root listing:");
    File root = SD_MMC.open("/");
    if (root) {
      int n = 0;
      for (File e = root.openNextFile(); e && n < 15; e = root.openNextFile(), ++n) {
        Serial.printf("      %-30s %s\n", e.name(), e.isDirectory() ? "<dir>" : "");
        e.close();
      }
      root.close();
    }

    if (i != 0) {
      Serial.println("    *** These pins are NOT what board_config.h says.");
      Serial.println("    *** Copy them into the SD section of board_config.h.");
    }
    return;
  }

  Serial.println("  No SDMMC pin set worked. Trying SPI mode on the configured pins...");
  if (trySdSpiFallback(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D3_PIN)) {
    Serial.println("    MOUNTED over SPI.");
    Serial.println("    *** This board wires the card as SPI, not SDMMC.");
    Serial.println("    *** Set NOMAD_SD_BUS to NOMAD_SD_BUS_SPI in board_config.h.");
  } else {
    Serial.println("    SPI mode failed too. Is a FAT32 card actually inserted,");
    Serial.println("    and are the pins in board_config.h right?");
  }
}

#endif

// ============================================================= sketch =======
void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println("\n\n########  Jcorp Nomad hardware self-test  ########");
  Serial.printf("Board profile: %s\n", NOMAD_BOARD_NAME);

  // --- 1. chip report ----------------------------------------------------
  Serial.println("\n--- 1. Chip ---");
  Serial.printf("  %s rev %d, %d core(s) @ %lu MHz\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getChipCores(), (unsigned long)getCpuFrequencyMhz());
  Serial.printf("  flash : %lu MB\n", (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)));
  if (ESP.getPsramSize()) {
    Serial.printf("  PSRAM : %lu KB  OK\n", (unsigned long)(ESP.getPsramSize() / 1024));
  } else {
    Serial.println("  PSRAM : NOT DETECTED");
    Serial.println("          On an N16R8 this means the board menu is wrong -");
    Serial.println("          set PSRAM to \"OPI PSRAM\" and Flash Size to 16MB.");
  }
  Serial.printf("  heap  : %lu KB free\n", (unsigned long)(ESP.getFreeHeap() / 1024));

  // --- 2/3. display ------------------------------------------------------
  Serial.println("\n--- 2. Backlight ---");
#if LCD_PIN_BL >= 0
  Serial.printf("  pin %d, active %s. Ramping 0%% -> 100%%.\n",
                LCD_PIN_BL, LCD_BL_ACTIVE_LEVEL ? "HIGH" : "LOW");
  Serial.println("  If the panel goes DARK as this ramps up, flip");
  Serial.println("  LCD_BL_ACTIVE_LEVEL in board_config.h.");
#else
  Serial.println("  No backlight GPIO configured; the panel should just be lit.");
  Serial.println("  If it is dark, find the backlight pin in the schematic and");
  Serial.println("  set LCD_PIN_BL in board_config.h.");
#endif

  lcdInit();
  lcdFill(0x0000);
  for (int p = 0; p <= 100; p += 5) { backlight(p); delay(40); }

  Serial.println("\n--- 3. Display ---");
  Serial.printf("  %dx%d, offsets X=%d Y=%d, MADCTL 0x%02X\n",
                LCD_WIDTH, LCD_HEIGHT, LCD_OFFSET_X, LCD_OFFSET_Y, LCD_MADCTL);
  Serial.println("  Expect, one second apart: RED, GREEN, BLUE, WHITE, then a");
  Serial.println("  white 1px frame around the very edge with R/G/B bars inside.");
  Serial.println("   - wrong colour order  -> flip the BGR bit (0x08) in LCD_MADCTL");
  Serial.println("   - frame clipped/offset-> adjust LCD_OFFSET_X / LCD_OFFSET_Y");
  Serial.println("   - image mirrored      -> flip the MX/MY bits (0x40/0x80)");
  Serial.println("   - washed out/negative -> toggle LCD_INVERT_COLORS");

  const uint16_t red = 0xF800, green = 0x07E0, blue = 0x001F, white = 0xFFFF;
  lcdFill(red);   delay(1000);
  lcdFill(green); delay(1000);
  lcdFill(blue);  delay(1000);
  lcdFill(white); delay(1000);

  lcdFill(0x0000);
  lcdFillRect(0, 0, LCD_WIDTH, 1, white);
  lcdFillRect(0, LCD_HEIGHT - 1, LCD_WIDTH, 1, white);
  lcdFillRect(0, 0, 1, LCD_HEIGHT, white);
  lcdFillRect(LCD_WIDTH - 1, 0, 1, LCD_HEIGHT, white);
  uint16_t barH = (LCD_HEIGHT - 8) / 3;
  lcdFillRect(4, 4, LCD_WIDTH - 8, barH, red);
  lcdFillRect(4, 4 + barH, LCD_WIDTH - 8, barH, green);
  lcdFillRect(4, 4 + 2 * barH, LCD_WIDTH - 8, barH, blue);

  // --- 4. LED ------------------------------------------------------------
  Serial.println("\n--- 4. RGB status LED ---");
#if NOMAD_LED_TYPE == NOMAD_LED_APA102
  Serial.printf("  APA102 on data %d / clock %d\n", LED_PIN_DATA, LED_PIN_CLOCK);
#elif NOMAD_LED_TYPE == NOMAD_LED_WS2812
  Serial.printf("  WS2812 on %d\n", LED_PIN_DATA);
#else
  Serial.println("  none configured");
#endif
  Serial.println("  Expect RED, GREEN, BLUE in that order. If the order is");
  Serial.println("  wrong, swap the channel bytes in RGB_lamp.cpp's led_write().");
  ledInit();
  ledSet(80, 0, 0); delay(800);
  ledSet(0, 80, 0); delay(800);
  ledSet(0, 0, 80); delay(800);
  ledSet(0, 0, 0);

  // --- 5. SD -------------------------------------------------------------
  sdSweep();

  // --- 6. button ---------------------------------------------------------
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  Serial.printf("\n--- 6. Boot button (GPIO %d) ---\n", BOOT_BUTTON_PIN);
  Serial.println("  Press it - each press should print a line below.");
  Serial.println("\nSelf-test finished. Anything above that did not match means");
  Serial.println("board_config.h needs an edit before flashing the firmware.\n");
}

void loop() {
  static bool wasDown = false;
  static uint32_t downAt = 0;

  bool down = (digitalRead(BOOT_BUTTON_PIN) == LOW);
  if (down != wasDown) {
    wasDown = down;
    if (down) {
      downAt = millis();
      Serial.println("[button] pressed");
      ledSet(40, 40, 0);
    } else {
      Serial.printf("[button] released after %lu ms\n", (unsigned long)(millis() - downAt));
      ledSet(0, 0, 0);
    }
    delay(40);  // debounce
  }
  delay(10);
}
