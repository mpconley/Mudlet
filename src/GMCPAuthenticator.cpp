/***************************************************************************
 *   Copyright (C) 2024 by Vadim Peretokin - vperetokin@gmail.com          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "GMCPAuthenticator.h"

#include "Host.h"
#include "ctelnet.h"

#include "pre_guard.h"
#include <QCryptographicHash>
#include <QDebug>
#include <QDesktopServices>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QUrl>
#include "post_guard.h"

GMCPAuthenticator::GMCPAuthenticator(Host* pHost)
: QObject(pHost)
, mpHost(pHost)
, mCallbackServer(nullptr)
, mOIDCTimeoutTimer(nullptr)
, mAuthInProgress(false)
, mServerListening(false)
{
    debugServerState("Constructor");
}

GMCPAuthenticator::~GMCPAuthenticator()
{
    cleanupOIDCSession();
}

void GMCPAuthenticator::saveSupportsSet(const QString& data)
{
    auto jsonDoc = QJsonDocument::fromJson(data.toUtf8());
    auto jsonObj = jsonDoc.object();

    if (jsonObj.contains("type")) {
        QJsonArray typesArray = jsonObj["type"].toArray();
        for (const auto& type : typesArray) {
            mSupportedAuthTypes.append(type.toString());
        }
    }

#if defined(DEBUG_GMCP_AUTHENTICATION)
    qDebug() << "Supported auth types:" << mSupportedAuthTypes;
#endif
}

void GMCPAuthenticator::sendCredentials()
{
    auto character = mpHost->getLogin();
    auto password = mpHost->getPass();
    QJsonObject credentials;
    if (!character.isEmpty() && !password.isEmpty()) {
        credentials["account"] = character;
        credentials["password"] = password;
    }
    QJsonDocument doc(credentials);
    QString gmcpMessage = doc.toJson(QJsonDocument::Compact);

    std::string output;
    output += TN_IAC;
    output += TN_SB;
    output += OPT_GMCP;
    output += "Char.Login.Credentials ";
    output += mpHost->mTelnet.encodeAndCookBytes(gmcpMessage.toStdString());
    output += TN_IAC;
    output += TN_SE;

    // Send credentials to server
    mpHost->mTelnet.socketOutRaw(output);
#if defined(DEBUG_GMCP_AUTHENTICATION)
    qDebug() << "Sent GMCP credentials";
#endif
}


void GMCPAuthenticator::handleAuthResult(const QString& data)
{
    auto doc = QJsonDocument::fromJson(data.toUtf8());
    auto obj = doc.object();

    // some game drivers can parse JSON for true or false, but may not be able to write booleans back
    auto result = obj[qsl("success")];
    bool success = (result.isBool() && result.toBool()) || (result.isString() && result.toString() == "true");
    auto message = obj[qsl("message")].toString();

    if (success) {
        QString user = obj[qsl("user")].toString();

        if (!user.isEmpty()) {
            mpHost->postMessage(tr("[ INFO ]  - Successfully authenticated as: %1").arg(user));
        } else {
            mpHost->postMessage(tr("[ INFO ]  - Authentication successful"));
        }
#if defined(DEBUG_GMCP_AUTHENTICATION)
        qDebug() << "GMCP login successful";
#endif
    } else {
#if defined(DEBUG_GMCP_AUTHENTICATION)
        qDebug() << "GMCP login failed:" << message;
#endif
        mpHost->mTelnet.setDontReconnect(true);
        if (message.isEmpty()) {
            mpHost->postMessage(tr("[ WARN ]  - Could not log in to the game, is the login information correct?"));
        } else {
            //: %1 shows the reason for failure, could be authentication, etc.
            mpHost->postMessage(tr("[ WARN ]  - Could not log in to the game: %1").arg(message));
        }
    }
    
    // Clean up OIDC session if it was active
    cleanupOIDCSession();
}

// controller for GMCP authentication
void GMCPAuthenticator::handleAuthGMCP(const QString& packageMessage, const QString& data)
{
    if (packageMessage == qsl("Char.Login.Default")) {
        saveSupportsSet(data);

        // Check for OIDC support first, but only if user has set a default provider
        QString defaultProvider = mpHost->getDefaultOIDCProvider();

        if (!defaultProvider.isEmpty() && mSupportedAuthTypes.contains(qsl("oidc"))) {
            mpHost->mTelnet.cancelLoginTimers();
            requestOIDCAuth(defaultProvider);
        } else if (mSupportedAuthTypes.contains(qsl("password-credentials"))) {
            mpHost->mTelnet.cancelLoginTimers();
            sendCredentials();
        } else {
#if defined(DEBUG_GMCP_AUTHENTICATION)
            qDebug() << "Server does not support credentials authentication and we don't support any other";
#endif
        }
        return;
    }

    if (packageMessage == qsl("Char.Login.Challenge")) {
        handleAuthChallenge(data);
        return;
    }

    if (packageMessage == qsl("Char.Login.Result")) {
        handleAuthResult(data);
        return;
    }

#if defined(DEBUG_GMCP_AUTHENTICATION)
    qDebug() << "Unknown GMCP auth package:" << packageMessage;
#endif
}

void GMCPAuthenticator::requestOIDCAuth(const QString& provider)
{
    debugServerState("Before requestOIDCAuth");
    
    // Check if authentication is already in progress
    if (mAuthInProgress) {
        qWarning() << "GMCPAuthenticator: Authentication already in progress, ignoring new request";
        return;
    }
    
    // Generate PKCE parameters for security (RFC 7636)
    generatePKCEChallenge();
    
    // Start callback listener (RFC 8252 - loopback interface)
    if (!startCallbackListener()) {
        mpHost->postMessage(tr("[ ERROR ] - Failed to start authentication callback listener"));
        debugServerState("Failed to start callback listener");
        return;
    }
    
    // Setup timeout for OIDC process
    if (!mOIDCTimeoutTimer) {
        mOIDCTimeoutTimer = new QTimer(this);
        mOIDCTimeoutTimer->setSingleShot(true);
        connect(mOIDCTimeoutTimer, &QTimer::timeout, this, &GMCPAuthenticator::handleOIDCTimeout);
    }

    mOIDCTimeoutTimer->start(OIDC_TIMEOUT_MS);
    mAuthInProgress = true;
    
    QJsonObject request;
    request["type"] = "oidc";
    request["provider"] = provider;
    request["redirect_uri"] = mRedirectUri;
    request["code_challenge"] = mCodeChallenge;
    request["code_challenge_method"] = "S256";
    
    sendGMCPMessage("Char.Login.Request", request);
    
    qDebug() << "GMCPAuthenticator: Requested OIDC auth with provider:" << provider;
    debugServerState("After requestOIDCAuth");
}

void GMCPAuthenticator::handleAuthChallenge(const QString& data)
{
    auto doc = QJsonDocument::fromJson(data.toUtf8());
    auto obj = doc.object();
    
    QString method = obj["method"].toString();
    QString authUrl = obj["url"].toString();
    QString state = obj["state"].toString();
    
    if (method == "oidc") {
        mPendingState = state;
        
        // RFC 8252: Use external user-agent (system browser)
        if (!QDesktopServices::openUrl(QUrl(authUrl))) {
            mpHost->postMessage(tr("[ ERROR ] - Failed to open browser for authentication"));
            cleanupOIDCSession();
            return;
        }
        
        mpHost->postMessage(tr("[ INFO ]  - Please complete authentication in your browser"));
        
#if defined(DEBUG_GMCP_AUTHENTICATION)
        qDebug() << "Opened browser for OIDC authentication";
#endif
    }
}

void GMCPAuthenticator::handleOIDCCallback(const QString& code, const QString& state)
{
    // RFC 8252: Validate state parameter (CSRF protection)
    if (state != mPendingState) {
        mpHost->postMessage(tr("[ ERROR ] - Authentication state mismatch"));
        cleanupOIDCSession();
        return;
    }
    
    // Send authorization code + PKCE verifier to server
    QJsonObject response;
    response["code"] = code;
    response["state"] = state;
    response["code_verifier"] = mCodeVerifier;
    
    sendGMCPMessage("Char.Login.Response", response);
    
#if defined(DEBUG_GMCP_AUTHENTICATION)
    qDebug() << "Sent OIDC callback response to server";
#endif
}

bool GMCPAuthenticator::startCallbackListener()
{
    debugServerState("Before starting callback listener");
    
    // Check if we already have a running server
    if (mCallbackServer && mCallbackServer->isListening()) {
        qWarning() << "GMCPAuthenticator: Callback server already listening, reusing existing server";
        mServerListening = true;
        return true;
    }
    
    // Clean up any existing server that's not listening
    if (mCallbackServer && !mCallbackServer->isListening()) {
        qDebug() << "GMCPAuthenticator: Cleaning up non-listening server";
        mCallbackServer->deleteLater();
        mCallbackServer = nullptr;
        mServerListening = false;
    }
    
    // RFC 8252: Use loopback interface for redirect
    if (!mCallbackServer) {
        qDebug() << "GMCPAuthenticator: Creating new callback server";
        mCallbackServer = new QTcpServer(this);
        connect(mCallbackServer, &QTcpServer::newConnection, 
                this, &GMCPAuthenticator::handleCallbackConnection);
    }
    
    if (!mCallbackServer->listen(QHostAddress::LocalHost)) {
        qWarning() << "GMCPAuthenticator: Failed to start callback listener:" << mCallbackServer->errorString();
        mServerListening = false;
        debugServerState("Failed to start listener");
        return false;
    }
    
    quint16 port = mCallbackServer->serverPort();
    mRedirectUri = QString("http://127.0.0.1:%1/callback").arg(port);
    mServerListening = true;
    
    qDebug() << "GMCPAuthenticator: Callback listener started on" << mRedirectUri;
    debugServerState("After starting callback listener");
    
    return true;
}

void GMCPAuthenticator::handleCallbackConnection()
{
    if (!mCallbackServer) {
        return;
    }
    
    QTcpSocket* socket = mCallbackServer->nextPendingConnection();
    connect(socket, &QTcpSocket::readyRead, [this, socket]() {
        QByteArray data = socket->readAll();
        QString request = QString::fromUtf8(data);
        
        // Parse authorization code from callback
        QRegularExpression codeRegex(R"(code=([^&\s]+))");
        QRegularExpression stateRegex(R"(state=([^&\s]+))");
        
        auto codeMatch = codeRegex.match(request);
        auto stateMatch = stateRegex.match(request);
        
        if (codeMatch.hasMatch() && stateMatch.hasMatch()) {
            QString code = codeMatch.captured(1);
            QString state = stateMatch.captured(1);
            
            // Send success response to browser
            socket->write("HTTP/1.1 200 OK\r\n\r\n"
                         "<html><body><h1>Authentication Complete</h1>"
                         "<p>You can close this window and return to Mudlet.</p>"
                         "</body></html>");
            
            handleOIDCCallback(code, state);
        } else {
            // Send error response
            socket->write("HTTP/1.1 400 Bad Request\r\n\r\n"
                         "<html><body><h1>Authentication Failed</h1></body></html>");
            mpHost->postMessage(tr("[ ERROR ] - Invalid authentication callback received"));
            cleanupOIDCSession();
        }
        
        socket->disconnectFromHost();
    });
}

void GMCPAuthenticator::generatePKCEChallenge()
{
    // Generate code_verifier (RFC 7636: 43-128 characters)
    const QString chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    mCodeVerifier.clear();
    
    for (int i = 0; i < 128; ++i) {
        mCodeVerifier += chars[QRandomGenerator::global()->bounded(chars.length())];
    }
    
    // Generate code_challenge = base64url(sha256(code_verifier))
    QByteArray hash = QCryptographicHash::hash(mCodeVerifier.toUtf8(), QCryptographicHash::Sha256);
    mCodeChallenge = hash.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

void GMCPAuthenticator::sendGMCPMessage(const QString& command, const QJsonObject& data)
{
    QJsonDocument doc(data);
    QString gmcpMessage = doc.toJson(QJsonDocument::Compact);

    std::string output;
    output += TN_IAC;
    output += TN_SB;
    output += OPT_GMCP;
    output += command.toStdString() + " ";
    output += mpHost->mTelnet.encodeAndCookBytes(gmcpMessage.toStdString());
    output += TN_IAC;
    output += TN_SE;

    mpHost->mTelnet.socketOutRaw(output);
}

void GMCPAuthenticator::handleOIDCTimeout()
{
    mpHost->postMessage(tr("[ ERROR ] - OIDC authentication timed out"));
    cleanupOIDCSession();
}

void GMCPAuthenticator::cleanupOIDCSession()
{
    debugServerState("Starting cleanup");
    
    // Clean up sensitive data
    mPendingState.clear();
    mCodeVerifier.clear();
    mCodeChallenge.clear();
    mRedirectUri.clear();
    
    if (mCallbackServer) {
        if (mCallbackServer->isListening()) {
            qDebug() << "GMCPAuthenticator: Stopping callback server during cleanup";
            mCallbackServer->close();
        }
        mCallbackServer->deleteLater();
        mCallbackServer = nullptr;
        mServerListening = false;
    }
    
    if (mOIDCTimeoutTimer) {
        mOIDCTimeoutTimer->stop();
        mOIDCTimeoutTimer->deleteLater();
        mOIDCTimeoutTimer = nullptr;
    }
    
    mAuthInProgress = false;
    debugServerState("Cleanup complete");
}

bool GMCPAuthenticator::isAuthenticationInProgress() const
{
    return mAuthInProgress;
}

void GMCPAuthenticator::forceCleanupAuthentication()
{
    qWarning() << "GMCPAuthenticator: Force cleanup requested";
    debugServerState("Before force cleanup");
    
    if (mCallbackServer && mCallbackServer->isListening()) {
        qWarning() << "GMCPAuthenticator: Force stopping callback server";
        mCallbackServer->close();
    }
    
    cleanupOIDCSession();
}

void GMCPAuthenticator::debugServerState(const QString& context) const
{
    qDebug() << QString("GMCPAuthenticator [%1]: AuthInProgress=%2, ServerListening=%3, HasServer=%4, IsListening=%5")
                .arg(context)
                .arg(mAuthInProgress ? "true" : "false")
                .arg(mServerListening ? "true" : "false")
                .arg(mCallbackServer ? "true" : "false")
                .arg((mCallbackServer && mCallbackServer->isListening()) ? "true" : "false");
}
