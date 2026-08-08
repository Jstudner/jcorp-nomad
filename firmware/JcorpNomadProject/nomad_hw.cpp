#include "nomad_hw.h"
#include <SD_MMC.h>
#include <esp_heap_caps.h>

// ============================================================ SD mounting ==
NomadSdMountResult NomadSD_Mount(uint8_t maxOpenFiles) {
  struct Attempt {
    bool     oneBit;
    uint32_t freq;
    const char *label;
  };

  // Fastest first. These clone dongles vary a lot in trace quality, so instead
  // of hard-failing at one fixed speed we walk down until the card answers.
  static const Attempt attempts[] = {
    {false, SDMMC_FREQ_HIGHSPEED, "4-bit @ 40 MHz"},
    {false, SDMMC_FREQ_DEFAULT,   "4-bit @ 20 MHz"},
    {true,  SDMMC_FREQ_DEFAULT,   "1-bit @ 20 MHz"},
    {true,  SDMMC_FREQ_PROBING,   "1-bit @ 400 kHz"},
  };

  NomadSdMountResult r = {false, false, 0, 0};

  for (size_t i = 0; i < sizeof(attempts) / sizeof(attempts[0]); ++i) {
    SD_MMC.end();
    delay(20);

    if (!SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN)) {
      Serial.println("[SD] setPins() rejected the board_config.h pin map");
      return r;
    }

    // format_if_mount_failed stays false on purpose: a bad mount must never
    // wipe somebody's media library.
    if (SD_MMC.begin("/sdcard", attempts[i].oneBit, false, (int)attempts[i].freq, maxOpenFiles)) {
      if (SD_MMC.cardType() == CARD_NONE) {
        Serial.printf("[SD] %s mounted but reports no card, trying slower\n", attempts[i].label);
        continue;
      }
      r.mounted = true;
      r.fourBit = !attempts[i].oneBit;
      r.freqHz = attempts[i].freq;
      r.cardSizeBytes = SD_MMC.cardSize();
      Serial.printf("[SD] Mounted %s (%.2f GB)\n", attempts[i].label,
                    (double)r.cardSizeBytes / (1024.0 * 1024.0 * 1024.0));
      return r;
    }

    Serial.printf("[SD] %s failed\n", attempts[i].label);
  }

  Serial.println("[SD] Could not mount the card at any speed.");
  Serial.println("[SD] Check the card is FAT32/exFAT and that the SD pins in "
                 "board_config.h match your board (see firmware/NomadHardwareTest).");
  return r;
}

// ================================================================= button ==
static bool     s_btnInit = false;
static bool     s_btnDown = false;
static bool     s_longFired = false;
static uint32_t s_btnChangedAt = 0;

void NomadButton_Init(void) {
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  s_btnInit = true;
  s_btnDown = false;
  s_longFired = false;
  s_btnChangedAt = millis();
}

NomadBtnEvent NomadButton_Poll(void) {
  if (!s_btnInit) NomadButton_Init();

  bool pressed = (digitalRead(BOOT_BUTTON_PIN) == LOW);
  uint32_t now = millis();

  if (pressed != s_btnDown) {
    if ((uint32_t)(now - s_btnChangedAt) < NOMAD_BTN_DEBOUNCE_MS) return NOMAD_BTN_NONE;

    s_btnChangedAt = now;
    s_btnDown = pressed;

    if (pressed) {
      s_longFired = false;
      return NOMAD_BTN_NONE;
    }

    // Released. A short tap only counts if the long-press already did not fire.
    if (!s_longFired) return NOMAD_BTN_SHORT;
    return NOMAD_BTN_NONE;
  }

  if (pressed && !s_longFired && (uint32_t)(now - s_btnChangedAt) >= NOMAD_BTN_LONGPRESS_MS) {
    s_longFired = true;
    return NOMAD_BTN_LONG;
  }

  return NOMAD_BTN_NONE;
}

// ================================================================= report ==
void NomadHW_PrintBoardInfo(Stream &out) {
  out.println();
  out.println("=========== Jcorp Nomad hardware ===========");
  out.printf("Board profile : %s\n", NOMAD_BOARD_NAME);
  out.printf("Chip          : %s rev %d, %d core(s) @ %lu MHz\n",
             ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
             (unsigned long)getCpuFrequencyMhz());
  out.printf("Flash         : %lu MB\n", (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)));
  if (ESP.getPsramSize() > 0) {
    out.printf("PSRAM         : %lu KB total, %lu KB free\n",
               (unsigned long)(ESP.getPsramSize() / 1024),
               (unsigned long)(ESP.getFreePsram() / 1024));
  } else {
    out.println("PSRAM         : not detected  <-- enable OPI PSRAM in the board menu");
  }
  out.printf("Free heap     : %lu KB\n", (unsigned long)(ESP.getFreeHeap() / 1024));
  out.printf("LCD           : %dx%d, MOSI %d SCLK %d CS %d DC %d RST %d BL %d @ %lu Hz\n",
             LCD_WIDTH, LCD_HEIGHT, LCD_PIN_MOSI, LCD_PIN_SCLK, LCD_PIN_CS,
             LCD_PIN_DC, LCD_PIN_RST, LCD_PIN_BL, (unsigned long)LCD_SPI_FREQ);
  out.printf("SD (SDMMC)    : CLK %d CMD %d D0 %d D1 %d D2 %d D3 %d\n",
             SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, SD_D1_PIN, SD_D2_PIN, SD_D3_PIN);
#if NOMAD_LED_TYPE == NOMAD_LED_APA102
  out.printf("RGB LED       : APA102, data %d clock %d\n", LED_PIN_DATA, LED_PIN_CLOCK);
#elif NOMAD_LED_TYPE == NOMAD_LED_WS2812
  out.printf("RGB LED       : WS2812, data %d\n", LED_PIN_DATA);
#else
  out.println("RGB LED       : none");
#endif
  out.printf("Boot button   : GPIO %d\n", BOOT_BUTTON_PIN);
  out.println("============================================");
  out.println();
}
