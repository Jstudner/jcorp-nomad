// NomadLed.cpp - see NomadLed.h.
#include "NomadLed.h"

namespace {
constexpr uint8_t PIN_LED = 38;
constexpr uint16_t LEG = 64;           // steps from one channel to the next
constexpr uint8_t SCALE = 3;           // 64 * 3 = 192 at the brightest: bright, not glaring
uint16_t s_calls = 0, s_step = 0;
}

void ledSet(uint8_t first, uint8_t second, uint8_t third) {
  rgbLedWrite(PIN_LED, first, second, third);
}

void ledRainbowStep(uint16_t every) {
  if (++s_calls < every) return;
  s_calls = 0;
  s_step = (s_step + 1) % (3 * LEG);
  // Three legs: channel 0 hands over to 1, 1 to 2, 2 back to 0. One fades out as the next fades in.
  const uint8_t leg = s_step / LEG, i = s_step % LEG;
  const uint8_t out = (LEG - i) * SCALE, in = (1 + i) * SCALE;
  uint8_t c[3] = { 0, 0, 0 };
  c[leg] = out;
  c[(leg + 1) % 3] = in;
  ledSet(c[0], c[1], c[2]);
}
