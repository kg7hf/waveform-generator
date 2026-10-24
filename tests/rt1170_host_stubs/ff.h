// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once
#include <stddef.h>
#include <stdint.h>
typedef unsigned int UINT;
typedef uint32_t FSIZE_t;
typedef int FRESULT;
typedef struct { int mounted; } FATFS;
typedef struct { const uint8_t* data; FSIZE_t size; FSIZE_t position; int opened; void* context; unsigned char mode; } FIL;
typedef struct { int opened; size_t position; void* context; } DIR;
typedef struct
{
    FSIZE_t fsize;
    uint16_t fdate;
    uint16_t ftime;
    uint8_t fattrib;
    char fname[256];
    char altname[13];
} FILINFO;
#define FR_OK 0
#define FR_DISK_ERR 1
#define FR_NO_FILE 4
#define FR_NO_PATH 5
#define FR_EXIST 8
#define FR_INVALID_OBJECT 9
#define AM_DIR 0x10
#define AM_VOL 0x08
#define FF_USE_LFN 1
#define FF_CODE_PAGE 437
#define FF_LFN_UNICODE 0
#define FF_SFN_BUF 12
#define FA_READ 1
#define FA_WRITE 2
#define FA_CREATE_NEW 4
#define f_size(file) ((file)->size)
#define f_tell(file) ((file)->position)
#ifdef __cplusplus
extern "C" {
#endif
FRESULT f_mount(FATFS* filesystem, const char* drive, unsigned char immediate);
FRESULT f_open(FIL* file, const char* path, unsigned char mode);
FRESULT f_read(FIL* file, void* data, UINT requested, UINT* actual);
FRESULT f_lseek(FIL* file, FSIZE_t offset);
FRESULT f_close(FIL* file);
FRESULT f_write(FIL* file, const void* data, UINT requested, UINT* actual);
FRESULT f_sync(FIL* file);
FRESULT f_stat(const char* path, FILINFO* info);
FRESULT f_rename(const char* old_path, const char* new_path);
FRESULT f_mkdir(const char* path);
FRESULT f_opendir(DIR* directory, const char* path);
FRESULT f_readdir(DIR* directory, FILINFO* info);
FRESULT f_closedir(DIR* directory);
#ifdef __cplusplus
}
#endif
