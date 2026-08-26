#ifndef ORDERQUERY_H
#define ORDERQUERY_H

#include <QWidget>
#include "common.h"
#include <cJSON.h>

QT_BEGIN_NAMESPACE
namespace Ui { class OrderQuery; }
QT_END_NAMESPACE

class OrderQuery : public QWidget
{
    Q_OBJECT

public:
    explicit OrderQuery(QWidget *parent = nullptr);
    ~OrderQuery();

public slots:
    void onOrderQueryResult(int code, QString msg, QList<order_info_t> orders);

private slots:
    void on_btn_query_clicked();
    void slotTableItemClicked(int row, int column);

private:
    Ui::OrderQuery *ui;
    QList<order_info_t> m_orderList;
    void refreshOrderTable();
};

#endif // ORDERQUERY_H
