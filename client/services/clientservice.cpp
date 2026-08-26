#include "clientservice.h"
#include "otaupdater.h"
#include "core/networkmanager.h"
#include "core/localdbmanager.h"
#include "config/client_config.h"
#include "logger.h"
#include <QDebug>
#include <QByteArray>

ClientService* ClientService::m_instance = nullptr;

ClientService::ClientService(QObject *parent)
    : QObject(parent)
    , m_otaUpdater(nullptr)
    , m_config(nullptr)
{
}

ClientService* ClientService::getInstance()
{
    if (!m_instance) {
        m_instance = new ClientService();
    }
    return m_instance;
}

void ClientService::init(OtaUpdater *otaUpdater)
{
    m_otaUpdater = otaUpdater;
    m_config = ClientConfig::getInstance();

    NetworkManager *nm = NetworkManager::getInstance();
    connect(nm, &NetworkManager::signalConnected,
            this, &ClientService::onConnected);
    connect(nm, &NetworkManager::signalReceiveData,
            this, &ClientService::onServerData);
    connect(nm, &NetworkManager::signalReconnected,
            this, &ClientService::requestGoodsSyncReport);
}

void ClientService::send(cJSON *root)
{
    NetworkManager::getInstance()->sendData(root);
}

void ClientService::registerClient(const QString &clientId)
{
    cJSON *regRoot = cJSON_CreateObject();
    cJSON_AddStringToObject(regRoot, "cmd", "client_register");
    cJSON_AddStringToObject(regRoot, "client_id", clientId.toUtf8().constData());
    send(regRoot);
    cJSON_Delete(regRoot);
}

void ClientService::checkOtaUpdate()
{
    emit signalStatusMessage("正在检查更新...", 2000);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "cmd", "ota_check");
    send(req);
    cJSON_Delete(req);
}

void ClientService::requestOtaFile(const QString &version, const QString &filename)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "cmd", "ota_file_request");
    cJSON_AddStringToObject(req, "version", version.toUtf8().constData());
    cJSON_AddStringToObject(req, "filename", filename.toUtf8().constData());
    send(req);
    cJSON_Delete(req);
}

// -- Business request implementations --

void ClientService::requestMemberQuery(const QString &uid)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "member_query");
    cJSON_AddStringToObject(root, "uid", uid.toUtf8().constData());
    send(root);
    cJSON_Delete(root);
}

void ClientService::requestPasswordVerify(const QString &uid, const QString &password)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "member_verify_password");
    cJSON_AddStringToObject(root, "uid", uid.toUtf8().constData());
    cJSON_AddStringToObject(root, "password", password.toUtf8().constData());
    send(root);
    cJSON_Delete(root);
}

void ClientService::requestMemberRegister(const QString &uid, const QString &name,
                                           const QString &phone, const QString &password,
                                           const QString &facePath, const QString &faceFeature)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "member_register");
    cJSON_AddStringToObject(root, "uid", uid.toUtf8().constData());
    cJSON_AddStringToObject(root, "name", name.toUtf8().constData());
    cJSON_AddStringToObject(root, "phone", phone.toUtf8().constData());
    cJSON_AddNumberToObject(root, "balance", 0);

    if (!password.isEmpty()) {
        cJSON_AddStringToObject(root, "password", password.toUtf8().constData());
    }
    if (!facePath.isEmpty()) {
        cJSON_AddStringToObject(root, "face", facePath.toUtf8().constData());
    }
    if (!faceFeature.isEmpty()) {
        cJSON_AddStringToObject(root, "feature", faceFeature.toUtf8().constData());
    }

    send(root);
    cJSON_Delete(root);
}

void ClientService::requestMemberRecharge(const QString &uid, double amount)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "member_recharge");
    cJSON_AddStringToObject(root, "uid", uid.toUtf8().constData());
    cJSON_AddNumberToObject(root, "amount", amount);
    send(root);
    cJSON_Delete(root);
}

void ClientService::requestOrderCreate(const QString &orderId, const QString &uid,
                                        double total, const QList<order_item_t> &cart)
{
    cJSON *orderRoot = cJSON_CreateObject();
    cJSON_AddStringToObject(orderRoot, "cmd", "order_create");
    cJSON_AddStringToObject(orderRoot, "order_id", orderId.toUtf8().constData());
    cJSON_AddStringToObject(orderRoot, "member_uid", uid.toUtf8().constData());
    cJSON_AddNumberToObject(orderRoot, "total_amount", total);

    cJSON *goodsArray = cJSON_CreateArray();
    for (const auto &item : cart) {
        cJSON *goodsItem = cJSON_CreateObject();
        cJSON_AddNumberToObject(goodsItem, "goods_id", item.goods_id);
        cJSON_AddStringToObject(goodsItem, "goods_name", item.goods_name);
        cJSON_AddNumberToObject(goodsItem, "num", item.num);
        cJSON_AddNumberToObject(goodsItem, "price", item.price);
        cJSON_AddNumberToObject(goodsItem, "subtotal", item.subtotal);
        cJSON_AddItemToArray(goodsArray, goodsItem);
    }
    cJSON_AddItemToObject(orderRoot, "goods_list", goodsArray);

    send(orderRoot);
    cJSON_Delete(orderRoot);
}

void ClientService::requestOrderQuery(const QString &condition)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "order_query");
    cJSON_AddStringToObject(root, "condition", condition.toUtf8().constData());
    send(root);
    cJSON_Delete(root);
}

void ClientService::requestFaceVerify(const QString &uid, const QString &faceFeature)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "face_verify");
    cJSON_AddStringToObject(root, "uid", uid.toUtf8().constData());
    cJSON_AddStringToObject(root, "face_feature", faceFeature.toUtf8().constData());
    send(root);
    cJSON_Delete(root);
}

void ClientService::requestGoodsManagementAuth(const QString &uid, const QString &password)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "goods_management_auth");
    cJSON_AddStringToObject(root, "uid", uid.toUtf8().constData());
    cJSON_AddStringToObject(root, "password", password.toUtf8().constData());
    send(root);
    cJSON_Delete(root);
}

void ClientService::requestGoodsSyncReport()
{
    if (!NetworkManager::getInstance()->isConnected()) {
        return;
    }

    QList<goods_info_t> goodsList;
    LocalDBManager::getInstance()->queryAllGoods(&goodsList);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "goods_sync_report");
    cJSON *goodsArray = cJSON_CreateArray();

    for (const auto &goods : goodsList) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "goods_id", goods.id);
        cJSON_AddStringToObject(item, "goods_name", goods.name);
        cJSON_AddNumberToObject(item, "price", goods.price);
        cJSON_AddStringToObject(item, "unit", goods.unit);
        cJSON_AddNumberToObject(item, "stock_num", goods.stock);
        cJSON_AddItemToArray(goodsArray, item);
    }

    cJSON_AddItemToObject(root, "goods_list", goodsArray);
    send(root);
    cJSON_Delete(root);

    qDebug() << "Notified server of stock change, total" << goodsList.size() << "items";
}

// -- Local database operations --

int ClientService::localAddGoods(const QString &name, double price,
                                  const QString &unit, int stock)
{
    return LocalDBManager::getInstance()->addGoods(name, price, unit, stock);
}

int ClientService::localUpdateGoods(int id, const QString &name, double price,
                                     const QString &unit, int stock)
{
    return LocalDBManager::getInstance()->updateGoods(id, name, price, unit, stock);
}

int ClientService::localDeleteGoods(int id)
{
    return LocalDBManager::getInstance()->deleteGoods(id);
}

int ClientService::localQueryGoodsById(int id, goods_info_t *goods)
{
    return LocalDBManager::getInstance()->queryGoodsById(id, goods);
}

int ClientService::localQueryAllGoods(QList<goods_info_t> *list)
{
    return LocalDBManager::getInstance()->queryAllGoods(list);
}

int ClientService::localDeductStock(int id, int qty)
{
    return LocalDBManager::getInstance()->deductStock(id, qty);
}

// -- Server data processing --

void ClientService::onConnected()
{
    ClientConfig *cfg = ClientConfig::getInstance();
    QString clientId = cfg->getClientId();
    registerClient(clientId);

    qDebug() << "Client registration sent, client_id:" << clientId;

    requestGoodsSyncReport();
}

void ClientService::processOtaPush(cJSON *root)
{
    cJSON *verItem = cJSON_GetObjectItem(root, "version");
    cJSON *fnItem = cJSON_GetObjectItem(root, "filename");
    cJSON *shaItem = cJSON_GetObjectItem(root, "sha256");
    cJSON *szItem = cJSON_GetObjectItem(root, "file_size");
    cJSON *descItem = cJSON_GetObjectItem(root, "description");
    cJSON *typeItem = cJSON_GetObjectItem(root, "type");

    if (verItem && fnItem && shaItem && szItem) {
        QString version = (verItem->valuestring && cJSON_IsString(verItem)) ? verItem->valuestring : "";
        QString filename = (fnItem->valuestring && cJSON_IsString(fnItem)) ? fnItem->valuestring : "";
        QString sha256 = (shaItem->valuestring && cJSON_IsString(shaItem)) ? shaItem->valuestring : "";
        int fileSize = szItem->valueint;
        QString desc = (descItem && descItem->valuestring && cJSON_IsString(descItem)) ? descItem->valuestring : "";
        /* Default to APP when type is missing (back-compat with older servers) */
        int type = (typeItem && cJSON_IsNumber(typeItem))
                       ? typeItem->valueint
                       : (int)OTA_TYPE_APP;

        LOGI("OTA push received: version=%s, file=%s, size=%d, type=%s",
             version.toUtf8().constData(),
             filename.toUtf8().constData(), fileSize,
             (type == OTA_TYPE_SYSTEM) ? "SYSTEM" : "APP");

        emit signalStatusMessage(
            QString("发现新版本 %1: %2").arg(version).arg(desc), 5000);

        emit signalOtaPush(version, filename, sha256, fileSize, desc, type);
    }
}

void ClientService::processOtaChunk(cJSON *root)
{
    cJSON *idxItem = cJSON_GetObjectItem(root, "chunk_index");
    cJSON *totalItem = cJSON_GetObjectItem(root, "total_chunks");
    cJSON *dataItem = cJSON_GetObjectItem(root, "data");

    if (idxItem && totalItem && dataItem && cJSON_IsString(dataItem)) {
        int idx = idxItem->valueint;
        int total = totalItem->valueint;
        QByteArray chunkData = QByteArray::fromBase64(
            QByteArray(dataItem->valuestring));

        emit signalOtaChunk(idx, total, chunkData);
    }
}

void ClientService::processMemberSync(cJSON *root)
{
    cJSON *actionItem = cJSON_GetObjectItem(root, "action");
    cJSON *uidItem = cJSON_GetObjectItem(root, "uid");
    cJSON *nameItem = cJSON_GetObjectItem(root, "name");

    if (actionItem && uidItem) {
        QString action = (actionItem->valuestring && cJSON_IsString(actionItem)) ? actionItem->valuestring : "";
        QString uid = (uidItem->valuestring && cJSON_IsString(uidItem)) ? uidItem->valuestring : "";
        QString name = (nameItem && nameItem->valuestring && cJSON_IsString(nameItem)) ? nameItem->valuestring : "";

        if (action == "add") {
            emit signalStatusMessage(
                QString("新会员注册: %1 (%2)").arg(name).arg(uid), 3000);
        } else if (action == "delete") {
            emit signalStatusMessage(
                QString("会员删除: %1").arg(uid), 3000);
        }
    }
}

void ClientService::routeResponse(cJSON *root)
{
    cJSON *cmdItem = cJSON_GetObjectItem(root, "cmd");
    cJSON *codeItem = cJSON_GetObjectItem(root, "code");

    if (!cmdItem || !codeItem) return;

    QString cmd = (cmdItem->valuestring && cJSON_IsString(cmdItem)) ? cmdItem->valuestring : "";
    int code = codeItem->valueint;
    QString msg;
    cJSON *msgItem = cJSON_GetObjectItem(root, "msg");
    if (msgItem && msgItem->valuestring && cJSON_IsString(msgItem)) msg = msgItem->valuestring;

    cJSON *data = cJSON_GetObjectItem(root, "data");

    qDebug() << "Server response received:" << cmd << "code:" << code << "msg:" << msg;

    if (cmd == "heartbeat_ack") {
        return;
    }

    if (cmd == "member_query") {
        member_info_t member;
        memset(&member, 0, sizeof(member));
        if (code == 0 && data) {
            cJSON *uidItem = cJSON_GetObjectItem(data, "uid");
            cJSON *nameItem = cJSON_GetObjectItem(data, "name");
            cJSON *balanceItem = cJSON_GetObjectItem(data, "balance");
            cJSON *facePathItem = cJSON_GetObjectItem(data, "face_path");
            cJSON *faceFeatureItem = cJSON_GetObjectItem(data, "face_feature");
            cJSON *phoneItem = cJSON_GetObjectItem(data, "phone");

            if (uidItem && uidItem->valuestring && cJSON_IsString(uidItem)) strncpy(member.uid, uidItem->valuestring, sizeof(member.uid)-1);
            if (nameItem && nameItem->valuestring && cJSON_IsString(nameItem)) strncpy(member.name, nameItem->valuestring, sizeof(member.name)-1);
            if (balanceItem) member.balance = balanceItem->valuedouble;
            if (facePathItem && facePathItem->valuestring && cJSON_IsString(facePathItem)) strncpy(member.face_path, facePathItem->valuestring, sizeof(member.face_path)-1);
            if (faceFeatureItem && faceFeatureItem->valuestring && cJSON_IsString(faceFeatureItem)) strncpy(member.face_feature, faceFeatureItem->valuestring, sizeof(member.face_feature)-1);
            if (phoneItem && phoneItem->valuestring && cJSON_IsString(phoneItem)) strncpy(member.phone, phoneItem->valuestring, sizeof(member.phone)-1);
        }
        emit signalMemberQueryResult(code, msg, member);

    } else if (cmd == "member_verify_password") {
        emit signalPasswordVerifyResult(code, msg);

    } else if (cmd == "member_register") {
        emit signalMemberRegisterResult(code, msg);

    } else if (cmd == "member_recharge") {
        emit signalMemberRechargeResult(code, msg);

    } else if (cmd == "order_create") {
        QString orderId;
        if (data) {
            cJSON *oid = cJSON_GetObjectItem(data, "order_id");
            if (oid && cJSON_IsString(oid)) orderId = oid->valuestring;
        }
        emit signalOrderCreateResult(code, msg, orderId);

    } else if (cmd == "order_query") {
        QList<order_info_t> orders;
        if (code == 0 && data) {
            cJSON *orderArray = cJSON_GetObjectItem(data, "order_list");
            if (orderArray && cJSON_IsArray(orderArray)) {
                int size = cJSON_GetArraySize(orderArray);
                for (int i = 0; i < size; i++) {
                    cJSON *item = cJSON_GetArrayItem(orderArray, i);
                    order_info_t order;
                    memset(&order, 0, sizeof(order));

                    cJSON *idItem = cJSON_GetObjectItem(item, "id");
                    cJSON *orderIdItem = cJSON_GetObjectItem(item, "order_id");
                    cJSON *uidItem = cJSON_GetObjectItem(item, "member_uid");
                    cJSON *totalItem = cJSON_GetObjectItem(item, "total_amount");
                    cJSON *timeItem = cJSON_GetObjectItem(item, "create_time");
                    cJSON *goodsItem = cJSON_GetObjectItem(item, "goods_list");

                    if (idItem) order.id = idItem->valueint;
                    if (orderIdItem && orderIdItem->valuestring && cJSON_IsString(orderIdItem)) strncpy(order.order_id, orderIdItem->valuestring, sizeof(order.order_id)-1);
                    if (uidItem && uidItem->valuestring && cJSON_IsString(uidItem)) strncpy(order.member_uid, uidItem->valuestring, sizeof(order.member_uid)-1);
                    if (totalItem) order.total = totalItem->valuedouble;
                    if (timeItem) order.create_time = (time_t)timeItem->valuedouble;
                    if (goodsItem && goodsItem->valuestring && cJSON_IsString(goodsItem)) strncpy(order.goods_list, goodsItem->valuestring, sizeof(order.goods_list)-1);

                    orders.append(order);
                }
            }
        }
        emit signalOrderQueryResult(code, msg, orders);

    } else if (cmd == "face_verify") {
        emit signalFaceVerifyResult(code, msg);

    } else if (cmd == "goods_management_auth_result") {
        emit signalGoodsManagementAuthResult(code, msg);

    } else if (cmd == "goods_add" || cmd == "goods_update" || cmd == "goods_delete") {
        emit signalStatusMessage(msg, 3000);

    } else if (cmd == "stock_deduct") {
        if (code != 0) {
            emit signalStatusMessage("库存扣减失败: " + msg, 5000);
        }

    } else if (cmd == "balance_update") {
        if (code != 0) {
            emit signalStatusMessage("余额更新失败: " + msg, 5000);
        }

    } else if (cmd == "ota_file_request") {
        if (code == 0) {
            cJSON *chunksItem = cJSON_GetObjectItem(root, "total_chunks");
            cJSON *sizeItem = cJSON_GetObjectItem(root, "file_size");
            int totalChunks = chunksItem ? chunksItem->valueint : 0;
            int fileSize = sizeItem ? sizeItem->valueint : 0;
            qDebug() << "OTA transfer confirmed: total chunks" << totalChunks
                     << "file size" << fileSize << "bytes";
        } else {
            emit signalStatusMessage("OTA文件请求失败: " + msg, 5000);
        }
    }
}

void ClientService::onServerData(QString jsonData)
{
    cJSON *root = cJSON_Parse(jsonData.toUtf8().constData());
    if (root == nullptr) {
        qDebug() << "Failed to parse server data";
        return;
    }

    cJSON *cmdItem = cJSON_GetObjectItem(root, "cmd");
    if (!cmdItem || !cJSON_IsString(cmdItem)) {
        cJSON_Delete(root);
        return;
    }

    QString cmd = cmdItem->valuestring;

    if (cmd == "ota_push") {
        processOtaPush(root);
    } else if (cmd == "ota_chunk") {
        processOtaChunk(root);
    } else if (cmd == "member_sync") {
        processMemberSync(root);
    } else if (cmd == "monitor_start") {
        /* Server command: start monitor - client opens camera + RTSP push */
        cJSON *urlItem = cJSON_GetObjectItem(root, "rtsp_url");
        if (!urlItem || !urlItem->valuestring)
            urlItem = cJSON_GetObjectItem(root, "rtmp_url");  /* Back-compat with server field name */
        QString rtspUrl = (urlItem && urlItem->valuestring) ? QString::fromUtf8(urlItem->valuestring) : "";
        qDebug() << "Server monitor command received: monitor_start, rtsp_url:" << rtspUrl;
        emit signalMonitorStart(rtspUrl);
    } else if (cmd == "monitor_stop") {
        /* Server command: stop monitor */
        qDebug() << "Server monitor command received: monitor_stop";
        emit signalMonitorStop();
    } else {
        routeResponse(root);
    }

    cJSON_Delete(root);
}
