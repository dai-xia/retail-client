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

/**
 * @brief Shelf/bin dispensing motor control function; drives the stepper motor to perform dispensing actions based on the order list
 * @param cart Order item list, containing the dispense quantity of each item
 * @note Based on 28BYJ-48 stepper motor (half-step mode) + ULN2003 driver, Linux character device motor interface
 * @note Uses Qt concurrent thread to execute motor actions, avoiding blocking the main/UI thread
 */
void HardwareService::rotateMotorForCart(const QList<order_item_t>& cart)
{
    // Validate motor device file handle; exit directly if invalid
    if (m_motorFd <= 0) return;

    // Cache motor handle and order list, passed to the async thread for use
    int motorFd = m_motorFd;
    QList<order_item_t> cartCopy = cart;

    // Launch Qt concurrent async thread: motor rotation, delay, and status polling all execute in a worker thread, not blocking the main thread
    QtConcurrent::run([motorFd, cartCopy]() {
        /*
         * Motor base configuration notes:
         * Hardware: 28BYJ-48 geared stepper motor, working in [half-step mode]
         * Step parameters: total steps per full revolution = 4096 steps/rev
         * Dispense action: for each item dispensed, motor rotates 1/4 rev (90 deg), corresponding to one slot switch
         * Per-item steps: 1/4 rev = 4096 / 4 = 1024 steps
         * Single-step pulse interval: default 1200us; per-item theoretical runtime ~1.2 seconds
         * Driver feature: after the stepping action completes, the driver layer automatically powers off and stops, no need to manually call stop command
         */
        const int steps_per_item = MOTOR_STEPS_QUARTER;  // Per-item steps: 1024 steps (90 deg / 1/4 rev)
        // Convert single-step duration (ms): step pulse interval us to ms, +1 for round-up error tolerance
        const int wait_ms_per_step = MOTOR_DEFAULT_INTERVAL_US / 1000 + 1;
        // Per-item total wait time: total stepping runtime + 300ms buffer, to prevent the next action before the previous completes
        const int wait_ms_per_item = steps_per_item * wait_ms_per_step + 300;

        // Iterate through the entire order list, processing different categories one by one
        for (const auto& item : cartCopy) {
            // Iterate the dispense quantity of the current item, performing dispense action per piece
            for (int i = 0; i < item.num; i++) {
                // Call driver interface: drive motor to rotate the specified number of steps, completing a single dispense (90 deg)
                motor_step(motorFd, steps_per_item);

                // Delay to wait for the motor to essentially complete rotation, reserving action execution time
                usleep(wait_ms_per_item * 1000);

                /****************************************************************************
                 * Motor status polling logic: confirm the motor has fully stopped before performing the next dispense
                 * Applicable scenario: non-blocking driver, cannot synchronously wait for action end, must poll status
                 * Polling rules:
                 *  1. status=1: motor running; status=0: motor stopped
                 *  2. Max retry 50 times, single poll interval 100ms, max total wait time = 50 * 100ms = 5 seconds
                 *  3. If timeout not stopped, forcibly exit polling to prevent thread stall
                 ***************************************************************************/
                int status = 1;
                int retry = 50;  // Polling retry upper limit, limits maximum wait time
                while (status == 1 && retry-- > 0) {
                    // Read current motor running status
                    motor_get_status(motorFd, &status);
                    // Motor still running, delay 100ms then poll again
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
