/*
 * squeezelite-ota.h
 *
 *  Created on: 25 sept. 2019
 *      Author: sle11
 */

#pragma once
#include "esp_attr.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"

#if RECOVERY_APPLICATION
#define CODE_RAM_LOCATION
#define RECOVERY_IRAM_FUNCTION IRAM_ATTR
#else
#define RECOVERY_IRAM_FUNCTION
#define CODE_RAM_LOCATION
#endif



// ERASE BLOCK needs to be a multiple of sector size. If a different multiple is passed
// the OTA process will adjust. Here, we need to strike the balance between speed and
// stability.  The larger the blocks, the faster the erase will be, but the more likely
// the system will throw WDT while the flash chip is locked and the more likely
// the OTA process will derail
#define OTA_FLASH_ERASE_BLOCK (uint32_t)249856

// We're running the OTA without squeezelite in the background, so we can set a comfortable
// amount of stack to avoid overflows.
#define OTA_STACK_SIZE 10240

// To speed up processing, we set this priority to a number that is higher than normal
// tasks
#define OTA_TASK_PRIOTITY 6

esp_err_t start_ota(const char * bin_url);
const char * ota_get_status();
uint8_t ota_get_pct_complete();


