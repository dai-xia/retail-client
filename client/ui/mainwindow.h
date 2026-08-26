#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include "core/facemanager.h"
#include "core/networkmanager.h"
#include "core/hw_watchdog.h"
#include <cJSON.h>

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void slotUpdateLight(int lux);
    void slotGetCardUID(QString uid);
    void slotConnected();
    void slotDisconnected();
    void slotStatusMessage(QString msg, int timeoutMs);

    void on_btn_connect_clicked();
    void on_btn_disconnect_clicked();
    void on_btn_check_update_clicked();
    void on_btn_exit_clicked();
    void on_btn_voice_wake_clicked();
    void on_btn_voice_send_clicked();
    void on_btn_voice_simulate_clicked();

signals:
    void signalCardDetected(QString uid);

private:
    Ui::MainWindow *ui;
    FaceManager *m_faceManager;

    class MemberRegister *m_memberRegister;
    class GoodsManager *m_goodsManager;
    class SettlementWidget *m_settlementWidget;
    class OrderQuery *m_orderQuery;
    class RechargeWidget *m_rechargeWidget;
    class MockPaymentProvider *m_mockPayment;
    class VoiceEngine *m_voiceEngine;

    class CrashHandler *m_crashHandler;
    class OtaUpdater *m_otaUpdater;
    hw_watchdog_t *m_hwWatchdog;

    bool m_voiceActive;
    bool m_voiceRechargeRequested;
    QTimer *m_voiceIdleTimer;

    void initSystem();
    void initSubPages();
    void connectSignals();
    void connectServices();
    void applyModernStyle();
    void openGoodsManagerWithAuth();

    void beginVoiceSession();
    void endVoiceSession();
    void abortVoiceSession();
    void resetVoiceIdleTimer();
};

#endif
