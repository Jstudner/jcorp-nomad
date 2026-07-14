// usb_mode.cpp
#include "boot_mode.h"      // for set_boot_mode()/MEDIA_MODE
#include <USB.h>
#include <USBMSC.h>
#include <SD_MMC.h>
#define BOOT_BUTTON_PIN 0 
// USB Mass Storage Class (MSC) object
USBMSC msc;

// DMA-safe buffer for USB MSC transfers
static uint8_t usb_buffer[8192] __attribute__((aligned(4)));

// SDMMC pin configuration
int clk = 14;
int cmd = 15;
int d0  = 16;
int d1  = 18;
int d2  = 17;
int d3  = 21;
bool onebit = false; // using full 4-bit wiring

// true once the host writes a sector this session. on exit it tells us the card
// changed and needs a rescan (read-only browsing doesnt)
static volatile bool s_usbWroteData = false;

// --------------------- Callbacks ---------------------

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  uint32_t secSize = SD_MMC.sectorSize();
  if (!secSize) {
    return false;  // disk error
  }
  s_usbWroteData = true;
  for (int x = 0; x < bufsize / secSize; x++) {
    if (!SD_MMC.writeRAW(buffer + secSize * x, lba + x)) {
      return false;
    }
  }
  return bufsize;
}

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  uint32_t secSize = SD_MMC.sectorSize();
  if (!secSize) {
    return false;  // disk error
  }
  for (int x = 0; x < bufsize / secSize; x++) {
    if (!SD_MMC.readRAW((uint8_t *)buffer + secSize * x, lba + x)) {
      return false;  // outside of volume boundary
    }
  }
  return bufsize;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
  log_i("Start/Stop power: %u\tstart: %d\teject: %d",
        power_condition, start, load_eject);
  return true;
}

static void usbEventCallback(void*, esp_event_base_t event_base,
                             int32_t event_id, void*) {
  if (event_base == ARDUINO_USB_EVENTS) {
    switch (event_id) {
      case ARDUINO_USB_STOPPED_EVENT:
        // Host ejected the drive > switch back to Media on next boot
        if (s_usbWroteData) {
          set_needs_reindex_flag();
        }
        set_boot_mode(MEDIA_MODE);
        esp_restart();
        break;
      default:
        break;
    }
  }
}

// ------------------ USB Mode Entry Points ------------------

void usb_setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(">>> USB mode: mounting SD & starting MSC");

  SD_MMC.setPins(clk, cmd, d0, d1, d2, d3);
  // Never format user media when USB mode cannot mount it.
  if (!SD_MMC.begin("/sdcard", false, false, 50000000)) {
    Serial.println("ERROR: SD card mount failed!");
    return;
  }

  // Configure MSC
  msc.vendorID("ESP32");
  msc.productID("USB_MSC");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.onStartStop(onStartStop);
  msc.mediaPresent(true);
  msc.begin(SD_MMC.numSectors(), SD_MMC.sectorSize());

  USB.begin();
  USB.onEvent(usbEventCallback);

  Serial.println("USB Mass Storage ready—awaiting host.");
}

void usb_loop() {
  delay(0);  // avoid watchdog

  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    if (s_usbWroteData) {
      set_needs_reindex_flag();
    }
    set_boot_mode(MEDIA_MODE);
    esp_restart();
  }
}
