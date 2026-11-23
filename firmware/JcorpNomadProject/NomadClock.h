// NomadClock.h - the time of day for the chat page, with no clock chip and no internet. Phones
// know the time: the chat page sends its phone's once a visit, and each message carries one.
// Only chat uses it; file dates and the rest keep the fixed date.
//
// After Meshtastic's src/gps/RTC.cpp (ranked sources, a sanity window around the build date, a
// wait before a phone may correct drift again) and Debian's fake-hwclock (save the time
// regularly and before a restart, restore it at start-up).
// Ranking: the build date (a floor) < the saved time (behind by however long Nomad was
// unplugged) < one phone (only moves the time forward) < two phones on different addresses
// agreeing within 2 minutes (may move it either way, and again after 6 hours).
// The system clock is left alone.
//
//   POST /api/clock?t=<ms since 1970>   a page's own time (anyone; answered 204)
//   GET  /api/clock                     {"now":ms,"iso":"...","source":"phones"}
#pragma once
#ifndef ARDUINO_STUB   // tools/clocktest builds this on a PC
#include <Arduino.h>
#include <esp_timer.h>
#endif
#include <sys/time.h>
#include <time.h>

enum : uint8_t { CLOCK_BUILD = 0, CLOCK_SAVED, CLOCK_PHONE, CLOCK_PHONES };
static const char *const kClockSource[] = { "build", "saved", "one phone", "phones" };

static portMUX_TYPE g_clockMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t g_clockQ = CLOCK_BUILD;
static int64_t g_clockOffMs = 0;        // wall time = monotonic ms + this
static uint32_t g_clockSetAt = 0;       // millis() of the last accepted time

static const int64_t CLOCK_AGREE_MS = 2LL * 60 * 1000;          // two phones this close agree
static const uint32_t CLOCK_SAMPLE_LIFE_MS = 10UL * 60 * 1000;  // a phone's time counts this long
static const uint32_t CLOCK_RESYNC_MS = 6UL * 60 * 60 * 1000;   // agreeing phones may correct it again

static inline int64_t clockMonoMs() { return esp_timer_get_time() / 1000; }

// When this firmware was built, from __DATE__ and __TIME__ (the build machine's local time,
// so hours off UTC: good enough for a floor and a sanity window).
static int64_t nomadBuildMs() {
  static int64_t cached = 0;
  if (cached) return cached;
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *d = __DATE__, *t = __TIME__;          // "Mmm dd yyyy", "hh:mm:ss"
  struct tm tm = {};
  for (int m = 0; m < 12; m++)
    if (!strncmp(months + m * 3, d, 3)) tm.tm_mon = m;
  tm.tm_mday = atoi(d + 4);
  tm.tm_year = atoi(d + 7) - 1900;
  tm.tm_hour = atoi(t);
  tm.tm_min = atoi(t + 3);
  tm.tm_sec = atoi(t + 6);
  setenv("TZ", "UTC0", 1);
  tzset();
  cached = (int64_t)mktime(&tm) * 1000;
  return cached;
}

static int64_t nomadNowMs() {
  portENTER_CRITICAL(&g_clockMux);
  const uint8_t q = g_clockQ;
  const int64_t off = g_clockOffMs;
  portEXIT_CRITICAL(&g_clockMux);
  return q == CLOCK_BUILD ? nomadBuildMs() + clockMonoMs() : clockMonoMs() + off;
}

static void nomadClockApply(int64_t wallMs, uint8_t q) {
  portENTER_CRITICAL(&g_clockMux);
  g_clockOffMs = wallMs - clockMonoMs();
  g_clockQ = q;
  g_clockSetAt = millis();
  portEXIT_CRITICAL(&g_clockMux);
}

// start-up: the saved time if it is later than the build date
static void nomadClockBoot(int64_t savedMs) {
  if (savedMs > nomadBuildMs()) nomadClockApply(savedMs, CLOCK_SAVED);
}

static bool clockSane(int64_t ms) {
  const int64_t b = nomadBuildMs();
  return ms > b - 24LL * 3600 * 1000 && ms < b + 40LL * 365 * 24 * 3600 * 1000;
}

// One page's time, from address `ip`. True when the time changed (the caller saves it).
static bool nomadClockFromPhone(uint32_t ip, int64_t phoneMs) {
  struct Sample { uint32_t ip; int64_t off; uint32_t at; };
  static Sample samples[4];
  if (!clockSane(phoneMs)) return false;
  const int64_t mono = clockMonoMs();
  const int64_t off = phoneMs - mono;
  const uint32_t now = millis();

  // another address, recently, within 2 minutes of this one: the two agree
  bool agree = false;
  int64_t otherOff = 0;
  for (auto &s : samples) {
    if (!s.at || s.ip == ip || now - s.at > CLOCK_SAMPLE_LIFE_MS) continue;
    if (llabs(s.off - off) <= CLOCK_AGREE_MS) { agree = true; otherOff = s.off; break; }
  }
  // keep this one: its address's slot, or the oldest
  Sample *slot = &samples[0];
  for (auto &s : samples) {
    if (s.ip == ip) { slot = &s; break; }
    if (s.at < slot->at) slot = &s;
  }
  *slot = { ip, off, now ? now : 1 };   // never 0 (an empty slot), never ahead of now

  const uint8_t q = g_clockQ;
  if (agree) {
    if (q < CLOCK_PHONES || now - g_clockSetAt > CLOCK_RESYNC_MS) {
      nomadClockApply(mono + (off + otherOff) / 2, CLOCK_PHONES);
      return true;
    }
    return false;
  }
  // one phone on its own moves the time forward only, and only until two phones agree
  if (q < CLOCK_PHONE && phoneMs >= nomadNowMs() - 60000) {
    nomadClockApply(phoneMs, CLOCK_PHONE);
    return true;
  }
  return false;
}

static String nomadClockIso(int64_t ms) {
  const time_t s = (time_t)(ms / 1000);
  struct tm t;
  gmtime_r(&s, &t);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
  return String(buf);
}
