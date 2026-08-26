#ifndef CRASHHANDLER_H
#define CRASHHANDLER_H

#include <QObject>
#include <QTimer>
#include "core/watchdog.h"
#include "logger.h"

class CrashHandler : public QObject
{
    Q_OBJECT

public:
    explicit CrashHandler(QObject *parent = nullptr);
    ~CrashHandler();

    bool init(const QString &dumpDir, int timeoutSec = 30, int feedInterval = 5);
    void start();
    void stop();

    int crashCount() const;

    static bool checkStartupSafety(const QString &dumpDir);

signals:
    void signalCrashDetected(int crashCount);

private slots:
    void slotFeedWatchdog();

private:
    watchdog_t *m_watchdog;
    QTimer *m_feedTimer;
};

#endif
