#ifndef SETTLEMENTWIDGET_H
#define SETTLEMENTWIDGET_H

#include <QWidget>
#include <QSpinBox>
#include <QPushButton>
#include "core/facemanager.h"
#include "common.h"
#include <cJSON.h>

class HardwareService;

QT_BEGIN_NAMESPACE
namespace Ui { class SettlementWidget; }
QT_END_NAMESPACE

class SettlementWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SettlementWidget(QWidget *parent = nullptr);
    ~SettlementWidget();

    int findComboIndex(const QString &goodsName) const;
    void voiceAddToCart(int goodsId);
    void refreshGoodsTable();
    bool isMemberAuthenticated() const { return m_memberAuthenticated; }
    bool isCartEmpty() const { return m_cartList.isEmpty(); }
    double getCartTotal() const;
    double getMemberBalance() const { return m_currentMember.balance; }
    QString getMemberName() const { return QString(m_currentMember.name); }

protected:
    void showEvent(QShowEvent *event) override;

signals:
    void signalStockChanged();

public slots:
    void slotGetCardUID(QString uid);
    void onMemberQueryResult(int code, QString msg, member_info_t member);
    void onPasswordVerifyResult(int code, QString msg);
    void onOrderCreateResult(int code, QString msg, QString orderId);
    void notifyServerGoodsChanged();

private slots:
    void on_btn_clear_cart_clicked();
    void on_btn_face_pay_clicked();
    void on_btn_password_pay_clicked();
    void on_btn_query_member_clicked();
    void calculateTotalPrice();

private:
    Ui::SettlementWidget *ui;
    FaceManager* m_faceManager;
    member_info_t m_currentMember;
    QList<order_item_t> m_cartList;
    QList<goods_info_t> m_goodsList;
    bool m_memberAuthenticated;
    QString m_pendingPassword;
    QString m_pendingOrderId;
    double m_pendingTotal;

    void processPayment();
    void clearMemberInfo();
};

#endif // SETTLEMENTWIDGET_H