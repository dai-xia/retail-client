#include "ota.h"
#include "watchdog.h"
#include "logger.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

#define OTA_NEW_VERSION_PATH "/opt/retail/ota_new_version"

#ifndef TOSTRING
#define TOSTRING_HELPER(x) #x
#define TOSTRING(x) TOSTRING_HELPER(x)
#endif

/************************* SHA256 *************************/

int ota_sha256_file(const char *filepath, char hex_out[OTA_SHA256_HEX_LEN])
{
    FILE *fp = fopen(filepath, "rb");
    if (!fp) return -1;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);

    uint8_t buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        EVP_DigestUpdate(ctx, buf, n);
    fclose(fp);

    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    for (unsigned int i = 0; i < digest_len; i++)
        snprintf(hex_out + i * 2, 3, "%02x", digest[i]);
    hex_out[OTA_SHA256_HEX_LEN - 1] = '\0';
    return 0;
}

int ota_sha256_verify(const char *filepath, const char *expected_hex)
{
    char actual[OTA_SHA256_HEX_LEN];
    if (ota_sha256_file(filepath, actual) != 0) return -1;
    return (strcasecmp(actual, expected_hex) == 0) ? 0 : -1;
}

/************************* OTA core *************************/

ota_t* ota_create(const char *work_dir, const char *current_version)
{
    ota_t *ota = calloc(1, sizeof(ota_t));
    if (!ota) return NULL;

    strncpy(ota->work_dir, work_dir, sizeof(ota->work_dir) - 1);
    strncpy(ota->current_version, current_version, sizeof(ota->current_version) - 1);

    snprintf(ota->backup_dir, sizeof(ota->backup_dir), "%s/ota_backup", work_dir);
    mkdir(ota->work_dir, 0755);
    mkdir(ota->backup_dir, 0755);

    ota->state = OTA_STATE_IDLE;
    ota->type  = OTA_TYPE_APP;   /* default to APP-level for backward compatibility */
    pthread_mutex_init(&ota->mtx, NULL);

    return ota;
}

void ota_destroy(ota_t *ota)
{
    if (!ota) return;
    pthread_mutex_destroy(&ota->mtx);
    free(ota);
}

int ota_rollback(ota_t *ota)
{
    if (!ota) return -1;

    /* Fixed backup filename, independent of version */
    const char *backup_path_fmt = "%s/backup_last.tar.gz";
    char backup_path[OTA_PATH_MAX];
    snprintf(backup_path, sizeof(backup_path), backup_path_fmt, ota->backup_dir);

    struct stat st;
    if (stat(backup_path, &st) != 0) {
        LOGE("OTA: backup file not found %s", backup_path);
        return -1;
    }

    /* Extract to a temp dir first, swap only after success to avoid bricking */
    char tmp_dir[OTA_PATH_MAX];
    snprintf(tmp_dir, sizeof(tmp_dir), "%s/ota_rollback_tmp", ota->work_dir);
    mkdir(tmp_dir, 0755);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "rm -rf '%s'/* 2>/dev/null && tar xzf '%s' -C '%s' 2>/dev/null",
             tmp_dir, backup_path, tmp_dir);
    if (system(cmd) != 0) {
        LOGE("OTA: extract backup to temp dir failed");
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp_dir);
        system(cmd);
        return -1;
    }

    /* Swap: workDir -> workDir/old, tmp -> workDir */
    char old_dir[OTA_PATH_MAX];
    snprintf(old_dir, sizeof(old_dir), "%s/ota_rollback_old", ota->work_dir);
    snprintf(cmd, sizeof(cmd),
             "rm -rf '%s' 2>/dev/null && mv '%s' '%s' && mv '%s' '%s' 2>/dev/null",
             old_dir, ota->work_dir, old_dir, tmp_dir, ota->work_dir);
    if (system(cmd) != 0) {
        LOGE("OTA: swap dirs failed, attempting restore");
        /* Restore: if workDir is gone but old_dir exists, move it back */
        snprintf(cmd, sizeof(cmd), "mv '%s' '%s' 2>/dev/null", old_dir, ota->work_dir);
        system(cmd);
        return -1;
    }

    /* Clean up old dir */
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", old_dir);
    system(cmd);

    LOGI("OTA: rollback succeeded");
    return 0;
}

/*
 * ======================== Boot failure auto-rollback (APP) ========================
 * Relies on the watchdog's crash_history to count crashes after an OTA upgrade;
 * no separate boot_count is kept to avoid duplicate counters.
 */

int ota_set_boot_mark(ota_t *ota)
{
    if (!ota) return -1;

    int fd = open(OTA_NEW_VERSION_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, ota->manifest.version, strlen(ota->manifest.version));
        write(fd, "\n", 1);
        close(fd);
    }

    return 0;
}

int ota_check_boot_failure(ota_t *ota, const char *dump_dir)
{
    if (!ota || !dump_dir) return 0;

    struct stat st;
    if (stat(OTA_NEW_VERSION_PATH, &st) != 0)
        return 0;

    int crash_count = watchdog_count_crashes_since(dump_dir, st.st_mtime);

    if (crash_count < OTA_BOOT_FAIL_THRESHOLD)
        return 0;

    if (ota_rollback(ota) == 0) {
        ota_clear_boot_mark(ota);
        return 1;
    }

    return -1;
}

void ota_clear_boot_mark(ota_t *ota)
{
    (void)ota;
    unlink(OTA_NEW_VERSION_PATH);
}

/************************* System-level OTA (A/B + swupdate) *************************/

/*
 * libubootenv wrapper: uses fw_printenv / fw_setenv so we don't need to link
 * libubootenv.so, reducing build dependencies. Requires uboot-envtools at
 * deploy time.
 */
static int uboot_get_env(const char *name, char *out, size_t out_len)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "fw_printenv -n %s 2>/dev/null", name);

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    if (fgets(out, (int)out_len, fp) == NULL) {
        pclose(fp);
        return -1;
    }
    pclose(fp);

    /* Trim trailing newline */
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = '\0';
    return 0;
}

static int uboot_set_env(const char *name, const char *value)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "fw_setenv %s '%s' 2>/dev/null", name, value);
    int ret = system(cmd);
    return (ret == 0) ? 0 : -1;
}

const char* ota_system_get_current_slot(void)
{
    static char slot[8];
    if (uboot_get_env("boot_slot", slot, sizeof(slot)) != 0) {
        /* Default _a */
        strncpy(slot, "_a", sizeof(slot) - 1);
        slot[sizeof(slot) - 1] = '\0';
    }
    return slot;
}

int ota_system_install(ota_t *ota, const char *swu_path)
{
    if (!ota || !swu_path) return -1;

    struct stat st;
    if (stat(swu_path, &st) != 0) {
        LOGE("OTA-SYS: image file not found %s", swu_path);
        return -1;
    }

    const char *cur_slot = ota_system_get_current_slot();
    const char *next_slot = (strcmp(cur_slot, "_a") == 0) ? "_b" : "_a";

    /* swu path is fixed to /tmp/update.swu by project convention: no spaces,
     * no special chars, safe to single-quote */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "swupdate -i '%s' -e stable,%s 2>&1", swu_path, next_slot);

    LOGI("OTA-SYS: calling swupdate to write inactive partition %s, cmd=%s", next_slot, cmd);
    int ret = system(cmd);

    /* system() return value must be parsed via WIFEXITED / WEXITSTATUS */
    int sw_ret = -1;
    if (WIFEXITED(ret)) {
        sw_ret = WEXITSTATUS(ret);
    }

    if (sw_ret != 0) {
        LOGE("OTA-SYS: swupdate failed, sw_ret=%d", sw_ret);
        return -1;
    }

    LOGI("OTA-SYS: image written to backup partition %s", next_slot);
    return 0;
}

int ota_system_set_upgrade_env(ota_t *ota)
{
    if (!ota) return -1;

    const char *cur_slot = ota_system_get_current_slot();
    const char *next_slot = (strcmp(cur_slot, "_a") == 0) ? "_b" : "_a";

    LOGI("OTA-SYS: switching active slot %s -> %s", cur_slot, next_slot);

    /* Set upgrade marker, reset bootcount, switch boot_slot */
    if (uboot_set_env("upgrade_available", "1") != 0) {
        LOGE("OTA-SYS: set upgrade_available failed");
        return -1;
    }
    if (uboot_set_env("bootcount", "0") != 0) {
        LOGE("OTA-SYS: set bootcount failed");
        return -1;
    }
    if (uboot_set_env("bootlimit", TOSTRING(OTA_SYSTEM_BOOTLIMIT)) != 0) {
        LOGE("OTA-SYS: set bootlimit failed");
        return -1;
    }
    if (uboot_set_env("ota_system_done", "0") != 0) {
        LOGE("OTA-SYS: reset ota_system_done failed");
        return -1;
    }
    if (uboot_set_env("boot_slot", next_slot) != 0) {
        LOGE("OTA-SYS: switch boot_slot failed");
        return -1;
    }

    LOGI("OTA-SYS: U-Boot env set, next boot will use new slot %s", next_slot);
    return 0;
}

int ota_system_confirm(ota_t *ota)
{
    if (!ota) return -1;

    /* Confirm upgrade after the new system boots stably */
    if (uboot_set_env("upgrade_available", "0") != 0) {
        LOGE("OTA-SYS: clear upgrade_available failed");
        return -1;
    }
    if (uboot_set_env("ota_system_done", "1") != 0) {
        LOGE("OTA-SYS: set ota_system_done failed");
        return -1;
    }
    if (uboot_set_env("bootcount", "0") != 0) {
        LOGE("OTA-SYS: reset bootcount failed");
        return -1;
    }

    LOGI("OTA-SYS: upgrade confirmed, system is stable");
    return 0;
}

int ota_system_check_bootcount(ota_t *ota)
{
    if (!ota) return 0;

    char val[16];
    if (uboot_get_env("upgrade_available", val, sizeof(val)) != 0)
        return 0;

    /* Not in an upgrade, nothing to check */
    if (atoi(val) != 1) return 0;

    if (uboot_get_env("bootcount", val, sizeof(val)) != 0)
        return 0;

    int cnt = atoi(val);
    char lim[16];
    int limit = OTA_SYSTEM_BOOTLIMIT;
    if (uboot_get_env("bootlimit", lim, sizeof(lim)) == 0)
        limit = atoi(lim);

    if (cnt >= limit) {
        LOGW("OTA-SYS: bootcount=%d over limit (limit=%d), U-Boot rolled back to old slot", cnt, limit);
        return 1;
    }

    LOGI("OTA-SYS: bootcount=%d / limit=%d, pending confirmation", cnt, limit);
    return 0;
}
