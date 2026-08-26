#include "goodsmanager.h"
#include "ui_goodsmanager.h"
#include "services/clientservice.h"
#include <QMessageBox>
#include <QHeaderView>
#include <cJSON.h>

GoodsManager::GoodsManager(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::GoodsManager)
    , m_selectedGoodsId(-1)
{
    ui->setupUi(this);

    ui->tableWidget->setColumnCount(5);
    ui->tableWidget->setHorizontalHeaderLabels({"ID", "商品名称", "单价", "单位", "库存"});
    ui->tableWidget->horizontalHeader()->setStretchLastSection(true);
    ui->tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(ui->tableWidget, &QTableWidget::cellClicked, this, &GoodsManager::slotTableItemClicked);

    ui->doubleSpinBox_price->setRange(0, 99999);
    ui->spinBox_stock->setRange(0, 99999);

    refreshGoodsTable();
}

GoodsManager::~GoodsManager()
{
    delete ui;
}

void GoodsManager::refreshGoodsTable()
{
    QList<goods_info_t> goodsList;
    ClientService::getInstance()->localQueryAllGoods(&goodsList);

    ui->tableWidget->setRowCount(0);
    for(int i = 0; i < goodsList.size(); i++)
    {
        int row = ui->tableWidget->rowCount();
        ui->tableWidget->insertRow(row);
        ui->tableWidget->setItem(row, 0, new QTableWidgetItem(QString::number(goodsList[i].id)));
        ui->tableWidget->setItem(row, 1, new QTableWidgetItem(goodsList[i].name));
        ui->tableWidget->setItem(row, 2, new QTableWidgetItem(QString::number(goodsList[i].price, 'f', 2)));
        ui->tableWidget->setItem(row, 3, new QTableWidgetItem(goodsList[i].unit));
        ui->tableWidget->setItem(row, 4, new QTableWidgetItem(QString::number(goodsList[i].stock)));
    }
}

void GoodsManager::slotTableItemClicked(int row, int column)
{
    Q_UNUSED(column);
    if(row < 0) return;

    int id = ui->tableWidget->item(row, 0)->text().toInt();
    goods_info_t goods;
    if(ClientService::getInstance()->localQueryGoodsById(id, &goods) == 0)
    {
        m_selectedGoodsId = goods.id;
        ui->lineEdit_name->setText(goods.name);
        ui->doubleSpinBox_price->setValue(goods.price);
        ui->lineEdit_unit->setText(goods.unit);
        ui->spinBox_stock->setValue(goods.stock);
    }
}

void GoodsManager::notifyServerGoodsChanged()
{
    ClientService::getInstance()->requestGoodsSyncReport();
}

void GoodsManager::on_btn_add_clicked()
{
    QString name = ui->lineEdit_name->text().trimmed();
    double price = ui->doubleSpinBox_price->value();
    QString unit = ui->lineEdit_unit->text().trimmed();
    int stock = ui->spinBox_stock->value();

    if(name.isEmpty() || unit.isEmpty())
    {
        QMessageBox::warning(this, "警告", "商品名称和单位不能为空！");
        return;
    }

    int newId = ClientService::getInstance()->localAddGoods(name, price, unit, stock);
    if(newId > 0)
    {
        QMessageBox::information(this, "成功", QString("商品添加成功！ID: %1").arg(newId));
        refreshGoodsTable();
        ClientService::getInstance()->requestGoodsSyncReport();
    }
    else
    {
        QMessageBox::warning(this, "失败", "商品添加失败！");
    }
}

void GoodsManager::on_btn_update_clicked()
{
    if(m_selectedGoodsId == -1)
    {
        QMessageBox::warning(this, "警告", "请先选择要修改的商品！");
        return;
    }

    QString name = ui->lineEdit_name->text().trimmed();
    double price = ui->doubleSpinBox_price->value();
    QString unit = ui->lineEdit_unit->text().trimmed();
    int stock = ui->spinBox_stock->value();

    int ret = ClientService::getInstance()->localUpdateGoods(m_selectedGoodsId, name, price, unit, stock);
    if(ret == 0)
    {
        QMessageBox::information(this, "成功", "商品修改成功！");
        refreshGoodsTable();
        ClientService::getInstance()->requestGoodsSyncReport();
    }
    else
    {
        QMessageBox::warning(this, "失败", "商品修改失败！");
    }
}

void GoodsManager::on_btn_delete_clicked()
{
    if(m_selectedGoodsId == -1)
    {
        QMessageBox::warning(this, "警告", "请先选择要删除的商品！");
        return;
    }

    if(QMessageBox::question(this, "确认", "确定要删除该商品吗？") != QMessageBox::Yes)
        return;

    int ret = ClientService::getInstance()->localDeleteGoods(m_selectedGoodsId);
    if(ret == 0)
    {
        QMessageBox::information(this, "成功", "商品删除成功！");
        m_selectedGoodsId = -1;
        refreshGoodsTable();
        ClientService::getInstance()->requestGoodsSyncReport();
    }
    else
    {
        QMessageBox::warning(this, "失败", "商品删除失败！");
    }
}

void GoodsManager::on_btn_refresh_clicked()
{
    refreshGoodsTable();
}

void GoodsManager::on_btn_sync_clicked()
{
    refreshGoodsTable();
    ClientService::getInstance()->requestGoodsSyncReport();
}