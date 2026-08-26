#include "client_config.h"
#include <QDebug>

const char* ClientConfig::DEFAULT_CLIENT_ID = "client_01";
const char* ClientConfig::DEFAULT_SERVER_IP = "127.0.0.1";

ClientConfig* ClientConfig::m_instance = nullptr;

ClientConfig::ClientConfig()
    : m_clientId(DEFAULT_CLIENT_ID)
    , m_localPort(DEFAULT_LOCAL_PORT)
    , m_serverIP(DEFAULT_SERVER_IP)
    , m_serverPort(DEFAULT_SERVER_PORT)
{
}

ClientConfig* ClientConfig::getInstance()
{
    if(m_instance == nullptr)
    {
        m_instance = new ClientConfig();
    }
    return m_instance;
}

void ClientConfig::parseArgs(int argc, char *argv[])
{
    if(argc >= 2)
    {
        m_clientId = QString::fromUtf8(argv[1]);
    }
    if(argc >= 3)
    {
        m_localPort = QString::fromUtf8(argv[2]).toInt();
    }
    if(argc >= 4)
    {
        m_serverIP = QString::fromUtf8(argv[3]);
    }
    if(argc >= 5)
    {
        m_serverPort = QString::fromUtf8(argv[4]).toInt();
    }

    applyDefaults();
}

void ClientConfig::applyDefaults()
{
    if(m_clientId.isEmpty())
        m_clientId = DEFAULT_CLIENT_ID;
    if(m_localPort < 0)
        m_localPort = 0;
    if(m_serverIP.isEmpty())
        m_serverIP = DEFAULT_SERVER_IP;
    if(m_serverPort <= 0)
        m_serverPort = DEFAULT_SERVER_PORT;

    qDebug() << "客户端配置:"
             << "client_id=" << m_clientId
             << "local_port=" << m_localPort
             << "server=" << m_serverIP << ":" << m_serverPort;
}
