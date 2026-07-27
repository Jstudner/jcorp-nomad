// usb_mode.cpp
#include "boot_mode.h"      // for set_boot_mode()/MEDIA_MODE
#include <USB.h>
#include <USBMSC.h>
#include "NomadSD.h"
#define BOOT_BUTTON_PIN 0

// TinyUSB soft connect/disconnect (compiled into the prebuilt core lib).
// Used to re-enumerate once the disk is actually ready - see usb_setup().
extern "C" {
  bool tud_disconnect(void);
  bool tud_connect(void);
}

// USB Mass Storage Class (MSC) object
USBMSC msc;

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
// The host moves data in chunks of CFG_TUD_MSC_BUFSIZE (4KB = 8 sectors). Each chunk
// is one multi-sector SD command; issuing 8 single-sector commands here used to spend
// more time on command overhead than on data.

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  uint32_t secSize = NomadSD.sectorSize();
  if (!secSize || offset != 0 || (bufsize % secSize) != 0) {
    return -1;  // disk error / partial-sector op we don't support
  }
  s_usbWroteData = true;
  if (!NomadSD.writeRAWMulti(buffer, lba, bufsize / secSize)) {
    return -1;
  }
  return bufsize;
}

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  uint32_t secSize = NomadSD.sectorSize();
  if (!secSize || offset != 0 || (bufsize % secSize) != 0) {
    return -1;
  }
  if (!NomadSD.readRAWMulti((uint8_t *)buffer, lba, bufsize / secSize)) {
    return -1;
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
  Serial.println(">>> USB mode: mounting SD & starting MSC");

  NomadSD.setPins(clk, cmd, d0, d1, d2, d3);
  // raw card init only: MSC shuttles sectors and the host interprets the filesystem, so
  // USB mode works whatever format the card holds, and can never trigger a
  // mount-failure auto-format like SD_MMC could
  if (!NomadSD.beginRaw(false, SDMMC_FREQ_HIGHSPEED)) {
    Serial.println("ERROR: SD card init failed!");
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
  msc.begin(NomadSD.numSectors(), NomadSD.sectorSize());

  USB.begin();
  USB.onEvent(usbEventCallback);

  // With CDC-on-boot the USB stack enumerated before setup() ran, when this LUN still
  // had no media, no geometry and no callbacks. The host saw an empty card reader,
  // backed off and took its time re-polling, which was the long wait before the drive
  // appeared. Drop off the bus and come back now that the disk is real.
  tud_disconnect();
  delay(150);
  tud_connect();

  Serial.println("USB Mass Storage ready—awaiting host.");
}

void usb_loop() {
  delay(25);  // feed WDT; button poll needs no more than this

  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    if (s_usbWroteData) {
      set_needs_reindex_flag();
    }
    set_boot_mode(MEDIA_MODE);
    esp_restart();
  }
}
