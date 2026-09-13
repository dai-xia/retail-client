#include "ui/mainwindow.h"
#include "config/client_config.h"
#include "logger.h"
#include "core/watchdog.h"
#include "core/ota.h"
#include "core/hw_watchdog.h"
#include "crypto.h"
#include <QApplication>
#include <QDir>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    crypto_init(CRYPTO_KEY_FILE);

    ClientConfig::getInstance()->parseArgs(argc, argv);

    QString dumpDir = "/var/log/retail/dump";
    QDir().mkpath(dumpDir);

    QString logDir = "/var/log/retail";
    QDir().mkpath(logDir);
    logger_t *log = logger_create(logDir.toUtf8().constData(), "client");
    if (log) {
        logger_set_level(log, LOG_LEVEL_DEBUG);
        logger_set_console(log, 1);
        logger_set_default(log);
        logger_start(log);
    }

    /* Phase 1: OTA boot-failure auto-rollback (system A/B + app crash rollback) */
    {
        ota_t *ota = ota_create("/opt/retail", "");
        if (ota) {
            /* System-level A/B: log only */
            int sys_ret = ota_system_check_bootcount(ota);
            if (sys_ret == 1) {
                LOGW("OTA-SYS: System upgrade failed, U-Boot has switched back to the old slot, continue booting");
            }

            int ret = ota_check_boot_failure(ota, dumpDir.toUtf8().constData());
            if (ret == 1) {
                LOGW("OTA: Consecutive boot crashes, rolled back to the previous version, restarting...");
                logger_flush(logger_get_default());
                hw_watchdog_emergency_disable();
                ota_destroy(ota);
                _exit(0);
            }
            if (ret < 0) {
                LOGE("OTA: Rollback failed, system may be unhealthy");
            }
            ota_destroy(ota);
        }
    }

    MainWindow w;
    w.show();

    /* Phase 2: confirm upgrade after boot stable */
    QTimer::singleShot(OTA_BOOT_STABLE_SEC * 1000, []() {
        ota_clear_boot_mark(NULL);
        LOGI("OTA: Application-level boot is stable, clearing rollback mark");

        ota_t *sys_ota = ota_create("/opt/retail", "");
        if (sys_ota) {
            if (ota_system_confirm(sys_ota) == 0) {
                LOGI("OTA-SYS: System upgrade confirmed");
            }
            ota_destroy(sys_ota);
        }
    });

    int ret = a.exec();

    if (log) {
        LOGI("Client exited normally");
        logger_flush(log);
        logger_destroy(log);
    }

    return ret;
}
