#ifndef NETWORKMANAGER_H
#define NETWORKMANAGER_H

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QMutex>
#include <QByteArray>
#include <cJSON.h>

class NetworkManager : public QObject
{
    Q_OBJECT
public:
    static NetworkManager* getInstance();

    bool connectToServer(const QString& ip, int port);
    void disconnectFromServer();
    bool isConnected();

    bool sendData(const QString& jsonData);
    bool sendData(cJSON* root);

    void setLocalPort(int port);

signals:
    void signalConnected();
    void signalDisconnected();
    void signalReconnected();
    void signalError(QString error);
    void signalReceiveData(QString jsonData);

private slots:
    void slotConnected();
    void slotDisconnected();
    void slotReadyRead();
    void slotError(QTcpSocket::SocketError error);
    void slotReconnect();
    void slotSendHeartbeat();

private:
    explicit NetworkManager(QObject *parent = nullptr);
    static NetworkManager* m_instance;

    QTcpSocket* m_socket;
    QString m_serverIP;
    int m_serverPort;
    int m_localPort;
    bool m_isConnected;
    QTimer* m_reconnectTimer;
    QTimer* m_heartbeatTimer;
    QMutex m_sendMutex;
    QByteArray m_recvBuf;

    int m_reconnectCount;
    static const int MAX_RECONNECT_COUNT = 10;
    static const int RECONNECT_INTERVAL = 5000;
    static const int HEARTBEAT_INTERVAL = 30000;

    void cleanupSocket();
};

#endif
