#ifndef LOCALDBMANAGER_H
#define LOCALDBMANAGER_H

#include <QObject>
#include <QMutex>
#include <sqlite3.h>
#include "common.h"

class LocalDBManager : public QObject
{
    Q_OBJECT
public:
    static LocalDBManager* getInstance();

    void setClientId(const QString& clientId);
    bool initDatabase();
    void closeDatabase();

    int addGoods(const QString& name, float price, const QString& unit, int stock);
    int updateGoods(int id, const QString& name, float price, const QString& unit, int stock);
    int deleteGoods(int id);
    int queryGoodsById(int id, goods_info_t* goods);
    int queryAllGoods(QList<goods_info_t>* goodsList);
    int deductStock(int id, int quantity);
    int addStock(int id, int quantity);

signals:
    void signalGoodsChanged();

private:
    explicit LocalDBManager(QObject *parent = nullptr);
    ~LocalDBManager();
    static LocalDBManager* m_instance;

    sqlite3* m_db;
    QMutex m_mutex;
    QString m_clientId;

    bool createTables();
};

#endif // LOCALDBMANAGER_H
