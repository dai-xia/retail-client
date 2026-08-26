#ifndef OTAUPDATER_H
#define OTAUPDATER_H

#include <QObject>
#include <QByteArray>
#include <QFile>
#include <QString>
#include "core/ota.h"

class OtaUpdater : public QObject
{
    Q_OBJECT

public:
    explicit OtaUpdater(QObject *parent = nullptr);
    ~OtaUpdater();

    bool init(const QString &workDir, const QString &currentVersion);

public slots:
    void slotStartTcpDownload(const QString &version, const QString &filename,
                              const QString &sha256, int fileSize,
                              int type /* OTA_TYPE_APP / OTA_TYPE_SYSTEM */);
    void slotReceiveChunk(int chunkIndex, int totalChunks, const QByteArray &data);

signals:
    void signalProgressChanged(ota_state_t state, int progress, const QString &message);
    void signalUpdateAvailable(const QString &version, const QString &description);
    void signalUpdateFinished(bool success, const QString &message);
    void signalRequestOtaFile(const QString &version, const QString &filename);

private:
    void slotInstallTcpDownload(const QString &newFilePath);
    void installAppOta(const QString &newFilePath);
    void installSystemOta(const QString &newFilePath);

    ota_t *m_ota;

    bool m_tcpDownloading;
    QString m_tcpVersion;
    QString m_tcpFilename;
    QString m_tcpSha256;
    int m_tcpFileSize;
    int m_tcpType;          /* OTA_TYPE_APP / OTA_TYPE_SYSTEM */
    int m_tcpTotalChunks;
    int m_expectedChunkIndex;
    QFile *m_tcpTempFile;       
    QString m_tcpTempPath;      
};

#endif
