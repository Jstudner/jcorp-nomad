// NomadEth.h - the wired NIC on the ESP32-P4-NANO.
//
// The NANO puts an IP101GRI behind an RJ45 (with PoE magnetics) on the P4's own EMAC.
// The radio on these boards is an ESP32-C6 over SDIO, and that link tops out well below
// what the card and the HTTP server can serve. Wired is the only interface that isn't
// throttled by ESP-Hosted.
//
// Ethernet comes up alongside the WiFi and never takes the hotspot down, so a board with
// no cable, or a pulled one, is still reachable on its own AP.
//
// Absent on the WIFI6 board and on the Mk4, where every call is a no-op stub.

#ifndef NOMAD_ETH_H
#define NOMAD_ETH_H

#include <Arduino.h>
#include <IPAddress.h>

// True on a board with a wired NIC fitted.
bool nomadEthSupported();

// Bring the PHY up and start watching for link. Returns false if the driver would not
// start. True only means the MAC/PHY were created, a cable may not be plugged in.
// Call once, from startNetwork().
bool nomadEthBegin();

// Cable present and the PHY has negotiated.
bool nomadEthLinkUp();

// Link up AND a DHCP lease in hand. Test this one before saying the board is on the LAN.
bool nomadEthHasIP();

IPAddress nomadEthIP();

// "absent" | "down" | "link" | "ready". down means started with no cable.
const char *nomadEthState();

// 10 or 100, 0 when there is no link.
uint16_t nomadEthSpeed();
bool nomadEthFullDuplex();

// How many times the cable has come up and gone away since boot. A number that climbs on
// its own means a bad cable or switch port.
uint32_t nomadEthLinkUps();
uint32_t nomadEthLinkDowns();

// Print what the wired side is doing. `when` labels the call site.
void nomadEthReport(const char *when);

#endif  // NOMAD_ETH_H
