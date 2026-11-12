/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "usb/msc_host.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct {
    uint8_t key;
    uint8_t code;
    uint8_t code_q;
} scsi_sense_data_t;

esp_err_t scsi_cmd_read10(msc_host_device_handle_t device,
                          uint8_t *data,
                          uint32_t sector_address,
                          uint32_t num_sectors,
                          uint32_t sector_size);

esp_err_t scsi_cmd_write10(msc_host_device_handle_t device,
                           const uint8_t *data,
                           uint32_t sector_address,
                           uint32_t num_sectors,
                           uint32_t sector_size);

esp_err_t scsi_cmd_read_capacity(msc_host_device_handle_t device,
                                 uint32_t *block_size,
                                 uint32_t *block_count);

esp_err_t scsi_cmd_sense(msc_host_device_handle_t device, scsi_sense_data_t *sense);

esp_err_t scsi_cmd_unit_ready(msc_host_device_handle_t device);

esp_err_t scsi_cmd_inquiry(msc_host_device_handle_t device);

esp_err_t scsi_cmd_prevent_removal(msc_host_device_handle_t device, bool prevent);

esp_err_t scsi_cmd_mode_sense(msc_host_device_handle_t device);

#ifdef __cplusplus
}
#endif
