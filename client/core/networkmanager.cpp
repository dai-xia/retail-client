#include "networkmanager.h"
#include "config/client_config.h"
#include "crypto.h"
#include <QDebug>
#include <arpa/inet.h>
#include <QHostAddress>

#define MAX_BUF_SIZE 65536

NetworkManager* NetworkManager::m_instance = nullptr;

NetworkManager::NetworkManager(QObject *parent)
    : QObject(parent)
    , m_socket(nullptr)
    , m_serverIP("127.0.0.1")
    , m_serverPort(9090)
    , m_localPort(0)
    , m_isConnected(false)
    , m_reconnectCount(0)
{
    m_reconnectTimer = new QTimer(this);
    connect(m_reconnectTimer, &QTimer::timeout, this, &NetworkManager::slotReconnect);

    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &NetworkManager::slotSendHeartbeat);
}

NetworkManager* NetworkManager::getInstance()
{
    if(m_instance == nullptr)
    {
        m_instance = new NetworkManager();
    }
    return m_instance;
}

bool NetworkManager::connectToServer(const QString& ip, int port)
{
    m_serverIP = ip;
    m_serverPort = port;

    cleanupSocket();

    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected, this, &NetworkManager::slotConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &NetworkManager::slotDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &NetworkManager::slotReadyRead);
    connect(m_socket, QOverload<QTcpSocket::SocketError>::of(&QTcpSocket::error),
            this, &NetworkManager::slotError);

    // bind local port to simulate multiple clients on one VM
    if(m_localPort > 0)
    {
        if(!m_socket->bind(QHostAddress::Any, m_localPort))
        {
            qWarning() << "bind local port failed:" << m_localPort << m_socket->errorString();
        }
        else
        {
            qDebug() << "bind local port ok:" << m_localPort;
        }
    }

    m_socket->connectToHost(ip, port);
    return m_socket->waitForConnected(5000);
}

void NetworkManager::setLocalPort(int port)
{
    m_localPort = port;
}

void NetworkManager::cleanupSocket()
{
    if(m_socket != nullptr)
    {
        disconnect(m_socket, nullptr, this, nullptr);
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

void NetworkManager::disconnectFromServer()
{
    m_reconnectTimer->stop();
    m_heartbeatTimer->stop();
    if(m_socket != nullptr)
    {
        m_socket->close();
    }
    m_isConnected = false;
}

bool NetworkManager::isConnected()
{
    return m_isConnected && m_socket != nullptr && m_socket->state() == QAbstractSocket::ConnectedState;
}


bool NetworkManager::sendData(const QString& jsonData)
{
    QMutexLocker locker(&m_sendMutex);
    if(!isConnected())
    {
        qDebug() << "network not connected, send failed";
        return false;
    }

    QByteArray plainData = jsonData.toUtf8();

    unsigned char *encData = nullptr;
    int encLen = 0;
    if (crypto_encrypt((const unsigned char*)plainData.constData(), plainData.size(), &encData, &encLen) != 0)
    {
        qDebug() << "encrypt failed";
        return false;
    }

    uint32_t len = htonl(encLen);
    m_socket->write((char*)&len, 4);
    m_socket->write((char*)encData, encLen);
    free(encData);
    return m_socket->waitForBytesWritten(5000);
}

bool NetworkManager::sendData(cJSON* root)
{
    char* jsonStr = cJSON_Print(root);
    bool ret = sendData(QString::fromUtf8(jsonStr));
    cJSON_free(jsonStr);
    return ret;
}


void NetworkManager::slotConnected()
{
    bool isReconnect = m_reconnectCount > 0;

    m_isConnected = true;
    m_reconnectCount = 0;
    m_reconnectTimer->stop();
    m_recvBuf.clear();
    m_heartbeatTimer->start(HEARTBEAT_INTERVAL);
    emit signalConnected();

    if(isReconnect)
    {
        emit signalReconnected();
    }

    qDebug() << "connected to server";

    slotSendHeartbeat();
}

void NetworkManager::slotDisconnected()
{
    bool wasConnected = m_isConnected;
    m_isConnected = false;
    m_recvBuf.clear();
    m_heartbeatTimer->stop();
    emit signalDisconnected();
    qDebug() << "disconnected from server";

    if(wasConnected && m_reconnectCount < MAX_RECONNECT_COUNT)
    {
        m_reconnectCount++;
        qDebug() << "start reconnect timer, attempt" << m_reconnectCount;
        m_reconnectTimer->start(RECONNECT_INTERVAL);
    }
    else if(m_reconnectCount >= MAX_RECONNECT_COUNT)
    {
        qDebug() << "max reconnect count reached (" << MAX_RECONNECT_COUNT << "), stopping";
        emit signalError("connection failed, max reconnect count reached, check network or server");
    }
}


void NetworkManager::slotReadyRead()
{
    m_recvBuf.append(m_socket->readAll());

    while(m_recvBuf.size() >= 4)
    {
        uint32_t head = 0;
        memcpy(&head, m_recvBuf.constData(), 4);
        uint32_t encLen = ntohl(head);

        if(encLen <= 0 || encLen > MAX_BUF_SIZE)
        {
            m_recvBuf.clear();
            qDebug() << "invalid packet length, recv buffer cleared";
            return;
        }

        if(m_recvBuf.size() < (int)(4 + encLen))
        {
            break;
        }

        QByteArray encData = m_recvBuf.mid(4, encLen);
        m_recvBuf.remove(0, 4 + encLen);

        unsigned char *plainData = nullptr;
        int plainLen = 0;
        if (crypto_decrypt((const unsigned char*)encData.constData(), encData.size(), &plainData, &plainLen) != 0)
        {
            qDebug() << "decrypt failed";
            continue;
        }

        char* result = (char*)malloc(plainLen + 1);
        memcpy(result, plainData, plainLen);
        result[plainLen] = '\0';
        free(plainData);

        emit signalReceiveData(QString::fromUtf8(result));
        free(result);
    }
}

void NetworkManager::slotError(QTcpSocket::SocketError error)
{
    Q_UNUSED(error)
    QString errorMsg = m_socket ? m_socket->errorString() : "unknown error";
    emit signalError(errorMsg);
    qDebug() << "network error:" << errorMsg;

    if(!m_isConnected && !m_reconnectTimer->isActive() && m_reconnectCount < MAX_RECONNECT_COUNT)
    {
        m_reconnectCount++;
        qDebug() << "network error, start reconnect timer, attempt" << m_reconnectCount;
        m_reconnectTimer->start(RECONNECT_INTERVAL);
    }
}



void NetworkManager::slotReconnect()
{
    if(!m_isConnected)
    {
        if(m_reconnectCount >= MAX_RECONNECT_COUNT)
        {
            qDebug() << "max reconnect count reached, stopping";
            m_reconnectTimer->stop();
            emit signalError("connection failed, max reconnect count reached");
            return;
        }

        m_reconnectCount++;
        qDebug() << "reconnecting... attempt" << m_reconnectCount << "/" << MAX_RECONNECT_COUNT;

        if(connectToServer(m_serverIP, m_serverPort))
        {
            qDebug() << "reconnect succeeded";
            m_reconnectCount = 0;
            m_reconnectTimer->stop();
        }
        else
        {
            qDebug() << "reconnect failed, will retry in" << RECONNECT_INTERVAL/1000 << "s";
        }
    }
}

void NetworkManager::slotSendHeartbeat()
{
    if(!isConnected()) return;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "heartbeat");
    sendData(root);
    cJSON_Delete(root);
    qDebug() << "heartbeat sent";
}
