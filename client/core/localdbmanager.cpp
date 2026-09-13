#include "localdbmanager.h"
#include "config/client_config.h"
#include <QDebug>
#include <QFile>
#include <QCoreApplication>

LocalDBManager* LocalDBManager::m_instance = nullptr;

LocalDBManager::LocalDBManager(QObject *parent)
    : QObject(parent)
    , m_db(nullptr)
{
}

LocalDBManager::~LocalDBManager()
{
    closeDatabase();
}

LocalDBManager* LocalDBManager::getInstance()
{
    if(m_instance == nullptr)
    {
        m_instance = new LocalDBManager();
    }
    return m_instance;
}

void LocalDBManager::setClientId(const QString& clientId)
{
    QMutexLocker locker(&m_mutex);
    m_clientId = clientId;
    qDebug() << "Local database client_id set to:" << clientId;
}

bool LocalDBManager::initDatabase()
{
    QMutexLocker locker(&m_mutex);

    if(m_clientId.isEmpty())
    {
        m_clientId = ClientConfig::getInstance()->getClientId();
    }

    QString dbPath = QString("%1/%2_data.db")
                         .arg(QCoreApplication::applicationDirPath())
                         .arg(m_clientId);
    qDebug() << "Initializing local database:" << dbPath;

    int ret = sqlite3_open(dbPath.toUtf8().constData(), &m_db);
    if(ret != SQLITE_OK)
    {
        qDebug() << "Failed to open local database:" << sqlite3_errmsg(m_db);
        return false;
    }

    qDebug() << "Local database opened successfully";
    return createTables();
}

void LocalDBManager::closeDatabase()
{
    QMutexLocker locker(&m_mutex);
    if(m_db != nullptr)
    {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

bool LocalDBManager::createTables()
{
    char* errMsg = nullptr;
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS goods (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            price REAL NOT NULL DEFAULT 0,
            unit TEXT NOT NULL DEFAULT '',
            stock INTEGER NOT NULL DEFAULT 0,
            create_time INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
        );
    )";

    int ret = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
    if(ret != SQLITE_OK)
    {
        qDebug() << "Failed to create goods table:" << errMsg;
        sqlite3_free(errMsg);
        return false;
    }

    qDebug() << "Goods table created/checked successfully";
    return true;
}

int LocalDBManager::addGoods(const QString& name, float price, const QString& unit, int stock)
{
    QMutexLocker locker(&m_mutex);

    const char* sql = "INSERT INTO goods (name, price, unit, stock) VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;

    int ret = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
    if(ret != SQLITE_OK)
    {
        qDebug() << "Add goods prepare statement failed:" << sqlite3_errmsg(m_db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, name.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, price);
    sqlite3_bind_text(stmt, 3, unit.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, stock);

    ret = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if(ret != SQLITE_DONE)
    {
        qDebug() << "Add goods failed:" << sqlite3_errmsg(m_db);
        return -1;
    }

    int newId = (int)sqlite3_last_insert_rowid(m_db);
    qDebug() << "Goods added successfully, ID:" << newId;
    return newId;
}

int LocalDBManager::updateGoods(int id, const QString& name, float price, const QString& unit, int stock)
{
    QMutexLocker locker(&m_mutex);

    const char* sql = "UPDATE goods SET name=?, price=?, unit=?, stock=? WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    int ret = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
    if(ret != SQLITE_OK)
    {
        qDebug() << "Update goods prepare statement failed:" << sqlite3_errmsg(m_db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, name.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, price);
    sqlite3_bind_text(stmt, 3, unit.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, stock);
    sqlite3_bind_int(stmt, 5, id);

    ret = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if(ret != SQLITE_DONE)
    {
        qDebug() << "Update goods failed:" << sqlite3_errmsg(m_db);
        return -1;
    }

    return 0;
}

int LocalDBManager::deleteGoods(int id)
{
    QMutexLocker locker(&m_mutex);

    const char* sql = "DELETE FROM goods WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    int ret = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
    if(ret != SQLITE_OK)
    {
        qDebug() << "Delete goods prepare statement failed:" << sqlite3_errmsg(m_db);
        return -1;
    }

    sqlite3_bind_int(stmt, 1, id);

    ret = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if(ret != SQLITE_DONE)
    {
        qDebug() << "Delete goods failed:" << sqlite3_errmsg(m_db);
        return -1;
    }

    return 0;
}

int LocalDBManager::queryGoodsById(int id, goods_info_t* goods)
{
    QMutexLocker locker(&m_mutex);

    char sql[256] = {0};
    snprintf(sql, sizeof(sql), "SELECT id, name, price, unit, stock, create_time FROM goods WHERE id=%d;", id);

    sqlite3_stmt* stmt = nullptr;
    int ret = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
    if(ret != SQLITE_OK)
    {
        return -1;
    }

    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
        goods->id = sqlite3_column_int(stmt, 0);
        strncpy(goods->name, (const char*)sqlite3_column_text(stmt, 1), sizeof(goods->name)-1);
        goods->price = sqlite3_column_double(stmt, 2);
        strncpy(goods->unit, (const char*)sqlite3_column_text(stmt, 3), sizeof(goods->unit)-1);
        goods->stock = sqlite3_column_int(stmt, 4);
        goods->create_time = sqlite3_column_int(stmt, 5);

        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    return -1;
}

int LocalDBManager::queryAllGoods(QList<goods_info_t>* goodsList)
{
    QMutexLocker locker(&m_mutex);

    const char* sql = "SELECT id, name, price, unit, stock, create_time FROM goods ORDER BY id;";
    sqlite3_stmt* stmt = nullptr;
    int ret = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
    if(ret != SQLITE_OK)
    {
        return -1;
    }

    goodsList->clear();
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
        goods_info_t goods;
        memset(&goods, 0, sizeof(goods_info_t));
        goods.id = sqlite3_column_int(stmt, 0);
        strncpy(goods.name, (const char*)sqlite3_column_text(stmt, 1), sizeof(goods.name)-1);
        goods.price = sqlite3_column_double(stmt, 2);
        strncpy(goods.unit, (const char*)sqlite3_column_text(stmt, 3), sizeof(goods.unit)-1);
        goods.stock = sqlite3_column_int(stmt, 4);
        goods.create_time = sqlite3_column_int(stmt, 5);
        goodsList->append(goods);
    }

    sqlite3_finalize(stmt);
    return 0;
}

int LocalDBManager::deductStock(int id, int quantity)
{
    QMutexLocker locker(&m_mutex);

    char sql[256] = {0};
    snprintf(sql, sizeof(sql),
        "UPDATE goods SET stock = stock - %d WHERE id=%d AND stock >= %d;",
        quantity, id, quantity);

    char* errMsg = nullptr;
    int ret = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
    if(ret != SQLITE_OK)
    {
        qDebug() << "Deduct stock failed:" << errMsg;
        sqlite3_free(errMsg);
        return -1;
    }

    if(sqlite3_changes(m_db) == 0)
    {
        qDebug() << "Insufficient stock or goods does not exist, ID:" << id;
        return -1;
    }

    return 0;
}

int LocalDBManager::addStock(int id, int quantity)
{
    QMutexLocker locker(&m_mutex);

    char sql[256] = {0};
    snprintf(sql, sizeof(sql),
        "UPDATE goods SET stock = stock + %d WHERE id=%d;",
        quantity, id);

    char* errMsg = nullptr;
    int ret = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
    if(ret != SQLITE_OK)
    {
        qDebug() << "Add stock failed:" << errMsg;
        sqlite3_free(errMsg);
        return -1;
    }

    return 0;
}
