// NomadScreen.cpp - see NomadScreen.h.
#include "NomadScreen.h"
#include <SPI.h>
#include <esp_timer.h>

volatile uint32_t g_lcdFlushes = 0;

namespace {

// The board's wiring.
constexpr int PIN_SCK = 40, PIN_MOSI = 45, PIN_CS = 42, PIN_DC = 41, PIN_RST = 39;
constexpr int PIN_BACKLIGHT = BOARD_USB_C ? 46 : 48;

constexpr uint32_t SPI_HZ = 80000000;
constexpr uint32_t BACKLIGHT_PWM_HZ = 20000;   // 1 kHz shows as moving bands to phone cameras (the QR code)
constexpr uint8_t BACKLIGHT_PWM_BITS = 10;       // 100 % is fully on (1023)
constexpr uint16_t COLUMN_OFFSET = 34;           // the 172 px panel sits mid-way across the controller's 240 px
constexpr uint32_t LVGL_TICK_MS = 5;

// ST7789 commands, by their datasheet names.
enum : uint8_t {
  SLPOUT = 0x11, INVON = 0x21, DISPON = 0x29, CASET = 0x2A, RASET = 0x2B, RAMWR = 0x2C,
  MADCTL = 0x36, COLMOD = 0x3A, RAMCTRL = 0xB0, PORCTRL = 0xB2, GCTRL = 0xB7, VCOMS = 0xBB,
  LCMCTRL = 0xC0, VDVVRHEN = 0xC2, VRHS = 0xC3, VDVS = 0xC4, FRCTRL2 = 0xC6, PWCTRL1 = 0xD0,
  PVGAMCTRL = 0xE0, NVGAMCTRL = 0xE1,
};
constexpr uint8_t MADCTL_UPRIGHT = 0x00;
constexpr uint8_t MADCTL_TURNED = 0xC0;          // MY and MX: both scan directions reversed

// Sent once after sleep-out: command, parameter count, parameters. The voltages, porch and
// gamma values are this panel's tuning.
struct Step { uint8_t cmd, n, data[14]; };
const Step kPanelSetup[] = {
  { MADCTL, 1, { MADCTL_UPRIGHT } },
  { COLMOD, 1, { 0x05 } },                       // 16 bits a pixel
  { RAMCTRL, 2, { 0x00, 0xE8 } },                // pixels come little-endian, the way LVGL keeps them
  { PORCTRL, 5, { 0x0C, 0x0C, 0x00, 0x33, 0x33 } },
  { GCTRL, 1, { 0x35 } },
  { VCOMS, 1, { 0x35 } },
  { LCMCTRL, 1, { 0x2C } },
  { VDVVRHEN, 1, { 0x01 } },
  { VRHS, 1, { 0x13 } },
  { VDVS, 1, { 0x20 } },
  { FRCTRL2, 1, { 0x0F } },                      // 60 Hz
  { PWCTRL1, 2, { 0xA4, 0xA1 } },
  { 0xD6, 1, { 0xA1 } },                         // undocumented, the panel maker sets it
  { PVGAMCTRL, 14, { 0xF0, 0x00, 0x04, 0x04, 0x04, 0x05, 0x29, 0x33, 0x3E, 0x38, 0x12, 0x12, 0x28, 0x30 } },
  { NVGAMCTRL, 14, { 0xF0, 0x07, 0x0A, 0x0D, 0x0B, 0x07, 0x28, 0x33, 0x3E, 0x36, 0x14, 0x14, 0x29, 0x32 } },
  { INVON, 0, {} },                              // an IPS panel: colours are only right inverted
};

SPIClass s_spi(FSPI);

// Driven the way the Waveshare demo drives it, byte for byte: the command in a chip-select of
// its own with DC low, each parameter byte in its own with DC high, pixels in one. A command and
// its parameters in one chip-select leave the panel black (backlight on).
void command(uint8_t cmd) {
  s_spi.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  digitalWrite(PIN_DC, LOW);
  s_spi.transfer(cmd);
  digitalWrite(PIN_CS, HIGH);
  s_spi.endTransaction();
}

void dataByte(uint8_t b) {
  s_spi.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  digitalWrite(PIN_DC, HIGH);
  s_spi.transfer(b);
  digitalWrite(PIN_CS, HIGH);
  s_spi.endTransaction();
}

void dataBlock(const uint8_t *data, size_t n) {
  s_spi.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  digitalWrite(PIN_DC, HIGH);
  s_spi.writeBytes(data, n);
  digitalWrite(PIN_CS, HIGH);
  s_spi.endTransaction();
}

void send(uint8_t cmd, const uint8_t *data, size_t n) {
  command(cmd);
  for (size_t i = 0; i < n; ++i) dataByte(data[i]);
}

void sendRange(uint8_t cmd, uint16_t first, uint16_t last) {
  const uint8_t b[4] = { uint8_t(first >> 8), uint8_t(first), uint8_t(last >> 8), uint8_t(last) };
  send(cmd, b, sizeof(b));
}

void panelBegin() {
  pinMode(PIN_CS, OUTPUT);
  pinMode(PIN_DC, OUTPUT);
  pinMode(PIN_RST, OUTPUT);
  ledcAttach(PIN_BACKLIGHT, BACKLIGHT_PWM_HZ, BACKLIGHT_PWM_BITS);
  ledcWrite(PIN_BACKLIGHT, 1023);                // full: the boot, error and USB screens must be readable
  s_spi.begin(PIN_SCK, -1, PIN_MOSI);

  // hardware reset, Sleep Out, the set-up, Sleep Out again, display on: the demo's order
  digitalWrite(PIN_CS, LOW);
  delay(50);
  digitalWrite(PIN_RST, LOW);
  delay(50);
  digitalWrite(PIN_RST, HIGH);
  delay(50);
  command(SLPOUT);
  delay(120);
  for (const Step &s : kPanelSetup) send(s.cmd, s.data, s.n);
  command(SLPOUT);
  delay(120);
  command(DISPON);
}

// LVGL draws a band of the screen at a time into this buffer. The flush returns only once the
// pixels are on the panel, so one buffer is enough.
lv_disp_draw_buf_t s_drawBuf;
lv_color_t s_band[SCREEN_W * SCREEN_H / 20];
lv_disp_drv_t s_disp;

void flush(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
  sendRange(CASET, a->x1 + COLUMN_OFFSET, a->x2 + COLUMN_OFFSET);
  sendRange(RASET, a->y1, a->y2);
  const size_t bytes = size_t(a->x2 - a->x1 + 1) * size_t(a->y2 - a->y1 + 1) * sizeof(lv_color_t);
  command(RAMWR);
  dataBlock(reinterpret_cast<const uint8_t *>(px), bytes);
  g_lcdFlushes = g_lcdFlushes + 1;
  lv_disp_flush_ready(drv);
}

void tick(void *) { lv_tick_inc(LVGL_TICK_MS); }

}  // namespace

void screenBegin() {
  panelBegin();

  lv_init();
  lv_disp_draw_buf_init(&s_drawBuf, s_band, nullptr, sizeof(s_band) / sizeof(s_band[0]));
  lv_disp_drv_init(&s_disp);
  s_disp.hor_res = SCREEN_W;
  s_disp.ver_res = SCREEN_H;
  s_disp.flush_cb = flush;
  s_disp.draw_buf = &s_drawBuf;
  lv_disp_drv_register(&s_disp);

  const esp_timer_create_args_t t = { .callback = tick, .name = "lvgl_tick" };
  esp_timer_handle_t h = nullptr;
  esp_timer_create(&t, &h);
  esp_timer_start_periodic(h, LVGL_TICK_MS * 1000);
}

void screenFlip(bool upsideDown) {
  const uint8_t m = upsideDown ? MADCTL_TURNED : MADCTL_UPRIGHT;
  send(MADCTL, &m, 1);
}

void screenBacklight(uint8_t percent) {
  if (percent <= 100) ledcWrite(PIN_BACKLIGHT, uint32_t(percent) * 1023 / 100);
}
