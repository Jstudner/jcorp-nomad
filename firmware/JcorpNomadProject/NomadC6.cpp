// NomadC6.cpp - see NomadC6.h. Reporting only, no slave OTA (the header says why).

#include "NomadC6.h"
#include "nomad_board.h"

bool nomadC6Supported() {
#if NOMAD_HAS_HOSTED_RADIO
  return true;
#else
  return false;
#endif
}

#if !NOMAD_HAS_HOSTED_RADIO

void nomadC6HostVersion(uint32_t &a, uint32_t &b, uint32_t &c) { a = b = c = 0; }
bool nomadC6SlaveVersion(uint32_t &a, uint32_t &b, uint32_t &c) { a = b = c = 0; return false; }
bool nomadC6SlaveUnprogrammed() { return false; }
void nomadC6Report(const char *) {}

#else  // NOMAD_HAS_HOSTED_RADIO

#include "esp_hosted.h"

void nomadC6HostVersion(uint32_t &major, uint32_t &minor, uint32_t &patch) {
  major = ESP_HOSTED_VERSION_MAJOR_1;
  minor = ESP_HOSTED_VERSION_MINOR_1;
  patch = ESP_HOSTED_VERSION_PATCH_1;
}

// Keep the three outcomes separate. An RPC that goes unanswered means the slave's
// firmware predates that RPC. A version of 0.0.0 means it answered and has never been
// given real slave firmware (Waveshare shipped early boards that way).
bool nomadC6SlaveVersion(uint32_t &major, uint32_t &minor, uint32_t &patch) {
  esp_hosted_coprocessor_fwver_t v = {};
  if (esp_hosted_get_coprocessor_fwversion(&v) != 0) return false;
  major = v.major1;
  minor = v.minor1;
  patch = v.patch1;
  return true;
}

bool nomadC6SlaveUnprogrammed() {
  uint32_t a = 0, b = 0, c = 0;
  if (!nomadC6SlaveVersion(a, b, c)) return false;   // no answer is a different thing
  return a == 0 && b == 0 && c == 0;
}

void nomadC6Report(const char *when) {
  uint32_t hm = 0, hn = 0, hp = 0, sm = 0, sn = 0, sp = 0;
  nomadC6HostVersion(hm, hn, hp);

  if (!nomadC6SlaveVersion(sm, sn, sp)) {
    // The usual case on a Waveshare board, and the cause of the ESP-Hosted warning earlier
    // in the log. Nothing to fix.
    Serial.printf("[C6] (%s) ESP-Hosted host %u.%u.%u; the co-processor does not answer the "
                  "version request\n", when, (unsigned)hm, (unsigned)hn, (unsigned)hp);
    Serial.println("[C6] That, and the \"Version on Host is NEWER\" warning above, are expected on");
    Serial.println("[C6] this board: the co-processor's firmware predates that request. WiFi is");
    Serial.println("[C6] unaffected. Nothing polls the co-processor at runtime, so a late reply");
    Serial.println("[C6] cannot crash the host. No action needed - and do NOT try to update it.");
    return;
  }

  if (sm == 0 && sn == 0 && sp == 0) {
    Serial.printf("[C6] (%s) ESP-Hosted host %u.%u.%u, co-processor reports 0.0.0 - it has never "
                  "been given slave firmware\n", when, (unsigned)hm, (unsigned)hn, (unsigned)hp);
    return;
  }

  const uint32_t host = (hm << 16) | (hn << 8) | hp;
  const uint32_t slave = (sm << 16) | (sn << 8) | sp;
  Serial.printf("[C6] (%s) ESP-Hosted host %u.%u.%u, co-processor %u.%u.%u%s\n", when,
                (unsigned)hm, (unsigned)hn, (unsigned)hp,
                (unsigned)sm, (unsigned)sn, (unsigned)sp,
                slave < host ? "  (older than the host, which is fine)" : "");
}

#endif  // NOMAD_HAS_HOSTED_RADIO
