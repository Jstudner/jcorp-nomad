// NomadDLNA.h - DLNA/UPnP MediaServer for Jcorp Nomad.
//
// DLNA discovery starts with SSDP (a UDP multicast M-SEARCH on 239.255.255.250:1900).
// This answers M-SEARCH, serves the device description and implements ContentDirectory
// Browse from the NDJSON indexes on the card. Streaming uses the /media range endpoint.
//
// Tested targets: DLNA player TVs (Samsung/LG/Sony), Roku Media Player, VLC, Kodi.

#ifndef NOMAD_DLNA_H
#define NOMAD_DLNA_H

#include "src/AsyncWebServer/ESPAsyncWebServer.h"   // carried here, with Nomad's limits (src/AsyncWebServer/README.md)

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
