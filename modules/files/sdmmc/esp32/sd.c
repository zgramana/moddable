/*
 * Copyright (c) 2016-2017  Moddable Tech, Inc.
 *
 *   This file is part of the Moddable SDK Runtime.
 *
 *   The Moddable SDK Runtime is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   The Moddable SDK Runtime is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with the Moddable SDK Runtime.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>


//#undef c_memcpy
//#undef c_printf
//#undef c_memset

#include "xsmc.h"
#include "xsHost.h"
#include "modInstrumentation.h"
#include "mc.defines.h"
#include "mc.xs.h"			// for xsID_ values

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "vfs_fat_internal.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "diskio.h"
#include "sdmmc/include/sdmmc_cmd.h"
// #include "fatfs/src/esp_vfs_fat.h"


//#include "esp_vfs_fat.h"
//#include "driver/sdspi_host.h"
//#include "sdmmc/sdmmc_common.h"

//#define USE_SPI_MODE

#define SD_OCR_SDHC_CAP                 (1<<30)

#ifndef MODDEF_SDCARD_MISO_PIN
	#define MODDEF_SDCARD_MISO_PIN 12
#endif
#ifndef MODDEF_SDCARD_MOSI_PIN
	#define MODDEF_SDCARD_MOSI_PIN 13
#endif
#ifndef MODDEF_SDCARD_SCLK_PIN
	#define MODDEF_SDCARD_SCLK_PIN 14
#endif
#ifndef MODDEF_SDCARD_CS_PIN
	#define MODDEF_SDCARD_CS_PIN 32
#endif
#ifndef MODDEF_SDCARD_SPI_PORT
	#define MODDEF_SDCARD_SPI_PORT VSPI_HOST
#endif
#ifndef MODDEF_SDCARD_HZ
	#define MODDEF_SDCARD_HZ SDMMC_FREQ_DEFAULT * 1000 // i.e. 20000000
#endif

static void *xsmcGetHostDataNullCheck(xsMachine *the)
{
	void *result = xsmcGetHostData(xsThis);
	if (result)
		return result;
	xsUnknownError("closed");
}

static uint16_t gUseCount;

void print_card_info(const sdmmc_card_t* card)
{
    bool print_scr = false;
    bool print_csd = false;
    const char* type;
	char stream[1024];
    sprintf(stream, "Name: %s\n", card->cid.name); modLog_transmit(stream);
    if (card->is_sdio) {
        type = "SDIO";
        print_scr = true;
        print_csd = true;
    } else if (card->is_mmc) {
        type = "MMC";
        print_csd = true;
    } else {
        type = (card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC";
    }
    sprintf(stream, "Type: %s\n", type); modLog_transmit(stream);
    if (card->max_freq_khz < 1000) {
        sprintf(stream, "Speed: %d kHz\n", card->max_freq_khz);
    } else {
        sprintf(stream, "Speed: %d MHz%s\n", card->max_freq_khz / 1000,
                card->is_ddr ? ", DDR" : "");
    }
    sprintf(stream, "Size: %lluMB\n", ((uint64_t) card->csd.capacity) * card->csd.sector_size / (1024 * 1024)); modLog_transmit(stream);

    if (print_csd) {
        sprintf(stream, "CSD: ver=%d, sector_size=%d, capacity=%d read_bl_len=%d\n",
                card->csd.csd_ver,
                card->csd.sector_size, card->csd.capacity, card->csd.read_block_len); modLog_transmit(stream);
    }
    if (print_scr) {
        sprintf(stream, "SCR: sd_spec=%d, bus_width=%d\n", card->scr.sd_spec, card->scr.bus_width); modLog_transmit(stream);
    }
}

void xs_sdmmc(xsMachine *the)
{
	char logB[100];

	if (0 == gUseCount++) {
		modLog("Initializing sdmmc...");

		sdmmc_host_t host = SDSPI_HOST_DEFAULT();
		host.slot = MODDEF_SDCARD_SPI_PORT;
		host.max_freq_khz = MODDEF_SDCARD_HZ / 1000;//SDMMC_FREQ_DEFAULT / 4; //
		sprintf(logB, "host configured (slot: %i, freq: %d", host.slot, host.max_freq_khz); modLog_transmit(logB);

		sdspi_slot_config_t slot_config = SDSPI_SLOT_CONFIG_DEFAULT();
		slot_config.gpio_cs   = MODDEF_SDCARD_CS_PIN;
		slot_config.gpio_mosi = MODDEF_SDCARD_MOSI_PIN;
		slot_config.gpio_miso = MODDEF_SDCARD_MISO_PIN;
		slot_config.gpio_sck  = MODDEF_SDCARD_SCLK_PIN;
		slot_config.dma_channel = 2;
		sprintf(logB, "slot configured (cs: %i, mosi: %i, miso: %i, sck: %i)", slot_config.gpio_cs, slot_config.gpio_mosi, slot_config.gpio_miso, slot_config.gpio_sck); modLog_transmit(logB);

		//esp_vfs_fat_sdmmc_mount_config_t mount_config;
		//memset(&mount_config, 0, sizeof(mount_config));
		esp_vfs_fat_sdmmc_mount_config_t mount_config = {
			mount_config.format_if_mount_failed = true,
			mount_config.max_files = 5,
			mount_config.allocation_unit_size = 16 * 1024
		};
		modLog("mount configured");

		sdmmc_card_t *card;

		modLog("preparing to mount");
		esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);

		if (ret != ESP_OK) {
			if (ret == ESP_FAIL) {
				modLog("Failed to mount or format filesystem");
			} else if (ret == ESP_ERR_NO_MEM) {
				modLog("Failed to allocate memory for filesystem");
			} else if (ret == ESP_ERR_NOT_FOUND) {
				modLog("Failed to find SD card");
			} else if (ret == ESP_ERR_INVALID_STATE) {
				modLog("Mount was already called, skipping.");
			} else {
				xsUnknownError("Failed to initialize SD card (%s)", esp_err_to_name(ret));
			}
		} else {
			modLog("Printing card info:");
			print_card_info(card);
		}
	}

	modLog("preparing to fopen");

    int argc = xsmcArgc;
    FILE *file;
    uint8_t write = (argc < 2) ? 0 : xsmcToBoolean(xsArg(1));

    //startVFS(); // After the POC code works, break up the all-in-one call similar to SPIFFS modFile.c
	char *path = xsmcToString(xsArg(0));
    file = fopen(path, write ? "rb+" : "rb"); // e.g. "/sdcard/foo.txt"
    if (NULL == file) {
        if (write)
            file = fopen(path, "wb+");
        if (NULL == file) {
			//stopVFS(); // After the POC code works, break up the all-in-one call similar to SPIFFS modFile.c
			xsUnknownError("file not found (%s)", path);
		}
    }
    xsmcSetHostData(xsThis, (void *)((uintptr_t)file));

	modInstrumentationAdjust(Files, +1);

	sprintf(logB, "File opened at path '%s'", path); modLog_transmit(logB);
}

void xs_sdmmc_destructor(void *data)
{
	if (data) {
		if (-1 != (uintptr_t)data) {
			fclose((FILE *)data);
			if (--gUseCount == 0) {
				modLog("Unmounting sdcard");
				esp_vfs_fat_sdmmc_unmount();
				//stopVFS();
			}
		}

		modInstrumentationAdjust(Files, -1);
	}
}

void xs_sdmmc_read(xsMachine *the)
{
    void *data = xsmcGetHostDataNullCheck(the);
    FILE *file = (FILE*)data;
    int32_t result;
    int argc = xsmcArgc;
    int dstLen = (argc < 2) ? -1 : xsmcToInteger(xsArg(1));
    void *dst;
    xsSlot *s1, *s2;
    struct stat buf;
    int fno;
    int32_t position = ftell(file);

    fno = fileno(file);
    fstat(fno, &buf);
    if ((-1 == dstLen) || (buf.st_size < (position + dstLen))) {
        if (position >= buf.st_size)
            xsUnknownError("read past end of file");
        dstLen = buf.st_size - position;
    }

    s1 = &xsArg(0);

    xsmcVars(1);
    xsmcGet(xsVar(0), xsGlobal, xsID_String);
    s2 = &xsVar(0);
    if (s1->data[2] == s2->data[2]) {
        xsResult = xsStringBuffer(NULL, dstLen);
        dst = xsmcToString(xsResult);
    }
    else {
        xsResult = xsArrayBuffer(NULL, dstLen);
        dst = xsmcToArrayBuffer(xsResult);
    }

    result = fread(dst, 1, dstLen, file);
    if (result != dstLen)
        xsUnknownError("file read failed");
}

void xs_sdmmc_write(xsMachine *the)
{
    void *data = xsmcGetHostDataNullCheck(the);
    FILE *file = ((FILE *)data);
    int32_t result;
    int argc = xsmcArgc, i;

    for (i = 0; i < argc; i++) {
        uint8_t *src;
        int32_t srcLen;
		int type = xsmcTypeOf(xsArg(i));
		uint8_t temp;

		if (xsStringType == type) {
			src = xsmcToString(xsArg(i));
			srcLen = strlen(src);
		}
		else if ((xsIntegerType == type) || (xsNumberType == type)) {
			temp = (uint8_t)xsmcToInteger(xsArg(i));
			src = &temp;
			srcLen = 1;
		}
        else {
            src = xsmcToArrayBuffer(xsArg(i));
            srcLen = xsGetArrayBufferLength(xsArg(i));
        }

		result = fwrite(src, 1, srcLen, file);
		if (result != srcLen)
			xsUnknownError("file write failed");
    }
	result = fflush(file);
	if (0 != result)
		xsUnknownError("file flush failed");
}

void xs_sdmmc_close(xsMachine *the)
{
    void *data = xsmcGetHostDataNullCheck(the);
    FILE *file = ((FILE*)data);
    xs_sdmmc_destructor((void *)((int)file));
    xsmcSetHostData(xsThis, (void *)NULL);
}

void xs_sdmmc_get_length(xsMachine *the)
{
    void *data = xsmcGetHostDataNullCheck(the);
    FILE *file = (FILE*)data;
    struct stat buf;
    int fno;

    fno = fileno(file);
    fstat(fno, &buf);
    xsResult = xsInteger(buf.st_size);
}

void xs_sdmmc_get_position(xsMachine *the)
{
    void *data = xsmcGetHostDataNullCheck(the);
    FILE *file = (FILE*)data;
    int32_t position = ftell(file);
    xsResult = xsInteger(position);
}

void xs_sdmmc_set_position(xsMachine *the)
{
    void *data = xsmcGetHostDataNullCheck(the);
    FILE *file = ((FILE*)data);
    int32_t position = xsmcToInteger(xsArg(0));
    fseek(file, position, SEEK_SET);
}

static const char* TAG = "sdmmc";
static sdmmc_card_t* s_card = NULL;
static uint8_t s_pdrv = 0;
static char * s_base_path = NULL;

esp_err_t esp_vfs_fat_sdmmc_mount(const char* base_path,
    const sdmmc_host_t* host_config,
    const void* slot_config,
    const esp_vfs_fat_mount_config_t* mount_config,
    sdmmc_card_t** out_card)
{
	char logB[100];
    modLog("starting esp_vfs_fat_sdmmc_mount");
    const size_t workbuf_size = 4096;
    void* workbuf = NULL;
    FATFS* fs = NULL;

    if (s_card != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // connect SDMMC driver to FATFS
    BYTE pdrv = 0xFF;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == 0xFF) {
		modLog("the maximum count of volumes is already mounted");
        // ESP_LOGD(TAG, "the maximum count of volumes is already mounted");
        return ESP_ERR_NO_MEM;
    }
	modLog("passed ff_diskio_get_drive");
    s_base_path = strdup(base_path);
    if(!s_base_path){
        ESP_LOGD(TAG, "could not copy base_path");
        return ESP_ERR_NO_MEM;
    }
	modLog("passed copy base_path");
    esp_err_t err = ESP_OK;
    s_card = malloc(sizeof(sdmmc_card_t));
    if (s_card == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    err = (*host_config->init)();
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "host init returned rc=0x%x", err);
        goto fail;
    }
	modLog("passed host init");

    // configure SD slot
    if (host_config->flags == SDMMC_HOST_FLAG_SPI) {
        err = sdspi_host_init_slot(host_config->slot,
                (const sdspi_slot_config_t*) slot_config);
		sprintf(logB, "Using sdspi_host_init_slot (%d)", host_config->slot); modLog_transmit(logB);
    } else {
        err = sdmmc_host_init_slot(host_config->slot,
                (const sdmmc_slot_config_t*) slot_config);
		modLog("Using sdmmc_host_init_slot");
    }
    if (err != ESP_OK) {
		sprintf(logB, "slot_config returned err (%s)", esp_err_to_name(err)); modLog_transmit(logB);
        goto fail;
    }
	modLog("passed slot_config check");

    // probe and initialize card
    err = sdmmc_card_init(host_config, s_card);
    if (err != ESP_OK) {
	    sprintf(logB, "sdmmc_card_init failed 0x(%x)", err); modLog_transmit(logB);
        goto fail;
    }
	modLog("passed sdmmc_card_init");
    if (out_card != NULL) {
        *out_card = s_card;
    }

    ff_diskio_register_sdmmc(pdrv, s_card);
    s_pdrv = pdrv;
    sprintf(logB, "using pdrv=%i", pdrv); modLog_transmit(logB);
    char drv[3] = {(char)('0' + pdrv), ':', 0};
	modLog("passed register_sdmmc");

    // connect FATFS to VFS
    err = esp_vfs_fat_register(base_path, drv, mount_config->max_files, &fs);
    if (err == ESP_ERR_INVALID_STATE) {
        modLog("it's okay, already registered with VFS");
    } else if (err != ESP_OK) {
        sprintf(logB, "esp_vfs_fat_register failed 0x(%x)", err); modLog_transmit(logB);
        goto fail;
    }
	modLog("passed fat_register");

    // Try to mount partition
    FRESULT res = f_mount(fs, drv, 1);
    if (res != FR_OK) {
        err = ESP_FAIL;
        sprintf(logB, "failed to mount card (err: %d, fs: %p, drv: {%i,%i,%i})", res, fs, drv[0], drv[1], drv[2]); modLog_transmit(logB);
        if (!(res == FR_NO_FILESYSTEM && mount_config->format_if_mount_failed)) {
            goto fail;
        }
        modLog("partitioning card");
        workbuf = malloc(workbuf_size);
        if (workbuf == NULL) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        DWORD plist[] = {100, 0, 0, 0};
        res = f_fdisk(s_pdrv, plist, workbuf);
        if (res != FR_OK) {
            err = ESP_FAIL;
            sprintf(logB, "f_fdisk failed (%d)", res); modLog_transmit(logB);
            goto fail;
        }
        size_t alloc_unit_size = esp_vfs_fat_get_allocation_unit_size(
                s_card->csd.sector_size,
                mount_config->allocation_unit_size);
        sprintf(logB, "formatting card, allocation unit size=%d", alloc_unit_size); modLog_transmit(logB);
        res = f_mkfs(drv, FM_ANY, alloc_unit_size, workbuf, workbuf_size);
        if (res != FR_OK) {
            err = ESP_FAIL;
            sprintf(logB, "f_mkfs failed (%d)", res); modLog_transmit(logB);
            goto fail;
        }
        free(workbuf);
        workbuf = NULL;
        modLog("mounting again");
        res = f_mount(fs, drv, 0);
        if (res != FR_OK) {
            err = ESP_FAIL;
            sprintf(logB, "f_mount failed after formatting (%d)", res); modLog_transmit(logB);
            goto fail;
        }
    }
	modLog("passed f_mount");
    return ESP_OK;

fail:
    host_config->deinit();
    free(workbuf);
    if (fs) {
        f_mount(NULL, drv, 0);
    }
    esp_vfs_fat_unregister_path(base_path);
    ff_diskio_unregister(pdrv);
    free(s_card);
    s_card = NULL;
    return err;
}

esp_err_t esp_vfs_fat_sdmmc_unmount()
{
    if (s_card == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // unmount
    char drv[3] = {(char)('0' + s_pdrv), ':', 0};
    f_mount(0, drv, 0);
    // release SD driver
    esp_err_t (*host_deinit)() = s_card->host.deinit;
    ff_diskio_unregister(s_pdrv);
    free(s_card);
    s_card = NULL;
    (*host_deinit)();
    esp_err_t err = esp_vfs_fat_unregister_path(s_base_path);
    free(s_base_path);
    s_base_path = NULL;
    return err;
}

void xs_sdmmc_blockSize(xsMachine *the)
{
	WORD bSec;
	DRESULT dRes = ff_sdmmc_ioctl(s_pdrv, GET_SECTOR_SIZE, &bSec);
	if (dRes == RES_OK) {
		xsmcSetInteger(xsResult, bSec);
	} else {
		xsmcSetInteger(xsResult, -1);
	}
}
