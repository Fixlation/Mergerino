// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/CurrentUserBadges.hpp"
#include "providers/twitch/api/TwitchModerationAuth.hpp"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUuid>
#include <QUrl>
#include <QVector>
#include <QtCore/qscopeguard.h>

#include <functional>
#include <utility>

namespace chatterino::twitch {
namespace detail {

inline constexpr auto TWITCH_BADGE_IDENTITY_GQL_CLIENT_VERSION =
    "ef928475-9403-42f2-8a34-55784bd08e16";
inline constexpr auto TWITCH_BADGE_IDENTITY_GQL_USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/108.0.0.0 Safari/537.36";

inline QString twitchBadgeIdentitySessionId()
{
    static const QString sessionId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    return sessionId;
}

inline QString twitchBadgeIdentityDeviceId()
{
    static const QString deviceId =
        QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
    return deviceId;
}

inline QString strippedTwitchOAuthToken(QString token)
{
    token = token.trimmed();
    for (const auto &prefix : {
             QStringLiteral("OAuth "),
             QStringLiteral("Bearer "),
             QStringLiteral("oauth:"),
         })
    {
        if (token.startsWith(prefix, Qt::CaseInsensitive))
        {
            token = token.mid(prefix.size()).trimmed();
            break;
        }
    }
    return token;
}

inline QString highResolutionBadgeImageUrl(QString imageUrl)
{
    imageUrl = imageUrl.trimmed();
    if (imageUrl.isEmpty())
    {
        return {};
    }

    QUrl url(imageUrl);
    auto path = url.path();
    if (path.endsWith(QStringLiteral("/1")) ||
        path.endsWith(QStringLiteral("/2")))
    {
        path.chop(1);
        path += QLatin1Char('3');
        url.setPath(path);
        return url.toString();
    }

    return imageUrl;
}

inline bool setTwitchBadgeIdentityRequestHeaders(
    QNetworkRequest &request, const TwitchModerationAuth::Account &account,
    const std::function<void(const QString &)> &failureCallback)
{
    if (!account.supportsWebGql())
    {
        failureCallback(
            QStringLiteral("Twitch browser helper token required."));
        return false;
    }

    const auto token = strippedTwitchOAuthToken(account.oauthToken);
    if (token.isEmpty())
    {
        failureCallback(
            QStringLiteral("Missing Twitch browser helper token."));
        return false;
    }

    const auto requestDeviceId =
        account.deviceId.trimmed().isEmpty() ? twitchBadgeIdentityDeviceId()
                                             : account.deviceId.trimmed();

    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Authorization",
                         QByteArray("OAuth ") + token.toUtf8());
    request.setRawHeader("Client-Id", account.clientId.trimmed().toUtf8());
    request.setRawHeader("Client-Session-Id",
                         twitchBadgeIdentitySessionId().toUtf8());
    request.setRawHeader("Client-Version",
                         TWITCH_BADGE_IDENTITY_GQL_CLIENT_VERSION);
    request.setRawHeader("Origin", "https://www.twitch.tv");
    request.setRawHeader("Referer", "https://www.twitch.tv/");
    request.setRawHeader("User-Agent", TWITCH_BADGE_IDENTITY_GQL_USER_AGENT);
    request.setRawHeader("X-Device-Id", requestDeviceId.toUtf8());

    const auto clientIntegrity = account.clientIntegrity.trimmed();
    if (!clientIntegrity.isEmpty())
    {
        request.setRawHeader("Client-Integrity", clientIntegrity.toUtf8());
    }
    return true;
}

inline QString twitchBadgeIdentityQuery()
{
    return QStringLiteral(
        "query MergerinoChatIdentityBadges($channelLogin: String!) {"
        "  user(login: $channelLogin) {"
        "    id login displayName"
        "    self {"
        "      selectedBadge { ...MergerinoBadgeIdentityFields }"
        "      displayBadges { ...MergerinoBadgeIdentityFields }"
        "      availableBadges { ...MergerinoBadgeIdentityFields }"
        "    }"
        "  }"
        "}"
        "fragment MergerinoBadgeIdentityFields on Badge {"
        "  id setID version title description clickURL imageURL"
        "  user { id login displayName }"
        "}");
}

inline QString firstTwitchBadgeIdentityGqlError(const QJsonObject &root)
{
    const auto errors = root.value(QStringLiteral("errors")).toArray();
    for (const auto &errorValue : errors)
    {
        const auto error = errorValue.toObject();
        auto message = error.value(QStringLiteral("message")).toString();
        if (message.trimmed().isEmpty())
        {
            message = error.value(QStringLiteral("reason")).toString();
        }
        if (!message.trimmed().isEmpty())
        {
            return message.trimmed();
        }
    }
    return {};
}

inline CurrentUserBadgeIdentity badgeIdentityFromObject(
    const QJsonObject &object, bool selected, bool displayed, bool available)
{
    CurrentUserBadgeIdentity badge;
    badge.id = object.value(QStringLiteral("id")).toString().trimmed();
    badge.setID = object.value(QStringLiteral("setID")).toString().trimmed();
    badge.versionID =
        object.value(QStringLiteral("version")).toString().trimmed();
    badge.title = object.value(QStringLiteral("title")).toString().trimmed();
    badge.description =
        object.value(QStringLiteral("description")).toString().trimmed();
    badge.imageUrl = highResolutionBadgeImageUrl(
        object.value(QStringLiteral("imageURL")).toString());
    badge.selected = selected;
    badge.displayed = displayed;
    badge.available = available;

    const auto user = object.value(QStringLiteral("user")).toObject();
    badge.channelLogin =
        user.value(QStringLiteral("login")).toString().trimmed();
    badge.channelDisplayName =
        user.value(QStringLiteral("displayName")).toString().trimmed();

    return badge;
}

inline void appendBadgeIdentity(QVector<CurrentUserBadgeIdentity> &badges,
                                QSet<QString> &seenKeys,
                                const QJsonValue &value, bool selected,
                                bool displayed, bool available)
{
    if (!value.isObject())
    {
        return;
    }

    auto badge =
        badgeIdentityFromObject(value.toObject(), selected, displayed, available);
    const auto key = badgeIdentityKey(badge);
    if (key.isEmpty())
    {
        return;
    }

    if (seenKeys.contains(key))
    {
        for (auto &existing : badges)
        {
            if (badgeIdentityKey(existing) == key)
            {
                existing.selected = existing.selected || selected;
                existing.displayed = existing.displayed || displayed;
                existing.available = existing.available || available;
                if (existing.channelLogin.isEmpty() &&
                    !badge.channelLogin.isEmpty())
                {
                    existing.channelLogin = badge.channelLogin;
                    existing.channelDisplayName = badge.channelDisplayName;
                }
                break;
            }
        }
        return;
    }

    seenKeys.insert(key);
    badges.push_back(std::move(badge));
}

inline QVector<CurrentUserBadgeIdentity> parseTwitchBadgeIdentityBadges(
    const QJsonObject &data)
{
    QVector<CurrentUserBadgeIdentity> badges;
    QSet<QString> seenKeys;

    const auto user = data.value(QStringLiteral("user")).toObject();
    const auto self = user.value(QStringLiteral("self")).toObject();

    appendBadgeIdentity(badges, seenKeys,
                        self.value(QStringLiteral("selectedBadge")), true,
                        false, true);

    const auto displayBadges =
        self.value(QStringLiteral("displayBadges")).toArray();
    for (const auto &badge : displayBadges)
    {
        appendBadgeIdentity(badges, seenKeys, badge, false, true, true);
    }

    const auto availableBadges =
        self.value(QStringLiteral("availableBadges")).toArray();
    for (const auto &badge : availableBadges)
    {
        appendBadgeIdentity(badges, seenKeys, badge, false, false, true);
    }

    return badges;
}

}  // namespace detail

inline QNetworkReply *requestCurrentUserBadgeIdentity(
    QNetworkAccessManager &network, const QString &channelLogin,
    const TwitchModerationAuth::Account &account, QObject *context,
    std::function<void(QVector<CurrentUserBadgeIdentity>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto login = channelLogin.trimmed().toLower();
    if (login.isEmpty())
    {
        failureCallback(QStringLiteral("Missing Twitch channel name."));
        return nullptr;
    }

    QJsonObject variables;
    variables.insert(QStringLiteral("channelLogin"), login);

    QJsonObject payload;
    payload.insert(QStringLiteral("operationName"),
                   QStringLiteral("MergerinoChatIdentityBadges"));
    payload.insert(QStringLiteral("variables"), variables);
    payload.insert(QStringLiteral("query"),
                   detail::twitchBadgeIdentityQuery());

    QNetworkRequest request(QUrl(QStringLiteral("https://gql.twitch.tv/gql")));
    if (!detail::setTwitchBadgeIdentityRequestHeaders(request, account,
                                                      failureCallback))
    {
        return nullptr;
    }

    const auto body =
        QJsonDocument(QJsonArray{payload}).toJson(QJsonDocument::Compact);
    auto *reply = network.post(request, body);

    QObject::connect(
        reply, &QNetworkReply::finished, context,
        [reply, successCallback = std::move(successCallback),
         failureCallback = std::move(failureCallback)]() mutable {
            const auto cleanup = qScopeGuard([reply] {
                reply->deleteLater();
            });

            const auto responseBody = reply->readAll();
            if (reply->error() != QNetworkReply::NoError)
            {
                auto message = reply->errorString();
                const auto bodyText =
                    QString::fromUtf8(responseBody).trimmed();
                if (!bodyText.isEmpty())
                {
                    message += QStringLiteral(" - ") + bodyText.left(200);
                }
                failureCallback(message);
                return;
            }

            QJsonParseError parseError;
            const auto document =
                QJsonDocument::fromJson(responseBody, &parseError);
            if (parseError.error != QJsonParseError::NoError)
            {
                failureCallback(QStringLiteral("Failed to parse Twitch badges: ") +
                                parseError.errorString());
                return;
            }

            QJsonObject root;
            if (document.isArray())
            {
                const auto rootArray = document.array();
                if (!rootArray.isEmpty())
                {
                    root = rootArray.at(0).toObject();
                }
            }
            else if (document.isObject())
            {
                root = document.object();
            }

            if (root.isEmpty())
            {
                failureCallback(
                    QStringLiteral("Twitch returned an empty badge response."));
                return;
            }

            const auto gqlError =
                detail::firstTwitchBadgeIdentityGqlError(root);
            if (!gqlError.isEmpty())
            {
                failureCallback(QStringLiteral("Twitch API Error: ") +
                                gqlError);
                return;
            }

            const auto data = root.value(QStringLiteral("data")).toObject();
            const auto badges =
                detail::parseTwitchBadgeIdentityBadges(data);
            successCallback(badges);
        });

    return reply;
}

inline QNetworkReply *requestSelectCurrentUserBadgeIdentity(
    QNetworkAccessManager &network, const QString &channelID,
    const CurrentUserBadgeIdentity &badge,
    const TwitchModerationAuth::Account &account, QObject *context,
    bool channelSpecific, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (badge.setID.trimmed().isEmpty())
    {
        failureCallback(QStringLiteral("Missing Twitch badge set."));
        return nullptr;
    }

    if (channelSpecific && channelID.trimmed().isEmpty())
    {
        failureCallback(QStringLiteral("Missing Twitch channel ID."));
        return nullptr;
    }

    QJsonObject input;
    input.insert(QStringLiteral("badgeSetID"), badge.setID.trimmed());
    if (!badge.versionID.trimmed().isEmpty())
    {
        input.insert(QStringLiteral("badgeSetVersion"),
                     badge.versionID.trimmed());
    }
    if (channelSpecific)
    {
        input.insert(QStringLiteral("channelID"), channelID.trimmed());
    }

    QJsonObject variables;
    variables.insert(QStringLiteral("input"), input);

    const auto operationName =
        channelSpecific ? QStringLiteral("MergerinoSelectChannelBadge")
                        : QStringLiteral("MergerinoSelectGlobalBadge");
    const auto payloadName =
        channelSpecific ? QStringLiteral("selectChannelBadge")
                        : QStringLiteral("selectGlobalBadge");
    const auto mutation =
        channelSpecific
            ? QStringLiteral(
                  "mutation MergerinoSelectChannelBadge($input: "
                  "SelectChannelBadgeInput!) {"
                  "  selectChannelBadge(input: $input) { isSuccessful }"
                  "}")
            : QStringLiteral(
                  "mutation MergerinoSelectGlobalBadge($input: "
                  "SelectGlobalBadgeInput!) {"
                  "  selectGlobalBadge(input: $input) { isSuccessful }"
                  "}");

    QJsonObject payload;
    payload.insert(QStringLiteral("operationName"), operationName);
    payload.insert(QStringLiteral("variables"), variables);
    payload.insert(QStringLiteral("query"), mutation);

    QNetworkRequest request(QUrl(QStringLiteral("https://gql.twitch.tv/gql")));
    if (!detail::setTwitchBadgeIdentityRequestHeaders(request, account,
                                                      failureCallback))
    {
        return nullptr;
    }

    const auto body =
        QJsonDocument(QJsonArray{payload}).toJson(QJsonDocument::Compact);
    auto *reply = network.post(request, body);

    QObject::connect(
        reply, &QNetworkReply::finished, context,
        [reply, payloadName, successCallback = std::move(successCallback),
         failureCallback = std::move(failureCallback)]() mutable {
            const auto cleanup = qScopeGuard([reply] {
                reply->deleteLater();
            });

            const auto responseBody = reply->readAll();
            if (reply->error() != QNetworkReply::NoError)
            {
                auto message = reply->errorString();
                const auto bodyText =
                    QString::fromUtf8(responseBody).trimmed();
                if (!bodyText.isEmpty())
                {
                    message += QStringLiteral(" - ") + bodyText.left(200);
                }
                failureCallback(message);
                return;
            }

            QJsonParseError parseError;
            const auto document =
                QJsonDocument::fromJson(responseBody, &parseError);
            if (parseError.error != QJsonParseError::NoError)
            {
                failureCallback(QStringLiteral("Failed to parse Twitch badge "
                                               "selection: ") +
                                parseError.errorString());
                return;
            }

            QJsonObject root;
            if (document.isArray())
            {
                const auto rootArray = document.array();
                if (!rootArray.isEmpty())
                {
                    root = rootArray.at(0).toObject();
                }
            }
            else if (document.isObject())
            {
                root = document.object();
            }

            if (root.isEmpty())
            {
                failureCallback(QStringLiteral(
                    "Twitch returned an empty badge selection response."));
                return;
            }

            const auto gqlError =
                detail::firstTwitchBadgeIdentityGqlError(root);
            if (!gqlError.isEmpty())
            {
                failureCallback(QStringLiteral("Twitch API Error: ") +
                                gqlError);
                return;
            }

            const auto payload =
                root.value(QStringLiteral("data")).toObject().value(payloadName)
                    .toObject();
            if (!payload.value(QStringLiteral("isSuccessful")).toBool(false))
            {
                failureCallback(QStringLiteral(
                    "Twitch did not accept the badge selection."));
                return;
            }

            successCallback();
        });

    return reply;
}

}  // namespace chatterino::twitch
