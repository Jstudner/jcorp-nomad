#include "RGB_lamp.h"

static bool s_ledReady = false;

// ------------------------------------------------------------- WS2812 ------
#if NOMAD_LED_TYPE == NOMAD_LED_WS2812

void RGB_Init(void) {
  s_ledReady = true;  // neopixelWrite() drives the RMT/bitbang itself
}

static void led_write(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(LED_PIN_DATA, r, g, b);
}

// ------------------------------------------------------------- APA102 ------
#elif NOMAD_LED_TYPE == NOMAD_LED_APA102

// APA102 is plain SPI without chip select, so bit-banging a handful of bytes is
// simpler (and cheaper) than tying up a hardware SPI bus for one LED.
static inline void apa_byte(uint8_t v) {
  for (int8_t bit = 7; bit >= 0; --bit) {
    digitalWrite(LED_PIN_DATA, (v >> bit) & 0x01);
    digitalWrite(LED_PIN_CLOCK, HIGH);
    digitalWrite(LED_PIN_CLOCK, LOW);
  }
}

void RGB_Init(void) {
  if (s_ledReady) return;
  pinMode(LED_PIN_DATA, OUTPUT);
  pinMode(LED_PIN_CLOCK, OUTPUT);
  digitalWrite(LED_PIN_DATA, LOW);
  digitalWrite(LED_PIN_CLOCK, LOW);
  s_ledReady = true;
}

static void led_write(uint8_t r, uint8_t g, uint8_t b) {
  RGB_Init();

  // Start frame: 32 zero bits.
  for (uint8_t i = 0; i < 4; ++i) apa_byte(0x00);

  // LED frame: 111 + 5-bit global current, then the pixel in BGR order.
  apa_byte(0xE0 | (LED_APA102_LEVEL & 0x1F));
  apa_byte(b);
  apa_byte(g);
  apa_byte(r);

  // End frame: at least n/2 clocks. One LED, so a single byte is plenty; four
  // keeps us comfortably inside spec.
  for (uint8_t i = 0; i < 4; ++i) apa_byte(0xFF);
}

// --------------------------------------------------------------- none ------
#else

void RGB_Init(void) {
  s_ledReady = true;
}
static void led_write(uint8_t r, uint8_t g, uint8_t b) {
  (void)r;
  (void)g;
  (void)b;
}

#endif

void Set_Color(uint8_t Red, uint8_t Green, uint8_t Blue) {
  if (!s_ledReady) RGB_Init();
  led_write(Red, Green, Blue);
}

// Hue (0..255) to RGB, fixed point, no float and no lookup table. Replaces the
// old 576-byte ramp table and gives a smoother sweep.
static void hue_to_rgb(uint8_t hue, uint8_t value, uint8_t *r, uint8_t *g, uint8_t *b) {
  uint8_t region = hue / 43;         // 0..5
  uint8_t remainder = (hue - region * 43) * 6;  // 0..255 within the region
  uint8_t p = 0;
  uint8_t q = (uint8_t)(((uint16_t)value * (255 - remainder)) >> 8);
  uint8_t t = (uint8_t)(((uint16_t)value * remainder) >> 8);

  switch (region) {
    case 0:  *r = value; *g = t;     *b = p;     break;
    case 1:  *r = q;     *g = value; *b = p;     break;
    case 2:  *r = p;     *g = value; *b = t;     break;
    case 3:  *r = p;     *g = q;     *b = value; break;
    case 4:  *r = t;     *g = p;     *b = value; break;
    default: *r = value; *g = p;     *b = q;     break;
  }
}

void RGB_Lamp_Loop(uint16_t Waiting) {
  static uint32_t lastStep = 0;
  static uint8_t hue = 0;

  if (Waiting == 0) Waiting = 1;
  uint32_t now = millis();
  if ((uint32_t)(now - lastStep) < Waiting) return;
  lastStep = now;

  uint8_t r, g, b;
  hue_to_rgb(hue++, 160, &r, &g, &b);
  Set_Color(r, g, b);
}
