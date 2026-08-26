#include "otaupdater.h"
#include "logger.h"
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTimer>

OtaUpdater::OtaUpdater(QObject *parent)
    : QObject(parent)
    , m_ota(nullptr)
    , m_tcpDownloading(false)
    , m_tcpFileSize(0)
    , m_tcpType(OTA_TYPE_APP)
    , m_tcpTotalChunks(0)
    , m_expectedChunkIndex(0)
    , m_tcpTempFile(nullptr)
{
}

OtaUpdater::~OtaUpdater()
{
    if (m_ota) {
        ota_destroy(m_ota);
        m_ota = nullptr;
    }
}

bool OtaUpdater::init(const QString &workDir, const QString &currentVersion)
{
    m_ota = ota_create(workDir.toUtf8().constData(),
                       currentVersion.toUtf8().constData());
    if (!m_ota) {
        LOGE("OtaUpdater: failed to create OTA instance");
        return false;
    }

    LOGI("OtaUpdater initialized, current version=%s",
         currentVersion.toUtf8().constData());
    return true;
}

void OtaUpdater::slotStartTcpDownload(const QString &version, const QString &filename,
                                       const QString &sha256, int fileSize, int type)
{
    m_tcpDownloading = true;
    m_tcpVersion = version;
    m_tcpFilename = filename;
    m_tcpSha256 = sha256;
    m_tcpFileSize = fileSize;
    m_tcpType = (type == OTA_TYPE_SYSTEM) ? OTA_TYPE_SYSTEM : OTA_TYPE_APP;
    m_tcpTotalChunks = 0;
    m_expectedChunkIndex = 0;

    /* Close any previous temp file (if present) */
    if (m_tcpTempFile) {
        m_tcpTempFile->close();
        delete m_tcpTempFile;
        m_tcpTempFile = nullptr;
    }

    /* Temp file path: SYSTEM OTA lands in /data/ota/staging, APP OTA in work_dir */
    QString saveDir = (m_tcpType == OTA_TYPE_SYSTEM)
                          ? QString::fromUtf8(OTA_SYSTEM_STAGING_DIR)
                          : QString::fromUtf8(m_ota->work_dir);
    QDir().mkpath(saveDir);
    m_tcpTempPath = QString("%1/%2_%3")
        .arg(saveDir).arg(version).arg(filename);

    /* Open temp file for streaming each chunk; no longer caches everything in memory */
    m_tcpTempFile = new QFile(m_tcpTempPath);
    if (!m_tcpTempFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        LOGE("OtaUpdater: cannot create temp file %s", m_tcpTempPath.toUtf8().constData());
        delete m_tcpTempFile;
        m_tcpTempFile = nullptr;
        m_tcpDownloading = false;
        emit signalUpdateFinished(false, "无法创建临时文件, 磁盘空间不足?");
        return;
    }

    LOGI("OtaUpdater: start streaming download %s v%s (%d bytes, type=%s) -> %s",
         filename.toUtf8().constData(), version.toUtf8().constData(), fileSize,
         (m_tcpType == OTA_TYPE_SYSTEM) ? "SYSTEM" : "APP",
         m_tcpTempPath.toUtf8().constData());

    emit signalRequestOtaFile(version, filename);
}

void OtaUpdater::slotReceiveChunk(int chunkIndex, int totalChunks, const QByteArray &data)
{
    if (!m_tcpDownloading) return;

    if (chunkIndex != m_expectedChunkIndex) {
        LOGE("OtaUpdater: chunk order error expected=%d got=%d",
             m_expectedChunkIndex, chunkIndex);
        m_tcpDownloading = false;
        if (m_tcpTempFile) {
            m_tcpTempFile->close();
            delete m_tcpTempFile;
            m_tcpTempFile = nullptr;
        }
        QFile::remove(m_tcpTempPath);
        emit signalUpdateFinished(false, "分块顺序错误，下载已中止");
        return;
    }

    m_tcpTotalChunks = totalChunks;

    /* Streaming write: append each chunk directly to the temp file, no in-memory cache */
    if (m_tcpTempFile && m_tcpTempFile->isOpen()) {
        qint64 written = m_tcpTempFile->write(data);
        if (written != data.size()) {
            LOGE("OtaUpdater: write to temp file failed, disk full?");
            m_tcpDownloading = false;
            m_tcpTempFile->close();
            delete m_tcpTempFile;
            m_tcpTempFile = nullptr;
            QFile::remove(m_tcpTempPath);
            emit signalUpdateFinished(false, "写入文件失败, 磁盘空间不足?");
            return;
        }
    }

    m_expectedChunkIndex++;

    int progress = (chunkIndex + 1) * 100 / totalChunks;
    emit signalProgressChanged(OTA_STATE_DOWNLOADING, progress,
                               QString("下载中 %1/%2").arg(chunkIndex + 1).arg(totalChunks));

    if (chunkIndex + 1 >= totalChunks) {
        m_tcpDownloading = false;

        if (m_expectedChunkIndex != totalChunks) {
            LOGE("OtaUpdater: chunk count mismatch expected=%d received=%d",
                 totalChunks, m_expectedChunkIndex);
            if (m_tcpTempFile) {
                m_tcpTempFile->close();
                delete m_tcpTempFile;
                m_tcpTempFile = nullptr;
            }
            QFile::remove(m_tcpTempPath);
            emit signalUpdateFinished(false, "分块数量不匹配");
            return;
        }

        /* Close temp file, finalize the write */
        if (m_tcpTempFile) {
            m_tcpTempFile->close();
            delete m_tcpTempFile;
            m_tcpTempFile = nullptr;
        }

        /* Verify the on-disk file size */
        QFileInfo fi(m_tcpTempPath);
        if (fi.size() != m_tcpFileSize) {
            LOGE("OtaUpdater: file size mismatch expected=%d actual=%lld",
                 m_tcpFileSize, fi.size());
            QFile::remove(m_tcpTempPath);
            emit signalUpdateFinished(false, "文件大小不匹配");
            return;
        }

        /* SHA256 verification */
        char shaHex[OTA_SHA256_HEX_LEN];
        if (ota_sha256_file(m_tcpTempPath.toUtf8().constData(), shaHex) != 0) {
            QFile::remove(m_tcpTempPath);
            emit signalUpdateFinished(false, "SHA256计算失败");
            return;
        }

        if (strcasecmp(shaHex, m_tcpSha256.toUtf8().constData()) != 0) {
            LOGE("OtaUpdater: SHA256 mismatch expected=%s actual=%s",
                 m_tcpSha256.toUtf8().constData(), shaHex);
            QFile::remove(m_tcpTempPath);
            emit signalUpdateFinished(false, "SHA256校验失败");
            return;
        }

        LOGI("OtaUpdater: streaming download complete, file written %s (%lld bytes), SHA256 verified",
             m_tcpTempPath.toUtf8().constData(), fi.size());

        snprintf(m_ota->manifest.version, sizeof(m_ota->manifest.version),
                 "%s", m_tcpVersion.toUtf8().constData());
        snprintf(m_ota->manifest.filename, sizeof(m_ota->manifest.filename),
                 "%s", m_tcpFilename.toUtf8().constData());
        snprintf(m_ota->manifest.sha256, sizeof(m_ota->manifest.sha256),
                 "%s", m_tcpSha256.toUtf8().constData());
        m_ota->manifest.file_size = m_tcpFileSize;
        m_ota->type = (ota_type_t)m_tcpType;

        emit signalUpdateAvailable(m_tcpVersion, "");

        QString savePath = m_tcpTempPath;  /* Capture current path so it is not overwritten later */
        QTimer::singleShot(500, this, [this, savePath]() {
            slotInstallTcpDownload(savePath);
        });
    }
}

void OtaUpdater::slotInstallTcpDownload(const QString &newFilePath)
{
    /* Route by type: APP uses tar.gz backup+replace, SYSTEM uses swupdate + A/B */
    if (m_tcpType == OTA_TYPE_SYSTEM) {
        installSystemOta(newFilePath);
    } else {
        installAppOta(newFilePath);
    }
}

/* ============== App-level OTA install (original logic) ============== */
void OtaUpdater::installAppOta(const QString &newFilePath)
{
    emit signalProgressChanged(OTA_STATE_INSTALLING, 100, "安装应用更新...");

    QString workDir = QString::fromUtf8(m_ota->work_dir);
    QString backupDir = QString::fromUtf8(m_ota->backup_dir);

    QString backupFile = QString("%1/backup_last.tar.gz").arg(backupDir);

    QString finalPath = QString("%1/%2")
        .arg(workDir).arg(QString::fromUtf8(m_ota->manifest.filename));

    QDir().mkpath(backupDir);
    QString cmd = QString("tar czf '%1' -C '%2' . 2>/dev/null")
        .arg(backupFile).arg(workDir);
    system(cmd.toUtf8().constData());

    bool renameOk = false;
    if (QFile::exists(finalPath)) {
        QFile::remove(finalPath);
    }
    renameOk = QFile::rename(newFilePath, finalPath);

    if (!renameOk) {
        LOGE("OtaUpdater: install failed, rename %s -> %s",
             newFilePath.toUtf8().constData(),
             finalPath.toUtf8().constData());
        emit signalUpdateFinished(false, "安装失败");
        return;
    }

    QFile::setPermissions(finalPath,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
        QFileDevice::ReadGroup | QFileDevice::ExeGroup |
        QFileDevice::ReadOther  | QFileDevice::ExeOther);

    strncpy(m_ota->current_version, m_ota->manifest.version,
            sizeof(m_ota->current_version) - 1);

    ota_set_boot_mark(m_ota);

    emit signalProgressChanged(OTA_STATE_SUCCESS, 100, "应用升级完成");
    emit signalUpdateFinished(true, "升级成功");
}

/* ============== System-level OTA install (swupdate + A/B) ============== */
void OtaUpdater::installSystemOta(const QString &newFilePath)
{
    emit signalProgressChanged(OTA_STATE_INSTALLING, 30, "写入非活动分区...");

    /* 1. Run swupdate to write the image to the inactive slot */
    if (ota_system_install(m_ota, newFilePath.toUtf8().constData()) != 0) {
        emit signalUpdateFinished(false, "swupdate 写入失败");
        return;
    }

    emit signalProgressChanged(OTA_STATE_INSTALLING, 60, "切换启动槽位...");

    /* 2. Set the upgrade mark + switch the active slot */
    if (ota_system_set_upgrade_env(m_ota) != 0) {
        emit signalUpdateFinished(false, "U-Boot 环境变量设置失败");
        return;
    }

    emit signalProgressChanged(OTA_STATE_INSTALLING, 90, "准备重启...");

    /* 3. Write the version into the boot mark so the post-boot health check can detect it */
    ota_set_boot_mark(m_ota);

    strncpy(m_ota->current_version, m_ota->manifest.version,
            sizeof(m_ota->current_version) - 1);

    emit signalProgressChanged(OTA_STATE_SUCCESS, 100, "系统镜像写入完成, 即将重启");
    emit signalUpdateFinished(true, "系统升级已就绪, 重启后生效");
}
