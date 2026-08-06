// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"
#include "util/QStringHash.hpp"

#include <pajlada/signals/signal.hpp>
#include <QDateTime>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

class QJsonObject;
class QJsonValue;

namespace chatterino {

class YouTubeAccount;

class YouTubeLiveChat
{
public:
    explicit YouTubeLiveChat(QString streamUrl);
    ~YouTubeLiveChat();

    void start();
    void stop();
    void setDiscoveryForeground(bool foreground);
    void sendMessage(const QString &message);
    void deleteMessage(const QString &messageId);
    void banUser(const QString &channelId, const QString &displayName = {});
    void timeoutUser(const QString &channelId, int durationSeconds,
                     const QString &displayName = {});
    void unbanUser(const QString &channelId, const QString &displayName = {});

    bool isLive() const;
    bool hasModeratorPrivileges() const;
    const QString &videoId() const;
    const QString &statusText() const;
    const QString &liveTitle() const;
    uint64_t liveViewerCount() const;
    QString liveUptime() const;
    QString previewThumbnailUrl() const;
    QString resolvedSource() const;

    pajlada::Signals::Signal<QString> sourceResolved;
    pajlada::Signals::Signal<MessagePtr> messageReceived;
    pajlada::Signals::Signal<MessagePtr> systemMessageReceived;
    pajlada::Signals::NoArgSignal liveStatusChanged;
    pajlada::Signals::NoArgSignal moderationStatusChanged;
    pajlada::Signals::Signal<int> sendWaitUpdate;

private:
    using ActiveLiveChatIdCallback =
        std::function<void(std::shared_ptr<YouTubeAccount>, QString)>;

    void resolveVideoId();
    void resolveChannelIdFromSource(const QString &source,
                                    std::function<void(QString)> onResolved);
    void resolveChannelIdFromHandle(const QString &handle,
                                    std::function<void(QString)> onResolved);
    void resolveChannelIdFromVideoId(const QString &videoId,
                                     std::function<void(QString)> onResolved);
    void resolveChannelIdFromSearch(const QString &source,
                                    std::function<void(QString)> onResolved);
    void probeLiveVideoIdFromSource(const QString &source,
                                    std::function<void(QString)> onResolved);
    void probeLiveVideoIdFromEmbed(const QString &channelId,
                                   std::function<void(QString)> onResolved);
    void probeLiveVideoIdFromPath(const QString &path,
                                  std::function<void(QString)> onResolved);
    void probeLiveVideoIdFromBrowse(const QString &channelId,
                                    std::function<void(QString)> onResolved);
    void probeLiveVideoIdFromBrowseTab(const QString &channelId,
                                       const QString &params,
                                       const QString &refererPath,
                                       std::function<void(QString)> onResolved);
    void bootstrapInnertubeContext(std::function<void()> onReady,
                                   QString failureText);
    void bootstrapInnertubeContextFromUrls(
        QStringList urls, int index, std::shared_ptr<std::function<void()>> onReady,
        QString failureText, QString blockedText = {});
    void fetchLiveChatPage(bool skipInitialBacklog = true);
    void fetchLiveChatPopoutPage(bool skipInitialBacklog,
                                 bool activeLiveRefresh,
                                 const QString &requestedVideoId);
    void handleMissingLiveChatContinuation(const QString &requestedVideoId,
                                           bool skipInitialBacklog,
                                           bool activeLiveRefresh);
    void beginLiveChatSession(QString continuation, bool skipInitialBacklog,
                              bool activeLiveRefresh,
                              const QJsonObject &nextResponse = {});
    void ensureActiveLiveChatId(std::shared_ptr<YouTubeAccount> account,
                                ActiveLiveChatIdCallback callback,
                                QString operationName);
    void postLiveChatMessage(std::shared_ptr<YouTubeAccount> account,
                             QString liveChatId, QString message);
    void postLiveChatBan(std::shared_ptr<YouTubeAccount> account,
                         QString liveChatId, QString channelId,
                         QString displayName, int durationSeconds);
    void deleteLiveChatBan(std::shared_ptr<YouTubeAccount> account,
                           QString banId, QString channelId,
                           QString displayName);
    void verifySourceLiveAfterMissingContinuation(
        const QString &requestedVideoId, bool skipInitialBacklog);
    void poll();
    void refreshLiveChatContinuation(QString text, int retryDelayMs);
    void schedulePoll(int delayMs);
    void scheduleHealthCheck(int delayMs);
    void scheduleResolve(int delayMs);
    int liveDiscoveryRetryDelayMs() const;
    QStringList innertubeBootstrapUrls() const;
    bool updateInnertubeContextFromHtml(const QString &html);
    void finishInnertubeBootstrapFailure(QString failureText,
                                         QString blockedText);
    void invalidateLiveChatSession();
    void recoverLiveChat(QString text, int retryDelayMs,
                         bool notifyAsSystemMessage = true);
    void waitForNextLive(QString text, int retryDelayMs);
    void resetInnertubeContext();
    void resetModeratorPrivileges();
    void setSendWait(std::chrono::milliseconds waitTime);
    void emitSendWait();
    bool moderationToolsDisabledForCurrentAccount() const;
    void reportModerationToolsUnavailable();
    void reportModerationActionFailure(const QString &action,
                                       const QString &error);
    void updateModeratorPrivilegesFromRenderer(const QJsonObject &renderer);
    void updateLiveViewerCount(uint64_t viewerCount);
    void setLive(bool live);
    void setStatusText(QString text, bool notifyAsSystemMessage = false);
    bool shouldResolveLiveStreamFromSource() const;

    static QString maybeExtractVideoId(const QString &url);
    static QString normalizeSource(const QString &source);
    static bool isLikelyChannelId(const QString &value);
    static QString sourceLivePath(const QString &source);
    static QString sourceStreamsPath(const QString &source);
    static bool isLikelyVideoId(const QString &value);
    static QString extractFirstMatch(const QString &text,
                                     const QStringList &patterns);
    static QString extractLiveChatContinuation(
        const QJsonObject &liveChatRenderer);
    static QString extractVideoChannelId(const QString &html);
    static QString extractEmbedLiveVideoId(const QString &html);
    static QString extractLiveVideoId(const QString &html,
                                      bool allowPageCanonical);
    static QString extractLiveOwnerChannelId(const QJsonObject &nextResponse);
    static QString extractLiveStreamTitle(const QJsonObject &nextResponse);
    static uint64_t extractLiveViewerCount(const QJsonValue &value);
    static QDateTime extractLiveStartTime(const QJsonObject &nextResponse);
    static bool rendererHasModeratorBadge(const QJsonObject &renderer);
    static QString parseText(const QJsonValue &value);
    static uint64_t parseViewerCount(const QJsonValue &value);
    static MessagePtr parseRendererMessage(const QJsonObject &renderer,
                                          const QString &rendererName,
                                          const QString &channelName);

    QString streamUrl_;
    QString videoId_;
    QString apiKey_;
    QString clientVersion_;
    QString visitorData_;
    QString continuation_;
    QString statusText_;
    QString liveTitle_;
    QString liveOwnerChannelId_;
    QString activeLiveChatVideoId_;
    QString activeLiveChatId_;
    QString moderatorPrivilegesVideoId_;
    QString moderatorPrivilegesAccountChannelId_;
    QString moderationDisabledVideoId_;
    QString moderationDisabledAccountChannelId_;
    QDateTime liveStartedAt_;

    bool running_{false};
    bool live_{false};
    bool hasModeratorPrivileges_{false};
    bool failureReported_{false};
    bool skipInitialBacklog_{false};
    bool discoveryForeground_{false};
    int activePollStreak_{0};
    int pollRefreshFallbackCount_{0};
    int resolveGeneration_{0};
    int chatSessionGeneration_{0};
    uint64_t liveViewerCount_{0};
    QElapsedTimer liveChatSessionRefreshTimer_;
    QElapsedTimer liveChatProgressTimer_;
    QTimer sendWaitTimer_;
    std::optional<std::chrono::steady_clock::time_point> sendWaitEnd_;

    std::shared_ptr<bool> lifetimeGuard_;
    std::unordered_set<QString> seenMessageIds_;
    std::unordered_map<QString, QString> liveChatBanIds_;
    QString joinedLiveVideoId_;
};

}  // namespace chatterino
