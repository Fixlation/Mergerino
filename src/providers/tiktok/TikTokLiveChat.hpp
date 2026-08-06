// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"

#include <pajlada/signals/signal.hpp>
#include <QString>
#include <QUrl>

#include <memory>

namespace chatterino {

class TikTokLiveChat
{
public:
    explicit TikTokLiveChat(QString source, bool showJoinMessages = true);
    ~TikTokLiveChat();

    void start();
    void stop();
    void setShowJoinMessages(bool enabled);

    bool isLive() const;
    const QString &roomId() const;
    const QString &statusText() const;
    const QString &liveTitle() const;
    const QString &resolvedSource() const;

    static QString normalizeSource(const QString &source);
    static QString cachedAvatarUrl(const QString &userName,
                                   const QString &userID = {});

    pajlada::Signals::Signal<QString> sourceResolved;
    pajlada::Signals::Signal<MessagePtr> messageReceived;
    pajlada::Signals::Signal<MessagePtr> systemMessageReceived;
    pajlada::Signals::NoArgSignal liveStatusChanged;

private:
    struct BrowserSession;

    void resolveRoom();
    void resolveRoomFromHtml();
    void applyResolvedRoomInfo(QString roomId, QString uniqueId,
                               QString nickname, QString liveTitle);

    void startBrowserSession();
    void stopBrowserSession();
    void ensureBrowserTarget();
    void connectBrowserTarget(const QUrl &devtoolsUrl);
    void requestBrowserSnapshot();
    void handleBrowserSocketOpened();
    void handleBrowserSocketClosed();
    void handleBrowserSocketMessage(QByteArray data);
    void handleBrowserSnapshot(const QByteArray &payload);
    void scheduleBrowserTargetCheck(int delayMs);
    void scheduleBrowserSnapshot(int delayMs);

    void scheduleResolve(int delayMs);
    void waitForNextLive(QString text, int retryDelayMs);
    void setLive(bool live);
    void setStatusText(QString text, bool notifyAsSystemMessage = false);
    QString sourceLabel() const;

    QString source_;
    QString resolvedSource_;
    QString roomId_;
    QString statusText_;
    QString liveTitle_;
    QString ownerNickname_;
    QString ownerUniqueId_;
    QString joinedRoomId_;

    bool running_{false};
    bool live_{false};
    bool failureReported_{false};
    bool browserSnapshotReceived_{false};
    bool showJoinMessages_{true};

    std::shared_ptr<bool> lifetimeGuard_;
    std::unique_ptr<BrowserSession> browser_;
};

}  // namespace chatterino
