#include "hardwarethread.h"
#include <unistd.h>

HardwareThread::HardwareThread(QObject *parent)
    : QThread(parent)
    , m_isRunning(false)
{
}

void HardwareThread::stopThread()
{
    m_isRunning.store(false);
    this->quit();
    this->wait();
}

void HardwareThread::run()
{
    m_isRunning.store(true);

    int bh1750Fd = bh1750_open();
    int rc522Fd = rc522_open();

    uint8_t uidBuf[4] = {0};
    char uidStr[9] = {0};

    while(m_isRunning.load())
    {
        int lightVal = bh1750_read_light(bh1750Fd);
        if(lightVal >= 0)
        {
            emit signalLightValue(lightVal);
        }

        if(rc522_get_uid(rc522Fd, uidBuf) == 0)
        {
            snprintf(uidStr, sizeof(uidStr), "%02X%02X%02X%02X", uidBuf[0], uidBuf[1], uidBuf[2], uidBuf[3]);
            emit signalCardUID(QString(uidStr));
            sleep(1);
        }

        usleep(100000);
    }

    bh1750_close(bh1750Fd);
    rc522_close(rc522Fd);
}