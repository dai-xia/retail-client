#ifndef HARDWARESERVICE_H
#define HARDWARESERVICE_H

#include <QObject>
#include "hardwarethread.h"
#include "hardware_api.h"
#include "common.h"

class HardwareService : public QObject
{
    Q_OBJECT

public:
    static HardwareService* getInstance();

    void init();
    void shutdown();

    int ledFd() const { return m_ledFd; }
    int motorFd() const { return m_motorFd; }

    void setFaceLight(bool on);

    void rotateMotor(int durationMs);
    void rotateMotorSteps(int steps);               
    void rotateMotorForCart(const QList<order_item_t>& cart);

signals:
    void signalLightChanged(int lux);
    void signalCardDetected(QString uid);

private slots:
    void onLightValue(int lux);
    void onCardUID(QString uid);

private:
    explicit HardwareService(QObject *parent = nullptr);
    static HardwareService* m_instance;

    HardwareThread* m_hwThread;
    int m_ledFd;
    int m_motorFd;
    bool m_faceRecognitionActive;
};

#endif
