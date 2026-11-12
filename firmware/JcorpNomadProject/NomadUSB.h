// NomadUSB.h - a USB mass-storage device as a second Nomad volume (ESP32-P4 only).
//
// The P4 has a USB 2.0 high-speed OTG controller separate from the USB-Serial-JTAG one,
// so a drive on the OTG port is readable while the board is still flashable. This layer
// enumerates the drive and hands raw sectors to NomadSD, which puts the same SdFat
// exFAT/FAT32 code on top that the card uses.
//
// No SdFat types in this header. Its headers drag in the global `typedef FsFile File`,
// which collides with fs::File across the rest of the firmware. NomadSD.cpp wraps these
// functions in a FsBlockDeviceInterface instead.
//
// Everything is a no-op returning false on a board without USB host, so call sites need
// no guards of their own.

#ifndef NOMAD_USB_H
#define NOMAD_USB_H

#include <Arduino.h>

// Install the USB host stack and start watching for a device. Returns false if the
// board has no USB host or the stack would not start. Safe to call once, from setup().
bool nomadUsbBegin();

// True once a device is enumerated, has answered READ CAPACITY and is readable.
bool nomadUsbReady();

uint32_t nomadUsbSectorCount();
uint32_t nomadUsbSectorSize();
uint64_t nomadUsbCapacityBytes();

// Product string from the device descriptor, or "". Shown in the admin storage panel.
const char *nomadUsbProduct();

bool nomadUsbReadSectors(uint32_t lba, uint8_t *dst, size_t count);
bool nomadUsbWriteSectors(uint32_t lba, const uint8_t *src, size_t count);

// One-shot edge flags. The sketch polls these and mounts or drops the volume outside the
// USB callback, where touching SdFat would deadlock against a transfer in flight.
bool nomadUsbTakeArrival();
bool nomadUsbTakeRemoval();

#endif  // NOMAD_USB_H
