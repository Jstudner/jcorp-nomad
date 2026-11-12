// NomadUI.h - the web interface served out of flash instead of off the card.
//
// The P4 board has 32 MB of flash, which leaves a 6.75 MB LittleFS partition next to a
// 13 MB app (the NANO uses the 9.9 MB ffat partition of the 16 MB layout). The interface
// held in flash is about 2.9 MB.
//
// With the interface in flash, firmware and interface can't disagree, and a card that is
// missing, wrong or half-copied still boots into something that can say so. An existing
// Nomad card works untouched, its copies of the interface are ignored once flash has its own.
//
// If a path exists in flash it is served from flash, otherwise from the card. Flash only
// ever holds interface files, so that never shadows media. The flash volume is not part of
// the NomadSD union either, so directory listings and the media index never see it.
//
// Not every interface file goes in flash. The whole interface (about 10 MB) is bigger than
// either partition, so the maps stack (/assets/maps/) and any single file over
// NOMAD_UI_FLASH_MAX_FILE stay on the card. Those are the map engine and fonts, the PDF,
// comic, e-book and chess engines, and each only opens something that's on the card anyway.
// firmware/build-p4.sh applies the same rule to the image.
//
// On a board without a UI partition every call returns "not present" and the card serves
// the interface.

#ifndef NOMAD_UI_H
#define NOMAD_UI_H

#include <Arduino.h>
#include <FS.h>

// Mount the UI partition. Safe to call once, from setup(), before the server starts.
bool nomadUiBegin();

// True when flash is mounted and actually holds an interface (index.html is the marker).
bool nomadUiActive();

// Does this exact path live in flash?
bool nomadUiHas(const String &path);

// The filesystem a request for this path should be served from. Returns the flash
// volume when it has the file, the card otherwise, so call sites stay one-liners.
fs::FS &nomadUiFS(const String &path);

// Copy the interface off the card into flash, on a background task. Returns false if a
// sync is already running or there is nothing to copy. Progress goes to the web log.
bool nomadUiStartSync();
bool nomadUiSyncing();

// Empty the flash copy, so the card serves the interface again.
bool nomadUiClear();

// The build id of the image in flash, from /.uiversion, or "" if the partition holds
// no shipped image. build-p4.sh derives it from the contents of SD_Card_Template/, so
// two boards reporting the same id are serving the same interface.
const char *nomadUiVersion();

// Status for the admin panel.
uint64_t nomadUiTotalBytes();
uint64_t nomadUiUsedBytes();
uint32_t nomadUiFileCount();

// True if this path is an interface file the sync would copy (media never is).
bool nomadUiIsInterfacePath(const String &path);

// The biggest single file the image (and the updater, and the sync) puts in flash.
static const uint32_t NOMAD_UI_FLASH_MAX_FILE = 200 * 1024;

// An interface file that belongs in flash under the rule above, given its size.
bool nomadUiBelongsInFlash(const String &path, uint64_t size);

// While flash is the interface: is this a different spelling (case) of a file flash holds?
// Flash is case-sensitive and the card isn't, so /ADMIN.HTML would miss flash's /admin.html
// and open the card's stale copy. Those get a 404. A path flash has no file for at all (a
// library left on the card) isn't a variant and is served from the card.
bool nomadUiShadows(const String &path);

// The flash volume itself, while it is the authoritative interface, else nullptr. For the
// interface updater (NomadUpdate.h), which on these boards has to write here: a page
// written to the card is never served while flash holds the interface.
fs::FS *nomadUiFlash();

// Free bytes in the UI partition (0 without one).
uint64_t nomadUiFreeBytes();

// After the interface updater replaced files in flash: /.uiversion becomes
// "<image id>+u<n>" (n counts updates since the image went on) and the cached id is
// dropped. Every flash file's ETag carries the id, so browsers fetch the changed set again.
void nomadUiNoteUpdated();

#endif  // NOMAD_UI_H
