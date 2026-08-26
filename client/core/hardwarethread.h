#ifndef HARDWARETHREAD_H
#define HARDWARETHREAD_H

#include <QThread>
#include <QString>
#include <atomic>
#include "hardware_api.h"

class HardwareThread : public QThread
{
    Q_OBJECT
public:
    explicit HardwareThread(QObject *parent = nullptr);
    void stopThread();

protected:
    void run() override;

signals:
    void signalCardUID(QString uid);
    void signalLightValue(int lux);

private:
    std::atomic<bool> m_isRunning{false};
};

#endif // HARDWARETHREAD_H