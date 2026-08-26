#ifndef RECHARGEWIDGET_H
#define RECHARGEWIDGET_H

#include <QWidget>
#include <functional>
#include <cJSON.h>
#include "common.h"

QT_BEGIN_NAMESPACE
namespace Ui { class RechargeWidget; }
QT_END_NAMESPACE

class PaymentProvider;

class RechargeWidget : public QWidget
{
    Q_OBJECT

public:
    explicit RechargeWidget(QWidget *parent = nullptr);
    ~RechargeWidget();

    void setPaymentProvider(PaymentProvider* provider);

    void setUid(const QString& uid);
    void setRechargeAmount(double amount);
    void resetToDefault();

public slots:
    void onMemberQueryResult(int code, QString msg, member_info_t member);
    void onMemberRechargeResult(int code, QString msg);
    void onPasswordVerifyResult(int code, QString msg);

private slots:
    void on_btn_query_member_clicked();
    void on_btn_recharge_clicked();

private:
    Ui::RechargeWidget *ui;
    PaymentProvider* m_paymentProvider;
    QString m_currentUid;
    double m_currentBalance;
    bool m_authenticated;
    QString m_pendingPassword;

    void applyStyle();
    void resetAuth();
    void updateBalanceDisplay();
};

#endif // RECHARGEWIDGET_H
