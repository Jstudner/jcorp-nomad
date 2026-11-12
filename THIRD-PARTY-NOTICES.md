# Third-party notices

`LICENSE.md` covers Jcorp Nomad's own work: the firmware, the web interface, the case
and the documentation. The components below are other people's work. They ship with
Nomad under their own licences, which `LICENSE.md` does not change.

## Firmware

Compiled into the firmware image.

| Component | Version | Licence |
|---|---|---|
| [Arduino core for ESP32](https://github.com/espressif/arduino-esp32) | 3.3.3 | LGPL-2.1 |
| [ESP-IDF](https://github.com/espressif/esp-idf) (via the Arduino core) | as bundled | Apache-2.0 |
| [ESP Async WebServer](https://github.com/ESP32Async/ESPAsyncWebServer) | 3.7.1 | LGPL-3.0 |
| [Async TCP](https://github.com/ESP32Async/AsyncTCP) | 3.4.7 | LGPL-3.0 |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | 7.3.0 | MIT |
| [SdFat](https://github.com/greiman/SdFat) | 2.3.0 | MIT |
| [LVGL](https://github.com/lvgl/lvgl) | 8.x | MIT |
| [XZ Embedded](https://github.com/tukaani-project/xz-embedded) (`firmware/JcorpNomadProject/src/xz/`, for archives made before 2021, licence text beside it) | v2024-12-30 | 0BSD |
| [Zstandard](https://github.com/facebook/zstd) decoder (`firmware/JcorpNomadProject/src/zstd/`, the release's lib/ decoder sources in one file, licence text beside it) | 1.5.7 | BSD-3-Clause (chosen from its BSD/GPLv2 dual licence) |

The firmware's complete source is this repository, so anyone can rebuild it against
modified versions of the LGPL libraries.

## Web interface (`SD_Card_Template/assets/`)

| File | Project | Licence |
|---|---|---|
| `jquery.min.js` | jQuery 3.6.0 | MIT |
| `epub.min.js` | [epub.js](https://github.com/futurepress/epub.js) | BSD-2-Clause |
| `jszip.min.js` | JSZip 3.10.1 | MIT (dual MIT / GPL-3.0; used under MIT) |
| `pdf.min.js`, `pdf.worker.min.js` | [PDF.js](https://github.com/mozilla/pdf.js), Mozilla Foundation | Apache-2.0 |
| `plyr.min.js`, `plyr.css`, `plyr.svg` | [Plyr](https://github.com/sampotts/plyr) | MIT |
| `leaflet/` | Leaflet 1.9.4 | BSD-2-Clause |
| `pako.min.js` | [pako](https://github.com/nodeca/pako) | MIT and Zlib |
| `fzstd.min.js` | [fzstd](https://github.com/101arrowz/fzstd) | MIT |
| `xzwasm.min.js` | [xzwasm](https://github.com/SteveSanderson/xzwasm), with xz-embedded (public domain) | MIT |
| `unzipit.min.js` | unzipit 1.4.3 | MIT |
| `uncompress.js` | [uncompress.js](https://github.com/workhorsy/uncompress.js), Matthew Brennan Jones | MIT |
| `libunrar.js`, `libunrar.js.mem` | [libunrar-js](https://github.com/wcchoi/libunrar-js), built from RARLAB's UnRAR source | UnRAR licence: free to use and redistribute for extracting RAR archives; may not be used to create a RAR-compatible archiver |
| `chess.min.js` | chess.js 0.12.1, Jeff Hlywa | BSD-2-Clause |
| `tenuki.min.js` | tenuki 0.3.1, Adam Prescott | MIT |

## Games (`SD_Card_Template/EmulatorJS/`, `SD_Card_Template/Games/`)

| Component | Licence | Text |
|---|---|---|
| [EmulatorJS](https://github.com/EmulatorJS/EmulatorJS) 4.2.3 | GPL-3.0 | `SD_Card_Template/EmulatorJS/LICENSE` |
| PrBoom libretro core (`EmulatorJS/data/cores/prboom-*`) | GPL-2.0-or-later | `SD_Card_Template/EmulatorJS/data/cores/LICENSE-prboom` |
| Gambatte libretro core (`EmulatorJS/data/cores/gambatte-*`) | GPL-2.0 | `SD_Card_Template/EmulatorJS/data/cores/LICENSE-gambatte` |
| FCEUmm libretro core (`EmulatorJS/data/cores/fceumm-*`) | GPL-2.0 | `SD_Card_Template/EmulatorJS/data/cores/LICENSE-fceumm` |
| mGBA libretro core (`EmulatorJS/data/cores/mgba-*`) | MPL-2.0 | `SD_Card_Template/EmulatorJS/data/cores/LICENSE-mgba` |
| Snes9x libretro core (`EmulatorJS/data/cores/snes9x-*`) | Snes9x licence, non-commercial only | `SD_Card_Template/EmulatorJS/data/cores/LICENSE-snes9x` |
| Genesis Plus GX libretro core (`EmulatorJS/data/cores/genesis_plus_gx-*`) | Genesis Plus GX licence, non-commercial only | `SD_Card_Template/EmulatorJS/data/cores/LICENSE-genesis_plus_gx` |
| [Freedoom](https://freedoom.github.io/) Phase 1, 0.13.0 (`Games/Freedoom.wad`) | BSD-3-Clause | `SD_Card_Template/Games/Freedoom-COPYING.txt` |

`Games/Freedoom.png` is rendered from the title screen inside `Freedoom.wad`. Earlier
versions of this repository shipped id Software's DOOM shareware WAD; Freedoom replaces
it because the shareware licence does not allow it to be sold on a card.

The GPL cores (PrBoom, Gambatte, FCEUmm) come from the libretro projects; a card that is sold has to come with their
source or a written offer of it. Snes9x and Genesis Plus GX are free to share but can't be sold, so a card for a prebuilt Nomad that is sold leaves them
out.

## Sample content on the card template

| Content | Source | Licence |
|---|---|---|
| `Archive/wikipedia_en_100_nopic_2026-07.zim` | Wikipedia, packaged by [Kiwix](https://kiwix.org) | CC BY-SA 4.0 (text) |
| `Maps/dc/` | USGS The National Map | Public domain (US Government work) |
