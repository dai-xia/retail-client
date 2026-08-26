#include "rechargewidget.h"
#include "ui_rechargewidget.h"
#include "config/payment_provider.h"
#include "services/clientservice.h"
#include "stylesheet.h"
#include <QMessageBox>
#include <QDateTime>
#include <QTimer>
#include <cJSON.h>

RechargeWidget::RechargeWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::RechargeWidget)
    , m_paymentProvider(nullptr)
    , m_currentBalance(0.0)
    , m_authenticated(false)
{
    ui->setupUi(this);
    applyStyle();
    resetAuth();
}

RechargeWidget::~RechargeWidget()
{
    delete ui;
}

void RechargeWidget::setPaymentProvider(PaymentProvider* provider)
{
    m_paymentProvider = provider;
}

void RechargeWidget::setUid(const QString& uid)
{
    resetAuth();
    ui->lineEdit_uid->setText(uid);
    ui->lineEdit_password->setFocus();
}

void RechargeWidget::setRechargeAmount(double amount)
{
    if (amount > 0 && amount <= 9999) {
        ui->doubleSpinBox_amount->setValue(amount);
    }
}

void RechargeWidget::resetToDefault()
{
    resetAuth();
    ui->doubleSpinBox_amount->setValue(0);
    ui->lineEdit_uid->clear();
    ui->lineEdit_password->clear();
}

void RechargeWidget::applyStyle()
{
    RetailStyle::apply(this);
}

void RechargeWidget::resetAuth()
{
    m_authenticated = false;
    m_currentUid.clear();
    m_pendingPassword.clear();
    m_currentBalance = 0.0;
    ui->label_name->setText("--");
    ui->label_balance->setText("0.00 元");
    ui->label_status->setText("请先认证会员身份");
    ui->label_status->setStyleSheet("color: #7f8c8d;");
    ui->btn_recharge->setEnabled(false);
}

void RechargeWidget::updateBalanceDisplay()
{
    ui->label_balance->setText(QString("%1 元").arg(m_currentBalance, 0, 'f', 2));
}

void RechargeWidget::on_btn_query_member_clicked()
{
    QString uid = ui->lineEdit_uid->text().trimmed();
    QString password = ui->lineEdit_password->text().trimmed();

    if(uid.isEmpty())
    {
        QMessageBox::warning(this, "警告", "请输入IC卡UID！");
        return;
    }

    if(password.isEmpty())
    {
        QMessageBox::warning(this, "警告", "请输入登录密码！");
        return;
    }

    m_currentUid = uid;
    m_pendingPassword = password;
    ClientService::getInstance()->requestMemberQuery(uid);

    ui->label_status->setText("正在查询会员信息...");
    ui->label_status->setStyleSheet("color: #2c3e50;");
}

void RechargeWidget::on_btn_recharge_clicked()
{
    if(!m_authenticated)
    {
        QMessageBox::warning(this, "警告", "请先认证会员身份！");
        return;
    }

    double amount = ui->doubleSpinBox_amount->value();
    if(amount <= 0)
    {
        QMessageBox::warning(this, "警告", "充值金额必须大于0！");
        return;
    }

    int payMethod = ui->comboBox_pay_method->currentIndex();
    if(payMethod == 1 || payMethod == 2)
    {
        QMessageBox::information(this, "提示", "支付宝/微信支付功能暂未接入，请使用模拟支付。");
        return;
    }

    if(!m_paymentProvider)
    {
        QMessageBox::warning(this, "错误", "支付服务未初始化！");
        return;
    }

    ui->btn_recharge->setEnabled(false);
    ui->label_status->setText("正在处理支付...");
    ui->label_status->setStyleSheet("color: #2c3e50;");

    QString orderId = QString("RECHARGE_%1_%2")
                      .arg(m_currentUid)
                      .arg(QDateTime::currentDateTime().toString("yyyyMMddHHmmss"));

    m_paymentProvider->pay(amount, orderId, [this, amount](bool ok, const QString& msg) {
        if (!ok) {
            ui->label_status->setText("支付失败: " + msg);
            ui->label_status->setStyleSheet("color: #e94560;");
            ui->btn_recharge->setEnabled(true);
            return;
        }

        ClientService::getInstance()->requestMemberRecharge(m_currentUid, amount);

        m_currentBalance += amount;
        updateBalanceDisplay();

        ui->label_status->setText(QString("充值成功！金额: ¥%1").arg(amount, 0, 'f', 2));
        ui->label_status->setStyleSheet("color: #27ae60; font-weight: bold;");
        ui->btn_recharge->setEnabled(true);
    });
}

void RechargeWidget::onMemberQueryResult(int code, QString msg, member_info_t member)
{
    if (code == 0) {
        ui->label_name->setText(member.name);
        m_currentBalance = member.balance;
        updateBalanceDisplay();

        if (!m_pendingPassword.isEmpty()) {
            ClientService::getInstance()->requestPasswordVerify(m_currentUid, m_pendingPassword);
            ui->label_status->setText("正在验证密码...");
            ui->label_status->setStyleSheet("color: #2c3e50;");
        } else {
            m_authenticated = true;
            ui->label_status->setText("会员认证成功，可以进行充值");
            ui->label_status->setStyleSheet("color: #27ae60; font-weight: bold;");
            ui->btn_recharge->setEnabled(true);
        }
    } else {
        resetAuth();
        ui->label_status->setText("会员认证失败: " + msg);
        ui->label_status->setStyleSheet("color: #e94560;");
    }
}

void RechargeWidget::onPasswordVerifyResult(int code, QString msg)
{
    Q_UNUSED(msg);
    if (code == 0) {
        m_pendingPassword.clear();
        m_authenticated = true;
        ui->label_status->setText("会员认证成功，可以进行充值");
        ui->label_status->setStyleSheet("color: #27ae60; font-weight: bold;");
        ui->btn_recharge->setEnabled(true);
    } else {
        m_pendingPassword.clear();
        resetAuth();
        ui->label_status->setText("密码验证失败！");
        ui->label_status->setStyleSheet("color: #e94560;");
    }
}

void RechargeWidget::onMemberRechargeResult(int code, QString msg)
{
    if (code == 0) {
        ui->label_status->setText("充值成功！");
        ui->label_status->setStyleSheet("color: #27ae60; font-weight: bold;");
        QTimer::singleShot(2000, this, [this]() {
            resetToDefault();
        });
    } else {
        m_currentBalance -= ui->doubleSpinBox_amount->value();
        updateBalanceDisplay();
        ui->label_status->setText("充值失败: " + msg);
        ui->label_status->setStyleSheet("color: #e94560;");
        ui->btn_recharge->setEnabled(true);
    }
}
