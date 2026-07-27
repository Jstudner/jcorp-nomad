// NomadDLNA.h - real DLNA/UPnP MediaServer for Jcorp Nomad.
//
// The old /dlna endpoints were HTTP only, and no TV ever asked for them because DLNA
// discovery starts with SSDP: a UDP multicast M-SEARCH on 239.255.255.250:1900. This
// module answers those, serves a proper device description, and implements the
// ContentDirectory Browse action fed by the NDJSON indexes already on the card.
// Streaming rides the proven /media range endpoint.
//
// Tested targets: DLNA player TVs (Samsung/LG/Sony), Roku Media Player, VLC, Kodi.

#ifndef NOMAD_DLNA_H
#define NOMAD_DLNA_H

#include <ESPAsyncWebServer.h>

// register /dlna/* HTTP routes; call before server.begin()
void dlnaRegisterRoutes(AsyncWebServer &server);

// bind the SSDP multicast listener; call once after the AP is up
void dlnaStart();

// rebind after an AP restart. The multicast membership lives on the AP netif,
// so tearing the AP down silently kills discovery until this runs.
void dlnaRestart();

// paces SSDP alive announcements; call from loop()
void dlnaTick();

// admin toggle. Off = ignore M-SEARCH, stop announcing, 404 the endpoints,
// and say goodbye so TVs drop us from their source lists.
void dlnaSetEnabled(bool en);
bool dlnaIsEnabled();

#endif  // NOMAD_DLNA_H
