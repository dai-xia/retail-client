#include "orderquery.h"
#include "ui_orderquery.h"
#include "services/clientservice.h"
#include <QHeaderView>
#include <QMessageBox>
#include <QDateTime>
#include <cJSON.h>

OrderQuery::OrderQuery(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::OrderQuery)
{
    ui->setupUi(this);

    ui->tableWidget_order->setColumnCount(5);
    ui->tableWidget_order->setHorizontalHeaderLabels({"订单ID", "订单号", "会员UID", "总价", "创建时间"});
    ui->tableWidget_order->horizontalHeader()->setStretchLastSection(true);
    ui->tableWidget_order->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(ui->tableWidget_order, &QTableWidget::cellClicked, this, &OrderQuery::slotTableItemClicked);

    ui->tableWidget_detail->setColumnCount(4);
    ui->tableWidget_detail->setHorizontalHeaderLabels({"商品ID", "商品名称", "数量", "小计"});
    ui->tableWidget_detail->horizontalHeader()->setStretchLastSection(true);
}

OrderQuery::~OrderQuery()
{
    delete ui;
}

void OrderQuery::onOrderQueryResult(int code, QString msg, QList<order_info_t> orders)
{
    if (code == 0) {
        m_orderList = orders;
        refreshOrderTable();
    } else {
        QMessageBox::information(this, "提示", "未查询到订单：" + msg);
    }
}

void OrderQuery::refreshOrderTable()
{
    ui->tableWidget_order->setRowCount(0);
    for(int i = 0; i < m_orderList.size(); i++)
    {
        int row = ui->tableWidget_order->rowCount();
        ui->tableWidget_order->insertRow(row);
        ui->tableWidget_order->setItem(row, 0, new QTableWidgetItem(QString::number(m_orderList[i].id)));
        ui->tableWidget_order->setItem(row, 1, new QTableWidgetItem(m_orderList[i].order_id));
        ui->tableWidget_order->setItem(row, 2, new QTableWidgetItem(m_orderList[i].member_uid));
        ui->tableWidget_order->setItem(row, 3, new QTableWidgetItem(QString::number(m_orderList[i].total, 'f', 2)));
        QDateTime time = QDateTime::fromSecsSinceEpoch(m_orderList[i].create_time);
        ui->tableWidget_order->setItem(row, 4, new QTableWidgetItem(time.toString("yyyy-MM-dd hh:mm:ss")));
    }
}

void OrderQuery::on_btn_query_clicked()
{
    QString condition = ui->lineEdit_condition->text().trimmed();
    if(condition.isEmpty())
    {
        QMessageBox::warning(this, "警告", "请输入会员UID或手机号！");
        return;
    }

    ClientService::getInstance()->requestOrderQuery(condition);
}

void OrderQuery::slotTableItemClicked(int row, int column)
{
    Q_UNUSED(column);
    if(row < 0 || row >= m_orderList.size()) return;

    // Parse the goods list embedded in the order
    ui->tableWidget_detail->setRowCount(0);
    QString goodsListStr = m_orderList[row].goods_list;
    if(goodsListStr.isEmpty()) return;

    cJSON* goodsArray = cJSON_Parse(goodsListStr.toUtf8().constData());
    if(goodsArray != nullptr && cJSON_IsArray(goodsArray))
    {
        int size = cJSON_GetArraySize(goodsArray);
        for(int i = 0; i < size; i++)
        {
            cJSON* item = cJSON_GetArrayItem(goodsArray, i);
            int detailRow = ui->tableWidget_detail->rowCount();
            ui->tableWidget_detail->insertRow(detailRow);

            cJSON* gidItem = cJSON_GetObjectItem(item, "goods_id");
            cJSON* nameItem = cJSON_GetObjectItem(item, "goods_name");
            cJSON* numItem = cJSON_GetObjectItem(item, "num");
            cJSON* subItem = cJSON_GetObjectItem(item, "subtotal");

            if(gidItem) ui->tableWidget_detail->setItem(detailRow, 0, new QTableWidgetItem(QString::number(gidItem->valueint)));
            if(nameItem) ui->tableWidget_detail->setItem(detailRow, 1, new QTableWidgetItem(nameItem->valuestring));
            if(numItem) ui->tableWidget_detail->setItem(detailRow, 2, new QTableWidgetItem(QString::number(numItem->valueint)));
            if(subItem) ui->tableWidget_detail->setItem(detailRow, 3, new QTableWidgetItem(QString::number(subItem->valuedouble, 'f', 2)));
        }
        cJSON_Delete(goodsArray);
    }
}
