/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "inttypes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "esp_check.h"
#include "esp_log.h"
#include "msc_common.h"
#include "msc_scsi_bot.h"
#include "usb/msc_host.h"

static const char *TAG = "USB_MSC_SCSI";

/* --------------------------- SCSI Definitions ----------------------------- */
#define CMD_SENSE_VALID_BIT (1 << 7)
#define SCSI_FLAG_DPO (1<<4)
#define SCSI_FLAG_FUA (1<<3)

#define SCSI_CMD_FORMAT_UNIT 0x04
#define SCSI_CMD_INQUIRY 0x12
#define SCSI_CMD_MODE_SELECT 0x55
#define SCSI_CMD_MODE_SENSE 0x5A
#define SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL 0x1E
#define SCSI_CMD_READ10 0x28
#define SCSI_CMD_READ12 0xA8
#define SCSI_CMD_READ_CAPACITY 0x25
#define SCSI_CMD_READ_FORMAT_CAPACITIES 0x23
#define SCSI_CMD_REQUEST_SENSE 0x03
#define SCSI_CMD_REZERO 0x01
#define SCSI_CMD_SEEK10 0x2B
#define SCSI_CMD_SEND_DIAGNOSTIC 0x1D
#define SCSI_CMD_START_STOP Unit 0x1B
#define SCSI_CMD_TEST_UNIT_READY 0x00
#define SCSI_CMD_VERIFY 0x2F
#define SCSI_CMD_WRITE10 0x2A
#define SCSI_CMD_WRITE12 0xAA
#define SCSI_CMD_WRITE_AND_VERIFY 0x2E

#define IN_DIR   CWB_FLAG_DIRECTION_IN
#define OUT_DIR  0

#define INQUIRY_VID_SIZE    8
#define INQUIRY_PID_SIZE    16
#define INQUIRY_REV_SIZE    4

#define CBW_CMD_SIZE(cmd) (sizeof(cmd) - sizeof(msc_cbw_t))

#define CBW_BASE_INIT(dir, cbw_len, data_len)   \
    .base = {                                   \
        .signature = 0x43425355,                \
        .tag = ++cbw_tag,                       \
        .flags = dir,                           \
        .lun = 0,                               \
        .data_length = data_len,                \
        .cbw_length = cbw_len,                  \
    }

#define CSW_SIGNATURE   0x53425355
#define CBW_SIZE        31

#define CWB_FLAG_DIRECTION_IN (1<<7) // device -> host

/**
 * @brief LUT with error codes and descriptions
 *
 * @see USB Mass Storage Class – UFI Command Specification, Revision 1.0
 * Table 51 - Sense Keys, ASC/ASCQ Listing for All Commands (sorted by Key)
 *
 */
typedef struct {
    uint8_t sense_key;
    uint8_t asc;
    uint8_t ascq;
    const char *description;
} sense_errors_t;

const sense_errors_t sense_errors_lut[] = {
    {0x00, 0x00, 0x00, "NO SENSE"},
    {0x07, 0x27, 0x00, "WRITE PROTECTED MEDIA"},

    // add more items as needed
};

#define SENSE_ERROR_COUNT (sizeof(sense_errors_lut) / sizeof(sense_errors_t))

/**
 * @brief Command Block Wrapper structure
 *
 * @see USB Mass Storage Class – Bulk Only Transport, Table 5.1
 */
typedef struct __attribute__((packed))
{
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cbw_length;
} msc_cbw_t;

/**
 * @brief Command Status Wrapper structure
 *
 * @see USB Mass Storage Class – Bulk Only Transport, Table 5.2
 */
typedef struct __attribute__((packed))
{
    uint32_t signature;
    uint32_t tag;
    uint32_t dataResidue;
    uint8_t status;
} msc_csw_t;

typedef struct __attribute__((packed))
{
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint32_t address;
    uint8_t reserved1;
    uint16_t length;
    uint8_t reserved2[3];
} cbw_read10_t;

typedef struct __attribute__((packed))
{
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint32_t address;
    uint8_t reserved1;
    uint16_t length;
    uint8_t reserved2[1];
} cbw_write10_t;

typedef struct __attribute__((packed))
{
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint32_t address;
    uint8_t reserved[6];
} cbw_read_capacity_t;

typedef struct __attribute__((packed))
{
    uint32_t block_count;
    uint32_t block_size;
} cbw_read_capacity_response_t;

typedef struct __attribute__((packed))
{
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint8_t reserved[10];
} cbw_unit_ready_t;

typedef struct __attribute__((packed))
{
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint8_t reserved_0[2];
    uint8_t allocation_length;
    uint8_t reserved_1[7];
} cbw_sense_t;

typedef struct __attribute__((packed))
{
    uint8_t error_code;
    uint8_t reserved_0;
    uint8_t sense_key;
    uint32_t info;
    uint8_t sense_len;
    uint32_t reserved_1;
    uint8_t sense_code;
    uint8_t sense_code_qualifier;
    uint32_t reserved_2;
} cbw_sense_response_t;

typedef struct __attribute__((packed))
{
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint8_t page_code;
    uint8_t reserved_0;
    uint8_t allocation_length;
    uint8_t reserved_1[7];
} cbw_inquiry_t;

typedef struct __attribute__((packed))
{
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint8_t pc_page_code;
    uint8_t reserved_1[4];
    uint16_t parameter_list_length;
    uint8_t reserved_2[3];
} mode_sense_t;

typedef struct __attribute__((packed))
{
    uint8_t data[8];
} mode_sense_response_t;

typedef struct __attribute__((packed))
{
    msc_cbw_t base;
    uint8_t opcode;
    uint8_t flags;
    uint8_t reserved_1[2];
    uint8_t prevent;
    uint8_t reserved_2[7];
} prevent_allow_medium_removal_t;

typedef struct __attribute__((packed))
{
    uint8_t data[36];
} cbw_inquiry_response_t;

// Unique number based on which MSC protocol pairs request and response
static uint32_t cbw_tag;

static esp_err_t check_csw(msc_csw_t *csw, uint32_t tag)
{
    const bool csw_ok = csw->signature == CSW_SIGNATURE && csw->tag == tag &&
                        csw->dataResidue == 0 && csw->status == 0;

    if (!csw_ok) {
        ESP_LOGV(TAG, "CSW failed: dCSWSignature = 0x%02"PRIx32", dCSWTag = 0x%02"PRIx32", dCSWDataResidue = 0x%02"PRIx32"",
                 csw->signature, csw->tag, csw->dataResidue);
        ESP_LOGD(TAG, "CSW failed: bCSWStatus 0x%02"PRIx8"", csw->status);
    }

    return csw_ok ? ESP_OK : ESP_FAIL;
}

/**
 * @brief Execute BOT command
 *
 * There are multiple stages in BOT command:
 * 1. Command transport
 * 2. Data transport (optional)
 * 3. Status transport
 * 3.1. Error recovery (in case of error)
 *
 * This function is not 'static' so it could be called from unit test
 *
 * @see USB Mass Storage Class – Bulk Only Transport, Chapter 5.3
 *
 * @param[in] device MSC device handle
 * @param[in] cbw    Command Block Wrapper
 * @param[in] data   Data (optional)
 * @param[in] size   Size of data in bytes
 * @return esp_err_t
 */
esp_err_t bot_execute_command(msc_device_t *device, msc_cbw_t *cbw, void *data, size_t size)
{
    msc_csw_t csw;
    msc_endpoint_t ep = (cbw->flags & CWB_FLAG_DIRECTION_IN) ? MSC_EP_IN : MSC_EP_OUT;

    // 1. Command transport
    MSC_RETURN_ON_ERROR( msc_bulk_transfer(device, (uint8_t *)cbw, CBW_SIZE, MSC_EP_OUT) );

    // 2. Optional data transport
    if (data) {
        MSC_RETURN_ON_ERROR( msc_bulk_transfer(device, (uint8_t *)data, size, ep) );
    }

    // 3. Status transport
    esp_err_t err = msc_bulk_transfer(device, (uint8_t *)&csw, sizeof(msc_csw_t), MSC_EP_IN);

    // 3.1 Error recovery
    if (err == ESP_ERR_MSC_STALL) {
        // In case of the status transport failure, we can try reading the status again after clearing feature
        ESP_RETURN_ON_ERROR( clear_feature(device, device->config.bulk_in_ep), TAG, "Clear feature failed" );
        err = msc_bulk_transfer(device, (uint8_t *)&csw, sizeof(msc_csw_t), MSC_EP_IN);
        if (ESP_OK != err) {
            // In case the repeated status transport failed we do reset recovery
            // We don't check the error code here, the command has already failed.
            msc_host_reset_recovery(device);
        }
    }

    MSC_RETURN_ON_ERROR(err);

    return check_csw(&csw, cbw->tag);
}

static const char *decode_sense_keys(cbw_sense_response_t *sense_response)
{
    // Only decode WRITE_PROTECTED_MEDIA sense key, other keys are not implemented
    for (int i = 0; i < SENSE_ERROR_COUNT; i++) {
        if (sense_errors_lut[i].sense_key == sense_response->sense_key &&
                sense_errors_lut[i].asc == sense_response->sense_code &&
                sense_errors_lut[i].ascq == sense_response->sense_code_qualifier) {
            return sense_errors_lut[i].description;
        }
    }

    return "not found, refer to USB Mass Storage Class – UFI Command Specification (Table 51)";
}


esp_err_t scsi_cmd_read10(msc_host_device_handle_t dev,
                          uint8_t *data,
                          uint32_t sector_address,
                          uint32_t num_sectors,
                          uint32_t sector_size)
{
    if (num_sectors != 0 && sector_size > UINT32_MAX / num_sectors) {
        return ESP_ERR_INVALID_SIZE;
    }

    msc_device_t *device = (msc_device_t *)dev;
    cbw_read10_t cbw = {
        CBW_BASE_INIT(IN_DIR, CBW_CMD_SIZE(cbw_read10_t), num_sectors * sector_size),
        .opcode = SCSI_CMD_READ10,
        .flags = 0, // lun
        .address = __builtin_bswap32(sector_address),
        .length = __builtin_bswap16(num_sectors),
    };

    esp_err_t ret = bot_execute_command(device, &cbw.base, data, num_sectors * sector_size);

    // In case of an error, get an error code
    if (unlikely(ret != ESP_OK)) {
        MSC_RETURN_ON_ERROR( scsi_cmd_sense(device, NULL));
    }
    return ret;
}

esp_err_t scsi_cmd_write10(msc_host_device_handle_t dev,
                           const uint8_t *data,
                           uint32_t sector_address,
                           uint32_t num_sectors,
                           uint32_t sector_size)
{
    if (num_sectors != 0 && sector_size > UINT32_MAX / num_sectors) {
        return ESP_ERR_INVALID_SIZE;
    }

    msc_device_t *device = (msc_device_t *)dev;
    cbw_write10_t cbw = {
        CBW_BASE_INIT(OUT_DIR, CBW_CMD_SIZE(cbw_write10_t), num_sectors * sector_size),
        .opcode = SCSI_CMD_WRITE10,
        .address = __builtin_bswap32(sector_address),
        .length = __builtin_bswap16(num_sectors),
    };

    esp_err_t ret = bot_execute_command(device, &cbw.base, (void *)data, num_sectors * sector_size);

    // In case of an error, get an error code
    if (unlikely(ret != ESP_OK)) {
        MSC_RETURN_ON_ERROR( scsi_cmd_sense(device, NULL));
    }
    return ret;
}

esp_err_t scsi_cmd_read_capacity(msc_host_device_handle_t dev, uint32_t *block_size, uint32_t *block_count)
{
    msc_device_t *device = (msc_device_t *)dev;
    cbw_read_capacity_response_t response;

    cbw_read_capacity_t cbw = {
        CBW_BASE_INIT(IN_DIR, CBW_CMD_SIZE(cbw_read_capacity_t), sizeof(response)),
        .opcode = SCSI_CMD_READ_CAPACITY,
    };

    esp_err_t ret = bot_execute_command(device, &cbw.base, &response, sizeof(response));

    // In case of an error, get an error code
    if (unlikely(ret != ESP_OK)) {
        MSC_RETURN_ON_ERROR( scsi_cmd_sense(device, NULL));
    }

    *block_count = __builtin_bswap32(response.block_count);
    *block_size = __builtin_bswap32(response.block_size);

    return ret;
}

esp_err_t scsi_cmd_unit_ready(msc_host_device_handle_t dev)
{
    msc_device_t *device = (msc_device_t *)dev;
    cbw_unit_ready_t cbw = {
        CBW_BASE_INIT(IN_DIR, CBW_CMD_SIZE(cbw_unit_ready_t), 0),
        .opcode = SCSI_CMD_TEST_UNIT_READY,
    };

    esp_err_t ret = bot_execute_command(device, &cbw.base, NULL, 0);

    // In case of an error, get an error code
    if (unlikely(ret != ESP_OK)) {
        MSC_RETURN_ON_ERROR( scsi_cmd_sense(device, NULL));
    }
    return ret;
}

esp_err_t scsi_cmd_sense(msc_host_device_handle_t dev, scsi_sense_data_t *sense)
{
    msc_device_t *device = (msc_device_t *)dev;
    cbw_sense_response_t response;

    cbw_sense_t cbw = {
        CBW_BASE_INIT(IN_DIR, CBW_CMD_SIZE(cbw_sense_t), sizeof(response)),
        .opcode = SCSI_CMD_REQUEST_SENSE,
        .allocation_length = sizeof(response),
    };

    MSC_RETURN_ON_ERROR( bot_execute_command(device, &cbw.base, &response, sizeof(response)) );

    if (sense == NULL) {
        ESP_LOGE(TAG, "Sense error codes: Sense Key 0x%02"PRIx8", ASC: 0x%02"PRIx8", ASCQ: 0x%02"PRIx8"",
                 response.sense_key, response.sense_code, response.sense_code_qualifier);
        const char *error_description = decode_sense_keys(&response);
        ESP_LOGE(TAG, "Sense error description: %s", error_description);
        return ESP_OK;
    }

    sense->key = response.sense_key;
    sense->code = response.sense_code;
    sense->code_q = response.sense_code_qualifier;

    return ESP_OK;
}

esp_err_t scsi_cmd_inquiry(msc_host_device_handle_t dev)
{
    msc_device_t *device = (msc_device_t *)dev;
    cbw_inquiry_response_t response = { 0 };

    cbw_inquiry_t cbw = {
        CBW_BASE_INIT(IN_DIR, CBW_CMD_SIZE(cbw_inquiry_t), sizeof(response)),
        .opcode = SCSI_CMD_INQUIRY,
        .allocation_length = sizeof(response),
    };

    esp_err_t ret = bot_execute_command(device, &cbw.base, &response, sizeof(response) );

    // In case of an error, get an error code
    if (unlikely(ret != ESP_OK)) {
        MSC_RETURN_ON_ERROR( scsi_cmd_sense(device, NULL));
    }
    return ret;
}

esp_err_t scsi_cmd_mode_sense(msc_host_device_handle_t dev)
{
    msc_device_t *device = (msc_device_t *)dev;
    mode_sense_response_t response = { 0 };

    mode_sense_t cbw = {
        CBW_BASE_INIT(IN_DIR, CBW_CMD_SIZE(mode_sense_t), sizeof(response)),
        .opcode = SCSI_CMD_MODE_SENSE,
        .pc_page_code = 0x3F,
        .parameter_list_length = sizeof(response),
    };

    esp_err_t ret = bot_execute_command(device, &cbw.base, &response, sizeof(response) );

    // In case of an error, get an error code
    if (unlikely(ret != ESP_OK)) {
        MSC_RETURN_ON_ERROR( scsi_cmd_sense(device, NULL));
    }
    return ret;
}

esp_err_t scsi_cmd_prevent_removal(msc_host_device_handle_t dev, bool prevent)
{
    msc_device_t *device = (msc_device_t *)dev;
    prevent_allow_medium_removal_t cbw = {
        CBW_BASE_INIT(OUT_DIR, CBW_CMD_SIZE(prevent_allow_medium_removal_t), 0),
        .opcode = SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL,
        .prevent = (uint8_t) prevent,
    };

    esp_err_t ret = bot_execute_command(device, &cbw.base, NULL, 0);

    // In case of an error, get an error code
    if (unlikely(ret != ESP_OK)) {
        MSC_RETURN_ON_ERROR( scsi_cmd_sense(device, NULL));
    }
    return ret;
}
