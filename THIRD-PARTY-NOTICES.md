# Third-party notices

[LICENSING.md](LICENSING.md) covers Jcorp Nomad's/My own files and content: the firmware, the web interface,
the case and the documentation. The components below are other people's work. They ship with
Nomad under their own licences, which Nomad's license does not change.

## Firmware

Compiled into the firmware image.

| Component | Version | Licence |
|---|---|---|
| [Arduino core for ESP32](https://github.com/espressif/arduino-esp32) | 3.3.3 | LGPL-2.1 |
| [ESP-IDF](https://github.com/espressif/esp-idf) (via the Arduino core) | as bundled | Apache-2.0 |
| [ESP Async WebServer](https://github.com/ESP32Async/ESPAsyncWebServer) (`firmware/JcorpNomadProject/src/AsyncWebServer/`, with Nomad's request limits, licence text and changes beside it) | 3.12.1 | LGPL-3.0 |
| SHA-1 backport inside it (`src/AsyncWebServer/BackPort_SHA1Builder.*`), from mbed TLS / Espressif | | Apache-2.0 |
| [Async TCP](https://github.com/ESP32Async/AsyncTCP) | 3.6.0 | LGPL-3.0 |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | 7.3.0 | MIT |
| [SdFat](https://github.com/greiman/SdFat) | 2.3.0 | MIT |
| [LVGL](https://github.com/lvgl/lvgl) | 8.3 | MIT |
| Montserrat and Font Awesome symbols, LVGL's built-in fonts | | OFL-1.1 |
| [Roboto](https://github.com/googlefonts/roboto) Bold, the boot screen's text (`ui_font_Font1.c`, `ui_font_Font2.c`) | 2011 | Apache-2.0 |
| [Roboto](https://fonts.google.com/specimen/Roboto) Bold and Medium, the status pages' text (`nomad_status_fonts.c`, from `tools/screen/fontgen.py`) | 2011 The Roboto Project Authors | OFL-1.1 |
| [Material Symbols](https://github.com/google/material-design-icons), the status pages' icons (`nomad_status_icons.c`; sources and list in `tools/screen/icons/`) | Google | Apache-2.0 |
| [Material Design Icons](https://pictogrammers.com/library/mdi/) micro-sd and usb, the status pages' card and USB icons (same files) | Pictogrammers | Apache-2.0 |
| [XZ Embedded](https://github.com/tukaani-project/xz-embedded) (`firmware/JcorpNomadProject/src/xz/`, for archives made before 2021, licence text beside it) | v2024-12-30 | 0BSD |
| [Zstandard](https://github.com/facebook/zstd) decoder (`firmware/JcorpNomadProject/src/zstd/`, the release's lib/ decoder sources in one file, licence text beside it) | 1.5.7 | BSD-3-Clause (chosen from its BSD/GPLv2 dual licence) |

The firmware's complete source is this repository, so anyone can rebuild it against
modified versions of the LGPL libraries. Espressif's WiFi and radio libraries come with the
Arduino core as compiled binaries, under Espressif's own licence; LICENSING.md covers how they
sit with Nomad's GPL.

## Web interface (`SD_Card_Template/assets/`)

| File | Project | Licence |
|---|---|---|
| `jquery.min.js` | jQuery 3.6.0 | MIT |
| `epub.min.js` | [epub.js](https://github.com/futurepress/epub.js) | BSD-2-Clause |
| `jszip.min.js` | JSZip 3.10.1 | MIT (dual MIT / GPL-3.0; used under MIT) |
| `pdf.min.js`, `pdf.worker.min.js` | [PDF.js](https://github.com/mozilla/pdf.js), Mozilla Foundation | Apache-2.0 |
| `plyr.min.js`, `plyr.css`, `plyr.svg` | [Plyr](https://github.com/sampotts/plyr) | MIT |
| `leaflet/` | Leaflet 1.9.4 | BSD-2-Clause |
| `leaflet/protomaps-leaflet.js` | [protomaps-leaflet](https://github.com/protomaps/protomaps-leaflet) 5.1.0 | BSD-3-Clause |
| `maps/maplibre-gl*` | [MapLibre GL JS](https://github.com/maplibre/maplibre-gl-js) 6.11.2 | BSD-3-Clause |
| `maps/pmtiles.js` | [PMTiles](https://github.com/protomaps/PMTiles) 4.5.0 | BSD-3-Clause |
| `maps/basemaps.js` | [@protomaps/basemaps](https://github.com/protomaps/basemaps) 5.7.2 | BSD-3-Clause; the style is CC0-1.0, the Mapzen/Tilezen parts MIT |
| `maps/sprites/` | Protomaps basemaps assets, icons from Mapzen | MIT |
| `maps/fonts/` | Noto Sans glyphs | OFL-1.1 |
| `pako.min.js` | [pako](https://github.com/nodeca/pako) | MIT and Zlib |
| `fzstd.min.js` | [fzstd](https://github.com/101arrowz/fzstd) | MIT |
| `xzwasm.min.js` | [xzwasm](https://github.com/SteveSanderson/xzwasm), with xz-embedded (public domain) | MIT |
| `unzipit.min.js` | unzipit 1.4.3 | MIT |
| `uncompress.js` | [uncompress.js](https://github.com/workhorsy/uncompress.js), Matthew Brennan Jones | MIT |
| `libunrar.js`, `libunrar.js.mem` | [libunrar-js](https://github.com/wcchoi/libunrar-js), built from RARLAB's UnRAR source | UnRAR licence: free to use and redistribute for extracting RAR archives; may not be used to create a RAR-compatible archiver |
| `chess.min.js` | chess.js 0.12.1, Jeff Hlywa | BSD-2-Clause |
| `tenuki.min.js` | tenuki 0.3.1, Adam Prescott | MIT |
| `nomad-qr.js` | Nomad's own, following [Project Nayuki's QR Code generator](https://www.nayuki.io/page/qr-code-generator-library) | MIT |
| `nomad-icons.js` | Nomad's own line icons; some follow [Feather](https://feathericons.com) | MIT |

Bundled inside the files above, under their own licences: core-js (MIT) and localForage
(Apache-2.0) in `epub.min.js`, rangetouch and loadjs (MIT) in `plyr.min.js`.

## Sample content included in the card template

| Content | Source | Licence |
|---|---|---|
| `Archive/wikipedia_en_100_nopic_2026-07.zim` | Wikipedia, packaged by [Kiwix](https://kiwix.org) | CC BY-SA 4.0 (text) |
| `Maps/dc/` | USGS The National Map | Public domain (US Government work) |
| `Books/MobyDick.epub` | Herman Melville, [Project Gutenberg](https://www.gutenberg.org/ebooks/2701) #2701 | Public domain text; Project Gutenberg's licence for its name |
| `Books/TheIliad.pdf` | Homer, Simcox translation (1865), [Internet Archive](https://archive.org/details/homersiliad00home) | Public domain |
| The Bach Aria recordings in `Music/` | [Musopen](https://musopen.org) | Public domain |
| `Games/Toolkit.png` photo | Jonas Bergsten, [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Swiss_army_knife_open_20050612_(cropped).jpg) | Public domain |
| `Games/Watch Together.png` photo | Sven Scheuermeier ([Unsplash](https://unsplash.com/@sveninho)), [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Vintage_television_(Unsplash).jpg) | CC0 |
