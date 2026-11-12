/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <wchar.h>
#include <stdint.h>
#include "esp_err.h"
#include "usb/usb_host.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_ERR_MSC_HOST_BASE        0x1700                      /*!< MSC host error code base */
#define ESP_ERR_MSC_MOUNT_FAILED    (ESP_ERR_MSC_HOST_BASE + 1)  /*!< Failed to mount storage */
#define ESP_ERR_MSC_FORMAT_FAILED   (ESP_ERR_MSC_HOST_BASE + 2)  /*!< Failed to format storage */
#define ESP_ERR_MSC_INTERNAL        (ESP_ERR_MSC_HOST_BASE + 3)  /*!< MSC host internal error */
#define ESP_ERR_MSC_STALL           (ESP_ERR_MSC_HOST_BASE + 4)  /*!< USB transfer stalled */

/** @brief Maximum string descriptor length returned by the MSC host driver. */
#define MSC_STR_DESC_SIZE 32

// For backward compatibility with IDF versions which do not have suspend/resume api
#ifdef USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND
/** @brief Indicates that suspend and resume events are available in this build. */
#define MSC_HOST_SUSPEND_RESUME_API_SUPPORTED
#endif

typedef struct msc_host_device *msc_host_device_handle_t;     /*!< Handle to a Mass Storage Device */

/**
 * @brief USB Mass Storage event containing event type and associated device handle.
 */
typedef struct {
    enum {
        MSC_DEVICE_CONNECTED,       /*!< MSC device has been connected to the system. */
        MSC_DEVICE_DISCONNECTED,    /*!< MSC device has been disconnected from the system. */
#ifdef MSC_HOST_SUSPEND_RESUME_API_SUPPORTED
        MSC_DEVICE_SUSPENDED,       /*!< MSC device has been suspended. */
        MSC_DEVICE_RESUMED,         /*!< MSC device has been resumed. */
#endif // MSC_HOST_SUSPEND_RESUME_API_SUPPORTED
    } event;                     /*!< Event type. */
    union {
        uint8_t address;                 /*!< Address of the connected MSC device. */
        msc_host_device_handle_t handle; /*!< Handle of the disconnected, suspended, or resumed device. */
    } device;                    /*!< Event-specific device information. */
} msc_host_event_t;

/**
 * @brief USB Mass Storage event callback.
 *
 * @param[in] event Mass storage event.
 * @param[in] arg User argument from the MSC driver configuration structure.
 */
typedef void (*msc_host_event_cb_t)(const msc_host_event_t *event, void *arg);

/**
 * @brief MSC driver configuration structure.
 */
typedef struct {
    bool create_backround_task;     /*!< When set to true, a background task handles USB events.
                                         Otherwise the application must periodically call msc_host_handle_events(). */
    size_t task_priority;           /*!< Task priority of the created background task. */
    size_t stack_size;              /*!< Stack size of the created background task. */
    BaseType_t core_id;             /*!< Core affinity of the created background task, or tskNO_AFFINITY. */
    msc_host_event_cb_t callback;   /*!< Callback invoked when MSC event occurs. Must not be NULL. */
    void *callback_arg;             /*!< User-provided argument passed to callback. */
} msc_host_driver_config_t;

/**
 * @brief MSC device info.
 */
typedef struct {
    uint32_t sector_count;                     /*!< Number of addressable sectors on the device. */
    uint32_t sector_size;                      /*!< Sector size in bytes. */
    uint16_t idProduct;                        /*!< USB product ID. */
    uint16_t idVendor;                         /*!< USB vendor ID. */
    wchar_t iManufacturer[MSC_STR_DESC_SIZE];  /*!< Manufacturer string. */
    wchar_t iProduct[MSC_STR_DESC_SIZE];       /*!< Product string. */
    wchar_t iSerialNumber[MSC_STR_DESC_SIZE];  /*!< Serial number string. */
} msc_host_device_info_t;

/**
 * @brief Install the USB Host Mass Storage Class driver.
 *
 * @param[in] config MSC driver configuration.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if config is NULL or invalid
 *      - ESP_ERR_INVALID_STATE if the driver is already installed
 *      - ESP_ERR_NO_MEM if memory allocation fails
 *      - Other error codes from the USB Host library
 */
esp_err_t msc_host_install(const msc_host_driver_config_t *config);

/**
 * @brief Uninstall the Mass Storage Class driver.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the driver is not installed or a device is still open
 */
esp_err_t msc_host_uninstall(void);

/**
 * @brief Initialize an MSC device after connection.
 *
 * @param[in] device_address Device address obtained from the MSC connection callback.
 * @param[out] device Mass storage device handle to use for subsequent API calls. Must not be NULL.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the driver is not installed
 *      - ESP_ERR_NO_MEM if memory allocation fails
 *      - Other error codes from the USB Host library or MSC transport layer
 */
esp_err_t msc_host_install_device(uint8_t device_address, msc_host_device_handle_t *device);

/**
 * @brief Deinitialize an MSC device.
 *
 * @param[in] device Device handle obtained from msc_host_install_device().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if device is NULL
 *      - Other error codes from the USB Host library or MSC transport layer
 */
esp_err_t msc_host_uninstall_device(msc_host_device_handle_t device);

/**
 * @brief Helper function for reading sector from mass storage device.
 *
 * @warning This call is not thread safe and should not be combined
 *          with accesses to storage through the file system.
 *
 * @note Provided sector and size cannot exceed the limits reported by
 *       msc_host_device_info_t.
 *
 * @param[in] device Device handle.
 * @param[in] sector Sector number to read.
 * @param[out] data Buffer that receives the sector data.
 * @param[in] size Number of bytes to read.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if device is NULL
 *      - Other error codes from the MSC transport layer
 */
esp_err_t msc_host_read_sector(msc_host_device_handle_t device, size_t sector, void *data, size_t size)
__attribute__((deprecated("use API from esp_private/msc_scsi_bot.h")));

/**
 * @brief Helper function for writing sector to mass storage device.
 *
 * @warning This call is not thread safe and should not be combined
 *          with accesses to storage through the file system.
 *
 * @note Provided sector and size cannot exceed the limits reported by
 *       msc_host_device_info_t.
 *
 * @param[in] device Device handle.
 * @param[in] sector Sector number to write.
 * @param[in] data Data to write to the sector.
 * @param[in] size Number of bytes to write.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if device is NULL
 *      - Other error codes from the MSC transport layer
 */
esp_err_t msc_host_write_sector(msc_host_device_handle_t device, size_t sector, const void *data, size_t size)
__attribute__((deprecated("use API from esp_private/msc_scsi_bot.h")));

/**
 * @brief Handle USB Host events for the MSC driver.
 *
 * If msc_host_install() is called with `create_backround_task = false`, the
 * application must call this function periodically to dispatch USB Host events.
 * Do not call this function when `create_backround_task = true`.
 *
 * @param[in] timeout Timeout in FreeRTOS ticks.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the driver is not installed
 *      - ESP_FAIL if event handling finished because the driver is being uninstalled
 *      - Other error codes returned by usb_host_client_handle_events()
 */
esp_err_t msc_host_handle_events(TickType_t timeout);

/**
 * @brief Get MSC device information.
 *
 * @param[in] device Device handle.
 * @param[out] info Structure to populate with device information.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if device or info is NULL
 *      - Other error codes from the USB Host library
 */
esp_err_t msc_host_get_device_info(msc_host_device_handle_t device, msc_host_device_info_t *info);

/**
 * @brief Print USB descriptors for an MSC device.
 *
 * @param[in] device Handle of the MSC device.
 *
 * @return
 *      - ESP_OK on success
 *      - Other error codes from the USB Host library
 */
esp_err_t msc_host_print_descriptors(msc_host_device_handle_t device);

/**
 * @brief Perform MSC Bulk-Only Transport reset recovery.
 *
 * @see USB Mass Storage Class – Bulk Only Transport, Chapter 5.3.4
 *
 * @param[in] device Handle of the MSC device.
 *
 * @return
 *      - ESP_OK on success
 *      - Other error codes from the MSC transport layer
 */
esp_err_t msc_host_reset_recovery(msc_host_device_handle_t device);

#ifdef __cplusplus
}
#endif //__cplusplus
