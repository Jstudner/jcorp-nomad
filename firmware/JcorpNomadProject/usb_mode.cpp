// usb_mode.cpp - USB mass-storage mode.
//
// On the Pocket-Dongle this is the headline feature: the board *is* a USB-A
// plug, so holding the boot button turns the Nomad into an ordinary thumb
// drive for loading media, then ejecting drops it straight back into media
// server mode.

#include "board_config.h"
#include "boot_mode.h"      // for set_boot_mode()/MEDIA_MODE
#include "nomad_hw.h"
#include "RGB_lamp.h"
#include <USB.h>
#include <USBMSC.h>
#include "nomad_sd.h"

// USB Mass Storage Class (MSC) object
USBMSC msc;

static bool s_mscReady = false;

// true once the host writes a sector this session. on exit it tells us the card
// changed and needs a rescan (read-only browsing doesnt)
static volatile bool s_usbWroteData = false;

// --------------------- Callbacks ---------------------

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  uint32_t secSize = NomadSD.sectorSize();
  if (!secSize) {
    return -1;  // disk error
  }
  s_usbWroteData = true;
  for (uint32_t x = 0; x < bufsize / secSize; x++) {
    if (!NomadSD.writeRAW(buffer + secSize * x, lba + x)) {
      return -1;
    }
  }
  return bufsize;
}

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  uint32_t secSize = NomadSD.sectorSize();
  if (!secSize) {
    return -1;  // disk error
  }
  for (uint32_t x = 0; x < bufsize / secSize; x++) {
    if (!NomadSD.readRAW((uint8_t *)buffer + secSize * x, lba + x)) {
      return -1;  // outside of volume boundary
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

  NomadButton_Init();

  // Tiered mount, and never formats on failure. Upstream fixed the same
  // format-on-failure bug in #122; this path additionally walks the bus speed
  // down instead of giving up at one setting.
  NomadSdMountResult sd = NomadSD_Mount(5);
  if (!sd.mounted) {
    Serial.println("ERROR: SD card mount failed - staying idle, hold the button to go back.");
    Set_Color(60, 0, 0);  // red: no card
    return;
  }

  // Configure MSC
  msc.vendorID("Jcorp");
  msc.productID("Nomad");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.onStartStop(onStartStop);
  msc.mediaPresent(true);
  msc.begin(NomadSD.numSectors(), NomadSD.sectorSize());

  USB.begin();
  USB.onEvent(usbEventCallback);
  s_mscReady = true;

  Set_Color(0, 0, 60);  // blue: acting as a thumb drive
  Serial.println("USB Mass Storage ready - awaiting host.");
}

void usb_loop() {
  delay(2);  // avoid watchdog

  // Any press (short or long) returns to media server mode.
  NomadBtnEvent ev = NomadButton_Poll();
  if (ev == NOMAD_BTN_SHORT || ev == NOMAD_BTN_LONG) {
    Serial.println(">>> Button pressed, leaving USB mode");
    Set_Color(0, 0, 0);
    if (s_usbWroteData) {
      set_needs_reindex_flag();
    }
    set_boot_mode(MEDIA_MODE);
    esp_restart();
  }

  // Slow heartbeat on the status LED so it is obvious the stick is alive even
  // when the host has not enumerated it.
  if (s_mscReady) {
    static uint32_t last = 0;
    static bool on = false;
    if (millis() - last > 1200) {
      last = millis();
      on = !on;
      Set_Color(0, 0, on ? 60 : 8);
    }
  }
}
