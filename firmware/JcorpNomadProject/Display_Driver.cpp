/*****************************************************************************
 * Display_Driver.cpp
 *
 * ST7789 (Waveshare 1.47", 172x320) and ST7735 (Pocket-Dongle 0.96", 160x80)
 * behind one interface. Board specifics come from board_config.h.
 *
 * Notes on the two behaviour fixes carried over from the original driver:
 *  - LCD_addWindow used to allocate a variable-length `Read_D[numBytes]` array
 *    on the caller's stack purely to satisfy transferBytes()'s read-back
 *    argument. On the 1.47" panel that was a >5 KB stack allocation on every
 *    flush. The read buffer is optional, so we pass NULL instead.
 *  - The window registers now compose a real 16-bit address instead of relying
 *    on the low byte wrapping, which only happened to work when the offset was
 *    zero.
 *****************************************************************************/
#include "Display_Driver.h"

static SPIClass LCDspi(FSPI);
static uint8_t s_backlightPercent = 0;

#define SPI_WRITE(_dat) LCDspi.transfer(_dat)

static void SPI_Init(void) {
  LCDspi.begin(LCD_PIN_SCLK, LCD_PIN_MISO, LCD_PIN_MOSI);
}

static void LCD_WriteCommand(uint8_t Cmd) {
  LCDspi.beginTransaction(SPISettings(LCD_SPI_FREQ, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_PIN_CS, LOW);
  digitalWrite(LCD_PIN_DC, LOW);
  SPI_WRITE(Cmd);
  digitalWrite(LCD_PIN_CS, HIGH);
  LCDspi.endTransaction();
}

static void LCD_WriteData(uint8_t Data) {
  LCDspi.beginTransaction(SPISettings(LCD_SPI_FREQ, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_PIN_CS, LOW);
  digitalWrite(LCD_PIN_DC, HIGH);
  SPI_WRITE(Data);
  digitalWrite(LCD_PIN_CS, HIGH);
  LCDspi.endTransaction();
}

// Bulk pixel push. `ReadData` may be NULL - and normally is, we never read the
// panel back.
static void LCD_WriteData_nbyte(const uint8_t *SetData, uint8_t *ReadData, uint32_t Size) {
  LCDspi.beginTransaction(SPISettings(LCD_SPI_FREQ, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_PIN_CS, LOW);
  digitalWrite(LCD_PIN_DC, HIGH);
  LCDspi.transferBytes((uint8_t *)SetData, ReadData, Size);
  digitalWrite(LCD_PIN_CS, HIGH);
  LCDspi.endTransaction();
}

// Convenience for the init tables below: command followed by N data bytes.
static void LCD_WriteCmdData(uint8_t cmd, const uint8_t *data, uint8_t len) {
  LCD_WriteCommand(cmd);
  for (uint8_t i = 0; i < len; ++i) LCD_WriteData(data[i]);
}

static void LCD_Reset(void) {
  digitalWrite(LCD_PIN_CS, LOW);
  delay(50);
  if (LCD_PIN_RST >= 0) {
    digitalWrite(LCD_PIN_RST, LOW);
    delay(50);
    digitalWrite(LCD_PIN_RST, HIGH);
    delay(120);
  }
  digitalWrite(LCD_PIN_CS, HIGH);
}

// ---------------------------------------------------------------- ST7789 ---
#if NOMAD_LCD_CONTROLLER == NOMAD_LCD_ST7789
static void LCD_PanelInit(void) {
  static const uint8_t porch[]  = {0x0C, 0x0C, 0x00, 0x33, 0x33};
  static const uint8_t gamma_p[] = {0xF0, 0x00, 0x04, 0x04, 0x04, 0x05, 0x29,
                                    0x33, 0x3E, 0x38, 0x12, 0x12, 0x28, 0x30};
  static const uint8_t gamma_n[] = {0xF0, 0x07, 0x0A, 0x0D, 0x0B, 0x07, 0x28,
                                    0x33, 0x3E, 0x36, 0x14, 0x14, 0x29, 0x32};

  LCD_WriteCommand(0x11);  // SLPOUT
  delay(120);

  LCD_WriteCommand(0x36);  // MADCTL
  LCD_WriteData(LCD_MADCTL);

  LCD_WriteCommand(0x3A);  // COLMOD - 16bpp
  LCD_WriteData(0x05);

  LCD_WriteCommand(0xB0);  // RAM control
  LCD_WriteData(0x00);
  LCD_WriteData(0xE8);

  LCD_WriteCmdData(0xB2, porch, sizeof(porch));

  LCD_WriteCommand(0xB7);
  LCD_WriteData(0x35);
  LCD_WriteCommand(0xBB);
  LCD_WriteData(0x35);
  LCD_WriteCommand(0xC0);
  LCD_WriteData(0x2C);
  LCD_WriteCommand(0xC2);
  LCD_WriteData(0x01);
  LCD_WriteCommand(0xC3);
  LCD_WriteData(0x13);
  LCD_WriteCommand(0xC4);
  LCD_WriteData(0x20);
  LCD_WriteCommand(0xC6);
  LCD_WriteData(0x0F);
  LCD_WriteCommand(0xD0);
  LCD_WriteData(0xA4);
  LCD_WriteData(0xA1);
  LCD_WriteCommand(0xD6);
  LCD_WriteData(0xA1);

  LCD_WriteCmdData(0xE0, gamma_p, sizeof(gamma_p));
  LCD_WriteCmdData(0xE1, gamma_n, sizeof(gamma_n));

#if LCD_INVERT_COLORS
  LCD_WriteCommand(0x21);  // INVON
#else
  LCD_WriteCommand(0x20);  // INVOFF
#endif

  LCD_WriteCommand(0x11);
  delay(120);
  LCD_WriteCommand(0x29);  // DISPON
}

// ---------------------------------------------------------------- ST7735 ---
#elif NOMAD_LCD_CONTROLLER == NOMAD_LCD_ST7735
static void LCD_PanelInit(void) {
  static const uint8_t frmctr1[] = {0x01, 0x2C, 0x2D};
  static const uint8_t frmctr3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
  static const uint8_t pwctr1[]  = {0xA2, 0x02, 0x84};
  static const uint8_t pwctr3[]  = {0x0A, 0x00};
  static const uint8_t pwctr4[]  = {0x8A, 0x2A};
  static const uint8_t pwctr5[]  = {0x8A, 0xEE};
  static const uint8_t gamma_p[] = {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                                    0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10};
  static const uint8_t gamma_n[] = {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                                    0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10};

  LCD_WriteCommand(0x01);  // SWRESET
  delay(150);
  LCD_WriteCommand(0x11);  // SLPOUT
  delay(150);

  LCD_WriteCmdData(0xB1, frmctr1, sizeof(frmctr1));  // frame rate, normal
  LCD_WriteCmdData(0xB2, frmctr1, sizeof(frmctr1));  // frame rate, idle
  LCD_WriteCmdData(0xB3, frmctr3, sizeof(frmctr3));  // frame rate, partial

  LCD_WriteCommand(0xB4);  // INVCTR - no inversion
  LCD_WriteData(0x07);

  LCD_WriteCmdData(0xC0, pwctr1, sizeof(pwctr1));
  LCD_WriteCommand(0xC1);
  LCD_WriteData(0xC5);
  LCD_WriteCmdData(0xC2, pwctr3, sizeof(pwctr3));
  LCD_WriteCmdData(0xC3, pwctr4, sizeof(pwctr4));
  LCD_WriteCmdData(0xC4, pwctr5, sizeof(pwctr5));

  LCD_WriteCommand(0xC5);  // VMCTR1
  LCD_WriteData(0x0E);

#if LCD_INVERT_COLORS
  LCD_WriteCommand(0x21);  // INVON - required on these IPS panels
#else
  LCD_WriteCommand(0x20);
#endif

  LCD_WriteCommand(0x36);  // MADCTL
  LCD_WriteData(LCD_MADCTL);

  LCD_WriteCommand(0x3A);  // COLMOD - 16bpp
  LCD_WriteData(0x05);

  LCD_WriteCmdData(0xE0, gamma_p, sizeof(gamma_p));
  LCD_WriteCmdData(0xE1, gamma_n, sizeof(gamma_n));

  LCD_WriteCommand(0x13);  // NORON
  delay(10);
  LCD_WriteCommand(0x29);  // DISPON
  delay(100);
}
#else
#error "board_config.h did not select a supported LCD controller"
#endif

// ------------------------------------------------------------- public API ---
void LCD_SetRotation180(bool flip) {
  // XORing MX|MY (0xC0) into MADCTL reverses both scan directions, which is a
  // 180-degree turn on either controller.
  uint8_t madctl = LCD_MADCTL;
  if (flip) madctl ^= 0xC0;
  LCD_WriteCommand(0x36);
  LCD_WriteData(madctl);
}

void LCD_SetCursor(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend) {
  uint16_t x0 = Xstart + LCD_OFFSET_X;
  uint16_t x1 = Xend + LCD_OFFSET_X;
  uint16_t y0 = Ystart + LCD_OFFSET_Y;
  uint16_t y1 = Yend + LCD_OFFSET_Y;

  LCD_WriteCommand(0x2A);  // CASET
  LCD_WriteData(x0 >> 8);
  LCD_WriteData(x0 & 0xFF);
  LCD_WriteData(x1 >> 8);
  LCD_WriteData(x1 & 0xFF);

  LCD_WriteCommand(0x2B);  // RASET
  LCD_WriteData(y0 >> 8);
  LCD_WriteData(y0 & 0xFF);
  LCD_WriteData(y1 >> 8);
  LCD_WriteData(y1 & 0xFF);

  LCD_WriteCommand(0x2C);  // RAMWR
}

void LCD_addWindow(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend, uint16_t *color) {
  uint32_t pixels = (uint32_t)(Xend - Xstart + 1) * (uint32_t)(Yend - Ystart + 1);
  LCD_SetCursor(Xstart, Ystart, Xend, Yend);
  LCD_WriteData_nbyte((const uint8_t *)color, NULL, pixels * sizeof(uint16_t));
}

void LCD_FillColor(uint16_t color565) {
  // One row at a time so the scratch buffer stays small. The whole fill happens
  // inside a single CS assertion so RAMWR keeps auto-incrementing.
  static uint16_t row[LCD_WIDTH];
  for (uint16_t x = 0; x < LCD_WIDTH; ++x) row[x] = color565;

  LCD_SetCursor(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);

  LCDspi.beginTransaction(SPISettings(LCD_SPI_FREQ, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_PIN_CS, LOW);
  digitalWrite(LCD_PIN_DC, HIGH);
  for (uint16_t y = 0; y < LCD_HEIGHT; ++y) {
    LCDspi.transferBytes((uint8_t *)row, NULL, sizeof(row));
  }
  digitalWrite(LCD_PIN_CS, HIGH);
  LCDspi.endTransaction();
}

void LCD_Init(void) {
  pinMode(LCD_PIN_CS, OUTPUT);
  pinMode(LCD_PIN_DC, OUTPUT);
  if (LCD_PIN_RST >= 0) pinMode(LCD_PIN_RST, OUTPUT);
  digitalWrite(LCD_PIN_CS, HIGH);

  Backlight_Init();  // starts fully off so the panel's power-on garbage is hidden
  SPI_Init();

  LCD_Reset();
  LCD_PanelInit();

  LCD_FillColor(0x0000);  // blank before anything becomes visible
  Set_Backlight(60);      // sane default until settings.json is loaded
                          // (no-op on boards with no backlight GPIO)
}

// -------------------------------------------------------------- backlight ---
// LCD_PIN_BL of -1 means the board ties the backlight on with no GPIO in the
// path. The panel is simply always lit and the brightness calls do nothing but
// remember the value, so the admin slider still round-trips instead of failing.
void Backlight_Init(void) {
  s_backlightPercent = 0;
#if LCD_PIN_BL >= 0
  ledcAttach(LCD_PIN_BL, LCD_BL_PWM_FREQ_HZ, LCD_BL_PWM_BITS);
#if LCD_BL_ACTIVE_LEVEL
  ledcWrite(LCD_PIN_BL, 0);
#else
  ledcWrite(LCD_PIN_BL, LCD_BL_PWM_MAX);
#endif
#endif
}

void Set_Backlight(uint8_t Light) {
  if (Light > 100) Light = 100;  // the admin slider is 1..100; clamp, don't bail
  s_backlightPercent = Light;

#if LCD_PIN_BL >= 0
  uint32_t duty = ((uint32_t)Light * LCD_BL_PWM_MAX) / 100u;
#if !LCD_BL_ACTIVE_LEVEL
  duty = LCD_BL_PWM_MAX - duty;  // inverted drive
#endif
  ledcWrite(LCD_PIN_BL, duty);
#endif
}

uint8_t Get_Backlight(void) {
  return s_backlightPercent;
}
