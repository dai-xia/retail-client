#ifndef CLIENTSERVICE_H
#define CLIENTSERVICE_H

#include <QObject>
#include <cJSON.h>
#include "common.h"

class OtaUpdater;
class ClientConfig;

class ClientService : public QObject
{
    Q_OBJECT

public:
    static ClientService* getInstance();

    void init(OtaUpdater *otaUpdater);


    void registerClient(const QString &clientId);
    void checkOtaUpdate();
    void requestOtaFile(const QString &version, const QString &filename);
    void requestMemberQuery(const QString &uid);
    void requestPasswordVerify(const QString &uid, const QString &password);
    void requestMemberRegister(const QString &uid, const QString &name,
                               const QString &phone, const QString &password,
                               const QString &facePath, const QString &faceFeature = "");
    void requestMemberRecharge(const QString &uid, double amount);
    void requestOrderCreate(const QString &orderId, const QString &uid,
                            double total, const QList<order_item_t> &cart);
    void requestOrderQuery(const QString &condition);
    void requestFaceVerify(const QString &uid, const QString &faceFeature);
    void requestGoodsSyncReport();
    void requestGoodsManagementAuth(const QString &uid, const QString &password);

    void send(cJSON *root);


    int localAddGoods(const QString &name, double price, const QString &unit, int stock);
    int localUpdateGoods(int id, const QString &name, double price,
                         const QString &unit, int stock);
    int localDeleteGoods(int id);
    int localQueryGoodsById(int id, goods_info_t *goods);
    int localQueryAllGoods(QList<goods_info_t> *list);
    int localDeductStock(int id, int qty);

signals:
    void signalStatusMessage(QString msg, int timeoutMs);


    void signalOtaPush(QString version, QString filename, QString sha256,
                       int fileSize, QString desc, int type);
    void signalOtaChunk(int chunkIndex, int totalChunks, QByteArray data);


    void signalMemberQueryResult(int code, QString msg, member_info_t member);
    void signalPasswordVerifyResult(int code, QString msg);
    void signalMemberRegisterResult(int code, QString msg);
    void signalMemberRechargeResult(int code, QString msg);
    void signalOrderCreateResult(int code, QString msg, QString orderId);
    void signalOrderQueryResult(int code, QString msg, QList<order_info_t> orders);
    void signalFaceVerifyResult(int code, QString msg);
    void signalGoodsManagementAuthResult(int code, QString msg);


    void signalMonitorStart(QString rtspUrl);
    void signalMonitorStop();

public slots:
    void onServerData(QString jsonData);
    void onConnected();

private:
    explicit ClientService(QObject *parent = nullptr);
    static ClientService* m_instance;

    OtaUpdater *m_otaUpdater;
    ClientConfig *m_config;

    void processOtaPush(cJSON *root);
    void processOtaChunk(cJSON *root);
    void processMemberSync(cJSON *root);
    void routeResponse(cJSON *root);
};

#endif
