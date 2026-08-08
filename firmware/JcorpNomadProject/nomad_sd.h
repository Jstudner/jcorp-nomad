// nomad_sd.h - which filesystem object the firmware talks to.
//
// The Waveshare board and the LilyGO T-Dongle wire their card as a 4-bit SDMMC
// bus. The GNPE Pocket-Dongle-S3 wires it as a plain 4-wire SPI card on its own
// SPI host - there is no CMD line and no DAT1/DAT2/DAT3 at all, so SD_MMC
// simply cannot drive it.
//
// Both Arduino classes derive from fs::FS and both expose the extras the
// firmware needs beyond the FS API - cardType/cardSize/totalBytes/usedBytes for
// the storage readouts, and numSectors/sectorSize/readRAW/writeRAW for USB mass
// storage. Only begin() and setPins() differ, and those are confined to
// NomadSD_Mount() in nomad_hw.cpp.
//
// Everything else in the firmware just says NomadSD.

#pragma once
#include "board_config.h"
#include <FS.h>

#if NOMAD_SD_BUS == NOMAD_SD_BUS_SPI
  #include <SD.h>
  #include <SPI.h>
  #define NomadSD SD
  // The card gets its own SPI host so it never contends with the display.
  extern SPIClass NomadSdSpi;
#else
  #include <SD_MMC.h>
  #define NomadSD SD_MMC
#endif
