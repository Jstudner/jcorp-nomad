// NomadC6.h - reporting what the radio co-processor is, and nothing more.
//
// On a board whose radio is a separate ESP32-C6 over SDIO, boot prints:
//
//   Version on Host is NEWER than version on co-processor
//   RPC requests sent by host may encounter timeout errors
//   or may not be supported by co-processor
//
// That warning is expected on these boards and is not a fault. Don't act on it.
//
// Don't use esp_hosted's slave OTA here. On slave firmware older than 2.6.0 there is no
// activate RPC and esp_hosted_slave_ota_end() itself commits the image. Tried on an
// ESP32-P4-WIFI6, it left the C6 unresponsive for good, and that board's C6 has no
// exposed programming pins to recover it.
//
// An old co-processor sometimes drops an RPC response and ESP-Hosted crashes on the
// missing reply. Nomad doesn't poll the co-processor at runtime (the client count comes
// from AP events), so there's nothing left to time out.

#ifndef NOMAD_C6_H
#define NOMAD_C6_H

#include <Arduino.h>

// True on a board whose radio is a separate ESP-Hosted co-processor.
bool nomadC6Supported();

// The ESP-Hosted version this firmware was built against. Compile-time.
void nomadC6HostVersion(uint32_t &major, uint32_t &minor, uint32_t &patch);

// What the co-processor reports over RPC. False when it does not answer at all, which is
// normal on a slave whose firmware predates that RPC, and not a fault by itself.
bool nomadC6SlaveVersion(uint32_t &major, uint32_t &minor, uint32_t &patch);

// True when the co-processor answered but reports 0.0.0, meaning it has never been given
// real slave firmware. Distinct from an RPC that goes unanswered.
bool nomadC6SlaveUnprogrammed();

// Print the host and co-processor versions and say plainly whether anything is wrong.
// `when` labels the call site, e.g. "boot" or "settled".
void nomadC6Report(const char *when);

#endif  // NOMAD_C6_H
