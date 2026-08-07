// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

namespace chatterino {

struct HelixPolls;
struct HelixPrediction;
struct HelixPredictions;
struct HelixChatterGroups;

struct TwitchResubNotification {
    QString id;
    int cumulativeTenureMonths = 0;
    int months = 0;
    int streakTenureMonths = 0;
    QString token;
    bool isGiftSubscription = false;
    QString gifterDisplayName;
};

struct TwitchChannelModerationPermissions {
    bool addModerator = false;
    bool removeModerator = false;
    bool addVip = false;
    bool removeVip = false;
    bool deleteModComments = false;
};

enum class TwitchChannelRoleAction {
    AddModerator,
    RemoveModerator,
    AddVip,
    RemoveVip,
};

struct TwitchModComment {
    QString id;
    QString timestamp;
    QString text;
    QString channelId;
    QString channelLogin;
    QString authorId;
    QString authorLogin;
    QString authorDisplayName;
    bool isShareable = false;
};

class TwitchWebApi
{
public:
    static void getChannelModerationPermissions(
        const QString &channelId, const QString &userId,
        const QString &oauthClient, const QString &oauthToken,
        std::function<void(const TwitchChannelModerationPermissions &)>
            successCallback,
        std::function<void(const QString &)> failureCallback);

    static void updateChannelRole(
        TwitchChannelRoleAction action, const QString &channelId,
        const QString &targetLogin, const QString &oauthClient,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void getModComments(
        const QString &channelId, const QString &targetId,
        const QString &oauthClient, const QString &oauthToken,
        std::function<void(const QVector<TwitchModComment> &)> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void deleteModComment(
        const QString &channelId, const QString &commentId,
        const QString &oauthClient, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void startPoll(const QString &channelId, const QString &title,
                          const QStringList &choices, int durationSeconds,
                          std::optional<int> pointsPerVote,
                          const QString &oauthClient, const QString &oauthToken,
                          std::function<void()> successCallback,
                          std::function<void(const QString &)> failureCallback);

    static void getPolls(
        const QString &channelId, QStringList ids, int first,
        const QString &after, const QString &oauthClient,
        const QString &oauthToken,
        std::function<void(const HelixPolls &)> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void endPoll(const QString &channelId, const QString &pollId,
                        const QString &oauthClient,
                        const QString &oauthToken,
                        std::function<void()> successCallback,
                        std::function<void(const QString &)> failureCallback);

    static void startPrediction(
        const QString &channelId, const QString &title,
        const QStringList &outcomes, int predictionWindowSeconds,
        const QString &oauthClient, const QString &oauthToken,
        std::function<void(const QString &, const QJsonObject &)>
            successCallback,
        std::function<void(const QString &)> failureCallback);

    static void makePrediction(
        const QString &predictionId, const QString &outcomeId, int points,
        const QString &oauthClient, const QString &oauthToken,
        std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void getPredictions(
        const QString &channelId, QStringList ids, int first,
        const QString &after, const QString &oauthClient,
        const QString &oauthToken,
        std::function<void(const HelixPredictions &)> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void getActivePollAndPredictions(
        const QString &channelId, const QString &oauthClient,
        const QString &oauthToken,
        std::function<void(const HelixPolls &, const HelixPredictions &)>
            successCallback,
        std::function<void(const QString &)> failureCallback);

    static void getChatterGroups(
        const QString &broadcasterName, const QString &oauthClient,
        const QString &oauthToken, const QString &clientIntegrity,
        const QString &deviceId,
        std::function<void(const HelixChatterGroups &)> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void getResubNotification(
        const QString &channelLogin, const QString &oauthClient,
        const QString &oauthToken,
        std::function<void(std::optional<TwitchResubNotification>)>
            successCallback,
        std::function<void(const QString &)> failureCallback);

    static void shareResubNotification(
        const QString &channelLogin, const QString &tokenID,
        const QString &message, bool includeStreak, const QString &oauthClient,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void getModeratorLogins(
        const QString &broadcasterId, const QString &oauthClient,
        const QString &oauthToken,
        std::function<void(const QStringList &)> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void getVipLogins(
        const QString &broadcasterId, const QString &oauthClient,
        const QString &oauthToken,
        std::function<void(const QStringList &)> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void endPrediction(
        const QString &channelId, const QString &predictionId,
        bool refundPoints, const QString &winningOutcomeId,
        const QString &oauthClient, const QString &oauthToken,
        std::function<void(const HelixPrediction &)> successCallback,
        std::function<void(const QString &)> failureCallback);

    static void endPredictionEvent(
        const QString &predictionId, bool refundPoints,
        const QString &winningOutcomeId, const QString &oauthClient,
        const QString &oauthToken, std::function<void()> successCallback,
        std::function<void(const QString &)> failureCallback);
};

}  // namespace chatterino
