#include "crashhandler.h"
#include <QDebug>

CrashHandler::CrashHandler(QObject *parent)
    : QObject(parent)
    , m_watchdog(nullptr)
    , m_feedTimer(nullptr)
{
}

CrashHandler::~CrashHandler()
{
    stop();
    if (m_watchdog) {
        watchdog_destroy(m_watchdog);
        m_watchdog = nullptr;
    }
}

bool CrashHandler::init(const QString &dumpDir, int timeoutSec, int feedInterval)
{
    m_watchdog = watchdog_create(timeoutSec, feedInterval);
    if (!m_watchdog) {
        qCritical() << "CrashHandler: watchdog creation failed";
        return false;
    }

    watchdog_set_dump_dir(m_watchdog, dumpDir.toUtf8().constData());

    if (watchdog_install_signal_handlers(m_watchdog) != 0) {
        qCritical() << "CrashHandler: signal handler installation failed";
        return false;
    }

    m_feedTimer = new QTimer(this);
    connect(m_feedTimer, &QTimer::timeout, this, &CrashHandler::slotFeedWatchdog);

    LOGI("CrashHandler initialized, timeout=%ds, feed interval=%ds", timeoutSec, feedInterval);
    return true;
}

void CrashHandler::start()
{
    if (!m_watchdog) return;

    if (watchdog_start(m_watchdog) == 0) {
        m_feedTimer->start(m_watchdog->feed_interval * 1000);
        LOGI("CrashHandler started");
    }
}

void CrashHandler::stop()
{
    if (m_feedTimer) m_feedTimer->stop();
    if (m_watchdog) watchdog_stop(m_watchdog);
}

int CrashHandler::crashCount() const
{
    return m_watchdog ? watchdog_get_crash_count(m_watchdog) : 0;
}

void CrashHandler::slotFeedWatchdog()
{
    watchdog_feed(m_watchdog);
}

bool CrashHandler::checkStartupSafety(const QString &dumpDir)
{
    return watchdog_check_startup_safety(dumpDir.toUtf8().constData()) == 0;
}
