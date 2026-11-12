// NomadThumbs.h - cover thumbnails, generated on the device by the P4's hardware.
//
// The grids ask for one image per card, and those are full-size cover art: 200-800 KB of
// JPEG painted into a box a few hundred pixels wide.
//
// The ESP32-P4 has a hardware JPEG codec (decode and encode) and a Pixel Processing
// Accelerator that scales, so the device bakes a small copy of every cover once and serves
// that to the grids instead:
//
//   read cover -> HW JPEG decode to RGB565 -> PPA scale to RGB888 -> HW JPEG encode
//
// RGB565 for the full-size intermediate since that buffer costs megabytes, RGB888 after
// the scaler since that one is small and the encoder gets clean input.
//
// Thumbnails live on the card under /.thumbs, a dot directory, so the indexer skips them.
// One is only served when the request asks for it, so detail pages still get full art.
//
// None of this exists on the S3, where NOMAD_HAS_THUMBNAILER is 0. Every entry point
// returns "no thumbnail" and the grids get the original art.

#ifndef NOMAD_THUMBS_H
#define NOMAD_THUMBS_H

#include <Arduino.h>

// Long edge of a generated thumbnail, and its JPEG quality. 360 is above the largest grid
// card on a phone at 3x, so the browser never stretches it.
#define NOMAD_THUMB_MAX_PX   360
#define NOMAD_THUMB_QUALITY  82

// Covers below this are left alone, re-encoding them costs time and saves nothing.
#define NOMAD_THUMB_MIN_SRC_BYTES 24576

// True when this board can make thumbnails at all.
bool nomadThumbsSupported();

// The thumbnail path for a source path, derived from the path alone with no I/O, so it
// is cheap enough to call on every request. Returns "" when unsupported or not an image.
String nomadThumbPathFor(const String &srcPath);

// The thumbnail to serve for this source, or "" if there isn't one. One existence check.
String nomadThumbLookup(const String &srcPath);

// Start a pass. rebuildAll re-encodes covers that already have a current thumbnail,
// otherwise only missing or out of date ones are touched. Returns false if a pass is
// already running, the card is busy indexing, or the board can't do it.
bool nomadThumbsStart(bool rebuildAll);

// Ask the running pass to stop at the next cover. It is never interrupted mid-file.
void nomadThumbsStop();

// Delete every generated thumbnail, on a background task. Returns false if a pass or an
// earlier clear is still running. True only means it started.
bool nomadThumbsClear();

// True while this module is doing anything heavy to the card. Indexing and the SD scan
// check this before starting, the same way this module checks their flags.
bool nomadThumbsBusy();

enum NomadThumbPhase {
  NOMAD_THUMB_IDLE = 0,     // never run this boot
  NOMAD_THUMB_SCANNING,     // walking the media folders, counting work
  NOMAD_THUMB_WORKING,      // converting
  NOMAD_THUMB_DONE,         // finished a pass of its own accord
  NOMAD_THUMB_STOPPED,      // finished early because it was asked to
  NOMAD_THUMB_FAILED        // could not start, or the hardware refused
};

struct NomadThumbStats {
  NomadThumbPhase phase;
  uint32_t covers;          // JPEG cover candidates found by the scan
  uint32_t upToDate;        // already had a current thumbnail
  uint32_t converted;       // written this pass
  uint32_t skipped;         // left alone on purpose, broken down below
  uint32_t progressive;     // progressive JPEG, which the hardware decoder cannot read
  uint32_t alreadySmall;    // already at or under the thumbnail size
  uint32_t noSaving;        // the thumbnail came out no smaller than the original
  uint32_t tooBig;          // would not fit in free PSRAM alongside everything else
  uint32_t notJpeg;         // named .jpg but isn't one (PNG is the usual culprit)
  uint32_t failed;          // tried and could not
  uint64_t srcBytes;        // total size of the covers converted this pass
  uint64_t thumbBytes;      // total size of the thumbnails written for them
  uint64_t progressiveBytes; // how much cover art is locked behind the baseline-only decoder
  uint32_t elapsedMs;       // wall clock of the current or last pass
  char current[96];         // cover being worked on, or ""
  char lastError[96];       // most recent failure reason, or ""
};

void nomadThumbsStatus(NomadThumbStats &out);

// True when a scan has run and found nothing left to do. The admin panel uses this for
// "all covers optimised", converted == 0 would also be true of a pass that failed.
bool nomadThumbsAllOptimised();

#endif  // NOMAD_THUMBS_H
