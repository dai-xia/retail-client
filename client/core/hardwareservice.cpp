#include "hardwareservice.h"
#include <QtConcurrent/QtConcurrent>
#include <unistd.h>

HardwareService* HardwareService::m_instance = nullptr;

HardwareService::HardwareService(QObject *parent)
    : QObject(parent)
    , m_hwThread(nullptr)
    , m_ledFd(-1)
    , m_motorFd(-1)
    , m_faceRecognitionActive(false)
{
}

HardwareService* HardwareService::getInstance()
{
    if (!m_instance) {
        m_instance = new HardwareService();
    }
    return m_instance;
}

void HardwareService::init()
{
    m_ledFd = led_open();

    m_motorFd = motor_open();

    m_hwThread = new HardwareThread(this);
    connect(m_hwThread, &HardwareThread::signalLightValue,
            this, &HardwareService::onLightValue);
    connect(m_hwThread, &HardwareThread::signalCardUID,
            this, &HardwareService::onCardUID);
    m_hwThread->start();
}

void HardwareService::shutdown()
{
    if (m_hwThread) {
        m_hwThread->stopThread();
        m_hwThread = nullptr;
    }
    if (m_ledFd > 0) {
        led_close(m_ledFd);
        m_ledFd = -1;
    }
    if (m_motorFd > 0) {
        motor_close(m_motorFd);
        m_motorFd = -1;
    }
}

void HardwareService::rotateMotor(int durationMs)
{
    if (m_motorFd > 0) {
        motor_rotate(m_motorFd, durationMs);
    }
}

void HardwareService::rotateMotorSteps(int steps)
{
    if (m_motorFd > 0) {
        motor_step(m_motorFd, steps);
    }
}

void HardwareService::rotateMotorForCart(const QList<order_item_t>& cart)
{
    if (m_motorFd <= 0) return;

    int motorFd = m_motorFd;
    QList<order_item_t> cartCopy = cart;

    QtConcurrent::run([motorFd, cartCopy]() {
        const int steps_per_item = MOTOR_STEPS_QUARTER;  // 1024 steps = 90 deg (1/4 rev)
        const int wait_ms_per_step = MOTOR_DEFAULT_INTERVAL_US / 1000 + 1;  // +1 rounds up us->ms
        const int wait_ms_per_item = steps_per_item * wait_ms_per_step + 300;  // +300ms buffer before next dispense

        for (const auto& item : cartCopy) {
            for (int i = 0; i < item.num; i++) {
                motor_step(motorFd, steps_per_item);
                usleep(wait_ms_per_item * 1000);

                // Poll until motor stops (status 0); max 50 * 100ms = 5s
                int status = 1;
                int retry = 50;
                while (status == 1 && retry-- > 0) {
                    motor_get_status(motorFd, &status);
                    if (status == 1) usleep(100000);
                }
            }
        }
    });
}

void HardwareService::onLightValue(int lux)
{
    emit signalLightChanged(lux);

    if (m_ledFd <= 0)
        return;

    if (!m_faceRecognitionActive) {
        led_set(m_ledFd, 0);
        return;
    }

    int brightness = 0;
    if (lux < 100) {
        brightness = 255;
    } else if (lux < 500) {
        brightness = 180;
    } else if (lux < 1000) {
        brightness = 120;
    } else {
        brightness = 50;
    }
    led_set(m_ledFd, brightness);
}

void HardwareService::setFaceLight(bool on)
{
    m_faceRecognitionActive = on;
    if (!on && m_ledFd > 0) {
        led_set(m_ledFd, 0);
    }
}

void HardwareService::onCardUID(QString uid)
{
    emit signalCardDetected(uid);
}
