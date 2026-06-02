/*
 * Copyright 2026 zCube
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shared mflash file registration for Matter preferences.
 *
 * Included from app_nvram.c so both appcfg.bin and Matter prefs share a
 * single mflash_init() table.
 */

#ifndef APP_MATTER_MATTER_FILE_SLOT_H
#define APP_MATTER_MATTER_FILE_SLOT_H

#ifndef APP_MATTER_PREFS_FILENAME
#define APP_MATTER_PREFS_FILENAME  "matter_prf.bin"
#endif

/* Header + fixed payload blob (CHIP fabric creds 등은 추후 같은 파일에 버전 업). */
#ifndef APP_MATTER_PREFS_MAX_FILE_SIZE
#define APP_MATTER_PREFS_MAX_FILE_SIZE  (192u + 24u)
#endif

#endif /* APP_MATTER_MATTER_FILE_SLOT_H */
