// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2019 The MMapper Authors
// Author: Ulf Hermann <ulfonk_mennhar@gmx.de> (Alve)
// Author: Marek Krejza <krejza@gmail.com> (Caligor)
// Author: Nils Schimmelmann <nschimme@gmail.com> (Jahara)

#include "proxy.h"

#include <cassert>
#include <memory>
#include <stdexcept>
#include <QByteArray>
#include <QMessageLogContext>
#include <QObject>
#include <QScopedPointer>
#include <QTcpSocket>

#include "../configuration/configuration.h"
#include "../display/mapcanvas.h"
#include "../display/prespammedpath.h"
#include "../expandoracommon/parseevent.h"
#include "../global/io.h"
#include "../mainwindow/mainwindow.h"
#include "../mpi/mpifilter.h"
#include "../mpi/remoteedit.h"
#include "../pandoragroup/mmapper2group.h"
#include "../parser/abstractparser.h"
#include "../parser/mumexmlparser.h"
#include "../pathmachine/mmapper2pathmachine.h"
#include "MudTelnet.h"
#include "UserTelnet.h"
#include "connectionlistener.h"
#include "mumesocket.h"
#include "telnetfilter.h"

#undef ERROR // Bad dog, Microsoft; bad dog!!!

template<typename T, typename... Args>
static inline auto makeQPointer(Args &&... args)
{
    static_assert(std::is_base_of_v<QObject, T>);
    auto tmp = std::make_unique<T>(std::forward<Args>(args)...);
    if (tmp->QObject::parent() == nullptr)
        throw std::invalid_argument("allocated QObject does not have a parent");
    return QPointer<T>{tmp.release()};
}

Proxy::Proxy(MapData *const md,
             Mmapper2PathMachine *const pm,
             PrespammedPath *const pp,
             Mmapper2Group *const gm,
             MumeClock *mc,
             MapCanvas *mca,
             qintptr &socketDescriptor,
             ConnectionListener *const listener)
    : QObject(nullptr)
    , m_mapData(md)
    , m_pathMachine(pm)
    , m_prespammedPath(pp)
    , m_groupManager(gm)
    , m_mumeClock(mc)
    , m_mapCanvas(mca)
    , m_listener(listener)
    , m_socketDescriptor(socketDescriptor)
    // TODO: pass this in as a non-owning pointer.
    , m_remoteEdit{makeQPointer<RemoteEdit>(m_listener->parent())}
{
    //
}

Proxy::~Proxy()
{
    if (auto userSocket = m_userSocket.data()) {
        userSocket->flush();
        userSocket->disconnectFromHost();
    }

    if (auto mudSocket = m_mudSocket.data()) {
        mudSocket->disconnectFromHost();
    }

    if (auto remoteEdit = m_remoteEdit.data()) {
        remoteEdit->deleteLater(); // Ugg.
    }
}

void Proxy::start()
{
    auto *const mw = dynamic_cast<MainWindow *>(m_listener->parent());
    if (mw == nullptr) {
        // dynamic cast can fail
        assert(false);
        return;
    }

    m_userSocket = [this]() -> QPointer<QTcpSocket> {
        auto userSock = makeQPointer<QTcpSocket>(this);
        if (!userSock->setSocketDescriptor(m_socketDescriptor)) {
            return {};
        }
        userSock->setSocketOption(QAbstractSocket::LowDelayOption, true);
        userSock->setSocketOption(QAbstractSocket::KeepAliveOption, true);
        return userSock;
    }();

    if (!m_userSocket) {
        deleteLater();
        // REVISIT: Under what conditions can this happen? This seems like a VERY serious failure,
        // so shouldn't we throw here instead of returning like everything's okay?
        return;
    }

    m_userTelnet = makeQPointer<UserTelnet>(this);
    m_mudTelnet = makeQPointer<MudTelnet>(this);
    m_telnetFilter = makeQPointer<TelnetFilter>(this);
    m_mpiFilter = makeQPointer<MpiFilter>(this);
    m_parserXml = makeQPointer<MumeXmlParser>(m_mapData, m_mumeClock, m_proxyParserApi, this);

    m_mudSocket = (NO_OPEN_SSL || !getConfig().connection.tlsEncryption)
                      ? QPointer<MumeSocket>(makeQPointer<MumeTcpSocket>(this))
                      : QPointer<MumeSocket>(makeQPointer<MumeSslSocket>(this));

    auto *const userSocket = m_userSocket.data();
    auto *const userTelnet = m_userTelnet.data();
    auto *const mudTelnet = m_mudTelnet.data();
    auto *const telnetFilter = m_telnetFilter.data();
    auto *const mpiFilter = m_mpiFilter.data();
    auto *const parserXml = m_parserXml.data();
    auto *const mudSocket = m_mudSocket.data();
    auto *const remoteEdit = m_remoteEdit.data();

    connect(this, &Proxy::log, mw, &MainWindow::log);

    connect(userSocket, &QAbstractSocket::disconnected, this, &Proxy::userTerminatedConnection);
    connect(userSocket, &QIODevice::readyRead, this, &Proxy::processUserStream);

    connect(userTelnet,
            &UserTelnet::analyzeUserStream,
            telnetFilter,
            &TelnetFilter::onAnalyzeUserStream);
    connect(userTelnet, &UserTelnet::sendToSocket, this, &Proxy::sendToUser);
    connect(userTelnet, &UserTelnet::relayNaws, mudTelnet, &MudTelnet::onRelayNaws);
    connect(userTelnet, &UserTelnet::relayTermType, mudTelnet, &MudTelnet::onRelayTermType);

    connect(mudTelnet,
            &MudTelnet::analyzeMudStream,
            telnetFilter,
            &TelnetFilter::onAnalyzeMudStream);
    connect(mudTelnet, &MudTelnet::sendToSocket, this, &Proxy::sendToMud);
    connect(mudTelnet, &MudTelnet::relayEchoMode, userTelnet, &UserTelnet::onRelayEchoMode);

    connect(this, &Proxy::analyzeUserStream, userTelnet, &UserTelnet::onAnalyzeUserStream);

    connect(telnetFilter,
            &TelnetFilter::parseNewMudInput,
            mpiFilter,
            &MpiFilter::analyzeNewMudInput);
    connect(mpiFilter, &MpiFilter::sendToMud, mudTelnet, &MudTelnet::onSendToMud);
    connect(mpiFilter, &MpiFilter::editMessage, remoteEdit, &RemoteEdit::remoteEdit);
    connect(mpiFilter, &MpiFilter::viewMessage, remoteEdit, &RemoteEdit::remoteView);
    connect(remoteEdit, &RemoteEdit::sendToSocket, mudTelnet, &MudTelnet::onSendToMud);

    connect(mpiFilter, &MpiFilter::parseNewMudInput, parserXml, &MumeXmlParser::parseNewMudInput);
    connect(telnetFilter,
            &TelnetFilter::parseNewUserInput,
            parserXml,
            &MumeXmlParser::parseNewUserInput);

    connect(parserXml, &MumeXmlParser::sendToMud, mudTelnet, &MudTelnet::onSendToMud);
    connect(parserXml, &MumeXmlParser::sig_sendToUser, userTelnet, &UserTelnet::onSendToUser);

    connect(parserXml,
            QOverload<const SigParseEvent &>::of(&MumeXmlParser::event),
            m_pathMachine,
            &Mmapper2PathMachine::event);
    connect(parserXml,
            &AbstractParser::releaseAllPaths,
            m_pathMachine,
            &PathMachine::releaseAllPaths);
    connect(parserXml, &AbstractParser::showPath, m_prespammedPath, &PrespammedPath::setPath);
    connect(parserXml, &AbstractParser::sig_mapChanged, m_mapCanvas, &MapCanvas::mapChanged);
    connect(parserXml,
            &AbstractParser::sig_graphicsSettingsChanged,
            m_mapCanvas,
            &MapCanvas::graphicsSettingsChanged);
    connect(parserXml, &AbstractParser::log, mw, &MainWindow::log);
    connect(parserXml, &AbstractParser::newRoomSelection, m_mapCanvas, &MapCanvas::setRoomSelection);

    connect(userSocket, &QAbstractSocket::disconnected, parserXml, &AbstractParser::reset);

    // Group Manager Support
    connect(parserXml,
            &MumeXmlParser::sendScoreLineEvent,
            m_groupManager,
            &Mmapper2Group::parseScoreInformation);
    connect(parserXml,
            &MumeXmlParser::sendPromptLineEvent,
            m_groupManager,
            &Mmapper2Group::parsePromptInformation);
    connect(parserXml,
            &MumeXmlParser::sendCharacterPositionEvent,
            m_groupManager,
            &Mmapper2Group::updateCharacterPosition);
    connect(parserXml,
            &MumeXmlParser::sendCharacterAffectEvent,
            m_groupManager,
            &Mmapper2Group::updateCharacterAffect);
    connect(parserXml,
            &AbstractParser::sendGroupTellEvent,
            m_groupManager,
            &Mmapper2Group::sendGroupTell);
    connect(parserXml,
            &AbstractParser::sendGroupKickEvent,
            m_groupManager,
            &Mmapper2Group::kickCharacter);
    connect(parserXml, &AbstractParser::showPath, m_groupManager, &Mmapper2Group::setPath);
    // Group Tell
    connect(m_groupManager,
            &Mmapper2Group::displayGroupTellEvent,
            parserXml,
            &AbstractParser::sendGTellToUser);

    emit log("Proxy", "Connection to client established ...");

    QByteArray ba = QString("\033[1;37;46mWelcome to MMapper!\033[0;37;46m"
                            "   Type \033[1m%1help\033[0m\033[37;46m for help.\033[0m\r\n")
                        .arg(getConfig().parser.prefixChar)
                        .toLatin1();
    m_userSocket->write(ba);

    connect(mudSocket, &MumeSocket::connected, this, &Proxy::onMudConnected);
    connect(mudSocket, &MumeSocket::connected, userTelnet, &UserTelnet::onConnected);
    connect(mudSocket, &MumeSocket::connected, mudTelnet, &MudTelnet::onConnected);
    connect(mudSocket, &MumeSocket::socketError, this, &Proxy::onMudError);
    connect(mudSocket, &MumeSocket::socketError, parserXml, &AbstractParser::reset);
    connect(mudSocket, &MumeSocket::socketError, m_groupManager, &Mmapper2Group::reset);
    connect(mudSocket, &MumeSocket::disconnected, this, &Proxy::mudTerminatedConnection);
    connect(mudSocket, &MumeSocket::disconnected, parserXml, &AbstractParser::reset);
    connect(mudSocket, &MumeSocket::disconnected, m_groupManager, &Mmapper2Group::reset);
    connect(mudSocket, &MumeSocket::processMudStream, mudTelnet, &MudTelnet::onAnalyzeMudStream);
    connect(mudSocket, &MumeSocket::log, mw, &MainWindow::log);

    connectToMud();
}

void Proxy::onMudConnected()
{
    const auto &settings = getConfig().mumeClientProtocol;

    m_serverState = ServerStateEnum::CONNECTED;

    emit log("Proxy", "Connection to server established ...");

    // send IAC-GA prompt request
    QByteArray idPrompt("~$#EP2\nG\n");
    emit log("Proxy", "Sent MUME Protocol Initiator IAC-GA prompt request");
    sendToMud(idPrompt);

    if (settings.remoteEditing) {
        QByteArray idRemoteEditing("~$#EI\n");
        emit log("Proxy", "Sent MUME Protocol Initiator remote editing request");
        sendToMud(idRemoteEditing);
    }

    sendToMud(QByteArray("~$#EX2\n3G\n"));
    emit log("Proxy", "Sent MUME Protocol Initiator XML request");
}

void Proxy::onMudError(const QString &errorStr)
{
    m_serverState = ServerStateEnum::ERROR;

    qWarning() << "Mud socket error" << errorStr;
    emit log("Proxy", errorStr);

    sendToUser("\r\n\033[1;37;46m" + errorStr.toLocal8Bit() + "\033[0m\r\n");

    if (!getConfig().connection.proxyConnectionStatus) {
        sendToUser("\r\n"
                   "\033[1;37;46mYou can explore the map offline or reconnect again...\033[0m\r\n");
        m_parserXml->sendPromptToUser();
        m_serverState = ServerStateEnum::OFFLINE;
    } else if (getConfig().general.mapMode == MapModeEnum::OFFLINE) {
        sendToUser("\r\n"
                   "\033[1;37;46mYou are now exploring the map offline.\033[0m\r\n");
        m_parserXml->sendPromptToUser();
        m_serverState = ServerStateEnum::OFFLINE;
    } else {
        // Terminate connection
        deleteLater();
    }
}

void Proxy::userTerminatedConnection()
{
    emit log("Proxy", "User terminated connection ...");
    deleteLater();
}

void Proxy::mudTerminatedConnection()
{
    if (!isConnected()) {
        return;
    }

    m_serverState = ServerStateEnum::DISCONNECTED;

    emit log("Proxy", "Mud terminated connection ...");

    sendToUser("\r\n\033[1;37;46mMUME closed the connection.\033[0m\r\n");

    if (getConfig().connection.proxyConnectionStatus) {
        if (getConfig().general.mapMode == MapModeEnum::OFFLINE) {
            sendToUser("\r\n"
                       "\033[1;37;46mYou are now exploring the map offline.\033[0m\r\n");
            m_parserXml->sendPromptToUser();
        } else {
            // Terminate connection
            deleteLater();
        }
    } else {
        sendToUser("\r\n"
                   "\033[1;37;46mYou can explore the map offline or reconnect again...\033[0m\r\n");
        m_parserXml->sendPromptToUser();
    }
}

void Proxy::processUserStream()
{
    if (m_userSocket != nullptr) {
        io::readAllAvailable(*m_userSocket, m_buffer, [this](const QByteArray &byteArray) {
            if (!byteArray.isEmpty())
                emit analyzeUserStream(byteArray);
        });
    }
}

void Proxy::sendToMud(const QByteArray &ba)
{
    if (m_mudSocket != nullptr) {
        if (m_mudSocket->state() != QAbstractSocket::ConnectedState) {
            sendToUser(
                "\033[1;37;46mMMapper is not connected to MUME. Please reconnect to play.\033[0m\r\n");
            m_parserXml->sendPromptToUser();
        } else {
            m_mudSocket->sendToMud(ba);
        }
    } else {
        qWarning() << "Mud socket not available";
    }
}

void Proxy::sendToUser(const QByteArray &ba)
{
    if (m_userSocket != nullptr) {
        m_userSocket->write(ba);
    } else {
        qWarning() << "User socket not available";
    }
}

bool Proxy::isConnected() const
{
    return m_serverState == ServerStateEnum::CONNECTED;
}

void Proxy::connectToMud()
{
    switch (m_serverState) {
    case ServerStateEnum::CONNECTING:
        sendToUser("Error: You're still connecting.\r\n");
        break;

    case ServerStateEnum::CONNECTED:
        sendToUser("Error: You're already connected.\r\n");
        break;

    case ServerStateEnum::DISCONNECTING:
        sendToUser("Error: You're still disconnecting.\r\n");
        break;

    case ServerStateEnum::INITIALIZED:
    case ServerStateEnum::OFFLINE:
    case ServerStateEnum::DISCONNECTED:
    case ServerStateEnum::ERROR: {
        if (getConfig().general.mapMode == MapModeEnum::OFFLINE) {
            sendToUser(
                "\r\n"
                "\033[1;37;46mMMapper is running in offline mode. Switch modes and reconnect to play MUME.\033[0m\r\n"
                "\r\n"
                "Welcome to the land of Middle-earth. May your visit here be... interesting.\r\n"
                "Never forget! Try to role-play...\r\n");
            m_parserXml->doMove(CommandEnum::LOOK);
            m_serverState = ServerStateEnum::OFFLINE;
            break;
        }

        if (auto sock = m_mudSocket.data()) {
            sendToUser("Connecting...\r\n");
            m_serverState = ServerStateEnum::CONNECTING;
            sock->connectToHost();
        } else {
            sendToUser("Internal eror while trying to connect.\r\n");
        }
        break;
    }
    }
}

void Proxy::disconnectFromMud()
{
    switch (m_serverState) {
    case ServerStateEnum::CONNECTING:
        sendToUser("Error: You're still connecting.\r\n");
        break;

    case ServerStateEnum::OFFLINE:
        m_serverState = ServerStateEnum::INITIALIZED;
        sendToUser("You disconnect your simulated link.\r\n");
        break;

    case ServerStateEnum::CONNECTED: {
        if (auto sock = m_mudSocket.data()) {
            sendToUser("Disconnecting...\r\n");
            m_serverState = ServerStateEnum::DISCONNECTING;
            sock->disconnectFromHost();
            sendToUser("Disconnected.\r\n");
            m_serverState = ServerStateEnum::DISCONNECTED;
        } else {
            sendToUser("Internal eror while trying to disconnect.\r\n");
        }
        break;
    }

    case ServerStateEnum::DISCONNECTING:
        sendToUser("Error: You're still disconnecting.\r\n");
        break;

    case ServerStateEnum::INITIALIZED:
    case ServerStateEnum::DISCONNECTED:
    case ServerStateEnum::ERROR:
        sendToUser("Error: You're not connected.\r\n");
        break;
    }
}
