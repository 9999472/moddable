/*
 * Copyright (c) 2016-2019  Moddable Tech, Inc.
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

#include "esp_err.h"
#include "esp_spiffs.h"
#include "spiffs_config.h"

#undef c_memcpy
#undef c_printf
#undef c_memset

#include "xsmc.h"
#include "xsesp.h"
#include "modInstrumentation.h"
#include "mc.xs.h"			// for xsID_ values

typedef struct {
    DIR *dir;
    char path[1];
} iteratorRecord, *iter;

static int startSPIFFS(void);
static void stopSPIFFS(void);

void xs_file_destructor(void *data)
{
	if (data) {
		if (-1 != (uintptr_t)data) {
			fclose((FILE *)data);
			stopSPIFFS();
		}
		modInstrumentationAdjust(Files, -1);
	}
}

void xs_File(xsMachine *the)
{
    int argc = xsmcArgc;
    FILE *file;
	char path[SPIFFS_OBJ_NAME_LEN];
    uint8_t write = (argc < 2) ? 0 : xsmcToBoolean(xsArg(1));

    startSPIFFS();

	xsmcToStringBuffer(xsArg(0), path, SPIFFS_OBJ_NAME_LEN);		// in case name is in ROM
    file = fopen(path, write ? "rb+" : "rb");
    if (NULL == file) {
        if (write)
            file = fopen(path, "wb+");
        if (NULL == file) {
			stopSPIFFS();
			xsUnknownError("file not found");
		}
    }
    xsmcSetHostData(xsThis, (void *)((uintptr_t)file));

	modInstrumentationAdjust(Files, +1);
}

void xs_file_read(xsMachine *the)
{
    void *data = xsmcGetHostData(xsThis);
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

void xs_file_write(xsMachine *the)
{
    void *data = xsmcGetHostData(xsThis);
    FILE *file = ((FILE *)data);
    int32_t result;
    int argc = xsmcArgc, i;

    for (i = 0; i < argc; i++) {
        uint8_t *src;
        int32_t srcLen;

        if (xsStringType == xsmcTypeOf(xsArg(i))) {
            src = (uint8_t *)xsmcToString(xsArg(i));
            srcLen = strlen((char *)src);
        }
        else {
            src = xsmcToArrayBuffer(xsArg(i));
            srcLen = xsGetArrayBufferLength(xsArg(i));
        }

        while (srcLen) {	// spool through RAM for data in flash
            unsigned char *buffer[128];
            int use = (srcLen <= (int)sizeof(buffer)) ? srcLen : 128;

            memcpy(buffer, src, use);
            src += use;
            srcLen -= use;

            result = fwrite(buffer, 1, use, file);
            if (result != use)
                xsUnknownError("file write failed");
            result = fflush(file);
			if (0 != result)
				xsUnknownError("file flush failed");
        }
    }
}

void xs_file_close(xsMachine *the)
{
    void *data = xsmcGetHostData(xsThis);
    FILE *file = ((FILE*)data);
    xs_file_destructor((void *)((int)file));
    xsmcSetHostData(xsThis, (void *)NULL);
}

void xs_file_get_length(xsMachine *the)
{
    void *data = xsmcGetHostData(xsThis);
    FILE *file = (FILE*)data;
    struct stat buf;
    int fno;

    fno = fileno(file);
    fstat(fno, &buf);
    xsResult = xsInteger(buf.st_size);
}

void xs_file_get_position(xsMachine *the)
{
    void *data = xsmcGetHostData(xsThis);
    FILE *file = (FILE*)data;
    int32_t position = ftell(file);
    xsResult = xsInteger(position);
}

void xs_file_set_position(xsMachine *the)
{
    void *data = xsmcGetHostData(xsThis);
    FILE *file = ((FILE*)data);
    int32_t position = xsmcToInteger(xsArg(0));
    fseek(file, position, SEEK_SET);
}

void xs_file_delete(xsMachine *the)
{
    char path[SPIFFS_OBJ_NAME_LEN];
    int32_t result;

    startSPIFFS();

	xsmcToStringBuffer(xsArg(0), path, SPIFFS_OBJ_NAME_LEN);		// in case name is in ROM
    result = unlink(path);

	stopSPIFFS();

    xsResult = xsBoolean(result == 0);
}

void xs_file_exists(xsMachine *the)
{
    char path[SPIFFS_OBJ_NAME_LEN];
    struct stat buf;
    int32_t result;

    startSPIFFS();

	xsmcToStringBuffer(xsArg(0), path, SPIFFS_OBJ_NAME_LEN);		// in case name is in ROM
    result = stat(path, &buf);

	stopSPIFFS();

    xsResult = xsBoolean(result == 0);
}

void xs_file_rename(xsMachine *the)
{
	char path[SPIFFS_OBJ_NAME_LEN];
	char name[SPIFFS_OBJ_NAME_LEN];
    int32_t result;

    startSPIFFS();

	xsmcToStringBuffer(xsArg(0), path, SPIFFS_OBJ_NAME_LEN);		// in case name is in ROM
	xsmcToStringBuffer(xsArg(1), name, SPIFFS_OBJ_NAME_LEN);		// in case name is in ROM
    result = rename(path, name);

	stopSPIFFS();

    xsResult = xsBoolean(result == 0);
}

void xs_file_iterator_destructor(void *data)
{
    iter d = data;

    if (d) {
        if (d->dir)
            closedir(d->dir);
        free(d);
    	stopSPIFFS();
    	modInstrumentationAdjust(Files, -1);
    }
}

void xs_File_Iterator(xsMachine *the)
{
    iter d;
    int i;
    char *p;

    startSPIFFS();

    p = xsmcToString(xsArg(0));
    i = strlen(p);
    if (i == 0) {
    	stopSPIFFS();
        xsUnknownError("no directory to iterate on");
    }
    d = calloc(1, sizeof(iteratorRecord) + i + 2);
    strcpy(d->path, p);
    if (p[i-1] != '/')
        d->path[i] = '/';

    if (NULL == (d->dir = opendir(d->path))) {
    	stopSPIFFS();
        xsUnknownError("failed to open directory");
    }
    xsmcSetHostData(xsThis, d);

	modInstrumentationAdjust(Files, +1);
}

void xs_file_iterator_next(xsMachine *the)
{
    iter d = xsmcGetHostData(xsThis);
    struct dirent *de;
    struct stat buf;
    char path[SPIFFS_OBJ_NAME_LEN];

    if (!d || !d->dir) return;

    do {
        if (NULL == (de = readdir(d->dir))) {
            xs_file_iterator_destructor(d);
            xsmcSetHostData(xsThis, NULL);
            return;
        }
    } while ((DT_DIR != de->d_type) && (DT_REG != de->d_type));

    xsResult = xsmcNewObject();
    xsmcVars(1);
    xsmcSetString(xsVar(0), de->d_name);
    xsmcSet(xsResult, xsID_name, xsVar(0));

    sprintf(path, "%s%s", d->path, de->d_name);
    if (DT_REG == de->d_type) {
        if (-1 == stat(path, &buf))
            fprintf(stderr, "stat %s returns errno: %d\n", path, errno);
        xsmcSetInteger(xsVar(0), buf.st_size);
        xsmcSet(xsResult, xsID_length, xsVar(0));
    }
}

void xs_file_system_config(xsMachine *the)
{
	xsResult = xsmcNewObject();
	xsmcVars(1);
	xsmcSetInteger(xsVar(0), SPIFFS_OBJ_NAME_LEN - 1);
	xsmcSet(xsResult, xsID_maxPathLength, xsVar(0));
}

void xs_file_system_info(xsMachine *the)
{
	startSPIFFS();

	size_t total = 0, used = 0;

	esp_err_t ret = esp_spiffs_info(NULL, &total, &used);

	if (ret != ESP_OK) {
		stopSPIFFS();
		xsUnknownError("system info failed");
	}

	stopSPIFFS();

    xsResult = xsmcNewObject();
	xsmcVars(1);
	xsmcSetInteger(xsVar(0), total);
	xsmcSet(xsResult, xsID_total, xsVar(0));
	xsmcSetInteger(xsVar(0), used);
	xsmcSet(xsResult, xsID_used, xsVar(0));
}

static uint16_t gUseCount;

int startSPIFFS(void)
{
	if (0 != gUseCount++)
		return 0;

	esp_vfs_spiffs_conf_t conf = {
			.base_path = "/spiffs",
			.partition_label = NULL,
			.max_files = 5,
			.format_if_mount_failed = true
	};

	// Use settings defined above to initialize and mount SPIFFS filesystem.
	// Note: esp_vfs_spiffs_register is an all-in-one convenience function.
	// https://docs.espressif.com/projects/esp-idf/en/latest/api-reference/storage/spiffs.html
	esp_err_t ret = esp_vfs_spiffs_register(&conf);

	if (ret != ESP_OK) {
		if (ret == ESP_FAIL) {
			modLog("Failed to mount or format filesystem");
		} else if (ret == ESP_ERR_NO_MEM) {
			modLog("Failed to allocate memory for filesystem");
		} else if (ret == ESP_ERR_NOT_FOUND) {
			modLog("Failed to find SPIFFS partition");
		} else {
			modLog("Failed to initialize SPIFFS");
		}

		gUseCount--;
	}

	return 0;
}

void stopSPIFFS(void)
{
	if (0 != --gUseCount)
		return;

	esp_vfs_spiffs_unregister(NULL);
}