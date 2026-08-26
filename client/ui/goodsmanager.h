#ifndef GOODSMANAGER_H
#define GOODSMANAGER_H

#include <QWidget>
#include "common.h"
#include <cJSON.h>

QT_BEGIN_NAMESPACE
namespace Ui { class GoodsManager; }
QT_END_NAMESPACE

class GoodsManager : public QWidget
{
    Q_OBJECT

public:
    explicit GoodsManager(QWidget *parent = nullptr);
    ~GoodsManager();
    void refreshGoodsTable();

public slots:
    void notifyServerGoodsChanged();

private slots:
    void on_btn_add_clicked();
    void on_btn_update_clicked();
    void on_btn_delete_clicked();
    void on_btn_refresh_clicked();
    void on_btn_sync_clicked();
    void slotTableItemClicked(int row, int column);

private:
    Ui::GoodsManager *ui;
    int m_selectedGoodsId;
    QList<goods_info_t> m_goodsList;
};

#endif // GOODSMANAGER_H
