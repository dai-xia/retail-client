#ifndef __OTA_H
#define __OTA_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_PATH_MAX        256
#define OTA_SHA256_HEX_LEN  65

/*
 * ====================== Application-level OTA (APP) ======================
 * Rollback threshold: if the new version crashes within BOOT_STABLE_SEC
 * seconds for BOOT_FAIL_THRESHOLD consecutive boots, roll back to the old one.
 */
#define OTA_BOOT_FAIL_THRESHOLD  3
#define OTA_BOOT_STABLE_SEC      60

/*
 * ====================== System-level OTA (SYSTEM / A/B) ======================
 * U-Boot env conventions (RK3568 android_boot BSP):
 *   boot_slot        : active slot "_a" / "_b"
 *   bootcount        : U-Boot cumulative boot count; auto slot switch on overrun
 *   bootlimit        : boot failure limit (default 3)
 *   upgrade_available: 1 = new version pending confirmation, 0 = confirmed
 *   ota_system_done  : 1 = new system boot confirmed healthy
 *
 * Health threshold: new system must boot successfully BOOT_STABLE_CNT times.
 */
#define OTA_SYSTEM_BOOTLIMIT     3
#define OTA_SYSTEM_STABLE_CNT    3

/* swupdate image staging dir (on data partition, not wiped by OTA) */
#define OTA_SYSTEM_STAGING_DIR   "/data/ota/staging"

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_CHECKING,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_INSTALLING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED
} ota_state_t;

typedef struct {
    char version[32];
    char filename[128];
    char sha256[OTA_SHA256_HEX_LEN];
    int64_t file_size;
} ota_manifest_t;

typedef struct {
    ota_state_t state;
    int progress;

    char work_dir[OTA_PATH_MAX];
    char backup_dir[OTA_PATH_MAX];
    char current_version[32];

    ota_type_t type;             /* APP / SYSTEM routing */
    ota_manifest_t manifest;

    pthread_mutex_t mtx;
} ota_t;

/* ============== Common ============== */
ota_t* ota_create(const char *work_dir, const char *current_version);
void   ota_destroy(ota_t *ota);

int ota_sha256_file(const char *filepath, char hex_out[OTA_SHA256_HEX_LEN]);
int ota_sha256_verify(const char *filepath, const char *expected_hex);

/* ============== Application-level OTA (APP) ============== */
int  ota_rollback(ota_t *ota);

int  ota_set_boot_mark(ota_t *ota);
int  ota_check_boot_failure(ota_t *ota, const char *dump_dir);
void ota_clear_boot_mark(ota_t *ota);

/* ============== System-level OTA (SYSTEM / A/B) ==============
 * Flow: ota_system_install -> set upgrade_available=1, bootcount=0,
 *       switch active slot -> reboot -> new system boots healthy N times ->
 *       ota_system_confirm. On boot failure U-Boot auto-rolls to old slot.
 */

/* Write image to inactive slot via swupdate; returns 0 on success */
int  ota_system_install(ota_t *ota, const char *swu_path);

/* Set upgrade markers + switch active slot via libubootenv */
int  ota_system_set_upgrade_env(ota_t *ota);

/* Confirm upgrade after the new system boots healthy (upgrade_available=0, ota_system_done=1) */
int  ota_system_confirm(ota_t *ota);

/* Boot failure check: read bootcount; return 1 if over limit (U-Boot rolled back), else 0 */
int  ota_system_check_bootcount(ota_t *ota);

/* Read current active slot ("_a" / "_b"); NULL on failure */
const char* ota_system_get_current_slot(void);

#ifdef __cplusplus
}
#endif

#endif
