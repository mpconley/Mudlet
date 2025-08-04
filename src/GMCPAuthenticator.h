#ifndef MUDLET_AUTHENTICATOR_H
#define MUDLET_AUTHENTICATOR_H

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

#include "Host.h"
#include "utils.h"

#include "pre_guard.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTimer>
#include <QVariantMap>
#include "post_guard.h"

class GMCPAuthenticator : public QObject
{
    Q_OBJECT

public:
    explicit GMCPAuthenticator(Host* pHost);
    ~GMCPAuthenticator();

    void saveSupportsSet(const QString& data);
    void sendCredentials();
    void handleAuthResult(const QString& data);
    void handleAuthGMCP(const QString& packageMessage, const QString& data);
    
    bool isAuthenticationInProgress() const;
    void forceCleanupAuthentication();

private slots:
    void handleCallbackConnection();
    void handleOIDCTimeout();

private:
    void requestOIDCAuth(const QString& provider);
    void handleAuthChallenge(const QString& data);
    void handleOIDCCallback(const QString& code, const QString& state);
    
    bool startCallbackListener();
    void generatePKCEChallenge();
    void sendGMCPMessage(const QString& command, const QJsonObject& data);
    void cleanupOIDCSession();

    Host* mpHost;
    QStringList mSupportedAuthTypes;
    
    // OIDC specific members
    QString mPendingState;
    QString mCodeVerifier;
    QString mCodeChallenge;
    QString mRedirectUri;
    QTcpServer* mCallbackServer;
    QTimer* mOIDCTimeoutTimer;
    bool mAuthInProgress;          // Track whether authentication is in progress
    bool mServerListening;         // Track server listening state
    
    void debugServerState(const QString& context) const;
    
    static constexpr int OIDC_TIMEOUT_MS = 300000; // 5 minutes
};

#endif // MUDLET_AUTHENTICATOR_H
