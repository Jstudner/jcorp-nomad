# ESP Async WebServer

A modified copy of [ESP Async WebServer](https://github.com/ESP32Async/ESPAsyncWebServer) 3.12.1
(ESP32Async), the `src/` folder of the release, included here so it builds with Nomad's request
limits and no library to install. LGPL-3.0, `LICENSE` here. It still needs
[Async TCP](https://github.com/ESP32Async/AsyncTCP) 3.6.0 or newer from the library manager.

Modified for Nomad, November 2026. Every change is flagged `NOMAD PATCH` in the source:

- `WebRequest.cpp`: a request keeps at most `NOMAD_WS_MAX_PARAMS` (48) parameters and
  `NOMAD_WS_MAX_HEADERS` (48) headers, and a request or header line over `NOMAD_WS_MAX_LINE`
  (4096 bytes) ends the connection. The release has no limits, and a URL with 1,500
  parameters exhausts Nomad's internal memory before any handler can refuse it. Each limit
  can be set from the build with `-D`.
- `WebResponses.cpp`: `AsyncAbstractResponse::_respond` clears the header list once it has
  been assembled into the outgoing text. A film reply lives for minutes, and its headers hold
  about 2 KB of internal RAM per viewer.
- `WebResponses.cpp` / `ESPAsyncWebServer.h`: `nomad_send_gate`, a hook the sketch sets, trims
  the window `AsyncAbstractResponse` fills from, so a reply can wait for its next ack while
  internal RAM is short. Null by default.
- `WebRequest.cpp`: `_onPoll` pushes out queued data when the socket has no free space. When
  the WiFi driver is out of memory, segments stay queued, and with nothing in flight no ack
  comes to send them: the reply stalled until the 5 s ack timeout closed it part way.
- `#include <X.h>` of the library's own headers (`AsyncJson.h`, `AsyncWebServerLogging.h`,
  `ChunkPrint.h`, `ESPAsyncWebServer.h`) made `#include "X.h"`, so a copy of the library
  installed in Arduino can't be mixed in.

To take a newer release: copy its `src/` over this folder, keep this README, and apply the
same changes again (search the old copy for `NOMAD PATCH`).
