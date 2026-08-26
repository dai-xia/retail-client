#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "memberregister.h"
#include "goodsmanager.h"
#include "settlementwidget.h"
#include "orderquery.h"
#include "rechargewidget.h"
#include "config/mock_payment_provider.h"
#include "services/voice_engine.h"
#include "core/localdbmanager.h"
#include "config/client_config.h"
#include "services/crashhandler.h"
#include "services/otaupdater.h"
#include "core/hw_watchdog.h"
#include "services/clientservice.h"
#include "core/hardwareservice.h"
#include "logger.h"
#include <QMessageBox>
#include <QDebug>
#include <QApplication>
#include <QInputDialog>
#include <QDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QTimer>
#include <QProcess>
#include <QInputDialog>
#include <QStatusBar>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_faceManager(FaceManager::getInstance())
    , m_memberRegister(nullptr)
    , m_goodsManager(nullptr)
    , m_settlementWidget(nullptr)
    , m_orderQuery(nullptr)
    , m_rechargeWidget(nullptr)
    , m_mockPayment(nullptr)
    , m_crashHandler(nullptr)
    , m_otaUpdater(nullptr)
    , m_hwWatchdog(nullptr)
    , m_voiceActive(false)
    , m_voiceRechargeRequested(false)
    , m_voiceIdleTimer(new QTimer(this))
{
    ui->setupUi(this);

    ClientConfig *cfg = ClientConfig::getInstance();
    QString clientId = cfg->getClientId();

    this->setWindowTitle(QString("无人零售自助收银系统 - 客户端[%1]").arg(clientId));
    this->resize(1500, 1000);

    applyModernStyle();

    ui->lineEdit_clientId->setText(clientId);

    HardwareService::getInstance()->init();

    initSystem();
    initSubPages();
    connectSignals();
    connectServices();
}

void MainWindow::applyModernStyle()
{
    this->setStyleSheet(
        "QMainWindow {"
        "    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
        "        stop:0 #f5f7fa, stop:1 #c3cfe2);"
        "}"
        "QStatusBar {"
        "    min-height: 28px;"
        "    max-height: 28px;"
        "    font-size: 15px;"
        "    background: #2c3e50;"
        "    color: #ecf0f1;"
        "    padding: 2px 8px;"
        "}"
        "QStatusBar::item {"
        "    border: none;"
        "}"
        "QTabWidget::pane {"
        "    border: 2px solid #3498db;"
        "    border-radius: 8px;"
        "    background: white;"
        "}"
        "QTabBar::tab {"
        "    background: #ecf0f1;"
        "    border: 1px solid #bdc3c7;"
        "    border-bottom: none;"
        "    border-top-left-radius: 6px;"
        "    border-top-right-radius: 6px;"
        "    padding: 8px 18px;"
        "    margin-right: 2px;"
        "    font-weight: bold;"
        "    font-size: 15px;"
        "    color: #2c3e50;"
        "}"
        "QTabBar::tab:selected {"
        "    background: #3498db;"
        "    color: white;"
        "    border-color: #3498db;"
        "}"
        "QTabBar::tab:hover:!selected {"
        "    background: #d5dbdb;"
        "}"
        "QPushButton {"
        "    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #3498db, stop:1 #2980b9);"
        "    color: white;"
        "    border: none;"
        "    border-radius: 5px;"
        "    padding: 6px 14px;"
        "    font-weight: bold;"
        "    font-size: 14px;"
        "    min-width: 70px;"
        "    min-height: 28px;"
        "}"
        "QPushButton:hover {"
        "    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #2980b9, stop:1 #1f618d);"
        "}"
        "QPushButton:pressed {"
        "    background: #1f618d;"
        "}"
        "QPushButton:disabled {"
        "    background: #bdc3c7;"
        "    color: #7f8c8d;"
        "}"
        "QLineEdit {"
        "    border: 2px solid #bdc3c7;"
        "    border-radius: 5px;"
        "    padding: 5px 8px;"
        "    background: white;"
        "    font-size: 15px;"
        "    selection-background-color: #3498db;"
        "}"
        "QLineEdit:focus {"
        "    border-color: #3498db;"
        "}"
        "QLabel {"
        "    color: #2c3e50;"
        "    font-size: 15px;"
        "}"
        "QTableWidget {"
        "    border: 2px solid #bdc3c7;"
        "    border-radius: 5px;"
        "    background: white;"
        "    font-size: 15px;"
        "    gridline-color: #ecf0f1;"
        "}"
        "QTableWidget::item {"
        "    padding: 4px 8px;"
        "}"
        "QHeaderView::section {"
        "    background: #3498db;"
        "    color: white;"
        "    font-weight: bold;"
        "    font-size: 14px;"
        "    padding: 6px 8px;"
        "    border: 1px solid #2980b9;"
        "}"
        "QComboBox {"
        "    border: 2px solid #bdc3c7;"
        "    border-radius: 5px;"
        "    padding: 5px 8px;"
        "    background: white;"
        "    font-size: 15px;"
        "    min-width: 90px;"
        "}"
        "QComboBox:focus {"
        "    border-color: #3498db;"
        "}"
        "QComboBox::drop-down {"
        "    border: none;"
        "    width: 24px;"
        "}"
        "QComboBox QAbstractItemView {"
        "    background: white;"
        "    color: #2c3e50;"
        "    selection-background-color: #3498db;"
        "    selection-color: white;"
        "}"
        "QSpinBox, QDoubleSpinBox {"
        "    border: 2px solid #bdc3c7;"
        "    border-radius: 5px;"
        "    padding: 5px 8px;"
        "    background: white;"
        "    font-size: 15px;"
        "}"
        "QGroupBox {"
        "    border: 2px solid #3498db;"
        "    border-radius: 6px;"
        "    margin-top: 8px;"
        "    padding: 16px 10px 8px 10px;"
        "    font-weight: bold;"
        "    font-size: 15px;"
        "    color: #2c3e50;"
        "}"
        "QGroupBox::title {"
        "    subcontrol-origin: margin;"
        "    left: 10px;"
        "    padding: 0 6px;"
        "    color: #3498db;"
        "}"
    );

    ui->label_status->setStyleSheet(
        "QLabel {"
        "    color: #e74c3c;"
        "    font-weight: bold;"
        "    font-size: 16px;"
        "    padding: 5px 10px;"
        "    border-radius: 4px;"
        "    background: rgba(231, 76, 60, 0.1);"
        "}"
    );

    ui->label_light->setStyleSheet(
        "QLabel {"
        "    color: #27ae60;"
        "    font-weight: bold;"
        "    padding: 5px 10px;"
        "    border-radius: 4px;"
        "    background: rgba(39, 174, 96, 0.1);"
        "}"
    );

    ui->label_card->setStyleSheet(
        "QLabel {"
        "    color: #9b59b6;"
        "    font-weight: bold;"
        "    padding: 5px 10px;"
        "    border-radius: 4px;"
        "    background: rgba(155, 89, 182, 0.1);"
        "}"
    );
}

MainWindow::~MainWindow()
{
    HardwareService::getInstance()->shutdown();
    NetworkManager::getInstance()->disconnectFromServer();

    if (m_hwWatchdog) {
        hw_watchdog_destroy(m_hwWatchdog);
        m_hwWatchdog = nullptr;
    }

    delete ui;
}

void MainWindow::initSystem()
{
    if (!LocalDBManager::getInstance()->initDatabase()) {
        QMessageBox::warning(this, "错误", "本地数据库初始化失败！");
    }

    HardwareService *hw = HardwareService::getInstance();
    connect(hw, &HardwareService::signalLightChanged,
            this, &MainWindow::slotUpdateLight);
    connect(hw, &HardwareService::signalCardDetected,
            this, &MainWindow::slotGetCardUID);

    connect(NetworkManager::getInstance(), &NetworkManager::signalConnected,
            this, &MainWindow::slotConnected);
    connect(NetworkManager::getInstance(), &NetworkManager::signalDisconnected,
            this, &MainWindow::slotDisconnected);
    connect(NetworkManager::getInstance(), &NetworkManager::signalError,
            this, [this](QString error) {
        statusBar()->showMessage("网络错误: " + error, 5000);
    });

    ClientConfig *cfg = ClientConfig::getInstance();
    NetworkManager::getInstance()->setLocalPort(cfg->getLocalPort());

    // OtaUpdater and ClientService must be initialized before connectToServer,
    // otherwise signalConnected fires before ClientService is wired up
    m_otaUpdater = new OtaUpdater(this);
    m_otaUpdater->init(QCoreApplication::applicationDirPath(), "1.0.0");
    ClientService::getInstance()->init(m_otaUpdater);

    NetworkManager::getInstance()->connectToServer(cfg->getServerIP(), cfg->getServerPort());

    if (!CrashHandler::checkStartupSafety("/var/log/retail/dump")) {
        LOGE("Startup breaker tripped: too many recent crashes, exiting");
        QMessageBox::critical(this, "启动失败",
            "程序近期崩溃过于频繁，已触发熔断保护。\n"
            "请等待5分钟后再试，或联系管理员检查系统。");
        /* Exit code 42 = breaker tripped; systemd RestartPreventExitStatus=42 prevents restart */
        QTimer::singleShot(0, this, []() { QApplication::exit(42); });
        return;
    }

    m_crashHandler = new CrashHandler(this);
    if (m_crashHandler->init("/var/log/retail/dump", 30, 5)) {
        m_crashHandler->start();
        LOGI("Watchdog daemon started");
    }

    m_hwWatchdog = hw_watchdog_create(60);
    if (m_hwWatchdog) {
        hw_watchdog_start(m_hwWatchdog);
    }

    connect(m_otaUpdater, &OtaUpdater::signalProgressChanged,
            this, [this](ota_state_t state, int progress, const QString &msg) {
        Q_UNUSED(state);
        statusBar()->showMessage(
            QString("OTA: %1 (%2%)").arg(msg).arg(progress), 3000);
    });

    connect(m_otaUpdater, &OtaUpdater::signalUpdateAvailable,
            this, [this](const QString &version, const QString &desc) {
        LOGI("New version found: %s - %s",
             version.toUtf8().constData(), desc.toUtf8().constData());
        statusBar()->showMessage(
            QString("发现新版本 %1，正在安装...").arg(version), 3000);
    });

    connect(m_otaUpdater, &OtaUpdater::signalUpdateFinished,
            this, [this](bool success, const QString &msg) {
        if (success) {
            LOGI("OTA upgrade succeeded: %s", msg.toUtf8().constData());
            statusBar()->showMessage("升级成功，即将重启...", 3000);
            /* Quit directly; systemd Restart=always relaunches the new version.
             * Avoid QProcess::startDetached to prevent dual-process conflicts. */
            QTimer::singleShot(2000, this, [this]() {
                QApplication::quit();
            });
        } else {
            LOGW("OTA upgrade result: %s", msg.toUtf8().constData());
        }
    });

}

void MainWindow::connectServices()
{
    ClientService *cs = ClientService::getInstance();

    connect(cs, &ClientService::signalStatusMessage,
            this, &MainWindow::slotStatusMessage);

    connect(cs, &ClientService::signalOtaPush,
            this, [this](const QString &version, const QString &filename,
                         const QString &sha256, int fileSize, const QString &desc, int type) {
        Q_UNUSED(desc);
        if (m_otaUpdater) {
            m_otaUpdater->slotStartTcpDownload(version, filename, sha256, fileSize, type);
        }
    });

    connect(cs, &ClientService::signalOtaChunk,
            this, [this](int chunkIndex, int totalChunks, const QByteArray &data) {
        if (m_otaUpdater) {
            m_otaUpdater->slotReceiveChunk(chunkIndex, totalChunks, data);
        }
    });

    connect(m_otaUpdater, &OtaUpdater::signalRequestOtaFile,
            this, [this](const QString &version, const QString &filename) {
        ClientService::getInstance()->requestOtaFile(version, filename);
    });

    /* Monitor commands: sent from server -> executed by FaceManager */
    connect(cs, &ClientService::signalMonitorStart,
            m_faceManager, &FaceManager::startMonitor);
    connect(cs, &ClientService::signalMonitorStop,
            m_faceManager, &FaceManager::stopMonitor);
}

void MainWindow::initSubPages()
{
    m_memberRegister = new MemberRegister(this);
    m_goodsManager = new GoodsManager(nullptr);  // No parent: pops up as an independent window
    m_goodsManager->setWindowFlags(Qt::Window);
    m_goodsManager->setWindowTitle("商品管理");
    m_goodsManager->resize(900, 650);
    m_settlementWidget = new SettlementWidget(this);
    m_orderQuery = new OrderQuery(this);
    m_rechargeWidget = new RechargeWidget(this);
    m_mockPayment = new MockPaymentProvider();
    m_rechargeWidget->setPaymentProvider(m_mockPayment);
    m_voiceEngine = new VoiceEngine(this);
    m_voiceEngine->init();

    ui->tabWidget->addTab(m_memberRegister, "会员注册");
    // Goods management is no longer a tab; replaced by a button + admin auth dialog
    ui->tabWidget->addTab(m_settlementWidget, "选购商品");
    ui->tabWidget->addTab(m_orderQuery, "订单查询");
    ui->tabWidget->addTab(m_rechargeWidget, "余额充值");

    while (ui->tabWidget->count() > 4) {
        ui->tabWidget->removeTab(4);
    }
    for (int i = 0; i < 4; i++) {
        ui->tabWidget->removeTab(0);
    }
    ui->tabWidget->addTab(m_memberRegister, "会员注册");
    ui->tabWidget->addTab(m_settlementWidget, "选购商品");
    ui->tabWidget->addTab(m_orderQuery, "订单查询");
    ui->tabWidget->addTab(m_rechargeWidget, "余额充值");

    QPushButton *btnGoodsManage = new QPushButton("商品管理(管理员)", this);
    btnGoodsManage->setObjectName("btn_goods_manage");
    btnGoodsManage->setMinimumHeight(40);
    btnGoodsManage->setCursor(Qt::PointingHandCursor);
    btnGoodsManage->setStyleSheet(
        "QPushButton {"
        "    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #e67e22, stop:1 #d35400);"
        "    color: white; font-weight: bold; font-size: 15px; border-radius: 5px;"
        "    padding: 8px 20px;"
        "}"
        "QPushButton:hover {"
        "    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #f39c12, stop:1 #e67e22);"
        "}");
    ui->verticalLayout->insertWidget(1, btnGoodsManage);
    connect(btnGoodsManage, &QPushButton::clicked, this, &MainWindow::openGoodsManagerWithAuth);
}

void MainWindow::connectSignals()
{
    connect(this, &MainWindow::signalCardDetected,
            m_memberRegister, &MemberRegister::slotGetCardUID);
    connect(this, &MainWindow::signalCardDetected,
            m_settlementWidget, &SettlementWidget::slotGetCardUID);
    connect(this, &MainWindow::signalCardDetected,
            m_rechargeWidget, &RechargeWidget::setUid);

    connect(m_voiceEngine, &VoiceEngine::signalAddToCart,
            this, [this](const QString &goodsName, int qty) {
        m_settlementWidget->refreshGoodsTable();
        int goodsId = m_settlementWidget->findComboIndex(goodsName);
        if (goodsId < 0) {
            statusBar()->showMessage(
                QString("未找到商品: %1，请确认库存中是否有此商品").arg(goodsName), 3000);
            return;
        }
        for (int i = 0; i < qty; i++) {
            m_settlementWidget->voiceAddToCart(goodsId);
        }
        double total = m_settlementWidget->getCartTotal();
        statusBar()->showMessage(
            QString("已添加 %1 x%2，购物车共 ￥%3。继续选购或说 结账/结束")
                .arg(goodsName).arg(qty).arg(total, 0, 'f', 1), 4000);
        ui->tabWidget->setCurrentWidget(m_settlementWidget);
    });

    connect(m_voiceEngine, &VoiceEngine::signalCheckBalance, this, [this]() {
        if (m_settlementWidget->isMemberAuthenticated()) {
            double bal = m_settlementWidget->getMemberBalance();
            QString name = m_settlementWidget->getMemberName();
            statusBar()->showMessage(
                QString("%1 当前余额: ￥%2").arg(name).arg(bal, 0, 'f', 2), 5000);
        } else {
            statusBar()->showMessage("请先刷卡认证会员，再查询余额", 4000);
            ui->tabWidget->setCurrentWidget(m_settlementWidget);
        }
    });

    connect(m_voiceEngine, &VoiceEngine::signalOpenPayment, this, [this]() {
        if (m_settlementWidget->isCartEmpty()) {
            statusBar()->showMessage("购物车为空，请先说 买一瓶可乐 选购商品", 4000);
            return;
        }
        if (!m_settlementWidget->isMemberAuthenticated()) {
            statusBar()->showMessage("请先刷卡认证会员！将卡靠近读卡器", 4000);
            ui->tabWidget->setCurrentWidget(m_settlementWidget);
            return;
        }
        double total = m_settlementWidget->getCartTotal();
        double bal = m_settlementWidget->getMemberBalance();
        QString name = m_settlementWidget->getMemberName();
        statusBar()->showMessage(
            QString("共 ￥%1，%2 余额 ￥%3。请刷脸或输入密码完成支付")
                .arg(total, 0, 'f', 1).arg(name).arg(bal, 0, 'f', 2), 6000);
        ui->tabWidget->setCurrentWidget(m_settlementWidget);
    });

    connect(m_voiceEngine, &VoiceEngine::signalClearCart, this, [this]() {
        QMetaObject::invokeMethod(m_settlementWidget, "on_btn_clear_cart_clicked",
                                  Qt::QueuedConnection);
        statusBar()->showMessage("购物车已清空。可以重新选购", 3000);
    });

    connect(m_voiceEngine, &VoiceEngine::signalOpenRecharge,
            this, [this](int amount) {
        m_voiceRechargeRequested = true;
        if (amount > 0) {
            m_rechargeWidget->setRechargeAmount(amount);
        }
        ui->tabWidget->setCurrentWidget(m_rechargeWidget);
        statusBar()->showMessage(
            amount > 0
                ? QString("已切换到余额充值页面，金额: ¥%1。说 结束 完成充值").arg(amount)
                : "已切换到余额充值页面。说 结束 完成充值", 3000);
    });

    connect(m_voiceEngine, &VoiceEngine::signalShowRecommend, this, [this]() {
        m_settlementWidget->refreshGoodsTable();
        ui->tabWidget->setCurrentWidget(m_settlementWidget);
        QStringList recs = m_voiceEngine->getRecommendList();
        QString msg = recs.join("  |  ");
        statusBar()->showMessage("热销推荐: " + msg, 8000);
    });

    connect(m_voiceEngine, &VoiceEngine::signalOpenRegister, this, [this]() {
        ui->tabWidget->setCurrentWidget(m_memberRegister);
    });

    connect(m_voiceEngine, &VoiceEngine::signalHelpRequested, this, [this]() {
        QStringList help = m_voiceEngine->getHelpText();
        QMessageBox::information(this, "语音帮助", help.join("\n"));
    });

    connect(m_voiceEngine, &VoiceEngine::signalPlayTTS, this, [this](const QString &text) {
        statusBar()->showMessage(text, 3000);
    });

    connect(m_voiceEngine, &VoiceEngine::signalEndSession, this, [this]() {
        endVoiceSession();
    });

    m_voiceIdleTimer->setSingleShot(true);
    connect(m_voiceIdleTimer, &QTimer::timeout, this, &MainWindow::abortVoiceSession);

    connect(m_voiceEngine, &VoiceEngine::signalVoiceRecognized,
            this, [this](const QString &text) {
        ui->lineEdit_voice->setText(text);
        statusBar()->showMessage("识别结果: " + text, 2000);
        QTimer::singleShot(600, this, [this]() {
            if (!ui->lineEdit_voice->text().isEmpty()) {
                on_btn_voice_send_clicked();
            }
        });
    });

    connect(m_voiceEngine, &VoiceEngine::signalVoicePartial,
            this, [this](const QString &text) {
        ui->lineEdit_voice->setText(text);
        statusBar()->showMessage(" 正在听: " + text, 1500);
    });

    connect(ui->lineEdit_voice, &QLineEdit::returnPressed,
            this, &MainWindow::on_btn_voice_send_clicked);

    ClientService *cs = ClientService::getInstance();
    connect(cs, &ClientService::signalMemberQueryResult,
            m_settlementWidget, &SettlementWidget::onMemberQueryResult);
    connect(cs, &ClientService::signalPasswordVerifyResult,
            m_settlementWidget, &SettlementWidget::onPasswordVerifyResult);
    connect(cs, &ClientService::signalOrderCreateResult,
            m_settlementWidget, &SettlementWidget::onOrderCreateResult);
    connect(cs, &ClientService::signalMemberQueryResult,
            m_rechargeWidget, &RechargeWidget::onMemberQueryResult);
    connect(cs, &ClientService::signalMemberRechargeResult,
            m_rechargeWidget, &RechargeWidget::onMemberRechargeResult);
    connect(cs, &ClientService::signalOrderQueryResult,
            m_orderQuery, &OrderQuery::onOrderQueryResult);
    connect(cs, &ClientService::signalMemberQueryResult,
            m_memberRegister, &MemberRegister::onMemberQueryResult);
    connect(cs, &ClientService::signalMemberRegisterResult,
            m_memberRegister, &MemberRegister::onMemberRegisterResult);
    connect(cs, &ClientService::signalFaceVerifyResult,
            m_settlementWidget, [this](int code, QString msg) {
        Q_UNUSED(code); Q_UNUSED(msg);
    });

    connect(NetworkManager::getInstance(), &NetworkManager::signalReconnected,
            m_goodsManager, &GoodsManager::refreshGoodsTable);

    connect(m_settlementWidget, &SettlementWidget::signalStockChanged,
            m_goodsManager, &GoodsManager::refreshGoodsTable);
    // Auto-refresh the goods list on the shopping page after a successful purchase
    connect(m_settlementWidget, &SettlementWidget::signalStockChanged,
            m_settlementWidget, &SettlementWidget::refreshGoodsTable);
}

void MainWindow::slotStatusMessage(QString msg, int timeoutMs)
{
    statusBar()->showMessage(msg, timeoutMs);
}

void MainWindow::slotUpdateLight(int lux)
{
    ui->label_light->setText(QString("光照: %1 lux").arg(lux));
}

void MainWindow::slotGetCardUID(QString uid)
{
    ui->label_card->setText(QString("IC卡: %1").arg(uid));
    statusBar()->showMessage(QString("检测到IC卡: %1").arg(uid), 3000);
    emit signalCardDetected(uid);
}

void MainWindow::slotConnected()
{
    ui->label_status->setText("状态: 已连接");
    ui->label_status->setStyleSheet("color: green;");
    ui->btn_connect->setEnabled(false);
    ui->btn_disconnect->setEnabled(true);
    statusBar()->showMessage("已连接到服务端", 3000);

    /* Auto-start monitor streaming on connect (for dev/debug).
     * Client streams to local mediamtx (via SSH tunnel to VM port 8554). */
    QString rtspUrl = QString("rtsp://127.0.0.1:8554/client_01");
    m_faceManager->startMonitor(rtspUrl);
    qDebug() << "Auto-start monitor stream:" << rtspUrl;
}

void MainWindow::slotDisconnected()
{
    ui->label_status->setText("状态: 未连接");
    ui->label_status->setStyleSheet("color: red;");
    ui->btn_connect->setEnabled(true);
    ui->btn_disconnect->setEnabled(false);
    statusBar()->showMessage("与服务端断开连接", 3000);
}

void MainWindow::on_btn_connect_clicked()
{
    ClientConfig *cfg = ClientConfig::getInstance();
    NetworkManager::getInstance()->setLocalPort(cfg->getLocalPort());
    bool connected = NetworkManager::getInstance()->connectToServer(
        cfg->getServerIP(), cfg->getServerPort());
    if (!connected) {
        statusBar()->showMessage("连接服务端失败，正在重试...", 5000);
    }
}

void MainWindow::on_btn_disconnect_clicked()
{
    NetworkManager::getInstance()->disconnectFromServer();
}

void MainWindow::on_btn_check_update_clicked()
{
    if (!m_otaUpdater) {
        statusBar()->showMessage("OTA模块未初始化", 3000);
        return;
    }

    ClientService::getInstance()->checkOtaUpdate();
}

void MainWindow::on_btn_exit_clicked()
{
    if (QMessageBox::question(this, "确认", "确定要退出系统吗？") == QMessageBox::Yes) {
        this->close();
    }
}

void MainWindow::on_btn_voice_wake_clicked()
{
    if (m_voiceActive) {
        endVoiceSession();
    } else {
        beginVoiceSession();
    }
}

void MainWindow::beginVoiceSession()
{
    m_voiceActive = true;
    ui->btn_voice_wake->setText("聆听中·点击结束");
    ui->btn_voice_wake->setMinimumWidth(205);
    ui->btn_voice_wake->setMaximumWidth(205);
    ui->btn_voice_wake->setStyleSheet(
        "background: #f44336; color: white; font-weight: bold; font-size: 12px; "
        "border-radius: 4px; padding: 4px 12px;");
    ui->btn_voice_wake->setChecked(true);
    ui->lineEdit_voice->setEnabled(true);
    ui->lineEdit_voice->setVisible(true);
    ui->btn_voice_send->setEnabled(true);
    ui->btn_voice_send->setVisible(true);
    ui->btn_voice_simulate->setEnabled(true);
    ui->btn_voice_simulate->setVisible(true);
    ui->lineEdit_voice->setFocus();
    statusBar()->showMessage(
        "语音助手已唤醒！请说出指令，如: 买一瓶可乐、推荐、结账。说 结束 退出", 5000);

    m_voiceRechargeRequested = false;
    m_voiceIdleTimer->start(10000);
    m_voiceEngine->startVoiceListening();
}

void MainWindow::endVoiceSession()
{
    m_voiceIdleTimer->stop();
    m_voiceEngine->stopVoiceListening();

    m_voiceActive = false;
    ui->btn_voice_wake->setText("▶ 唤醒语音助手");
    ui->btn_voice_wake->setMinimumWidth(195);
    ui->btn_voice_wake->setMaximumWidth(195);
    ui->btn_voice_wake->setStyleSheet(
        "background: #4CAF50; color: white; font-weight: bold; font-size: 12px; "
        "border-radius: 4px; padding: 4px 12px;");
    ui->btn_voice_wake->setChecked(false);
    ui->lineEdit_voice->setEnabled(false);
    ui->lineEdit_voice->setVisible(false);
    ui->lineEdit_voice->clear();
    ui->btn_voice_send->setEnabled(false);
    ui->btn_voice_send->setVisible(false);
    ui->btn_voice_simulate->setEnabled(false);
    ui->btn_voice_simulate->setVisible(false);

    if (!m_settlementWidget->isCartEmpty()) {
        double total = m_settlementWidget->getCartTotal();
        ui->tabWidget->setCurrentWidget(m_settlementWidget);
        statusBar()->showMessage(
            QString("购物车共 ￥%1。请输入UID或刷卡认证会员，然后刷脸/输密码完成支付")
                .arg(total, 0, 'f', 1), 6000);
    } else if (m_voiceRechargeRequested) {
        ui->tabWidget->setCurrentWidget(m_rechargeWidget);
        statusBar()->showMessage("请在充值页面输入UID和充值金额，按确认完成充值", 5000);
    } else {
        statusBar()->showMessage("语音助手已关闭", 4000);
    }
}

void MainWindow::on_btn_voice_send_clicked()
{
    if (!m_voiceActive) return;

    QString text = ui->lineEdit_voice->text().trimmed();
    if (text.isEmpty()) return;

    statusBar()->showMessage("语音输入: " + text, 2000);
    resetVoiceIdleTimer();
    VoiceResult result = m_voiceEngine->processCommand(text);

    if (result.matched) {
        if (result.category == "buy" || result.category == "buy_failed") {
        } else if (result.category == "end_session") {
        } else if (result.category == "checkout") {
            if (m_settlementWidget->isCartEmpty())
                statusBar()->showMessage("购物车为空，请先说 买一瓶可乐 选购商品", 4000);
            else if (!m_settlementWidget->isMemberAuthenticated())
                statusBar()->showMessage("请先刷卡认证会员！", 4000);
            else
                statusBar()->showMessage(
                    QString("共 ￥%1，请刷脸或输入密码支付")
                        .arg(m_settlementWidget->getCartTotal(), 0, 'f', 1), 6000);
        } else {
            statusBar()->showMessage(
                QString("意图: %1 ← 继续选购或说 结账/结束").arg(result.category), 3000);
        }
    } else {
        statusBar()->showMessage(
            QString("未识别: \"%1\"。试试: 买一瓶可乐、推荐、结账、充值五十块、结束").arg(text),
            4000);
    }

    ui->lineEdit_voice->clear();
    ui->lineEdit_voice->setFocus();
}

void MainWindow::on_btn_voice_simulate_clicked()
{
    if (!m_voiceActive) return;

    bool ok;
    QString text = QInputDialog::getText(this, "模拟语音输入",
        "输入你想说的话（模拟麦克风识别结果）:",
        QLineEdit::Normal, "买一瓶可乐", &ok);
    if (ok && !text.trimmed().isEmpty()) {
        m_voiceEngine->simulateVoiceInput(text.trimmed());
    }
}

void MainWindow::resetVoiceIdleTimer()
{
    if (m_voiceActive) {
        m_voiceIdleTimer->start(15000);
    }
}

void MainWindow::abortVoiceSession()
{
    if (!m_voiceActive) return;

    m_voiceIdleTimer->stop();
    m_voiceEngine->stopVoiceListening();

    QMetaObject::invokeMethod(m_settlementWidget, "on_btn_clear_cart_clicked",
                              Qt::QueuedConnection);
    m_rechargeWidget->resetToDefault();
    m_voiceRechargeRequested = false;

    m_voiceActive = false;
    ui->btn_voice_wake->setText("▶ 唤醒语音助手");
    ui->btn_voice_wake->setMinimumWidth(195);
    ui->btn_voice_wake->setMaximumWidth(195);
    ui->btn_voice_wake->setStyleSheet(
        "background: #4CAF50; color: white; font-weight: bold; font-size: 12px; "
        "border-radius: 4px; padding: 4px 12px;");
    ui->btn_voice_wake->setChecked(false);
    ui->lineEdit_voice->setEnabled(false);
    ui->lineEdit_voice->setVisible(false);
    ui->lineEdit_voice->clear();
    ui->btn_voice_send->setEnabled(false);
    ui->btn_voice_send->setVisible(false);
    ui->btn_voice_simulate->setEnabled(false);
    ui->btn_voice_simulate->setVisible(false);
    statusBar()->showMessage("10秒无操作，语音助手已自动关闭。购物车和充值信息已清空", 5000);
}

void MainWindow::openGoodsManagerWithAuth()
{
    QDialog loginDialog(this);
    loginDialog.setWindowTitle("管理员认证");
    loginDialog.setFixedSize(380, 200);
    loginDialog.setStyleSheet(
        "QDialog { background: #f5f7fa; }"
        "QLabel { font-size: 14px; color: #2c3e50; }"
        "QLineEdit { border: 2px solid #bdc3c7; border-radius: 5px; padding: 8px; font-size: 14px; }"
        "QLineEdit:focus { border-color: #3498db; }"
        "QPushButton { background: #3498db; color: white; border: none; border-radius: 5px;"
        "  padding: 8px 20px; font-weight: bold; font-size: 14px; }"
        "QPushButton:hover { background: #2980b9; }");

    QFormLayout *form = new QFormLayout(&loginDialog);
    form->setContentsMargins(30, 20, 30, 10);
    form->setSpacing(12);

    QLineEdit *editUid = new QLineEdit();
    editUid->setPlaceholderText("请输入管理员UID");

    QLineEdit *editPwd = new QLineEdit();
    editPwd->setEchoMode(QLineEdit::Password);
    editPwd->setPlaceholderText("请输入管理员密码");

    form->addRow("管理员UID:", editUid);
    form->addRow("管理员密码:", editPwd);

    QHBoxLayout *btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(20);
    QPushButton *btnOk = new QPushButton("登录");
    QPushButton *btnCancel = new QPushButton("取消");
    btnOk->setDefault(true);
    btnOk->setStyleSheet("background: #27ae60; color: white; padding: 8px 30px;");
    btnOk->setCursor(Qt::PointingHandCursor);
    btnCancel->setStyleSheet("background: #95a5a6; color: white; padding: 8px 30px;");
    btnCancel->setCursor(Qt::PointingHandCursor);
    btnLayout->addStretch();
    btnLayout->addWidget(btnOk);
    btnLayout->addWidget(btnCancel);
    form->addRow(btnLayout);

    connect(btnOk, &QPushButton::clicked, &loginDialog, &QDialog::accept);
    connect(btnCancel, &QPushButton::clicked, &loginDialog, &QDialog::reject);

    if (loginDialog.exec() != QDialog::Accepted) return;

    QString uid = editUid->text().trimmed();
    QString password = editPwd->text().trimmed();
    if (uid.isEmpty() || password.isEmpty()) {
        QMessageBox::warning(this, "输入错误", "UID和密码不能为空！");
        return;
    }

    // Wire a one-shot signal to wait for the server auth result
    QMetaObject::Connection *conn = new QMetaObject::Connection();
    *conn = connect(ClientService::getInstance(), &ClientService::signalGoodsManagementAuthResult,
        this, [this, conn](int code, QString msg) {
            disconnect(*conn);
            delete conn;
            if (code == 0) {
                m_goodsManager->refreshGoodsTable();
                m_goodsManager->show();
                m_goodsManager->raise();
                m_goodsManager->activateWindow();
                statusBar()->showMessage("管理员认证成功，商品管理页面已打开", 3000);
            } else {
                QMessageBox::warning(this, "认证失败", msg);
            }
        });

    ClientService::getInstance()->requestGoodsManagementAuth(uid, password);
}
