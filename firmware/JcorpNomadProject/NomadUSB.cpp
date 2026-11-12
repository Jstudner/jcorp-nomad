// NomadUSB.cpp - USB mass-storage host. See NomadUSB.h for the why.

#include "NomadUSB.h"
#include "nomad_board.h"

#if NOMAD_HAS_USB_HOST

#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "esp_private/msc_scsi_bot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static msc_host_device_handle_t s_dev = nullptr;
static volatile bool s_ready = false;
static uint32_t s_sectorSize = 0;
static uint32_t s_sectorCount = 0;
static char s_product[40] = {0};

static SemaphoreHandle_t s_io = nullptr;      // one SCSI command at a time
static volatile uint8_t s_pendingAddr = 0;    // device address from the connect event
static volatile bool s_arrival = false;
static volatile bool s_removal = false;
static bool s_installed = false;

// The driver bounces every transfer through its own DMA buffer sized to the request, so
// an unbounded multi-sector read would claim megabytes of internal RAM. 32 sectors is
// 16 KB, the range handler's chunk, and avoids the per-command overhead of single-sector
// reads.
static const size_t USB_MAX_SECTORS_PER_CMD = 32;

static void mscEventCb(const msc_host_event_t *event, void *) {
  switch (event->event) {
    case msc_host_event_t::MSC_DEVICE_CONNECTED:
      // Only record the address. Installing the device talks to it over USB, which
      // must not happen on the driver's own event task.
      s_pendingAddr = event->device.address;
      s_arrival = true;
      break;
    case msc_host_event_t::MSC_DEVICE_DISCONNECTED:
      s_ready = false;
      s_removal = true;
      break;
    default:
      break;
  }
}

// The USB host library's own event loop. The MSC driver runs its client task itself.
static void usbHostDaemonTask(void *) {
  for (;;) {
    uint32_t flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
    // ALL_FREE is ignored: the drive is expected back, so the stack stays installed and
    // re-enumerates.
  }
}

// Finish enumeration off the callback: open the device, read its geometry, publish it.
static void usbAttachTask(void *) {
  for (;;) {
    if (s_arrival && !s_ready) {
      uint8_t addr = s_pendingAddr;
      msc_host_device_handle_t dev = nullptr;
      if (msc_host_install_device(addr, &dev) == ESP_OK) {
        msc_host_device_info_t info = {};
        if (msc_host_get_device_info(dev, &info) == ESP_OK && info.sector_size && info.sector_count) {
          s_sectorSize = info.sector_size;
          s_sectorCount = info.sector_count;
          size_t i = 0;
          for (; i + 1 < sizeof(s_product) && info.iProduct[i]; ++i) {
            wchar_t c = info.iProduct[i];
            s_product[i] = (c >= 32 && c < 127) ? (char)c : '?';
          }
          s_product[i] = 0;
          s_dev = dev;
          s_ready = true;
          log_i("NomadUSB: %s, %u sectors of %u bytes", s_product,
                (unsigned)s_sectorCount, (unsigned)s_sectorSize);
        } else {
          log_e("NomadUSB: device would not report its geometry");
          msc_host_uninstall_device(dev);
        }
      } else {
        log_e("NomadUSB: could not open device at address %u", (unsigned)addr);
      }
      s_arrival = false;
    }
    // This task owns s_removal, it's the only thing allowed to call into the MSC driver.
    // Clear it even with no device to release, or a disconnect that arrives before the
    // geometry was published leaves the flag set forever.
    if (s_removal) {
      s_removal = false;
      if (s_dev) {
        msc_host_uninstall_device(s_dev);
        s_dev = nullptr;
        s_sectorSize = s_sectorCount = 0;
        s_product[0] = 0;
        log_i("NomadUSB: device released, ready for the next one");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

bool nomadUsbBegin() {
  if (s_installed) return true;

  usb_host_config_t hostCfg = {};
  hostCfg.skip_phy_setup = false;
  hostCfg.intr_flags = ESP_INTR_FLAG_LEVEL1;

#if defined(CONFIG_IDF_TARGET_ESP32P4)
  // Without this a high-speed drive is refused outright:
  //   HCD DWC: EP MPS (512) exceeds supported limit (256)
  //   USBH: EP Alloc error: ESP_ERR_NOT_SUPPORTED
  // The device enumerates, then the MSC interface can't be claimed. Looks like a broken
  // drive, isn't.
  //
  // The core's prebuilt IDF uses CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT (meant for
  // USB audio), which gives most of the 1024 FIFO lines to the periodic queue and leaves
  // the RX FIFO around 64 lines. Max packet is about rx_fifo_lines * 4, so 256 bytes, and
  // a high-speed bulk endpoint is 512.
  //
  // From the HAL:
  //     IN mps = (rx - 2) * 4      non-periodic OUT mps = nptx * 4
  // so 512-byte bulk needs rx >= 130 and nptx >= 128. Nomad has no isochronous or
  // interrupt OUT, so the room comes out of the periodic queue.
  //
  // The total can't be the full 1024 of OTG20_DFIFO_DEPTH: the core reserves the EPINFO
  // area on top (HWCFG3.DfifoDepth vs EPINFO_CTL), and 1024 fails at install with
  // "Invalid FIFO config". 704 stays well under that and gives both directions 4x what
  // a high-speed bulk endpoint needs.
  hostCfg.fifo_settings_custom.rx_fifo_lines   = 384;  // IN  mps limit 1528 bytes
  hostCfg.fifo_settings_custom.nptx_fifo_lines = 256;  // OUT mps limit 1024 bytes
  hostCfg.fifo_settings_custom.ptx_fifo_lines  = 64;   // unused by mass storage
#endif

  esp_err_t err = usb_host_install(&hostCfg);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    log_e("NomadUSB: usb_host_install failed (0x%x)", err);
    return false;
  }

  if (!s_io) s_io = xSemaphoreCreateMutex();
  if (!s_io) return false;

  if (xTaskCreatePinnedToCore(usbHostDaemonTask, "UsbDaemon", 4096, nullptr, 5, nullptr, 0) != pdPASS) {
    log_e("NomadUSB: could not start the host daemon task");
    return false;
  }

  msc_host_driver_config_t mscCfg = {};
  mscCfg.create_backround_task = true;
  mscCfg.task_priority = 5;
  mscCfg.stack_size = 4096;
  mscCfg.core_id = 0;
  mscCfg.callback = mscEventCb;
  mscCfg.callback_arg = nullptr;
  err = msc_host_install(&mscCfg);
  if (err != ESP_OK) {
    log_e("NomadUSB: msc_host_install failed (0x%x)", err);
    return false;
  }

  if (xTaskCreatePinnedToCore(usbAttachTask, "UsbAttach", 4096, nullptr, 4, nullptr, 0) != pdPASS) {
    log_e("NomadUSB: could not start the attach task");
    return false;
  }

  s_installed = true;
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  log_i("NomadUSB: host stack up (FIFO rx/nptx/ptx = %u/%u/%u lines), waiting for a device",
        (unsigned)hostCfg.fifo_settings_custom.rx_fifo_lines,
        (unsigned)hostCfg.fifo_settings_custom.nptx_fifo_lines,
        (unsigned)hostCfg.fifo_settings_custom.ptx_fifo_lines);
#else
  log_i("NomadUSB: host stack up, waiting for a device");
#endif
  return true;
}

bool nomadUsbReady() {
  return s_ready && s_dev != nullptr;
}

uint32_t nomadUsbSectorCount() {
  return s_ready ? s_sectorCount : 0;
}

uint32_t nomadUsbSectorSize() {
  return s_ready ? s_sectorSize : 0;
}

uint64_t nomadUsbCapacityBytes() {
  return (uint64_t)nomadUsbSectorCount() * (uint64_t)nomadUsbSectorSize();
}

const char *nomadUsbProduct() {
  return s_product;
}

bool nomadUsbReadSectors(uint32_t lba, uint8_t *dst, size_t count) {
  if (!nomadUsbReady() || !dst || count == 0) return false;
  if ((uint64_t)lba + count > s_sectorCount) return false;
  if (xSemaphoreTake(s_io, pdMS_TO_TICKS(8000)) != pdTRUE) return false;
  bool ok = true;
  while (count && ok) {
    size_t n = count > USB_MAX_SECTORS_PER_CMD ? USB_MAX_SECTORS_PER_CMD : count;
    ok = (scsi_cmd_read10(s_dev, dst, lba, n, s_sectorSize) == ESP_OK);
    dst += n * s_sectorSize;
    lba += n;
    count -= n;
  }
  xSemaphoreGive(s_io);
  return ok;
}

bool nomadUsbWriteSectors(uint32_t lba, const uint8_t *src, size_t count) {
  if (!nomadUsbReady() || !src || count == 0) return false;
  if ((uint64_t)lba + count > s_sectorCount) return false;
  if (xSemaphoreTake(s_io, pdMS_TO_TICKS(8000)) != pdTRUE) return false;
  bool ok = true;
  while (count && ok) {
    size_t n = count > USB_MAX_SECTORS_PER_CMD ? USB_MAX_SECTORS_PER_CMD : count;
    ok = (scsi_cmd_write10(s_dev, src, lba, n, s_sectorSize) == ESP_OK);
    src += n * s_sectorSize;
    lba += n;
    count -= n;
  }
  xSemaphoreGive(s_io);
  return ok;
}

bool nomadUsbTakeArrival() {
  // The attach task clears s_arrival once it has published the geometry, so the sketch
  // sees the edge only when the volume is actually mountable.
  static bool lastReady = false;
  bool now = nomadUsbReady();
  bool edge = now && !lastReady;
  lastReady = now;
  return edge;
}

bool nomadUsbTakeRemoval() {
  // Edge-detected off the published state like arrival above. Don't consume s_removal
  // here: loop() runs far more often than the attach task polls and would clear it first,
  // so msc_host_uninstall_device() never runs and the next drive never enumerates.
  static bool lastReady = false;
  bool now = nomadUsbReady();
  bool edge = !now && lastReady;
  lastReady = now;
  return edge;
}

#else  // no USB host on this board

bool nomadUsbBegin() { return false; }
bool nomadUsbReady() { return false; }
uint32_t nomadUsbSectorCount() { return 0; }
uint32_t nomadUsbSectorSize() { return 0; }
uint64_t nomadUsbCapacityBytes() { return 0; }
const char *nomadUsbProduct() { return ""; }
bool nomadUsbReadSectors(uint32_t, uint8_t *, size_t) { return false; }
bool nomadUsbWriteSectors(uint32_t, const uint8_t *, size_t) { return false; }
bool nomadUsbTakeArrival() { return false; }
bool nomadUsbTakeRemoval() { return false; }

#endif  // NOMAD_HAS_USB_HOST
