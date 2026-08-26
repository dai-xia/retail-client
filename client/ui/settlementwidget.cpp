#include "settlementwidget.h"
#include "ui_settlementwidget.h"
#include "facepaywidget.h"
#include "core/hardwareservice.h"
#include "services/clientservice.h"
#include <QMessageBox>
#include <QInputDialog>
#include <QHeaderView>
#include <QShowEvent>
#include <QDebug>
#include <QHBoxLayout>
#include <cJSON.h>

SettlementWidget::SettlementWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::SettlementWidget)
    , m_faceManager(FaceManager::getInstance())
    , m_memberAuthenticated(false)
{
    ui->setupUi(this);

    // Goods list : cart = 3:2 width ratio
    ui->splitter_main->setStretchFactor(0, 3);
    ui->splitter_main->setStretchFactor(1, 2);

    memset(&m_currentMember, 0, sizeof(member_info_t));

    ui->tableWidget_cart->setColumnCount(4);
    ui->tableWidget_cart->setHorizontalHeaderLabels({"商品ID", "商品名称", "数量", "小计"});
    ui->tableWidget_cart->horizontalHeader()->setStretchLastSection(true);

    ui->tableWidget_goods->setColumnCount(4);
    ui->tableWidget_goods->setHorizontalHeaderLabels({"商品名称", "单价", "库存", "操作"});
    ui->tableWidget_goods->horizontalHeader()->setStretchLastSection(true);
    ui->tableWidget_goods->setSelectionBehavior(QAbstractItemView::SelectRows);

    refreshGoodsTable();
}

SettlementWidget::~SettlementWidget()
{
    delete ui;
}

void SettlementWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refreshGoodsTable();
}

void SettlementWidget::clearMemberInfo()
{
    memset(&m_currentMember, 0, sizeof(member_info_t));
    m_memberAuthenticated = false;
    m_pendingPassword.clear();
    ui->label_member_info->setText("请先认证会员");
    ui->lineEdit_password->clear();
}

void SettlementWidget::slotGetCardUID(QString uid)
{
    ui->lineEdit_uid->setText(uid);

    ClientService::getInstance()->requestMemberQuery(uid);
}

int SettlementWidget::findComboIndex(const QString &goodsName) const
{
    for (int i = 0; i < m_goodsList.size(); i++) {
        if (QString(m_goodsList[i].name).contains(goodsName))
            return m_goodsList[i].id;
    }
    return -1;
}

void SettlementWidget::voiceAddToCart(int goodsId)
{
    for (int i = 0; i < m_goodsList.size(); i++) {
        if (m_goodsList[i].id == goodsId) {
            const goods_info_t &g = m_goodsList[i];
            if (g.stock < 1) return;

            order_item_t item;
            memset(&item, 0, sizeof(order_item_t));
            item.goods_id = g.id;
            strncpy(item.goods_name, g.name, sizeof(item.goods_name)-1);
            item.num = 1;
            item.price = g.price;
            item.subtotal = g.price;
            m_cartList.append(item);

            int row = ui->tableWidget_cart->rowCount();
            ui->tableWidget_cart->insertRow(row);
            ui->tableWidget_cart->setItem(row, 0, new QTableWidgetItem(QString::number(item.goods_id)));
            ui->tableWidget_cart->setItem(row, 1, new QTableWidgetItem(item.goods_name));
            ui->tableWidget_cart->setItem(row, 2, new QTableWidgetItem(QString::number(item.num)));
            ui->tableWidget_cart->setItem(row, 3, new QTableWidgetItem(QString::number(item.subtotal, 'f', 2)));
            calculateTotalPrice();
            return;
        }
    }
}

double SettlementWidget::getCartTotal() const
{
    double total = 0;
    for (const auto& item : m_cartList) {
        total += item.subtotal;
    }
    return total;
}

void SettlementWidget::onMemberQueryResult(int code, QString msg, member_info_t member)
{
    if (code == 0) {
        m_currentMember = member;
        m_memberAuthenticated = true;
        ui->label_member_info->setText(QString("会员：%1  余额：%2元")
            .arg(member.name).arg(member.balance, 0, 'f', 2));
    } else {
        QMessageBox::warning(this, "查询失败", "会员不存在！");
        clearMemberInfo();
    }
}

void SettlementWidget::onPasswordVerifyResult(int code, QString msg)
{
    Q_UNUSED(msg);
    if (code == 0) {
        if (!m_pendingPassword.isEmpty()) {
            m_pendingPassword.clear();
            processPayment();
        }
    } else {
        QMessageBox::warning(this, "认证失败", "密码错误！");
        m_pendingPassword.clear();
    }
}

void SettlementWidget::onOrderCreateResult(int code, QString msg, QString orderId)
{
    Q_UNUSED(orderId);
    if (code == 0) {
        bool deductOk = true;
        for (const auto& item : m_cartList) {
            if (ClientService::getInstance()->localDeductStock(item.goods_id, item.num) != 0) {
                qWarning() << "Failed to deduct local goods stock:" << item.goods_name;
                deductOk = false;
                break;
            }
        }

        if (!deductOk) {
            QMessageBox::critical(this, "错误", "本地库存扣减失败，请联系管理员处理！");
            return;
        }

        m_currentMember.balance -= m_pendingTotal;
        ui->label_member_info->setText(QString("会员：%1  余额：%2元")
            .arg(m_currentMember.name).arg(m_currentMember.balance, 0, 'f', 2));

        QMessageBox::information(this, "成功", "支付成功！订单已创建！");

        HardwareService::getInstance()->rotateMotorForCart(m_cartList);

        ClientService::getInstance()->requestGoodsSyncReport();
        emit signalStockChanged();
        on_btn_clear_cart_clicked();
    } else {
        QMessageBox::warning(this, "支付失败", msg);
    }
}

void SettlementWidget::refreshGoodsTable()
{
    ClientService::getInstance()->localQueryAllGoods(&m_goodsList);

    ui->tableWidget_goods->setRowCount(0);

    for (int i = 0; i < m_goodsList.size(); i++) {
        const goods_info_t &g = m_goodsList[i];
        ui->tableWidget_goods->insertRow(i);
        ui->tableWidget_goods->setItem(i, 0, new QTableWidgetItem(g.name));
        ui->tableWidget_goods->setItem(i, 1, new QTableWidgetItem(QString::number(g.price, 'f', 2)));
        ui->tableWidget_goods->setItem(i, 2, new QTableWidgetItem(QString::number(g.stock)));

        // Column 3: embed SpinBox + Add button
        QWidget *opWidget = new QWidget();
        QHBoxLayout *opLayout = new QHBoxLayout(opWidget);
        opLayout->setContentsMargins(2, 2, 2, 2);
        opLayout->setSpacing(4);

        QSpinBox *spinBox = new QSpinBox();
        spinBox->setMinimum(1);
        spinBox->setMaximum(g.stock > 0 ? g.stock : 1);
        spinBox->setValue(1);
        spinBox->setFixedWidth(60);

        QPushButton *addBtn = new QPushButton("加入");
        addBtn->setFixedWidth(50);
        addBtn->setStyleSheet("background: #27ae60; color: white; border: none; border-radius: 3px; padding: 3px 6px;");

        // Capture i by value (lambda outlives the loop)
        int rowIndex = i;
        connect(addBtn, &QPushButton::clicked, this, [this, rowIndex, spinBox]() {
            int qty = spinBox->value();
            const goods_info_t &g = m_goodsList[rowIndex];
            if (g.stock < qty) {
                QMessageBox::warning(this, "警告", "库存不足！当前库存：" + QString::number(g.stock));
                return;
            }

            order_item_t item;
            memset(&item, 0, sizeof(order_item_t));
            item.goods_id = g.id;
            strncpy(item.goods_name, g.name, sizeof(item.goods_name)-1);
            item.num = qty;
            item.price = g.price;
            item.subtotal = g.price * qty;
            m_cartList.append(item);

            int cartRow = ui->tableWidget_cart->rowCount();
            ui->tableWidget_cart->insertRow(cartRow);
            ui->tableWidget_cart->setItem(cartRow, 0, new QTableWidgetItem(QString::number(item.goods_id)));
            ui->tableWidget_cart->setItem(cartRow, 1, new QTableWidgetItem(item.goods_name));
            ui->tableWidget_cart->setItem(cartRow, 2, new QTableWidgetItem(QString::number(item.num)));
            ui->tableWidget_cart->setItem(cartRow, 3, new QTableWidgetItem(QString::number(item.subtotal, 'f', 2)));
            calculateTotalPrice();
        });

        opLayout->addWidget(spinBox);
        opLayout->addWidget(addBtn);
        ui->tableWidget_goods->setCellWidget(i, 3, opWidget);
    }
}

void SettlementWidget::notifyServerGoodsChanged()
{
    ClientService::getInstance()->requestGoodsSyncReport();
}

void SettlementWidget::calculateTotalPrice()
{
    double total = 0;
    for(const auto& item : m_cartList)
    {
        total += item.subtotal;
    }
    ui->label_total->setText(QString("总价：%1元").arg(total, 0, 'f', 2));
}

void SettlementWidget::on_btn_clear_cart_clicked()
{
    m_cartList.clear();
    ui->tableWidget_cart->setRowCount(0);
    calculateTotalPrice();
    clearMemberInfo();
    refreshGoodsTable();
}

void SettlementWidget::processPayment()
{
    if(m_cartList.isEmpty())
    {
        QMessageBox::warning(this, "警告", "购物车为空！");
        return;
    }

    double total = 0;
    for(const auto& item : m_cartList)
    {
        total += item.subtotal;
    }

    if(m_currentMember.balance < total)
    {
        QMessageBox::warning(this, "警告", "余额不足！当前余额：" + QString::number(m_currentMember.balance, 'f', 2) + "元");
        return;
    }

    for(const auto& item : m_cartList)
    {
        goods_info_t g;
        if(ClientService::getInstance()->localQueryGoodsById(item.goods_id, &g) != 0)
        {
            QMessageBox::warning(this, "错误", QString("商品 %1 查询失败！").arg(item.goods_name));
            return;
        }
        if(g.stock < item.num)
        {
            QMessageBox::warning(this, "警告", QString("商品 %1 库存不足！当前库存：%2").arg(item.goods_name).arg(g.stock));
            return;
        }
    }

    time_t now = time(NULL);
    char orderId[32] = {0};
    snprintf(orderId, sizeof(orderId), "ORD%ld%04d", now, rand() % 10000);

    m_pendingOrderId = orderId;
    m_pendingTotal = total;

    ClientService::getInstance()->requestOrderCreate(orderId, m_currentMember.uid, total, m_cartList);

    qDebug() << "Order sent, awaiting server confirmation:" << orderId;
}

void SettlementWidget::on_btn_face_pay_clicked()
{
    if(!m_memberAuthenticated)
    {
        QMessageBox::warning(this, "警告", "请先认证会员！");
        return;
    }

    FacePayWidget facePay(m_currentMember, m_cartList, this);
    if(facePay.exec() == QDialog::Accepted)
    {
        processPayment();
    }
}

void SettlementWidget::on_btn_password_pay_clicked()
{
    if(!m_memberAuthenticated)
    {
        QMessageBox::warning(this, "警告", "请先认证会员！");
        return;
    }

    bool ok;
    QString password = QInputDialog::getText(this, "支付密码",
        QString("会员：%1\n请输入支付密码确认付款：").arg(m_currentMember.name),
        QLineEdit::Password, "", &ok);

    if(!ok || password.isEmpty()) return;

    m_pendingPassword = password;

    ClientService::getInstance()->requestPasswordVerify(
        m_currentMember.uid, password);
}

void SettlementWidget::on_btn_query_member_clicked()
{
    QString uid = ui->lineEdit_uid->text().trimmed();
    if(uid.isEmpty())
    {
        QMessageBox::warning(this, "警告", "请输入会员UID！");
        return;
    }

    QString password = ui->lineEdit_password->text().trimmed();
    if(password.isEmpty())
    {
        QMessageBox::warning(this, "警告", "请输入会员密码！");
        return;
    }

    m_pendingPassword = password;

    ClientService::getInstance()->requestMemberQuery(uid);
}