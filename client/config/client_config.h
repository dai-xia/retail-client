#ifndef __CLIENT_CONFIG_H__
#define __CLIENT_CONFIG_H__

#include <QString>

class ClientConfig
{
public:
    static ClientConfig* getInstance();

    void setClientId(const QString& id) { m_clientId = id; }
    void setLocalPort(int port) { m_localPort = port; }
    void setServerIP(const QString& ip) { m_serverIP = ip; }
    void setServerPort(int port) { m_serverPort = port; }

    QString getClientId() const { return m_clientId; }
    int getLocalPort() const { return m_localPort; }
    QString getServerIP() const { return m_serverIP; }
    int getServerPort() const { return m_serverPort; }

    void parseArgs(int argc, char *argv[]);
    void applyDefaults();

private:
    ClientConfig();

    static ClientConfig* m_instance;

    QString m_clientId;
    int m_localPort;
    QString m_serverIP;
    int m_serverPort;

    static const char* DEFAULT_CLIENT_ID;
    static const int DEFAULT_LOCAL_PORT = 0;
    static const char* DEFAULT_SERVER_IP;
    static const int DEFAULT_SERVER_PORT = 9090;
};

#endif
