// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once
#include <stddef.h>
#include <stdint.h>
typedef unsigned int UINT;
typedef uint32_t FSIZE_t;
typedef int FRESULT;
typedef struct { int mounted; } FATFS;
typedef struct { const uint8_t* data; FSIZE_t size; FSIZE_t position; int opened; void* context; unsigned char mode; } FIL;
typedef struct { FSIZE_t fsize; } FILINFO;
#define FR_OK 0
#define FR_DISK_ERR 1
#define FR_NO_FILE 4
#define FR_NO_PATH 5
#define FR_EXIST 8
#define FA_READ 1
#define FA_WRITE 2
#define FA_CREATE_NEW 4
#define f_size(file) ((file)->size)
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
#ifdef __cplusplus
}
#endif
