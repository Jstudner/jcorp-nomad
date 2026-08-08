// nomad_hw.h - board bring-up helpers shared by the firmware and the
// standalone hardware self-test sketch.

#pragma once
#include <Arduino.h>
#include "board_config.h"

// ------------------------------------------------------------------ SD ----
struct NomadSdMountResult {
  bool     mounted;
  bool     fourBit;
  uint32_t freqHz;
  uint64_t cardSizeBytes;
};

// Mounts the card at the pins from board_config.h, walking down through
// 4-bit/40 MHz -> 4-bit/20 MHz -> 1-bit/20 MHz -> 1-bit/probe speed until one
// combination sticks. Never formats the card on failure.
NomadSdMountResult NomadSD_Mount(uint8_t maxOpenFiles = 12);

// ------------------------------------------------------------- button ----
enum NomadBtnEvent {
  NOMAD_BTN_NONE = 0,
  NOMAD_BTN_SHORT,  // tap - cycle the on-screen page
  NOMAD_BTN_LONG    // hold - drop into USB mass storage
};

void NomadButton_Init(void);

// Poll from a task or from loop(). Returns at most one event per press.
// NOMAD_BTN_LONG fires as soon as the hold time is reached, without waiting
// for the release, so the user gets feedback while still holding.
NomadBtnEvent NomadButton_Poll(void);

// ------------------------------------------------------------- report ----
void NomadHW_PrintBoardInfo(Stream &out);
