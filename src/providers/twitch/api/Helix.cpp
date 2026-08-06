// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/api/Helix.hpp"

#include "Application.hpp"
#include "common/Args.hpp"
#include "common/Literals.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "providers/twitch/api/TwitchWebApi.hpp"
#include "providers/twitch/api/TwitchModerationAuth.hpp"
#include "singletons/Paths.hpp"
#include "util/Clipboard.hpp"
#include "util/CancellationToken.hpp"
#include "util/QMagicEnum.hpp"

#ifdef USEWINSDK
#    include <Windows.h>
#endif

#include <magic_enum/magic_enum.hpp>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QStringBuilder>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace {

using namespace chatterino;

constexpr auto NUM_MODERATORS_TO_FETCH_PER_REQUEST = 100;

constexpr auto NUM_CHATTERS_TO_FETCH = 1000;

std::unordered_set<QString> parseChatterGroup(const QJsonObject &chatters,
                                              const QString &key)
{
    std::unordered_set<QString> users;

    const auto values = chatters.value(key).toArray();
    for (const auto &value : values)
    {
        QString user;
        if (value.isObject())
        {
            user = value.toObject().value(QStringLiteral("login")).toString();
        }
        else
        {
            user = value.toString();
        }

        user = user.trimmed().toLower();
        if (!user.isEmpty())
        {
            users.insert(user);
        }
    }

    return users;
}

}  // namespace

namespace chatterino {

using namespace literals;

static IHelix *instance = nullptr;

HelixChatters::HelixChatters(const QJsonObject &jsonObject)
    : total(jsonObject.value("total").toInt())
    , cursor(
          jsonObject.value("pagination").toObject().value("cursor").toString())
{
    const auto &data = jsonObject.value("data").toArray();
    for (const auto &chatter : data)
    {
        auto userLogin = chatter.toObject().value("user_login").toString();
        this->chatters.insert(userLogin);
    }
}

HelixChatterGroups::HelixChatterGroups(const QJsonObject &jsonObject)
    : broadcaster(parseChatterGroup(jsonObject.value("chatters").toObject(),
                                    QStringLiteral("broadcaster")))
    , vips(parseChatterGroup(jsonObject.value("chatters").toObject(),
                             QStringLiteral("vips")))
    , moderators(parseChatterGroup(jsonObject.value("chatters").toObject(),
                                   QStringLiteral("moderators")))
    , staff(parseChatterGroup(jsonObject.value("chatters").toObject(),
                              QStringLiteral("staff")))
    , admins(parseChatterGroup(jsonObject.value("chatters").toObject(),
                               QStringLiteral("admins")))
    , globalMods(parseChatterGroup(jsonObject.value("chatters").toObject(),
                                   QStringLiteral("global_mods")))
    , viewers(parseChatterGroup(jsonObject.value("chatters").toObject(),
                                QStringLiteral("viewers")))
    , total(jsonObject.value("chatter_count").toInt())
{
    const auto chatters = jsonObject.value("chatters").toObject();
    this->broadcaster.merge(parseChatterGroup(chatters,
                                             QStringLiteral("broadcasters")));
    if (this->total == 0)
    {
        this->total = chatters.value(QStringLiteral("count")).toInt();
    }
}

void Helix::fetchUsers(QStringList userIds, QStringList userLogins,
                       ResultCallback<std::vector<HelixUser>> successCallback,
                       HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;

    for (const auto &id : userIds)
    {
        urlQuery.addQueryItem("id", id);
    }

    for (const auto &login : userLogins)
    {
        urlQuery.addQueryItem("login", login);
    }

    // TODO: set on success and on error
    this->makeGet("users", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixUser> users;

            for (const auto &jsonUser : data.toArray())
            {
                users.emplace_back(jsonUser.toObject());
            }

            successCallback(users);
        })
        .onError([failureCallback](auto /*result*/) {
            // TODO: make better xd
            failureCallback();
        })
        .execute();
}

void Helix::getUserByName(QString userName,
                          ResultCallback<HelixUser> successCallback,
                          HelixFailureCallback failureCallback)
{
    QStringList userIds;
    QStringList userLogins{std::move(userName)};

    this->fetchUsers(
        userIds, userLogins,
        [successCallback,
         failureCallback](const std::vector<HelixUser> &users) {
            if (users.empty())
            {
                failureCallback();
                return;
            }
            successCallback(users[0]);
        },
        failureCallback);
}

void Helix::getUserById(QString userId,
                        ResultCallback<HelixUser> successCallback,
                        HelixFailureCallback failureCallback)
{
    QStringList userIds{std::move(userId)};
    QStringList userLogins;

    this->fetchUsers(
        userIds, userLogins,
        [successCallback, failureCallback](const auto &users) {
            if (users.empty())
            {
                failureCallback();
                return;
            }
            successCallback(users[0]);
        },
        failureCallback);
}

void Helix::getChannelFollowers(
    QString broadcasterID,
    ResultCallback<HelixGetChannelFollowersResponse> successCallback,
    std::function<void(QString)> failureCallback)
{
    assert(!broadcasterID.isEmpty());

    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", broadcasterID);

    // TODO: set on success and on error
    this->makeGet("channels/followers", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            if (root.empty())
            {
                failureCallback("Bad JSON response");
                return;
            }
            successCallback(HelixGetChannelFollowersResponse(root));
        })
        .onError([failureCallback](auto result) {
            auto root = result.parseJson();
            if (root.empty())
            {
                failureCallback("Unknown error");
                return;
            }

            // Forward "message" from Twitch
            HelixError error(root);
            failureCallback(error.message);
        })
        .execute();
}

void Helix::fetchStreams(
    QStringList userIds, QStringList userLogins,
    ResultCallback<std::vector<HelixStream>> successCallback,
    HelixFailureCallback failureCallback, std::function<void()> finallyCallback)
{
    QUrlQuery urlQuery;

    for (const auto &id : userIds)
    {
        urlQuery.addQueryItem("user_id", id);
    }

    for (const auto &login : userLogins)
    {
        urlQuery.addQueryItem("user_login", login);
    }

    // TODO: set on success and on error
    this->makeGet("streams", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixStream> streams;

            for (const auto &jsonStream : data.toArray())
            {
                streams.emplace_back(jsonStream.toObject());
            }

            successCallback(streams);
        })
        .onError([failureCallback](auto /*result*/) {
            // TODO: make better xd
            failureCallback();
        })
        .finally(finallyCallback)
        .execute();
}

void Helix::getStreamById(QString userId,
                          ResultCallback<bool, HelixStream> successCallback,
                          HelixFailureCallback failureCallback,
                          std::function<void()> finallyCallback)
{
    QStringList userIds{std::move(userId)};
    QStringList userLogins;

    this->fetchStreams(
        userIds, userLogins,
        [successCallback](const auto &streams) {
            if (streams.empty())
            {
                successCallback(false, HelixStream());
                return;
            }
            successCallback(true, streams[0]);
        },
        failureCallback, finallyCallback);
}

void Helix::getStreamByName(QString userName,
                            ResultCallback<bool, HelixStream> successCallback,
                            HelixFailureCallback failureCallback,
                            std::function<void()> finallyCallback)
{
    QStringList userIds;
    QStringList userLogins{std::move(userName)};

    this->fetchStreams(
        userIds, userLogins,
        [successCallback, failureCallback](const auto &streams) {
            if (streams.empty())
            {
                successCallback(false, HelixStream());
                return;
            }
            successCallback(true, streams[0]);
        },
        failureCallback, finallyCallback);
}

///

void Helix::fetchGames(QStringList gameIds, QStringList gameNames,
                       ResultCallback<std::vector<HelixGame>> successCallback,
                       HelixFailureCallback failureCallback)
{
    assert((gameIds.length() + gameNames.length()) > 0);

    QUrlQuery urlQuery;

    for (const auto &id : gameIds)
    {
        urlQuery.addQueryItem("id", id);
    }

    for (const auto &login : gameNames)
    {
        urlQuery.addQueryItem("name", login);
    }

    // TODO: set on success and on error
    this->makeGet("games", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixGame> games;

            for (const auto &jsonStream : data.toArray())
            {
                games.emplace_back(jsonStream.toObject());
            }

            successCallback(games);
        })
        .onError([failureCallback](auto /*result*/) {
            // TODO: make better xd
            failureCallback();
        })
        .execute();
}

void Helix::searchGames(QString gameName,
                        ResultCallback<std::vector<HelixGame>> successCallback,
                        HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("query", gameName);

    this->makeGet("search/categories", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixGame> games;

            for (const auto &jsonStream : data.toArray())
            {
                games.emplace_back(jsonStream.toObject());
            }

            successCallback(games);
        })
        .onError([failureCallback](auto /*result*/) {
            // TODO: make better xd
            failureCallback();
        })
        .execute();
}

void Helix::getGameById(QString gameId,
                        ResultCallback<HelixGame> successCallback,
                        HelixFailureCallback failureCallback)
{
    QStringList gameIds{std::move(gameId)};
    QStringList gameNames;

    this->fetchGames(
        gameIds, gameNames,
        [successCallback, failureCallback](const auto &games) {
            if (games.empty())
            {
                failureCallback();
                return;
            }
            successCallback(games[0]);
        },
        failureCallback);
}

void Helix::createClip(
    QString channelId, QString title, std::optional<int> duration,
    ResultCallback<HelixClip> successCallback,
    std::function<void(HelixClipError, QString)> failureCallback,
    std::function<void()> finallyCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", channelId);

    if (!title.isEmpty())
    {
        urlQuery.addQueryItem("title", title);
    }

    if (duration.has_value())
    {
        urlQuery.addQueryItem("duration", QString::number(*duration));
    }

    this->makePost("clips", urlQuery)
        .header("Content-Type", "application/json")
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback(HelixClipError::Unknown, "No clip was created");
                return;
            }

            HelixClip clip(data.toArray()[0].toObject());

            successCallback(clip);
        })
        .onError([failureCallback](auto result) {
            auto obj = result.parseJson();
            auto message = obj.value("message").toString();
            switch (result.status().value_or(0))
            {
                case 503: {
                    // We should not necessarily handle this, so the error messaging will use `message` if it exists
                    failureCallback(HelixClipError::ClipsUnavailable, message);
                }
                break;

                case 401: {
                    // User does not have the required scope to be able to create clips, user must reauthenticate
                    failureCallback(HelixClipError::UserNotAuthenticated,
                                    message);
                }
                break;

                case 403: {
                    if (message.contains("restricted for this channel"))
                    {
                        failureCallback(HelixClipError::ClipsDisabled, message);
                    }
                    else if (message.contains("User does not have permissions"))
                    {
                        failureCallback(HelixClipError::ClipsRestricted,
                                        message);
                    }
                    else if (message.contains("restricted for this category"))
                    {
                        failureCallback(HelixClipError::ClipsRestrictedCategory,
                                        message);
                    }
                    else
                    {
                        qCDebug(chatterinoTwitch)
                            << "Failed to create a clip: "
                            << result.formatError() << result.getData();
                        failureCallback(HelixClipError::Unknown, message);
                    }
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Failed to create a clip: " << result.formatError()
                        << result.getData();
                    failureCallback(HelixClipError::Unknown, message);
                }
                break;
            }
        })
        .finally(std::move(finallyCallback))
        .execute();
}

void Helix::fetchChannels(
    QStringList userIDs,
    ResultCallback<std::vector<HelixChannel>> successCallback,
    HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;

    for (const auto &userID : userIDs)
    {
        urlQuery.addQueryItem("broadcaster_id", userID);
    }

    this->makeGet("channels", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixChannel> channels;

            for (const auto &unparsedChannel : data.toArray())
            {
                channels.emplace_back(unparsedChannel.toObject());
            }

            successCallback(channels);
        })
        .onError([failureCallback](auto /*result*/) {
            failureCallback();
        })
        .execute();
}

void Helix::getChannel(QString broadcasterId,
                       ResultCallback<HelixChannel> successCallback,
                       HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", broadcasterId);

    this->makeGet("channels", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            HelixChannel channel(data.toArray()[0].toObject());

            successCallback(channel);
        })
        .onError([failureCallback](auto /*result*/) {
            failureCallback();
        })
        .execute();
}

void Helix::createStreamMarker(
    QString broadcasterId, QString description,
    ResultCallback<HelixStreamMarker> successCallback,
    std::function<void(HelixStreamMarkerError)> failureCallback)
{
    QJsonObject payload;

    if (!description.isEmpty())
    {
        payload.insert("description", QJsonValue(description));
    }
    payload.insert("user_id", QJsonValue(broadcasterId));

    this->makePost("streams/markers", QUrlQuery())
        .json(payload)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback(HelixStreamMarkerError::Unknown);
                return;
            }

            HelixStreamMarker streamMarker(data.toArray()[0].toObject());

            successCallback(streamMarker);
        })
        .onError([failureCallback](NetworkResult result) {
            switch (result.status().value_or(0))
            {
                case 403: {
                    // User isn't a Channel Editor, so he can't create markers
                    failureCallback(HelixStreamMarkerError::UserNotAuthorized);
                }
                break;

                case 401: {
                    // User does not have the required scope to be able to create stream markers, user must reauthenticate
                    failureCallback(
                        HelixStreamMarkerError::UserNotAuthenticated);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Failed to create a stream marker: "
                        << result.formatError() << result.getData();
                    failureCallback(HelixStreamMarkerError::Unknown);
                }
                break;
            }
        })
        .execute();
};

void Helix::loadBlocks(QString userId,
                       ResultCallback<std::vector<HelixBlock>> pageCallback,
                       FailureCallback<QString> failureCallback,
                       CancellationToken &&token)
{
    constexpr const size_t blockLimit = 1000;

    // TODO(Qt 5.13): use initializer list
    QUrlQuery query;
    query.addQueryItem(u"broadcaster_id"_s, userId);
    query.addQueryItem(u"first"_s, u"100"_s);

    size_t receivedItems = 0;
    this->paginate(
        u"users/blocks"_s, query,
        [pageCallback, receivedItems](const QJsonObject &json,
                                      const auto & /*state*/) mutable {
            const auto data = json["data"_L1].toArray();

            if (data.isEmpty())
            {
                return false;
            }

            std::vector<HelixBlock> ignores;
            ignores.reserve(data.count());

            for (const auto &ignore : data)
            {
                ignores.emplace_back(ignore.toObject());
            }

            pageCallback(ignores);

            receivedItems += data.count();

            if (receivedItems >= blockLimit)
            {
                qCInfo(chatterinoTwitch) << "Reached the limit of" << blockLimit
                                         << "Twitch blocks fetched";
                return false;
            }

            return true;
        },
        [failureCallback](const NetworkResult &result) {
            failureCallback(result.formatError());
        },
        std::move(token));
}

void Helix::blockUser(QString targetUserId, const QObject *caller,
                      std::function<void()> successCallback,
                      HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("target_user_id", targetUserId);

    this->makePut("users/blocks", urlQuery)
        .caller(caller)
        .onSuccess([successCallback](auto /*result*/) {
            successCallback();
        })
        .onError([failureCallback](auto /*result*/) {
            // TODO: make better xd
            failureCallback();
        })
        .execute();
}

void Helix::unblockUser(QString targetUserId, const QObject *caller,
                        std::function<void()> successCallback,
                        HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("target_user_id", targetUserId);

    this->makeDelete("users/blocks", urlQuery)
        .caller(caller)
        .onSuccess([successCallback](auto /*result*/) {
            successCallback();
        })
        .onError([failureCallback](auto /*result*/) {
            // TODO: make better xd
            failureCallback();
        })
        .execute();
}

void Helix::updateChannel(
    QString broadcasterId, QString gameId, QString language, QString title,
    std::function<void(NetworkResult)> successCallback,
    FailureCallback<HelixUpdateChannelError, QString> failureCallback)
{
    using Error = HelixUpdateChannelError;

    QUrlQuery urlQuery;
    auto obj = QJsonObject();
    if (!gameId.isEmpty())
    {
        obj.insert("game_id", gameId);
    }
    if (!language.isEmpty())
    {
        obj.insert("broadcaster_language", language);
    }
    if (!title.isEmpty())
    {
        obj.insert("title", title);
    }

    if (title.isEmpty() && gameId.isEmpty() && language.isEmpty())
    {
        qCDebug(chatterinoCommon) << "Tried to update channel with no changes!";
        return;
    }

    urlQuery.addQueryItem("broadcaster_id", broadcasterId);
    this->makePatch("channels", urlQuery)
        .json(obj)
        .onSuccess([successCallback, failureCallback](auto result) {
            successCallback(result);
        })
        .onError([failureCallback](NetworkResult result) {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare(
                                 "The ID in broadcaster_id must match the user "
                                 "ID found in the request's OAuth token.",
                                 Qt::CaseInsensitive) == 0)
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 400:
                case 403: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                case 500: {
                    failureCallback(Error::Unknown, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Helix update channel, unhandled error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::manageAutoModMessages(
    QString userID, QString msgID, QString action,
    std::function<void()> successCallback,
    std::function<void(HelixAutoModMessageError)> failureCallback)
{
    QJsonObject payload;

    payload.insert("user_id", userID);
    payload.insert("msg_id", msgID);
    payload.insert("action", action);

    this->makePost("moderation/automod/message", QUrlQuery())
        .json(payload)
        .onSuccess([successCallback, failureCallback](auto result) {
            successCallback();
        })
        .onError([failureCallback, msgID, action](NetworkResult result) {
            switch (result.status().value_or(0))
            {
                case 400: {
                    // Message was already processed
                    failureCallback(
                        HelixAutoModMessageError::MessageAlreadyProcessed);
                }
                break;

                case 401: {
                    // User is missing the required scope
                    failureCallback(
                        HelixAutoModMessageError::UserNotAuthenticated);
                }
                break;

                case 403: {
                    // Requesting user is not authorized to manage messages
                    failureCallback(
                        HelixAutoModMessageError::UserNotAuthorized);
                }
                break;

                case 404: {
                    // Message not found or invalid msgID
                    failureCallback(HelixAutoModMessageError::MessageNotFound);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Failed to manage automod message: " << action
                        << msgID << result.formatError() << result.getData();
                    failureCallback(HelixAutoModMessageError::Unknown);
                }
                break;
            }
        })
        .execute();
}

void Helix::getCheermotes(
    QString broadcasterId,
    ResultCallback<std::vector<HelixCheermoteSet>> successCallback,
    HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterId);

    this->makeGet("bits/cheermotes", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixCheermoteSet> cheermoteSets;

            for (const auto &jsonStream : data.toArray())
            {
                cheermoteSets.emplace_back(jsonStream.toObject());
            }

            successCallback(cheermoteSets);
        })
        .onError([broadcasterId, failureCallback](NetworkResult result) {
            qCDebug(chatterinoTwitch)
                << "Failed to get cheermotes(broadcaster_id=" << broadcasterId
                << "): " << result.formatError() << result.getData();
            failureCallback();
        })
        .execute();
}

void Helix::getEmoteSetData(QString emoteSetId,
                            ResultCallback<HelixEmoteSetData> successCallback,
                            HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;

    urlQuery.addQueryItem("emote_set_id", emoteSetId);

    this->makeGet("chat/emotes/set", urlQuery)
        .onSuccess([successCallback, failureCallback, emoteSetId](auto result) {
            QJsonObject root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray() || data.toArray().isEmpty())
            {
                failureCallback();
                return;
            }

            HelixEmoteSetData emoteSetData(data.toArray()[0].toObject());

            successCallback(emoteSetData);
        })
        .onError([failureCallback](NetworkResult result) {
            // TODO: make better xd
            failureCallback();
        })
        .execute();
}

void Helix::getChannelEmotes(
    QString broadcasterId,
    ResultCallback<std::vector<HelixChannelEmote>> successCallback,
    HelixFailureCallback failureCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", broadcasterId);

    this->makeGet("chat/emotes", urlQuery)
        .onSuccess([successCallback, failureCallback](NetworkResult result) {
            QJsonObject root = result.parseJson();
            auto data = root.value("data");

            if (!data.isArray())
            {
                failureCallback();
                return;
            }

            std::vector<HelixChannelEmote> channelEmotes;

            for (const auto &jsonStream : data.toArray())
            {
                channelEmotes.emplace_back(jsonStream.toObject());
            }

            successCallback(channelEmotes);
        })
        .onError([failureCallback](auto result) {
            // TODO: make better xd
            failureCallback();
        })
        .execute();
}

void Helix::updateUserChatColor(
    QString userID, QString color, ResultCallback<> successCallback,
    FailureCallback<HelixUpdateUserChatColorError, QString> failureCallback)
{
    using Error = HelixUpdateUserChatColorError;

    QJsonObject payload;

    payload.insert("user_id", QJsonValue(userID));
    payload.insert("color", QJsonValue(color));

    this->makePut("chat/color", QUrlQuery())
        .json(payload)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto obj = result.parseJson();
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for updating chat color was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("invalid color",
                                           Qt::CaseInsensitive))
                    {
                        // Handle this error specifically since it allows us to list out the available colors
                        failureCallback(Error::InvalidColor, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        // Handle this error specifically because its API error is especially unfriendly
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error changing user color:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
};

void Helix::deleteChatMessages(
    QString broadcasterID, QString moderatorID, QString messageID,
    ResultCallback<> successCallback,
    FailureCallback<HelixDeleteChatMessagesError, QString> failureCallback)
{
    using Error = HelixDeleteChatMessagesError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    if (!messageID.isEmpty())
    {
        // If message ID is empty, it's equivalent to /clear
        urlQuery.addQueryItem("message_id", messageID);
    }

    this->makeDelete("moderation/chat", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for deleting chat messages was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 404: {
                    // A 404 on this endpoint means message id is invalid or unable to be deleted.
                    // See: https://dev.twitch.tv/docs/api/reference#delete-chat-messages
                    failureCallback(Error::MessageUnavailable, message);
                }
                break;

                case 400: {
                    // These errors are generally well formatted, so we just forward them.
                    // This is currently undocumented behaviour, see: https://github.com/twitchdev/issues/issues/660
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 403: {
                    // 403 endpoint means the user does not have permission to perform this action in that channel
                    // Most likely to missing moderator permissions
                    // Missing documentation issue: https://github.com/twitchdev/issues/issues/659
                    // `message` value is well-formed so no need for a specific error type
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        // Handle this error specifically because its API error is especially unfriendly
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error deleting chat messages:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::getPinnedChatMessage(
    QString broadcasterID, QString moderatorID,
    ResultCallback<std::optional<HelixPinnedChatMessage>> successCallback,
    FailureCallback<QString> failureCallback)
{
    QUrlQuery query;
    query.addQueryItem("broadcaster_id", broadcasterID);
    query.addQueryItem("moderator_id", moderatorID);

    this->makeGet("chat/pins", query)
        .onSuccess([successCallback, failureCallback](const auto &result) {
            if (result.status() != 200)
            {
                failureCallback(result.formatError());
                return;
            }

            const auto data = result.parseJson().value("data").toArray();
            if (data.isEmpty())
            {
                successCallback(std::nullopt);
                return;
            }
            successCallback(HelixPinnedChatMessage(data.first().toObject()));
        })
        .onError([failureCallback](const auto &result) {
            auto message = result.parseJson().value("message").toString();
            failureCallback(message.isEmpty() ? result.formatError() : message);
        })
        .execute();
}

void Helix::pinChatMessage(
    QString broadcasterID, QString moderatorID, QString messageID,
    std::optional<int> durationSeconds, ResultCallback<> successCallback,
    FailureCallback<QString> failureCallback)
{
    QUrlQuery query;
    query.addQueryItem("broadcaster_id", broadcasterID);
    query.addQueryItem("moderator_id", moderatorID);
    query.addQueryItem("message_id", messageID);
    if (durationSeconds)
    {
        query.addQueryItem("duration_seconds",
                           QString::number(*durationSeconds));
    }

    this->makePut("chat/pins", query)
        .onSuccess([successCallback, failureCallback](const auto &result) {
            if (result.status() != 204)
            {
                failureCallback(result.formatError());
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const auto &result) {
            auto message = result.parseJson().value("message").toString();
            failureCallback(message.isEmpty() ? result.formatError() : message);
        })
        .execute();
}

void Helix::updatePinnedChatMessage(
    QString broadcasterID, QString moderatorID, QString messageID,
    std::optional<int> durationSeconds, ResultCallback<> successCallback,
    FailureCallback<QString> failureCallback)
{
    QUrlQuery query;
    query.addQueryItem("broadcaster_id", broadcasterID);
    query.addQueryItem("moderator_id", moderatorID);
    query.addQueryItem("message_id", messageID);
    if (durationSeconds)
    {
        query.addQueryItem("duration_seconds",
                           QString::number(*durationSeconds));
    }

    this->makePatch("chat/pins", query)
        .onSuccess([successCallback, failureCallback](const auto &result) {
            if (result.status() != 204)
            {
                failureCallback(result.formatError());
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const auto &result) {
            auto message = result.parseJson().value("message").toString();
            failureCallback(message.isEmpty() ? result.formatError() : message);
        })
        .execute();
}

void Helix::unpinChatMessage(
    QString broadcasterID, QString moderatorID, QString messageID,
    ResultCallback<> successCallback, FailureCallback<QString> failureCallback)
{
    QUrlQuery query;
    query.addQueryItem("broadcaster_id", broadcasterID);
    query.addQueryItem("moderator_id", moderatorID);
    query.addQueryItem("message_id", messageID);

    this->makeDelete("chat/pins", query)
        .onSuccess([successCallback, failureCallback](const auto &result) {
            if (result.status() != 204)
            {
                failureCallback(result.formatError());
                return;
            }
            successCallback();
        })
        .onError([failureCallback](const auto &result) {
            auto message = result.parseJson().value("message").toString();
            failureCallback(message.isEmpty() ? result.formatError() : message);
        })
        .execute();
}

void Helix::addChannelModerator(
    QString broadcasterID, QString userID, ResultCallback<> successCallback,
    FailureCallback<HelixAddChannelModeratorError, QString> failureCallback)
{
    using Error = HelixAddChannelModeratorError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("user_id", userID);

    this->makePost("moderation/moderators", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for adding a moderator was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        // Handle this error specifically because its API error is especially unfriendly
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare("incorrect user authorization",
                                             Qt::CaseInsensitive) == 0)
                    {
                        // This error is pretty ugly, but essentially means they're not authorized to mod people in this channel
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 400: {
                    if (message.compare("user is already a mod",
                                        Qt::CaseInsensitive) == 0)
                    {
                        // This error is particularly ugly, handle it separately
                        failureCallback(Error::TargetAlreadyModded, message);
                    }
                    else
                    {
                        // The Twitch API error sufficiently tells the user what went wrong
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 422: {
                    // Target is already a VIP
                    failureCallback(Error::TargetIsVIP, message);
                }
                break;

                case 429: {
                    // Endpoint has a strict ratelimit
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error adding channel moderator:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::removeChannelModerator(
    QString broadcasterID, QString userID, ResultCallback<> successCallback,
    FailureCallback<HelixRemoveChannelModeratorError, QString> failureCallback)
{
    using Error = HelixRemoveChannelModeratorError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("user_id", userID);

    this->makeDelete("moderation/moderators", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for unmodding user was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.compare("user is not a mod",
                                        Qt::CaseInsensitive) == 0)
                    {
                        // This error message is particularly ugly, so we handle it differently
                        failureCallback(Error::TargetNotModded, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        // Handle this error specifically because its API error is especially unfriendly
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare("incorrect user authorization",
                                             Qt::CaseInsensitive) == 0)
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error unmodding user:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::sendChatAnnouncement(
    QString broadcasterID, QString moderatorID, QString message,
    HelixAnnouncementColor color, ResultCallback<> successCallback,
    FailureCallback<HelixSendChatAnnouncementError, QString> failureCallback)
{
    using Error = HelixSendChatAnnouncementError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    QJsonObject body;
    body.insert("message", message);
    body.insert("color", qmagicenum::enumNameString(color).toLower());

    this->makePost("chat/announcements", urlQuery)
        .json(body)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for sending an announcement was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    // These errors are generally well formatted, so we just forward them.
                    // This is currently undocumented behaviour, see: https://github.com/twitchdev/issues/issues/660
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 403: {
                    // 403 endpoint means the user does not have permission to perform this action in that channel
                    // `message` value is well-formed so no need for a specific error type
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        // Handle this error specifically because its API error is especially unfriendly
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error sending an announcement:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::addChannelVIP(
    QString broadcasterID, QString userID, ResultCallback<> successCallback,
    FailureCallback<HelixAddChannelVIPError, QString> failureCallback)
{
    using Error = HelixAddChannelVIPError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("user_id", userID);

    this->makePost("channels/vips", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for adding channel VIP was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400:
                case 409:
                case 422:
                case 425: {
                    // Most of the errors returned by this endpoint are pretty good. We can rely on Twitch's API messages
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        // Handle this error specifically because its API error is especially unfriendly
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare("incorrect user authorization",
                                             Qt::CaseInsensitive) == 0 ||
                             message.startsWith("the id in broadcaster_id must "
                                                "match the user id",
                                                Qt::CaseInsensitive))
                    {
                        // This error is particularly ugly, but is the equivalent to a user not having permissions
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error adding channel VIP:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::removeChannelVIP(
    QString broadcasterID, QString userID, ResultCallback<> successCallback,
    FailureCallback<HelixRemoveChannelVIPError, QString> failureCallback)
{
    using Error = HelixRemoveChannelVIPError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("user_id", userID);

    this->makeDelete("channels/vips", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for removing channel VIP was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400:
                case 409:
                case 422: {
                    // Most of the errors returned by this endpoint are pretty good. We can rely on Twitch's API messages
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        // Handle this error specifically because its API error is especially unfriendly
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare("incorrect user authorization",
                                             Qt::CaseInsensitive) == 0 ||
                             message.startsWith("the id in broadcaster_id must "
                                                "match the user id",
                                                Qt::CaseInsensitive))
                    {
                        // This error is particularly ugly, but is the equivalent to a user not having permissions
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error removing channel VIP:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::unbanUser(
    QString broadcasterID, QString moderatorID, QString userID,
    ResultCallback<> successCallback,
    FailureCallback<HelixUnbanUserError, QString> failureCallback)
{
    using Error = HelixUnbanUserError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);
    urlQuery.addQueryItem("user_id", userID);

    this->makeDelete("moderation/bans", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for unbanning user was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("The user in the user_id query "
                                           "parameter is not banned",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::TargetNotBanned, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 409: {
                    failureCallback(Error::ConflictingOperation, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        // Handle this error specifically because its API error is especially unfriendly
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare("incorrect user authorization",
                                             Qt::CaseInsensitive) == 0 ||
                             message.startsWith("the id in broadcaster_id must "
                                                "match the user id",
                                                Qt::CaseInsensitive))
                    {
                        // This error is particularly ugly, but is the equivalent to a user not having permissions
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error unbanning user:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::startRaid(
    QString fromBroadcasterID, QString toBroadcasterID,
    ResultCallback<> successCallback,
    FailureCallback<HelixStartRaidError, QString> failureCallback)
{
    using Error = HelixStartRaidError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("from_broadcaster_id", fromBroadcasterID);
    urlQuery.addQueryItem("to_broadcaster_id", toBroadcasterID);

    this->makePost("raids", urlQuery)
        .onSuccess([successCallback, failureCallback](auto /*result*/) {
            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.compare("The IDs in from_broadcaster_id and "
                                        "to_broadcaster_id cannot be the same.",
                                        Qt::CaseInsensitive) == 0)
                    {
                        failureCallback(Error::CantRaidYourself, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare(
                                 "The ID in broadcaster_id must match the user "
                                 "ID "
                                 "found in the request's OAuth token.",
                                 Qt::CaseInsensitive) == 0)
                    {
                        // Must be the broadcaster.
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 409: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error while starting a raid:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::cancelRaid(
    QString broadcasterID, ResultCallback<> successCallback,
    FailureCallback<HelixCancelRaidError, QString> failureCallback)
{
    using Error = HelixCancelRaidError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);

    this->makeDelete("raids", urlQuery)
        .onSuccess([successCallback, failureCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for canceling the raid was"
                    << result.formatError()
                    << "but we only expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare(
                                 "The ID in broadcaster_id must match the user "
                                 "ID "
                                 "found in the request's OAuth token.",
                                 Qt::CaseInsensitive) == 0)
                    {
                        // Must be the broadcaster.
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 404: {
                    failureCallback(Error::NoRaidPending, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error while canceling the raid:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}  // cancelRaid

void Helix::updateEmoteMode(
    QString broadcasterID, QString moderatorID, bool emoteMode,
    ResultCallback<HelixChatSettings> successCallback,
    FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
{
    QJsonObject json;
    json["emote_mode"] = emoteMode;
    this->updateChatSettings(broadcasterID, moderatorID, json, successCallback,
                             failureCallback);
}

void Helix::updateFollowerMode(
    QString broadcasterID, QString moderatorID,
    std::optional<int> followerModeDuration,
    ResultCallback<HelixChatSettings> successCallback,
    FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
{
    QJsonObject json;
    json["follower_mode"] = followerModeDuration.has_value();
    if (followerModeDuration)
    {
        json["follower_mode_duration"] = *followerModeDuration;
    }

    this->updateChatSettings(broadcasterID, moderatorID, json, successCallback,
                             failureCallback);
}

void Helix::updateNonModeratorChatDelay(
    QString broadcasterID, QString moderatorID,
    std::optional<int> nonModeratorChatDelayDuration,
    ResultCallback<HelixChatSettings> successCallback,
    FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
{
    QJsonObject json;
    json["non_moderator_chat_delay"] =
        nonModeratorChatDelayDuration.has_value();
    if (nonModeratorChatDelayDuration)
    {
        json["non_moderator_chat_delay_duration"] =
            *nonModeratorChatDelayDuration;
    }

    this->updateChatSettings(broadcasterID, moderatorID, json, successCallback,
                             failureCallback);
}

void Helix::updateSlowMode(
    QString broadcasterID, QString moderatorID,
    std::optional<int> slowModeWaitTime,
    ResultCallback<HelixChatSettings> successCallback,
    FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
{
    QJsonObject json;
    json["slow_mode"] = slowModeWaitTime.has_value();
    if (slowModeWaitTime)
    {
        json["slow_mode_wait_time"] = *slowModeWaitTime;
    }

    this->updateChatSettings(broadcasterID, moderatorID, json, successCallback,
                             failureCallback);
}

void Helix::updateSubscriberMode(
    QString broadcasterID, QString moderatorID, bool subscriberMode,
    ResultCallback<HelixChatSettings> successCallback,
    FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
{
    QJsonObject json;
    json["subscriber_mode"] = subscriberMode;
    this->updateChatSettings(broadcasterID, moderatorID, json, successCallback,
                             failureCallback);
}

void Helix::updateUniqueChatMode(
    QString broadcasterID, QString moderatorID, bool uniqueChatMode,
    ResultCallback<HelixChatSettings> successCallback,
    FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
{
    QJsonObject json;
    json["unique_chat_mode"] = uniqueChatMode;
    this->updateChatSettings(broadcasterID, moderatorID, json, successCallback,
                             failureCallback);
}

void Helix::updateChatSettings(
    QString broadcasterID, QString moderatorID, QJsonObject payload,
    ResultCallback<HelixChatSettings> successCallback,
    FailureCallback<HelixUpdateChatSettingsError, QString> failureCallback)
{
    using Error = HelixUpdateChatSettingsError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    this->makePatch("chat/settings", urlQuery)
        .json(payload)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for updating chat settings was"
                    << result.formatError() << "but we expected it to be 200";
            }
            auto response = result.parseJson();
            successCallback(HelixChatSettings(
                response.value("data").toArray().first().toObject()));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.contains("must be in the range"))
                    {
                        failureCallback(Error::OutOfRange, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;
                case 409:
                case 422:
                case 425: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        // Handle this error specifically because its API error is especially unfriendly
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error updating chat settings:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::onFetchChattersSuccess(
    std::shared_ptr<HelixChatters> finalChatters, QString broadcasterID,
    QString moderatorID, size_t maxChattersToFetch,
    ResultCallback<HelixChatters> successCallback,
    FailureCallback<HelixGetChattersError, QString> failureCallback,
    HelixChatters chatters)
{
    qCDebug(chatterinoTwitch)
        << "Fetched" << chatters.chatters.size() << "chatters";

    finalChatters->chatters.merge(chatters.chatters);
    finalChatters->total = chatters.total;

    if (chatters.cursor.isEmpty() ||
        finalChatters->chatters.size() >= maxChattersToFetch)
    {
        // Done paginating
        successCallback(*finalChatters);
        return;
    }

    this->fetchChatters(
        broadcasterID, moderatorID, NUM_CHATTERS_TO_FETCH, chatters.cursor,
        [=, this](auto chatters) {
            this->onFetchChattersSuccess(
                finalChatters, broadcasterID, moderatorID, maxChattersToFetch,
                successCallback, failureCallback, chatters);
        },
        failureCallback);
}

// https://dev.twitch.tv/docs/api/reference#get-chatters
void Helix::fetchChatters(
    QString broadcasterID, QString moderatorID, int first, QString after,
    ResultCallback<HelixChatters> successCallback,
    FailureCallback<HelixGetChattersError, QString> failureCallback)
{
    using Error = HelixGetChattersError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);
    urlQuery.addQueryItem("first", QString::number(first));

    if (!after.isEmpty())
    {
        urlQuery.addQueryItem("after", after);
    }

    this->makeGet("chat/chatters", urlQuery)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting chatters was "
                    << result.formatError() << "but we expected it to be 200";
            }

            auto response = result.parseJson();
            successCallback(HelixChatters(response));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.contains("OAuth token"))
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error data:" << result.formatError()
                        << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::onFetchModeratorsSuccess(
    std::shared_ptr<std::vector<HelixModerator>> finalModerators,
    QString broadcasterID, size_t maxModeratorsToFetch,
    ResultCallback<std::vector<HelixModerator>> successCallback,
    FailureCallback<HelixGetModeratorsError, QString> failureCallback,
    HelixModerators moderators)
{
    qCDebug(chatterinoTwitch)
        << "Fetched " << moderators.moderators.size() << " moderators";

    std::for_each(moderators.moderators.begin(), moderators.moderators.end(),
                  [finalModerators](auto mod) {
                      finalModerators->push_back(mod);
                  });

    if (moderators.cursor.isEmpty() ||
        finalModerators->size() >= maxModeratorsToFetch)
    {
        // Done paginating
        successCallback(*finalModerators);
        return;
    }

    this->fetchModerators(
        broadcasterID, NUM_MODERATORS_TO_FETCH_PER_REQUEST, moderators.cursor,
        [=, this](auto moderators) {
            this->onFetchModeratorsSuccess(
                finalModerators, broadcasterID, maxModeratorsToFetch,
                successCallback, failureCallback, moderators);
        },
        failureCallback);
}

// https://dev.twitch.tv/docs/api/reference#get-moderators
void Helix::fetchModerators(
    QString broadcasterID, int first, QString after,
    ResultCallback<HelixModerators> successCallback,
    FailureCallback<HelixGetModeratorsError, QString> failureCallback)
{
    using Error = HelixGetModeratorsError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("first", QString::number(first));

    if (!after.isEmpty())
    {
        urlQuery.addQueryItem("after", after);
    }

    this->makeGet("moderation/moderators", urlQuery)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting moderators was "
                    << result.formatError() << "but we expected it to be 200";
            }

            auto response = result.parseJson();
            successCallback(HelixModerators(response));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.contains("OAuth token"))
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error data:" << result.formatError()
                        << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

// Ban/timeout a user
// https://dev.twitch.tv/docs/api/reference#ban-user
void Helix::banUser(QString broadcasterID, QString moderatorID, QString userID,
                    std::optional<int> duration, QString reason,
                    ResultCallback<> successCallback,
                    FailureCallback<HelixBanUserError, QString> failureCallback)
{
    using Error = HelixBanUserError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    QJsonObject payload;
    {
        QJsonObject data;
        data["reason"] = reason;
        data["user_id"] = userID;
        if (duration)
        {
            data["duration"] = *duration;
        }

        payload["data"] = data;
    }

    this->makePost("moderation/bans", urlQuery)
        .json(payload)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for banning a user was"
                    << result.formatError() << "but we expected it to be 200";
            }
            // we don't care about the response
            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("The user specified in the user_id "
                                           "field is already banned",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::TargetBanned, message);
                    }
                    else if (message.startsWith(
                                 "The user specified in the user_id field may "
                                 "not be banned",
                                 Qt::CaseInsensitive))
                    {
                        failureCallback(Error::CannotBanUser, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 409: {
                    failureCallback(Error::ConflictingOperation, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        // Handle this error specifically because its API error is especially unfriendly
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error banning user:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

// Warn a user
// https://dev.twitch.tv/docs/api/reference#warn-chat-user
void Helix::warnUser(
    QString broadcasterID, QString moderatorID, QString userID, QString reason,
    ResultCallback<> successCallback,
    FailureCallback<HelixWarnUserError, QString> failureCallback)
{
    using Error = HelixWarnUserError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    QJsonObject payload;
    {
        QJsonObject data;
        data["reason"] = reason;
        data["user_id"] = userID;

        payload["data"] = data;
    }

    this->makePost("moderation/warnings", urlQuery)
        .json(payload)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for warning a user was"
                    << result.formatError() << "but we expected it to be 200";
            }
            // we don't care about the response
            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("The user specified in the user_id "
                                           "field may not be warned",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::CannotWarnUser, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                case 409: {
                    failureCallback(Error::ConflictingOperation, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error warning user:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::addSuspiciousUser(const QString broadcasterID,
                              const QString moderatorID, const QString userID,
                              const bool restricted,
                              ResultCallback<> successCallback,
                              FailureCallback<QString> failureCallback)
{
    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    QJsonObject payload;
    payload["user_id"] = userID;
    payload["status"] = restricted ? "RESTRICTED" : "ACTIVE_MONITORING";

    this->makePost("moderation/suspicious_users", urlQuery)
        .json(payload)
        .onSuccess([successCallback](const auto &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for treating a suspicious user was"
                    << result.formatError() << "but we expected it to be 200";
            }
            // we don't care about the response
            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::removeSuspiciousUser(const QString broadcasterID,
                                 const QString moderatorID,
                                 const QString userID,
                                 ResultCallback<> successCallback,
                                 FailureCallback<QString> failureCallback)
{
    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);
    urlQuery.addQueryItem("user_id", userID);

    this->makeDelete("moderation/suspicious_users", urlQuery)
        .onSuccess([successCallback](const auto &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for un-treating a suspicious user was"
                    << result.formatError() << "but we expected it to be 200";
            }
            // we don't care about the response
            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

// https://dev.twitch.tv/docs/api/reference#send-whisper
void Helix::sendWhisper(
    QString fromUserID, QString toUserID, QString message,
    ResultCallback<> successCallback,
    FailureCallback<HelixWhisperError, QString> failureCallback)
{
    using Error = HelixWhisperError;

    QUrlQuery urlQuery;

    urlQuery.addQueryItem("from_user_id", fromUserID);
    urlQuery.addQueryItem("to_user_id", toUserID);

    QJsonObject payload;
    payload["message"] = message;

    this->makePost("whispers", urlQuery)
        .json(payload)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for sending a whisper was"
                    << result.formatError() << "but we expected it to be 204";
            }
            // we don't care about the response
            successCallback();
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("A user cannot whisper themself",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::WhisperSelf, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        // Handle this error specifically because its API error is especially unfriendly
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.startsWith("the sender does not have a "
                                                "verified phone number",
                                                Qt::CaseInsensitive))
                    {
                        failureCallback(Error::NoVerifiedPhone, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    if (message.startsWith("The recipient's settings prevent "
                                           "this sender from whispering them",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::RecipientBlockedUser, message);
                    }
                    else
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                }
                break;

                case 404: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error banning user:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

// https://dev.twitch.tv/docs/api/reference#get-chatters
void Helix::getChatters(
    QString broadcasterID, QString moderatorID, size_t maxChattersToFetch,
    ResultCallback<HelixChatters> successCallback,
    FailureCallback<HelixGetChattersError, QString> failureCallback)
{
    auto finalChatters = std::make_shared<HelixChatters>();

    // Initiate the recursive calls
    this->fetchChatters(
        broadcasterID, moderatorID, NUM_CHATTERS_TO_FETCH, "",
        [=, this](auto chatters) {
            this->onFetchChattersSuccess(
                finalChatters, broadcasterID, moderatorID, maxChattersToFetch,
                successCallback, failureCallback, chatters);
        },
        failureCallback);
}

// https://dev.twitch.tv/docs/api/reference#get-moderators
void Helix::getModerators(
    QString broadcasterID, int maxModeratorsToFetch,
    ResultCallback<std::vector<HelixModerator>> successCallback,
    FailureCallback<HelixGetModeratorsError, QString> failureCallback)
{
    auto finalModerators = std::make_shared<std::vector<HelixModerator>>();

    // Initiate the recursive calls
    this->fetchModerators(
        broadcasterID, NUM_MODERATORS_TO_FETCH_PER_REQUEST, "",
        [=, this](auto moderators) {
            this->onFetchModeratorsSuccess(
                finalModerators, broadcasterID, maxModeratorsToFetch,
                successCallback, failureCallback, moderators);
        },
        failureCallback);
}

// List the VIPs of a channel
// https://dev.twitch.tv/docs/api/reference#get-vips
void Helix::getChannelVIPs(
    QString broadcasterID,
    ResultCallback<std::vector<HelixVip>> successCallback,
    FailureCallback<HelixListVIPsError, QString> failureCallback)
{
    using Error = HelixListVIPsError;
    QUrlQuery urlQuery;

    urlQuery.addQueryItem("broadcaster_id", broadcasterID);

    // No point pagi/pajanating, Twitch's max VIP count doesn't go over 100
    // TODO(jammehcow): probably still implement pagination
    //   as the mod list can go over 100 (I assume, I see no limit)
    urlQuery.addQueryItem("first", "100");

    this->makeGet("channels/vips", urlQuery)
        .header("Content-Type", "application/json")
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting VIPs was"
                    << result.formatError() << "but we expected it to be 200";
            }

            auto response = result.parseJson();

            std::vector<HelixVip> channelVips;
            for (const auto &jsonStream : response.value("data").toArray())
            {
                channelVips.emplace_back(jsonStream.toObject());
            }

            successCallback(channelVips);
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.compare(
                                 "The ID in broadcaster_id must match the user "
                                 "ID found in the request's OAuth token.",
                                 Qt::CaseInsensitive) == 0)
                    {
                        // Must be the broadcaster.
                        failureCallback(Error::UserNotBroadcaster, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error listing VIPs:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

void Helix::startCommercial(
    QString broadcasterID, int length,
    ResultCallback<HelixStartCommercialResponse> successCallback,
    FailureCallback<HelixStartCommercialError, QString> failureCallback)
{
    using Error = HelixStartCommercialError;

    QJsonObject payload;

    payload.insert("broadcaster_id", QJsonValue(broadcasterID));
    payload.insert("length", QJsonValue(length));

    this->makePost("channels/commercial", QUrlQuery())
        .json(payload)
        .onSuccess([successCallback, failureCallback](auto result) {
            auto obj = result.parseJson();
            if (obj.isEmpty())
            {
                failureCallback(
                    Error::Unknown,
                    "Twitch didn't send any information about this error.");
                return;
            }

            successCallback(HelixStartCommercialResponse(obj));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else if (message.contains(
                                 "To start a commercial, the broadcaster must "
                                 "be streaming live.",
                                 Qt::CaseInsensitive))
                    {
                        failureCallback(Error::BroadcasterNotStreaming,
                                        message);
                    }
                    else if (message.startsWith("Missing required parameter",
                                                Qt::CaseInsensitive))
                    {
                        failureCallback(Error::MissingLengthParameter, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 401: {
                    if (message.contains(
                            "The ID in broadcaster_id must match the user ID "
                            "found in the request's OAuth token.",
                            Qt::CaseInsensitive))
                    {
                        failureCallback(Error::TokenMustMatchBroadcaster,
                                        message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 429: {
                    // The cooldown period is implied to be included
                    // in the error's "retry_after" response field but isn't.
                    // If this becomes available we should append that to the error message.
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                default: {
                    qCDebug(chatterinoTwitch)
                        << "Unhandled error starting commercial:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

// Twitch global badges
// https://dev.twitch.tv/docs/api/reference/#get-global-chat-badges
void Helix::getGlobalBadges(
    ResultCallback<HelixGlobalBadges> successCallback,
    FailureCallback<HelixGetGlobalBadgesError, QString> failureCallback)
{
    using Error = HelixGetGlobalBadgesError;

    this->makeGet("chat/badges/global", QUrlQuery())
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting global badges was "
                    << result.formatError() << "but we expected it to be 200";
            }

            auto response = result.parseJson();
            successCallback(HelixGlobalBadges(response));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 401: {
                    failureCallback(Error::UserNotAuthenticated, message);
                }
                break;

                default: {
                    qCWarning(chatterinoTwitch)
                        << "Helix global badges, unhandled error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

// Badges for the `broadcasterID` channel
// https://dev.twitch.tv/docs/api/reference/#get-channel-chat-badges
void Helix::getChannelBadges(
    QString broadcasterID, ResultCallback<HelixChannelBadges> successCallback,
    FailureCallback<HelixGetChannelBadgesError, QString> failureCallback)
{
    using Error = HelixGetChannelBadgesError;

    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", broadcasterID);

    this->makeGet("chat/badges", urlQuery)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting badges was "
                    << result.formatError() << "but we expected it to be 200";
            }

            auto response = result.parseJson();
            successCallback(HelixChannelBadges(response));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();

            switch (*result.status())
            {
                case 400: {
                    failureCallback(Error::Forwarded, message);
                }
                break;

                case 401: {
                    failureCallback(Error::UserNotAuthenticated, message);
                }
                break;

                default: {
                    qCWarning(chatterinoTwitch)
                        << "Helix channel badges, unhandled error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

// https://dev.twitch.tv/docs/api/reference/#update-shield-mode-status
void Helix::updateShieldMode(
    QString broadcasterID, QString moderatorID, bool isActive,
    ResultCallback<HelixShieldModeStatus> successCallback,
    FailureCallback<HelixUpdateShieldModeError, QString> failureCallback)
{
    using Error = HelixUpdateShieldModeError;

    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    QJsonObject payload;
    payload["is_active"] = isActive;

    this->makePut("moderation/shield_mode", urlQuery)
        .json(payload)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for updating shield mode was "
                    << result.formatError() << "but we expected it to be 200";
            }

            const auto response = result.parseJson();
            successCallback(
                HelixShieldModeStatus(response["data"][0].toObject()));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            auto message = obj["message"].toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                        break;
                    }

                    failureCallback(Error::Forwarded, message);
                }
                break;
                case 401: {
                    failureCallback(Error::Forwarded, message);
                }
                break;
                case 403: {
                    if (message.startsWith(
                            "Requester does not have permissions",
                            Qt::CaseInsensitive))
                    {
                        failureCallback(Error::MissingPermission, message);
                        break;
                    }
                }

                default: {
                    qCWarning(chatterinoTwitch)
                        << "Helix shield mode, unhandled error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
                break;
            }
        })
        .execute();
}

// https://dev.twitch.tv/docs/api/reference/#send-a-shoutout
void Helix::sendShoutout(
    QString fromBroadcasterID, QString toBroadcasterID, QString moderatorID,
    ResultCallback<> successCallback,
    FailureCallback<HelixSendShoutoutError, QString> failureCallback)
{
    using Error = HelixSendShoutoutError;

    QUrlQuery urlQuery;
    urlQuery.addQueryItem("from_broadcaster_id", fromBroadcasterID);
    urlQuery.addQueryItem("to_broadcaster_id", toBroadcasterID);
    urlQuery.addQueryItem("moderator_id", moderatorID);

    this->makePost("chat/shoutouts", urlQuery)
        .header("Content-Type", "application/json")
        .onSuccess([successCallback](NetworkResult result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for sending shoutout was "
                    << result.formatError() << "but we expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            auto message = obj["message"].toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith("The broadcaster may not give "
                                           "themselves a Shoutout.",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserIsBroadcaster, message);
                    }
                    else if (message.startsWith(
                                 "The broadcaster is not streaming live or "
                                 "does not have one or more viewers.",
                                 Qt::CaseInsensitive))
                    {
                        failureCallback(Error::BroadcasterNotLive, message);
                    }
                    else
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                }
                break;

                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::UserNotAuthorized, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::UserNotAuthorized, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                case 500: {
                    if (message.isEmpty())
                    {
                        failureCallback(Error::Unknown,
                                        "Twitch internal server error");
                    }
                    else
                    {
                        failureCallback(Error::Unknown, message);
                    }
                }
                break;

                default: {
                    qCWarning(chatterinoTwitch)
                        << "Helix send shoutout, unhandled error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
            }
        })
        .execute();
}

// https://dev.twitch.tv/docs/api/reference/#send-chat-message
void Helix::sendChatMessage(
    HelixSendMessageArgs args, ResultCallback<HelixSentMessage> successCallback,
    FailureCallback<HelixSendMessageError, QString> failureCallback)
{
    using Error = HelixSendMessageError;

    QJsonObject json{{
        {"broadcaster_id", args.broadcasterID},
        {"sender_id", args.senderID},
        {"message", args.message},
    }};
    if (!args.replyParentMessageID.isEmpty())
    {
        json["reply_parent_message_id"] = args.replyParentMessageID;
    }
    if (args.pin)
    {
        json["pin"] = true;
    }

    this->makePost("chat/messages", {})
        .json(json)
        .onSuccess([successCallback](const NetworkResult &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for sending chat message was "
                    << result.formatError() << "but we expected it to be 200";
            }
            auto json = result.parseJson();

            successCallback(HelixSentMessage(
                json.value("data").toArray().at(0).toObject()));
        })
        .onError([failureCallback](const NetworkResult &result) -> void {
            if (!result.status())
            {
                failureCallback(Error::Unknown, result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            auto message = obj["message"].toString();

            if (message.isEmpty())
            {
                message = u"Twitch internal server error (" %
                          result.formatError() % ')';
            }

            switch (*result.status())
            {
                case 400: {
                    failureCallback(Error::Unknown, message);
                }
                break;

                case 401: {
                    if (message.startsWith("User access token requires the",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback(Error::UserMissingScope, message);
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                case 403: {
                    failureCallback(Error::Forbidden, message);
                }
                break;

                case 422: {
                    failureCallback(Error::MessageTooLarge, message);
                }
                break;

                case 500: {
                    failureCallback(Error::Unknown, message);
                }
                break;

                default: {
                    qCWarning(chatterinoTwitch)
                        << "Helix send chat message, unhandled error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Unknown, message);
                }
            }
        })
        .execute();
}

void Helix::getUserEmotes(
    QString userID, QString broadcasterID,
    ResultCallback<std::vector<HelixChannelEmote>, HelixPaginationState>
        pageCallback,
    FailureCallback<QString> failureCallback, CancellationToken &&token)
{
    QUrlQuery query{{u"user_id"_s, userID}};
    if (!broadcasterID.isEmpty())
    {
        query.addQueryItem(u"broadcaster_id"_s, broadcasterID);
    }

    this->paginate(
        u"chat/emotes/user"_s, query,
        [pageCallback](const QJsonObject &json, const auto &state) mutable {
            const auto data = json["data"_L1].toArray();

            std::vector<HelixChannelEmote> emotes;
            emotes.reserve(data.count());

            for (const auto &emote : data)
            {
                emotes.emplace_back(emote.toObject());
            }

            pageCallback(emotes, state);

            return true;
        },
        [failureCallback](const NetworkResult &result) {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            auto message = obj["message"].toString();

            switch (*result.status())
            {
                case 401: {
                    if (message.startsWith("Missing scope",
                                           Qt::CaseInsensitive))
                    {
                        failureCallback("Missing required scope. Re-login with "
                                        "your account and try again.");
                        break;
                    }
                    [[fallthrough]];
                }
                default: {
                    qCWarning(chatterinoTwitch)
                        << "Helix get user emotes, unhandled error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(message);
                }
            }
        },
        std::move(token));
}

void Helix::getFollowedChannel(
    QString userID, QString broadcasterID, const QObject *caller,
    ResultCallback<std::optional<HelixFollowedChannel>> successCallback,
    FailureCallback<QString> failureCallback)
{
    this->makeGet("channels/followed",
                  {
                      {u"user_id"_s, userID},
                      {u"broadcaster_id"_s, broadcasterID},
                  })
        .caller(caller)
        .onSuccess([successCallback](auto result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting badges was "
                    << result.formatError() << "but we expected it to be 200";
            }

            const auto response = result.parseJson();
            const auto channel = response["data"_L1].toArray().at(0);
            if (channel.isObject())
            {
                successCallback(HelixFollowedChannel(channel.toObject()));
            }
            else
            {
                successCallback(std::nullopt);
            }
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::createPoll(QString broadcasterID, QString title,
                       QStringList choices, const std::chrono::seconds duration,
                       const int pointsPerVote,
                       ResultCallback<> successCallback,
                       FailureCallback<QString> failureCallback)
{
    // Prepare request body
    QJsonArray choiceArray;
    for (auto choice : choices)
    {
        choiceArray.append(QJsonObject{{{"title", choice}}});
    }

    QJsonObject json{{{"broadcaster_id", broadcasterID},
                      {"title", title},
                      {"duration", static_cast<int>(duration.count())},
                      {"choices", choiceArray}}};

    if (pointsPerVote > 0)
    {
        json["channel_points_voting_enabled"] = true;
        json["channel_points_per_vote"] = static_cast<qint64>(pointsPerVote);
    }

    // Execute API call
    this->makePost("polls", {})
        .json(json)
        .onSuccess([successCallback](const NetworkResult &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for creating a poll was "
                    << result.formatError() << "but we expected it to be 200";
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            const auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::getPolls(const QString broadcasterID, QStringList ids,
                     const int first, const QString after,
                     ResultCallback<HelixPolls> successCallback,
                     FailureCallback<QString> failureCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("first", QString::number(first));

    if (!after.isEmpty())
    {
        urlQuery.addQueryItem("after", after);
    }

    for (const auto &id : ids)
    {
        urlQuery.addQueryItem("id", id);
    }

    this->makeGet("polls", urlQuery)
        .onSuccess([successCallback](const auto &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting polls was "
                    << result.formatError() << "but we expected it to be 200";
            }

            const auto response = result.parseJson();
            successCallback(HelixPolls(response));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::endPoll(const QString broadcasterID, const QString id,
                    const bool immediatelyHide,
                    ResultCallback<HelixPoll> successCallback,
                    FailureCallback<QString> failureCallback)
{
    QJsonObject payload;
    payload.insert("broadcaster_id", broadcasterID);
    payload.insert("id", id);
    payload.insert("status", immediatelyHide ? "ARCHIVED" : "TERMINATED");

    this->makePatch("polls", {})
        .json(payload)
        .onSuccess([successCallback](const NetworkResult &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for ending a poll was "
                    << result.formatError() << "but we expected it to be 200";
            }

            const auto response = result.parseJson();
            const auto data = HelixPolls(response);
            successCallback(data.polls.front());
        })
        .onError([failureCallback](const NetworkResult &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            const auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::createPrediction(const QString broadcasterID, const QString title,
                             QStringList outcomes,
                             const std::chrono::seconds duration,
                             ResultCallback<> successCallback,
                             FailureCallback<QString> failureCallback)
{
    // Prepare request body
    QJsonArray outcomeArray;
    for (auto outcome : outcomes)
    {
        outcomeArray.append(QJsonObject{{{"title", outcome}}});
    }

    QJsonObject payload;
    payload.insert("broadcaster_id", broadcasterID);
    payload.insert("title", title);
    payload.insert("prediction_window", static_cast<int>(duration.count()));
    payload.insert("outcomes", outcomeArray);

    // Execute API call
    this->makePost("predictions", {})
        .json(payload)
        .onSuccess([successCallback](const NetworkResult &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for creating a prediction was "
                    << result.formatError() << "but we expected it to be 200";
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            const auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::getPredictions(const QString broadcasterID, QStringList ids,
                           const int first, const QString after,
                           ResultCallback<HelixPredictions> successCallback,
                           FailureCallback<QString> failureCallback)
{
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("broadcaster_id", broadcasterID);
    urlQuery.addQueryItem("first", QString::number(first));

    if (!after.isEmpty())
    {
        urlQuery.addQueryItem("after", after);
    }

    for (const auto &id : ids)
    {
        urlQuery.addQueryItem("id", id);
    }

    this->makeGet("predictions", urlQuery)
        .onSuccess([successCallback](const auto &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting predictions was "
                    << result.formatError() << "but we expected it to be 200";
            }

            const auto response = result.parseJson();
            successCallback(HelixPredictions(response));
        })
        .onError([failureCallback](const auto &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            auto obj = result.parseJson();
            auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

// End prediction can lock, cancel, or resolve an outstanding prediction.
void Helix::endPrediction(const QString broadcasterID, const QString id,
                          const bool refundPoints,
                          const QString winningOutcomeID,
                          ResultCallback<HelixPrediction> successCallback,
                          FailureCallback<QString> failureCallback)
{
    QJsonObject payload;
    payload.insert("broadcaster_id", broadcasterID);
    payload.insert("id", id);
    if (refundPoints)
    {
        payload.insert("status", "CANCELED");
    }
    else if (winningOutcomeID.isEmpty())
    {
        payload.insert("status", "LOCKED");
    }
    else
    {
        payload.insert("status", "RESOLVED");
        payload.insert("winning_outcome_id", winningOutcomeID);
    }

    this->makePatch("predictions", {})
        .json(payload)
        .onSuccess([successCallback](const NetworkResult &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for ending a prediction was "
                    << result.formatError() << "but we expected it to be 200";
            }

            const auto response = result.parseJson();
            const auto data = HelixPredictions(response);
            successCallback(data.predictions.front());
        })
        .onError([failureCallback](const NetworkResult &result) -> void {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            const auto message = obj.value("message").toString();
            if (!message.isEmpty())
            {
                failureCallback(message);
            }
            else
            {
                failureCallback(result.formatError());
            }
        })
        .execute();
}

void Helix::createEventSubSubscription(
    const eventsub::SubscriptionRequest &request, const QString &sessionID,
    ResultCallback<HelixCreateEventSubSubscriptionResponse> successCallback,
    FailureCallback<HelixCreateEventSubSubscriptionError, QString>
        failureCallback)
{
    using Error = HelixCreateEventSubSubscriptionError;

    QJsonObject condition;
    for (const auto &[conditionKey, conditionValue] : request.conditions)
    {
        condition.insert(conditionKey, conditionValue);
    }

    QJsonObject body;
    body.insert("type", request.subscriptionType);
    body.insert("version", request.subscriptionVersion);
    body.insert("condition", condition);

    QJsonObject transport;
    transport.insert("method", "websocket");
    transport.insert("session_id", sessionID);

    body.insert("transport", transport);

    this->makePost("eventsub/subscriptions", {})
        .json(body)
        .onSuccess([successCallback](const auto &result) {
            if (result.status() != 202)
            {
                qCWarning(chatterinoTwitchEventSub)
                    << "Success result for creating eventsub subscription was "
                    << result.formatError() << "but we expected it to be 202";
            }

            HelixCreateEventSubSubscriptionResponse response(
                result.parseJson());

            successCallback(response);
        })
        .onError([failureCallback](const NetworkResult &result) {
            if (!result.status())
            {
                failureCallback(Error::Forwarded, result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            auto message = obj["message"].toString();

            switch (*result.status())
            {
                case 400: {
                    if (message.startsWith(
                            "websocket transport session does not exist",
                            Qt::CaseInsensitive))
                    {
                        failureCallback(Error::NoSession, message);
                    }
                    else
                    {
                        failureCallback(Error::BadRequest, message);
                    }
                }
                break;

                case 401: {
                    failureCallback(Error::Unauthorized, message);
                }
                break;

                case 403: {
                    failureCallback(Error::Forbidden, message);
                }
                break;
                case 409: {
                    failureCallback(Error::Conflict, message);
                }
                break;

                case 429: {
                    failureCallback(Error::Ratelimited, message);
                }
                break;

                case 500: {
                    if (message.isEmpty())
                    {
                        failureCallback(Error::Forwarded,
                                        "Twitch internal server error");
                    }
                    else
                    {
                        failureCallback(Error::Forwarded, message);
                    }
                }
                break;

                default: {
                    qCWarning(chatterinoTwitchEventSub)
                        << "Helix Create EventSub Subscription, unhandled "
                           "error data:"
                        << result.formatError() << result.getData() << obj;
                    failureCallback(Error::Forwarded, message);
                }
            }
        })
        .execute();
}

QDebug &operator<<(QDebug &dbg,
                   const HelixCreateEventSubSubscriptionResponse &data)
{
    dbg << "HelixCreateEventSubSubscriptionResponse{ id:" << data.subscriptionID
        << "status:" << data.subscriptionStatus
        << "type:" << data.subscriptionType
        << "version:" << data.subscriptionVersion
        << "condition:" << data.subscriptionCondition
        << "createdAt:" << data.subscriptionCreatedAt
        << "sessionID:" << data.subscriptionSessionID
        << "connectedAt:" << data.subscriptionConnectedAt
        << "cost:" << data.subscriptionCost << "total:" << data.total
        << "totalCost:" << data.totalCost
        << "maxTotalCost:" << data.maxTotalCost << '}';
    return dbg;
}

void Helix::deleteEventSubSubscription(const QString &subscriptionID,
                                       ResultCallback<> successCallback,
                                       FailureCallback<QString> failureCallback)
{
    QUrlQuery query;
    query.addQueryItem("id", subscriptionID);

    this->makeDelete("eventsub/subscriptions", query)
        .onSuccess([successCallback](const auto &result) {
            if (result.status() != 204)
            {
                qCWarning(chatterinoTwitchEventSub)
                    << "Success result for deleting eventsub subscription was "
                    << result.formatError() << "but we expected it to be 204";
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            if (!result.status())
            {
                failureCallback(result.formatError());
                return;
            }

            const auto obj = result.parseJson();
            auto message = obj["message"].toString();

            if (message.isEmpty())
            {
                failureCallback("Twitch internal server error");
            }
            else
            {
                failureCallback(message);
            }
        })
        .execute();
}

NetworkRequest Helix::makeRequest(const QString &url, const QUrlQuery &urlQuery,
                                  NetworkRequestType type)
{
    assert(!url.startsWith("/"));

    if (this->clientId.isEmpty())
    {
        qCDebug(chatterinoTwitch)
            << "Helix::makeRequest called without a client ID set BabyRage";
        // return std::nullopt;
    }

    if (this->oauthToken.isEmpty())
    {
        qCDebug(chatterinoTwitch)
            << "Helix::makeRequest called without an oauth token set BabyRage";
        // return std::nullopt;
    }

    QString baseUrl("https://api.twitch.tv/helix/");

#ifndef NDEBUG
    bool ignoreSslErrors =
        getApp()->getArgs().useLocalEventsub && url == "eventsub/subscriptions";
    if (ignoreSslErrors)
    {
        baseUrl = "https://127.0.0.1:3012/";
    }
#endif

    QUrl fullUrl(baseUrl + url);

    fullUrl.setQuery(urlQuery);

    return NetworkRequest(fullUrl, type)
        .timeout(5 * 1000)
        .header("Accept", "application/json")
        .header("Client-ID", this->clientId)
        .header("Authorization", "Bearer " + this->oauthToken)
#ifndef NDEBUG
        .ignoreSslErrors(ignoreSslErrors)
#endif
        ;
}

NetworkRequest Helix::makeGet(const QString &url, const QUrlQuery &urlQuery)
{
    return this->makeRequest(url, urlQuery, NetworkRequestType::Get);
}

NetworkRequest Helix::makeDelete(const QString &url, const QUrlQuery &urlQuery)
{
    return this->makeRequest(url, urlQuery, NetworkRequestType::Delete);
}

NetworkRequest Helix::makePost(const QString &url, const QUrlQuery &urlQuery)
{
    return this->makeRequest(url, urlQuery, NetworkRequestType::Post);
}

NetworkRequest Helix::makePut(const QString &url, const QUrlQuery &urlQuery)
{
    return this->makeRequest(url, urlQuery, NetworkRequestType::Put);
}

NetworkRequest Helix::makePatch(const QString &url, const QUrlQuery &urlQuery)
{
    return this->makeRequest(url, urlQuery, NetworkRequestType::Patch);
}

void Helix::paginate(
    const QString &url, const QUrlQuery &baseQuery,
    std::function<bool(const QJsonObject &, const HelixPaginationState &state)>
        onPage,
    std::function<void(NetworkResult)> onError,
    CancellationToken &&cancellationToken)
{
    auto onSuccess =
        std::make_shared<std::function<void(NetworkResult)>>(nullptr);
    // This is the actual callback passed to NetworkRequest.
    // It wraps the shared-ptr.
    auto onSuccessCb = [onSuccess](const auto &res) {
        return (*onSuccess)(res);
    };

    *onSuccess = [this, onPage = std::move(onPage), onError, onSuccessCb,
                  url{url}, baseQuery{baseQuery},
                  cancellationToken =
                      std::move(cancellationToken)](const NetworkResult &res) {
        if (cancellationToken.isCancelled())
        {
            return;
        }

        const auto json = res.parseJson();
        const auto pagination = json["pagination"_L1].toObject();

        auto cursor = pagination["cursor"_L1].toString();
        HelixPaginationState state{.done = cursor.isEmpty()};

        if (!onPage(json, state))
        {
            // The consumer doesn't want any more pages
            qCDebug(chatterinoTwitch)
                << "paginate onPage returned false for" << url;
            return;
        }

        // After done is set, onPage must never be called again
        if (state.done)
        {
            return;
        }

        auto query = baseQuery;
        query.removeAllQueryItems(u"after"_s);
        query.addQueryItem(u"after"_s, cursor);

        this->makeGet(url, query)
            .onSuccess(onSuccessCb)
            .onError(onError)
            .execute();
    };

    this->makeGet(url, baseQuery)
        .onSuccess(std::move(onSuccessCb))
        .onError(std::move(onError))
        .execute();
}

namespace {

constexpr auto TWITCH_WEB_GQL_CLIENT_ID = "kimne78kx3ncx6brgo4mv6wki5h1ko";
constexpr auto TWITCH_WEB_GQL_CLIENT_VERSION =
    "ef928475-9403-42f2-8a34-55784bd08e16";
constexpr auto TWITCH_WEB_GQL_USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/108.0.0.0 Safari/537.36";
constexpr auto TWITCH_TV_CLIENT_ID = "ue6666qo983tsx6so1t0vnawi233wa";
constexpr auto TWITCH_TV_USER_AGENT =
    "Mozilla/5.0 (Linux; Android 7.1; Smart Box C1) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/108.0.0.0 Safari/537.36";
constexpr auto TWITCH_TV_ORIGIN = "https://android.tv.twitch.tv";
constexpr auto TWITCH_TV_REFERER = "https://android.tv.twitch.tv/";
constexpr auto TWITCH_TV_SCOPES =
    "chat:read chat:edit channel:moderate channel:manage:polls "
    "channel:manage:predictions moderator:manage:announcements "
    "moderator:manage:chat_messages moderator:manage:chat_settings "
    "moderator:read:chat_settings moderator:read:followers";
constexpr auto CREATE_POLL_HASH =
    "4b1461a13fe166a59044961db192747d606f71a89abc3bfdecf79fe862d205cf";
constexpr auto CREATE_PREDICTION_HASH =
    "92268878ac4abe722bcdcba85a4e43acdd7a99d86b05851759e1d8f385cc32ea";
constexpr int TWITCH_WEB_GQL_TIMEOUT_MS = 15 * 1000;
constexpr int TWITCH_TV_AUTH_TIMEOUT_MS = 20 * 1000;
constexpr int TWITCH_AUTH_TOKEN_MAX_LENGTH = 4096;

QString moderationAuthSidecarPath()
{
    return getApp()->getPaths().settingsDirectory +
           QStringLiteral("/twitch-moderation-auth.json");
}

QJsonObject readJsonObjectFile(const QString &path, bool *found = nullptr)
{
    QFile file(path);
    if (found != nullptr)
    {
        *found = file.exists();
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return {};
    }

    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
    {
        return {};
    }

    return document.object();
}

QJsonObject readModerationAuthSidecarObject(bool *found = nullptr)
{
    return readJsonObjectFile(moderationAuthSidecarPath(), found);
}

pajlada::Signals::NoArgSignal &moderationAuthAccountChangedSignal()
{
    static pajlada::Signals::NoArgSignal signal;
    return signal;
}

QJsonObject readModerationAuthSettingsObject()
{
    const auto settingsPath =
        getApp()->getPaths().settingsDirectory + QStringLiteral("/settings.json");
    const auto root = readJsonObjectFile(settingsPath);

    const auto object = root.value(QStringLiteral("misc"))
                            .toObject()
                            .value(QStringLiteral("twitch"))
                            .toObject()
                            .value(QStringLiteral("moderationAuth"))
                            .toObject();
    return object;
}

QString readModerationAuthString(const QJsonObject &object, const QString &key)
{
    return object.value(key).toString().trimmed();
}

QString stripTwitchOAuthPrefix(const QString &oauthToken)
{
    auto token = oauthToken.trimmed();
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

QString authResponseText(const NetworkResult &result)
{
    const auto body = QString::fromUtf8(result.getData()).trimmed();
    if (!body.isEmpty())
    {
        return body.left(200);
    }

    return result.formatError();
}

QString moderationAuthClipboardText()
{
#ifdef USEWINSDK
    if (!OpenClipboard(nullptr))
    {
        return {};
    }

    struct CloseClipboardGuard {
        ~CloseClipboardGuard()
        {
            CloseClipboard();
        }
    } guard;

    auto *handle = GetClipboardData(CF_UNICODETEXT);
    if (handle == nullptr)
    {
        return {};
    }

    auto *text = static_cast<const wchar_t *>(GlobalLock(handle));
    if (text == nullptr)
    {
        return {};
    }

    auto result = QString::fromWCharArray(text);
    GlobalUnlock(handle);
    return result;
#else
    return getClipboardText();
#endif
}

QString moderationAuthClipboardScript()
{
    return QStringLiteral(R"MERGERINO_JS(copy(await (async () => {
  const normalize = value => {
    if (!value) {
      return '';
    }

    let text = String(value).trim();
    return String(text || '')
      .replace(/^(OAuth|Bearer)\s+/i, '')
      .replace(/^oauth:/i, '')
      .trim()
      .match(/^[A-Za-z0-9._~+/=-]{20,4096}$/)?.[0] || '';
  };

  const extractFromObject = (value, depth = 0) => {
    if (!value || depth > 8) {
      return '';
    }

    if (typeof value === 'string') {
      const text = value.trim();
      if (text.startsWith('{') || text.startsWith('[')) {
        try {
          const token = extractFromObject(JSON.parse(text), depth + 1);
          if (token) {
            return token;
          }
        } catch (_) {
        }
      }

      return normalize(text);
    }

    if (Array.isArray(value)) {
      for (const entry of value) {
        const token = extractFromObject(entry, depth + 1);
        if (token) {
          return token;
        }
      }
      return '';
    }

    if (typeof value === 'object') {
      const keys = Object.keys(value).sort((a, b) => {
        const score = key => {
          const lower = key.toLowerCase();
          return (lower.includes('token') ? 4 : 0) +
            (lower.includes('auth') || lower.includes('oauth') ? 3 : 0) +
            (lower.includes('access') ? 2 : 0);
        };
        return score(b) - score(a);
      });

      for (const key of keys) {
        const token = extractFromObject(value[key], depth + 1);
        if (token) {
          return token;
        }
      }
    }

    return '';
  };

  const clean = value => extractFromObject(value);
  const clientId = 'kimne78kx3ncx6brgo4mv6wki5h1ko';
  const randomDeviceId = () => {
    const fallback = () => Math.random().toString(16).slice(2) +
      Math.random().toString(16).slice(2);
    try {
      if (crypto && crypto.randomUUID) {
        return crypto.randomUUID().replace(/-/g, '');
      }
    } catch (_) {
    }
    return fallback().replace(/[^A-Za-z0-9]/g, '').slice(0, 32);
  };
  const storageValue = key => {
    try {
      return localStorage.getItem(key) || '';
    } catch (_) {
      return '';
    }
  };
  const deviceId = clean(storageValue('twilight.deviceId')) ||
    clean(storageValue('twilight.uniqueID')) ||
    clean(storageValue('unique_id')) ||
    randomDeviceId();
  const configureIntegrity = async () => {
    if (!window.KPSDK || !window.KPSDK.configure) {
      return;
    }
    try {
      const configured = Promise.resolve(window.KPSDK.configure([{
        protocol: 'https:',
        method: 'POST',
        domain: 'gql.twitch.tv',
        path: '/integrity',
      }])).catch(error => {
        const message = String(error && error.message || error || '');
        if (!message.toLowerCase().includes('already been configured')) {
          console.warn('Mergerino could not configure Twitch integrity:', message);
        }
      });
      await Promise.race([
        configured,
        new Promise(resolve => setTimeout(resolve, 1500)),
      ]);
    } catch (_) {
    }
  };
  const fetchIntegrity = async token => {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 5000);
    try {
      await configureIntegrity();
      const headers = {
        'Client-ID': clientId,
        'X-Device-Id': deviceId,
      };
      if (token) {
        headers.Authorization = `OAuth ${token}`;
      }
      const response = await fetch('https://gql.twitch.tv/integrity', {
        method: 'POST',
        mode: 'cors',
        credentials: 'omit',
        headers,
        signal: controller.signal,
      });
      if (!response.ok) {
        return '';
      }
      const body = await response.json();
      return clean(body && body.token);
    } catch (_) {
      return '';
    } finally {
      clearTimeout(timeout);
    }
  };
  const cookieToken = () => {
    const cookies = String(document.cookie || '').split(';');
    const preferred = [
      'auth-token',
      'api_token',
      'twilight-user',
      'persistent',
    ];

    for (const name of preferred) {
      const prefix = `${name}=`;
      const entry = cookies.map(cookie => cookie.trim())
        .find(cookie => cookie.startsWith(prefix));
      if (!entry) {
        continue;
      }

      const token = clean(decodeURIComponent(entry.slice(prefix.length)));
      if (token) {
        return token;
      }
    }

    for (const cookie of cookies) {
      const [name, ...parts] = cookie.trim().split('=');
      const lower = String(name || '').toLowerCase();
      if (!lower.includes('token') && !lower.includes('auth') &&
          !lower.includes('twilight')) {
        continue;
      }

      const token = clean(decodeURIComponent(parts.join('=')));
      if (token) {
        return token;
      }
    }

    return '';
  };

  const safeStorageGet = (storage, key) => {
    try {
      return storage.getItem(key);
    } catch (_) {
      return '';
    }
  };

  const storages = [];
  try {
    storages.push(localStorage);
  } catch (_) {
  }
  try {
    storages.push(sessionStorage);
  } catch (_) {
  }

  let token = cookieToken();
  if (!token) {
    const preferredKeys = [
      'twilight.authToken',
      'twilight-user',
      'auth-token',
      'token',
      'access_token',
    ];
    for (const storage of storages) {
      for (const key of preferredKeys) {
        token = clean(safeStorageGet(storage, key));
        if (token) {
          break;
        }
      }
      if (token) {
        break;
      }
    }
  }

  if (!token) {
    for (const storage of storages) {
      let length = 0;
      try {
        length = storage.length;
      } catch (_) {
        length = 0;
      }
      for (let i = 0; i < length; ++i) {
        let key = '';
        try {
          key = storage.key(i) || '';
        } catch (_) {
        }
        const lower = key.toLowerCase();
        if (lower.includes('token') &&
            (lower.includes('auth') || lower.includes('oauth') ||
             lower.includes('access') || lower.includes('twilight'))) {
          token = clean(safeStorageGet(storage, key));
          if (token) {
            break;
          }
        }
      }
      if (token) {
        break;
      }
    }
  }

  if (!token) {
    alert('Mergerino could not find a Twitch auth token in cookies or browser storage. Make sure you are logged in on twitch.tv, refresh, and try again.');
    throw new Error('Mergerino could not find a Twitch auth token.');
  }

  const integrityToken = await fetchIntegrity(token);
  const payload = JSON.stringify({
    mergerino: 'twitch-browser-helper',
    version: 2,
    token,
    clientId,
    deviceId,
    clientIntegrity: integrityToken,
  });
  return payload;
})());
console.log('Mergerino token copied. Return to Mergerino and click Paste Token.');)MERGERINO_JS");
}

void saveModerationAccount(const TwitchModerationAuth::Account &account)
{
    QSaveFile file(moderationAuthSidecarPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return;
    }

    QJsonObject object;
    object.insert(QStringLiteral("token"), account.oauthToken);
    object.insert(QStringLiteral("clientId"), account.clientId);
    object.insert(QStringLiteral("clientIntegrity"), account.clientIntegrity);
    object.insert(QStringLiteral("deviceId"), account.deviceId);
    object.insert(QStringLiteral("userId"), account.userId);
    object.insert(QStringLiteral("login"), account.login);
    object.insert(QStringLiteral("displayName"), account.displayName);
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    file.commit();
}

TwitchModerationAuth::Account accountFromModerationAuthObject(
    const QJsonObject &moderationAuth)
{
    auto clientId = readModerationAuthString(moderationAuth,
                                             QStringLiteral("clientId"));
    if (clientId.isEmpty())
    {
        clientId = QString::fromLatin1(TWITCH_WEB_GQL_CLIENT_ID);
    }

    auto token = TwitchModerationAuth::normalizeToken(
        readModerationAuthString(moderationAuth, QStringLiteral("token")));

    auto clientIntegrity =
        readModerationAuthString(moderationAuth,
                                 QStringLiteral("clientIntegrity"));

    auto deviceId =
        readModerationAuthString(moderationAuth, QStringLiteral("deviceId"));

    auto userId =
        readModerationAuthString(moderationAuth, QStringLiteral("userId"));

    auto login =
        readModerationAuthString(moderationAuth, QStringLiteral("login"))
            .toLower();

    auto displayName =
        readModerationAuthString(moderationAuth,
                                 QStringLiteral("displayName"));

    return {
        clientId,
        token,
        clientIntegrity,
        deviceId,
        userId,
        login,
        displayName,
    };
}

void clearModerationAccount()
{
    QSaveFile file(moderationAuthSidecarPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        return;
    }

    file.write(QJsonDocument(QJsonObject{}).toJson(QJsonDocument::Compact));
    file.commit();
}

QString compactUuid()
{
    auto id = generateUuid();
    id.remove(QLatin1Char('{'));
    id.remove(QLatin1Char('}'));
    id.remove(QLatin1Char('-'));
    return id;
}

QString webGqlDeviceId()
{
    static const auto deviceId = compactUuid();
    return deviceId;
}

QString webGqlSessionId()
{
    static const auto sessionId = compactUuid();
    return sessionId;
}

QString predictionOutcomeColor(int index, int outcomeCount)
{
    if (outcomeCount == 2)
    {
        return index == 0 ? QStringLiteral("BLUE") : QStringLiteral("PINK");
    }

    return QStringLiteral("BLUE");
}

QJsonObject firstWebGqlEnvelope(const QJsonValue &root)
{
    if (root.isObject())
    {
        return root.toObject();
    }

    const auto batch = root.toArray();
    if (!batch.isEmpty() && batch.first().isObject())
    {
        return batch.first().toObject();
    }

    return {};
}

QString firstWebGqlError(const QJsonObject &envelope)
{
    const auto errors = envelope.value(QStringLiteral("errors")).toArray();
    for (const auto &errorValue : errors)
    {
        const auto error = errorValue.toObject();
        const auto message = error.value(QStringLiteral("message")).toString();
        if (!message.isEmpty())
        {
            return message;
        }
    }

    return {};
}

QString firstWebGqlError(const QJsonValue &root)
{
    if (!root.isArray())
    {
        return firstWebGqlError(firstWebGqlEnvelope(root));
    }

    for (const auto &envelopeValue : root.toArray())
    {
        const auto message = firstWebGqlError(envelopeValue.toObject());
        if (!message.isEmpty())
        {
            return message;
        }
    }
    return {};
}

QJsonObject webGqlData(const QJsonValue &root)
{
    return firstWebGqlEnvelope(root).value(QStringLiteral("data")).toObject();
}

QString webGqlMutationErrorText(const QJsonObject &payload,
                                const QString &fallback)
{
    const auto value = payload.value(QStringLiteral("error"));
    if (value.isUndefined() || value.isNull())
    {
        return {};
    }
    if (value.isString())
    {
        return value.toString().trimmed();
    }
    if (!value.isObject())
    {
        return fallback;
    }

    const auto obj = value.toObject();
    for (const auto &key : {
             QStringLiteral("message"),
             QStringLiteral("code"),
             QStringLiteral("reason"),
         })
    {
        const auto text = obj.value(key).toString().trimmed();
        if (!text.isEmpty())
        {
            return text;
        }
    }

    return fallback;
}

NetworkRequest makeTwitchWebGqlRequest(const QString &operationName,
                                       const char *operationHash,
                                       const QJsonObject &variables,
                                       const QString &oauthClient,
                                       const QString &oauthToken)
{
    (void)oauthClient;

    const QJsonObject persistedQuery{
        {QStringLiteral("version"), 1},
        {QStringLiteral("sha256Hash"), QString::fromLatin1(operationHash)},
    };
    const QJsonObject payload{
        {QStringLiteral("operationName"), operationName},
        {QStringLiteral("variables"), variables},
        {QStringLiteral("extensions"),
         QJsonObject{{QStringLiteral("persistedQuery"), persistedQuery}}},
    };

    auto request =
        NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
            .timeout(TWITCH_WEB_GQL_TIMEOUT_MS)
            .header("Client-Id", TWITCH_WEB_GQL_CLIENT_ID)
            .header("Client-Session-Id", webGqlSessionId())
            .header("Client-Version", TWITCH_WEB_GQL_CLIENT_VERSION)
            .header("User-Agent", TWITCH_WEB_GQL_USER_AGENT)
            .header("X-Device-Id", webGqlDeviceId())
            .json(QJsonArray{payload});

    const auto token = stripTwitchOAuthPrefix(oauthToken);
    if (!token.isEmpty())
    {
        request = std::move(request).header("Authorization", "OAuth " + token);
    }

    return request;
}

NetworkRequest makeTwitchWebGqlDocumentRequest(const QString &operationName,
                                               const QString &query,
                                               const QJsonObject &variables,
                                               const QString &oauthClient,
                                               const QString &oauthToken,
                                               const QString &clientIntegrity =
                                                   QString(),
                                               const QString &deviceId =
                                                   QString())
{
    (void)oauthClient;

    const QJsonObject payload{
        {QStringLiteral("operationName"), operationName},
        {QStringLiteral("variables"), variables},
        {QStringLiteral("query"), query},
    };

    const auto requestDeviceId =
        deviceId.trimmed().isEmpty() ? webGqlDeviceId() : deviceId.trimmed();
    auto request =
        NetworkRequest("https://gql.twitch.tv/gql", NetworkRequestType::Post)
            .timeout(TWITCH_WEB_GQL_TIMEOUT_MS)
            .header("Client-Id", TWITCH_WEB_GQL_CLIENT_ID)
            .header("Client-Session-Id", webGqlSessionId())
            .header("Client-Version", TWITCH_WEB_GQL_CLIENT_VERSION)
            .header("User-Agent", TWITCH_WEB_GQL_USER_AGENT)
            .header("X-Device-Id", requestDeviceId)
            .json(QJsonArray{payload});

    const auto integrity = clientIntegrity.trimmed();
    if (!integrity.isEmpty())
    {
        request = std::move(request).header("Client-Integrity", integrity);
    }

    const auto token = stripTwitchOAuthPrefix(oauthToken);
    if (!token.isEmpty())
    {
        request = std::move(request).header("Authorization", "OAuth " + token);
    }

    return request;
}

bool isTwitchIntegrityError(const QString &message)
{
    const auto lower = message.toLower();
    return lower.contains(QStringLiteral("integrity")) ||
           lower == QStringLiteral("failed");
}

QString twitchBrowserHelperTokenMessage()
{
    return QStringLiteral(
        "Twitch rejected the browser helper token. Re-copy the Twitch mod "
        "actions helper token in Settings -> Accounts.");
}

void fetchTwitchWebIntegrity(
    const QString &oauthToken, const QString &deviceId,
    std::function<void(const QString &)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto token = stripTwitchOAuthPrefix(oauthToken);
    const auto requestDeviceId =
        deviceId.trimmed().isEmpty() ? webGqlDeviceId() : deviceId.trimmed();

    auto request =
        NetworkRequest("https://gql.twitch.tv/integrity", NetworkRequestType::Post)
            .timeout(TWITCH_WEB_GQL_TIMEOUT_MS)
            .header("Accept", "*/*")
            .header("Client-ID", TWITCH_WEB_GQL_CLIENT_ID)
            .header("Origin", "https://www.twitch.tv")
            .header("Referer", "https://www.twitch.tv/")
            .header("User-Agent", TWITCH_WEB_GQL_USER_AGENT)
            .header("X-Device-Id", requestDeviceId);

    if (!token.isEmpty())
    {
        request = std::move(request).header("Authorization", "OAuth " + token);
    }

    std::move(request)
        .onSuccess([successCallback = std::move(successCallback),
                    failureCallback](const NetworkResult &result) {
            const auto token =
                result.parseJson().value(QStringLiteral("token")).toString();
            if (token.trimmed().isEmpty())
            {
                failureCallback(QStringLiteral(
                    "Twitch did not return a browser integrity token."));
                return;
            }

            successCallback(token.trimmed());
        })
        .onError([failureCallback = std::move(failureCallback)](
                     const NetworkResult &result) {
            auto message =
                QStringLiteral("Network Error: ") + result.formatError();
            const auto body = QString::fromUtf8(result.getData()).trimmed();
            if (!body.isEmpty())
            {
                message += QStringLiteral(" - ") + body.left(200);
            }
            failureCallback(message);
        })
        .execute();
}

void rememberTwitchWebIntegrity(const QString &oauthToken,
                                const QString &deviceId,
                                const QString &clientIntegrity)
{
    const auto token = stripTwitchOAuthPrefix(oauthToken);
    const auto integrity = clientIntegrity.trimmed();
    if (token.isEmpty())
    {
        return;
    }

    auto account = TwitchModerationAuth::savedAccount();
    if (stripTwitchOAuthPrefix(account.oauthToken) != token)
    {
        return;
    }

    const auto originalIntegrity = account.clientIntegrity;
    const auto originalDeviceId = account.deviceId;
    account.clientIntegrity = integrity;
    if (!deviceId.trimmed().isEmpty())
    {
        account.deviceId = deviceId.trimmed();
    }
    else if (account.deviceId.trimmed().isEmpty())
    {
        account.deviceId = webGqlDeviceId();
    }

    if (account.clientIntegrity == originalIntegrity &&
        account.deviceId == originalDeviceId)
    {
        return;
    }

    TwitchModerationAuth::saveAccount(account);
}

QString twitchWebHelixClientID(const QString &oauthClient)
{
    const auto clientID = oauthClient.trimmed();
    if (!clientID.isEmpty())
    {
        return clientID;
    }

    return QString::fromLatin1(TWITCH_WEB_GQL_CLIENT_ID);
}

NetworkRequest makeTwitchWebHelixRequest(const QString &path,
                                         const QUrlQuery &query,
                                         NetworkRequestType type,
                                         const QString &oauthClient,
                                         const QString &oauthToken)
{
    QUrl url(QStringLiteral("https://api.twitch.tv/helix/%1").arg(path));
    url.setQuery(query);

    return NetworkRequest(url, type)
        .timeout(TWITCH_WEB_GQL_TIMEOUT_MS)
        .header("Accept", "application/json")
        .header("Client-ID", twitchWebHelixClientID(oauthClient))
        .header("Authorization",
                "Bearer " + stripTwitchOAuthPrefix(oauthToken))
        .header("User-Agent", TWITCH_WEB_GQL_USER_AGENT);
}

QString webGqlNetworkError(const NetworkResult &result)
{
    const auto body = QString::fromUtf8(result.getData()).trimmed();
    const auto lowerBody = body.toLower();
    if (lowerBody.contains(QStringLiteral("authorization")) &&
        lowerBody.contains(QStringLiteral("invalid")))
    {
        return QStringLiteral(
            "Twitch rejected the saved browser token. Reconnect Twitch mod "
            "actions in Settings -> Accounts.");
    }

    auto message = QStringLiteral("Network Error: ") + result.formatError();
    if (!body.isEmpty())
    {
        message += QStringLiteral(" - ") + body.left(200);
    }

    return message;
}

void executeTwitchWebGqlDocument(
    const QString &operationName, const QString &query,
    const QJsonObject &variables, const QString &oauthClient,
    const QString &oauthToken,
    std::function<void(const QJsonValue &)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    auto success = std::make_shared<std::function<void(const QJsonValue &)>>(
        std::move(successCallback));
    auto failure = std::make_shared<std::function<void(const QString &)>>(
        std::move(failureCallback));
    auto runRequest =
        std::make_shared<std::function<void(const QString &, bool)>>();
    const auto savedAccount = TwitchModerationAuth::savedAccount();
    const bool usesSavedAccount =
        stripTwitchOAuthPrefix(savedAccount.oauthToken) ==
        stripTwitchOAuthPrefix(oauthToken);
    const auto deviceId =
        usesSavedAccount && !savedAccount.deviceId.trimmed().isEmpty()
            ? savedAccount.deviceId.trimmed()
            : webGqlDeviceId();
    const auto initialIntegrity =
        usesSavedAccount ? savedAccount.clientIntegrity.trimmed() : QString();

    auto refreshIntegrityAndRetry = [=](const QString &originalError) {
        fetchTwitchWebIntegrity(
            oauthToken, deviceId,
            [=](const QString &freshIntegrity) {
                rememberTwitchWebIntegrity(oauthToken, deviceId,
                                           freshIntegrity);
                (*runRequest)(freshIntegrity, false);
            },
            [=](const QString &refreshError) {
                QString message =
                    QStringLiteral("Twitch API Error: ") + originalError;
                if (!refreshError.isEmpty())
                {
                    message += QStringLiteral(" Integrity refresh failed: ") +
                               refreshError;
                }
                *runRequest = {};
                (*failure)(message);
            });
    };

    *runRequest = [=](const QString &integrity, bool allowIntegrityRefresh) {
        makeTwitchWebGqlDocumentRequest(operationName, query, variables,
                                        oauthClient, oauthToken, integrity,
                                        deviceId)
            .onSuccess([=](const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                if (root.isUndefined() || root.isNull())
                {
                    *runRequest = {};
                    (*failure)(QStringLiteral("Failed to parse GQL response"));
                    return;
                }

                const auto gqlError = firstWebGqlError(root);
                if (!gqlError.isEmpty())
                {
                    if (allowIntegrityRefresh &&
                        isTwitchIntegrityError(gqlError))
                    {
                        refreshIntegrityAndRetry(gqlError);
                        return;
                    }

                    *runRequest = {};
                    (*failure)(QStringLiteral("Twitch API Error: ") + gqlError);
                    return;
                }

                *runRequest = {};
                (*success)(root);
            })
            .onError([=](const NetworkResult &result) {
                const auto error = webGqlNetworkError(result);
                if (allowIntegrityRefresh && isTwitchIntegrityError(error))
                {
                    refreshIntegrityAndRetry(error);
                    return;
                }

                *runRequest = {};
                (*failure)(error);
            })
            .execute();
    };

    if (!initialIntegrity.isEmpty())
    {
        (*runRequest)(initialIntegrity, true);
    }
    else
    {
        (*runRequest)(QString(), true);
    }
}

QString webHelixNetworkError(const NetworkResult &result)
{
    if (!result.status())
    {
        return result.formatError();
    }

    const auto obj = result.parseJson();
    const auto message = obj.value(QStringLiteral("message")).toString();
    if (!message.isEmpty())
    {
        return message;
    }

    const auto body = QString::fromUtf8(result.getData()).trimmed();
    if (!body.isEmpty())
    {
        return body.left(200);
    }

    return result.formatError();
}

void fetchTwitchWebHelixUserLogins(
    const QString &path, QUrlQuery urlQuery, const QString &oauthClient,
    const QString &oauthToken, int maxUsers,
    std::shared_ptr<QStringList> userLogins,
    std::function<void(const QStringList &)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    makeTwitchWebHelixRequest(path, urlQuery, NetworkRequestType::Get,
                              oauthClient, oauthToken)
        .onSuccess([=](const NetworkResult &result) mutable {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting Twitch web role users was"
                    << result.formatError() << "but we expected it to be 200";
            }

            const auto response = result.parseJson();
            for (const auto &userValue :
                 response.value(QStringLiteral("data")).toArray())
            {
                const auto login =
                    userValue.toObject()
                        .value(QStringLiteral("user_login"))
                        .toString()
                        .trimmed()
                        .toLower();
                if (!login.isEmpty())
                {
                    userLogins->append(login);
                }
            }

            const auto cursor = response.value(QStringLiteral("pagination"))
                                    .toObject()
                                    .value(QStringLiteral("cursor"))
                                    .toString();
            if (cursor.isEmpty() || userLogins->size() >= maxUsers)
            {
                successCallback(*userLogins);
                return;
            }

            urlQuery.removeAllQueryItems(QStringLiteral("after"));
            urlQuery.addQueryItem(QStringLiteral("after"), cursor);
            fetchTwitchWebHelixUserLogins(
                path, urlQuery, oauthClient, oauthToken, maxUsers, userLogins,
                std::move(successCallback), std::move(failureCallback));
        })
        .onError([failureCallback = std::move(failureCallback)](
                     const NetworkResult &result) {
            failureCallback(webHelixNetworkError(result));
        })
        .execute();
}

int gqlInt(const QJsonObject &object, const QString &key)
{
    const auto value = object.value(key);
    if (value.isDouble())
    {
        return value.toInt();
    }

    bool ok = false;
    const auto number = value.toString().toInt(&ok);
    return ok ? number : 0;
}

QString gqlStatus(const QJsonObject &object)
{
    return object.value(QStringLiteral("status")).toString().toUpper();
}

QJsonObject gqlPollToHelixPoll(const QJsonObject &poll)
{
    QJsonObject normalized;
    normalized.insert(QStringLiteral("id"),
                      poll.value(QStringLiteral("id")).toString());
    normalized.insert(QStringLiteral("title"),
                      poll.value(QStringLiteral("title")).toString());
    normalized.insert(QStringLiteral("status"), gqlStatus(poll));

    QJsonArray choices;
    for (const auto &choiceValue :
         poll.value(QStringLiteral("choices")).toArray())
    {
        const auto choice = choiceValue.toObject();
        const auto votes = choice.value(QStringLiteral("votes")).toObject();

        QJsonObject normalizedChoice;
        normalizedChoice.insert(QStringLiteral("id"),
                                choice.value(QStringLiteral("id")).toString());
        normalizedChoice.insert(
            QStringLiteral("title"),
            choice.value(QStringLiteral("title")).toString());
        normalizedChoice.insert(QStringLiteral("votes"),
                                gqlInt(votes, QStringLiteral("total")));
        choices.append(normalizedChoice);
    }
    normalized.insert(QStringLiteral("choices"), choices);

    return normalized;
}

QJsonObject gqlPredictionToHelixPrediction(const QJsonObject &prediction)
{
    QJsonObject normalized;
    normalized.insert(QStringLiteral("id"),
                      prediction.value(QStringLiteral("id")).toString());
    normalized.insert(QStringLiteral("title"),
                      prediction.value(QStringLiteral("title")).toString());
    normalized.insert(QStringLiteral("status"), gqlStatus(prediction));
    normalized.insert(
        QStringLiteral("prediction_window"),
        gqlInt(prediction, QStringLiteral("predictionWindowSeconds")));
    normalized.insert(QStringLiteral("created_at"),
                      prediction.value(QStringLiteral("createdAt")).toString());
    normalized.insert(QStringLiteral("locked_at"),
                      prediction.value(QStringLiteral("lockedAt")).toString());
    normalized.insert(QStringLiteral("ended_at"),
                      prediction.value(QStringLiteral("endedAt")).toString());

    const auto winningOutcome =
        prediction.value(QStringLiteral("winningOutcome")).toObject();
    normalized.insert(QStringLiteral("winning_outcome_id"),
                      winningOutcome.value(QStringLiteral("id")).toString());

    QJsonArray outcomes;
    for (const auto &outcomeValue :
         prediction.value(QStringLiteral("outcomes")).toArray())
    {
        const auto outcome = outcomeValue.toObject();

        QJsonObject normalizedOutcome;
        normalizedOutcome.insert(
            QStringLiteral("id"),
            outcome.value(QStringLiteral("id")).toString());
        normalizedOutcome.insert(
            QStringLiteral("title"),
            outcome.value(QStringLiteral("title")).toString());
        normalizedOutcome.insert(QStringLiteral("users"),
                                 gqlInt(outcome, QStringLiteral("totalUsers")));
        normalizedOutcome.insert(
            QStringLiteral("channel_points"),
            gqlInt(outcome, QStringLiteral("totalPoints")));
        outcomes.append(normalizedOutcome);
    }
    normalized.insert(QStringLiteral("outcomes"), outcomes);

    return normalized;
}

}  // namespace

bool TwitchModerationAuth::Account::isValid() const
{
    return !this->oauthToken.trimmed().isEmpty();
}

bool TwitchModerationAuth::Account::supportsWebGql() const
{
    const auto client = this->clientId.trimmed();
    return this->isValid() && !client.isEmpty() &&
           client != QString::fromLatin1(TWITCH_TV_CLIENT_ID);
}

bool TwitchModerationAuth::Account::supportsWebIntegrity() const
{
    return this->supportsWebGql() && !this->clientIntegrity.trimmed().isEmpty() &&
           !this->deviceId.trimmed().isEmpty();
}

QString TwitchModerationAuth::Account::displayLabel() const
{
    const auto display = this->displayName.trimmed();
    const auto loginText = this->login.trimmed();

    if (!display.isEmpty() && !loginText.isEmpty() &&
        display.compare(loginText, Qt::CaseInsensitive) != 0)
    {
        return QStringLiteral("%1 (@%2)").arg(display, loginText);
    }
    if (!display.isEmpty())
    {
        return display;
    }
    if (!loginText.isEmpty())
    {
        return QStringLiteral("@%1").arg(loginText);
    }

    return QStringLiteral("Saved Twitch account");
}

bool TwitchModerationAuth::DeviceCode::isValid() const
{
    return !this->deviceCode.trimmed().isEmpty() &&
           !this->userCode.trimmed().isEmpty() &&
           !this->verificationUri.trimmed().isEmpty();
}

QString TwitchModerationAuth::normalizeToken(QString token)
{
    token = token.trimmed();
    if (token.size() > TWITCH_AUTH_TOKEN_MAX_LENGTH)
    {
        return {};
    }

    for (int pass = 0; pass < 8 && !token.isEmpty(); ++pass)
    {
        const auto previous = token;

        if (token.size() >= 2 &&
            ((token.startsWith(QLatin1Char('"')) &&
              token.endsWith(QLatin1Char('"'))) ||
             (token.startsWith(QLatin1Char('\'')) &&
              token.endsWith(QLatin1Char('\'')))))
        {
            token = token.mid(1, token.size() - 2).trimmed();
        }

        for (const auto &prefix : {
                 QStringLiteral("Authorization:"),
                 QStringLiteral("OAuth "),
                 QStringLiteral("Bearer "),
                 QStringLiteral("oauth:"),
             })
        {
            if (token.startsWith(prefix, Qt::CaseInsensitive))
            {
                token = token.mid(prefix.size()).trimmed();
                if (token.size() > TWITCH_AUTH_TOKEN_MAX_LENGTH)
                {
                    return {};
                }
            }
        }

        if (token == previous)
        {
            break;
        }
    }

    return token;
}

TwitchModerationAuth::ClipboardPayload
    TwitchModerationAuth::parseClipboardPayload(const QString &text)
{
    ClipboardPayload payload;
    const auto trimmed = text.trimmed();
    const auto document = QJsonDocument::fromJson(trimmed.toUtf8());
    if (document.isObject())
    {
        const auto object = document.object();
        payload.oauthToken = TwitchModerationAuth::normalizeToken(
            object.value(QStringLiteral("token")).toString());
        payload.clientIntegrity =
            object.value(QStringLiteral("clientIntegrity")).toString().trimmed();
        payload.deviceId =
            object.value(QStringLiteral("deviceId")).toString().trimmed();
        return payload;
    }

    payload.oauthToken = TwitchModerationAuth::normalizeToken(trimmed);
    return payload;
}

QString TwitchModerationAuth::helperClipboardScript()
{
    return moderationAuthClipboardScript();
}

QString TwitchModerationAuth::clipboardText()
{
    return moderationAuthClipboardText();
}

void TwitchModerationAuth::copyHelperToClipboard()
{
    crossPlatformCopy(TwitchModerationAuth::helperClipboardScript());
}

int TwitchModerationAuth::maxTokenLength()
{
    return TWITCH_AUTH_TOKEN_MAX_LENGTH;
}

TwitchModerationAuth::Account TwitchModerationAuth::savedAccount()
{
    bool sidecarFound = false;
    const auto sidecar = readModerationAuthSidecarObject(&sidecarFound);
    if (sidecarFound)
    {
        return accountFromModerationAuthObject(sidecar);
    }

    return accountFromModerationAuthObject(readModerationAuthSettingsObject());
}

void TwitchModerationAuth::saveAccount(const Account &account)
{
    saveModerationAccount(account);
    TwitchModerationAuth::accountChanged().invoke();
}

void TwitchModerationAuth::clearSavedAccount()
{
    clearModerationAccount();
    TwitchModerationAuth::accountChanged().invoke();
}

pajlada::Signals::NoArgSignal &TwitchModerationAuth::accountChanged()
{
    return moderationAuthAccountChangedSignal();
}

TwitchModerationAuth::Account TwitchModerationAuth::resolveForCurrentUser(
    const QString &currentUserId, QString *errorMessage)
{
    auto account = savedAccount();
    if (!account.isValid())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QStringLiteral(
                "Log in under Settings -> Accounts -> Twitch mod actions first.");
        }
        return {};
    }

    const auto normalizedCurrentUserId = currentUserId.trimmed();
    if (!normalizedCurrentUserId.isEmpty() && !account.userId.isEmpty() &&
        account.userId != normalizedCurrentUserId)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QStringLiteral(
                "Twitch mod actions are logged in as a different Twitch "
                "account. Reconnect Twitch mod actions in Settings -> "
                "Accounts.");
        }
        return {};
    }
    if (!account.supportsWebGql())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QStringLiteral(
                "Update Settings -> Accounts -> Twitch mod actions with the "
                "browser helper token first.");
        }
        return {};
    }

    return account;
}

void TwitchModerationAuth::requestDeviceCode(
    std::function<void(DeviceCode)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("client_id"), TWITCH_TV_CLIENT_ID);
    body.addQueryItem(QStringLiteral("scopes"), TWITCH_TV_SCOPES);

    NetworkRequest(QUrl(QStringLiteral("https://id.twitch.tv/oauth2/device")),
                   NetworkRequestType::Post)
        .timeout(TWITCH_TV_AUTH_TIMEOUT_MS)
        .hideRequestBody()
        .followRedirects(true)
        .header("Client-Id", TWITCH_TV_CLIENT_ID)
        .header("Accept", "application/json")
        .header("Content-Type", "application/x-www-form-urlencoded")
        .header("Origin", TWITCH_TV_ORIGIN)
        .header("Referer", TWITCH_TV_REFERER)
        .header("User-Agent", TWITCH_TV_USER_AGENT)
        .header("X-Device-Id", webGqlDeviceId())
        .payload(body.toString(QUrl::FullyEncoded).toUtf8())
        .onSuccess([successCallback = std::move(successCallback),
                    failureCallback](const NetworkResult &result) {
            const auto json = result.parseJson();

            DeviceCode code;
            code.deviceCode =
                json.value(QStringLiteral("device_code")).toString().trimmed();
            code.userCode =
                json.value(QStringLiteral("user_code")).toString().trimmed();
            code.verificationUri =
                json.value(QStringLiteral("verification_uri_complete"))
                    .toString()
                    .trimmed();
            if (code.verificationUri.isEmpty())
            {
                code.verificationUri =
                    json.value(QStringLiteral("verification_uri"))
                        .toString()
                        .trimmed();
            }
            const auto intervalSeconds =
                json.value(QStringLiteral("interval")).toInt(5);
            code.intervalMs = std::max(1, intervalSeconds) * 1000;

            if (!code.isValid())
            {
                failureCallback(QStringLiteral(
                    "Twitch did not return a complete activation code."));
                return;
            }

            successCallback(std::move(code));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(QStringLiteral("Device login setup failed: ") +
                            authResponseText(result));
        })
        .execute();
}

void TwitchModerationAuth::pollDeviceToken(
    const QString &deviceCode, std::function<void(DeviceTokenResult)> callback)
{
    QUrlQuery body;
    body.addQueryItem(QStringLiteral("client_id"), TWITCH_TV_CLIENT_ID);
    body.addQueryItem(QStringLiteral("device_code"), deviceCode);
    body.addQueryItem(QStringLiteral("grant_type"),
                      QStringLiteral(
                          "urn:ietf:params:oauth:grant-type:device_code"));

    auto handleResult = [callback = std::move(callback)](
                            const NetworkResult &result) mutable {
        const auto json = result.parseJson();
        const auto accessToken =
            json.value(QStringLiteral("access_token")).toString().trimmed();
        if (!accessToken.isEmpty())
        {
            callback({DeviceTokenStatus::Authorized, accessToken, {}});
            return;
        }

        const auto error =
            json.value(QStringLiteral("error")).toString().trimmed();
        const auto message =
            json.value(QStringLiteral("message")).toString().trimmed();

        if (error == QStringLiteral("authorization_pending"))
        {
            callback({DeviceTokenStatus::Pending, {}, {}});
            return;
        }
        if (error == QStringLiteral("slow_down"))
        {
            callback({DeviceTokenStatus::SlowDown, {}, {}});
            return;
        }

        auto errorText = message;
        if (errorText.isEmpty())
        {
            errorText = error;
        }
        if (errorText.isEmpty())
        {
            errorText = authResponseText(result);
        }
        callback({DeviceTokenStatus::Failed, {}, errorText});
    };

    NetworkRequest(QUrl(QStringLiteral("https://id.twitch.tv/oauth2/token")),
                   NetworkRequestType::Post)
        .timeout(TWITCH_TV_AUTH_TIMEOUT_MS)
        .hideRequestBody()
        .followRedirects(true)
        .header("Client-Id", TWITCH_TV_CLIENT_ID)
        .header("Accept", "application/json")
        .header("Content-Type", "application/x-www-form-urlencoded")
        .header("Origin", TWITCH_TV_ORIGIN)
        .header("Referer", TWITCH_TV_REFERER)
        .header("User-Agent", TWITCH_TV_USER_AGENT)
        .header("X-Device-Id", webGqlDeviceId())
        .payload(body.toString(QUrl::FullyEncoded).toUtf8())
        .onSuccess(handleResult)
        .onError(handleResult)
        .execute();
}

void TwitchModerationAuth::validateToken(
    const QString &oauthToken, std::function<void(Account)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto token = normalizeToken(oauthToken);
    if (token.isEmpty())
    {
        failureCallback(QStringLiteral("No token was provided."));
        return;
    }

    NetworkRequest(QUrl(QStringLiteral("https://id.twitch.tv/oauth2/validate")),
                   NetworkRequestType::Get)
        .timeout(TWITCH_TV_AUTH_TIMEOUT_MS)
        .hideRequestBody()
        .followRedirects(true)
        .header("Accept", "application/json")
        .header("Authorization", "OAuth " + token)
        .onSuccess([token, successCallback = std::move(successCallback),
                    failureCallback](const NetworkResult &result) {
            const auto json = result.parseJson();

            Account account;
            account.clientId =
                json.value(QStringLiteral("client_id")).toString().trimmed();
            if (account.clientId.isEmpty())
            {
                account.clientId = QString(TWITCH_TV_CLIENT_ID);
            }
            account.oauthToken = token;
            account.clientIntegrity = {};
            account.deviceId = {};
            account.userId =
                json.value(QStringLiteral("user_id")).toString().trimmed();
            account.login =
                json.value(QStringLiteral("login")).toString().trimmed().toLower();
            account.displayName = account.login;

            if (account.userId.isEmpty() || account.login.isEmpty())
            {
                failureCallback(QStringLiteral(
                    "Twitch validated the token without account details."));
                return;
            }

            successCallback(std::move(account));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(QStringLiteral("Token validation failed: ") +
                            authResponseText(result));
        })
        .execute();
}

void TwitchWebApi::getChatterGroups(
    const QString &broadcasterName, const QString &oauthClient,
    const QString &oauthToken, const QString &clientIntegrity,
    const QString &deviceId,
    std::function<void(const HelixChatterGroups &)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto channelName = broadcasterName.trimmed().toLower();
    if (channelName.isEmpty())
    {
        failureCallback(QStringLiteral("Missing channel name."));
        return;
    }

    QJsonObject variables;
    variables.insert(QStringLiteral("login"), channelName);

    const auto query = QStringLiteral(
        "query MergerinoChatterGroups($login: String!) {"
        "  channel(name: $login) {"
        "    chatters {"
        "      count"
        "      broadcasters { login }"
        "      moderators { login }"
        "      vips { login }"
        "      staff { login }"
        "      viewers { login }"
        "    }"
        "  }"
        "}");

    auto success = std::make_shared<std::function<void(const HelixChatterGroups &)>>(
        std::move(successCallback));
    auto failure = std::make_shared<std::function<void(const QString &)>>(
        std::move(failureCallback));
    auto runRequest =
        std::make_shared<std::function<void(const QString &, bool, bool)>>();

    auto refreshAndRetry = [=](const QString &originalError) {
        fetchTwitchWebIntegrity(
            oauthToken, deviceId,
            [=](const QString &freshIntegrity) {
                rememberTwitchWebIntegrity(oauthToken, deviceId,
                                           freshIntegrity);
                (*runRequest)(freshIntegrity, false, true);
            },
            [=](const QString &refreshError) {
                if (isTwitchIntegrityError(originalError))
                {
                    QString message = twitchBrowserHelperTokenMessage();
                    if (!refreshError.isEmpty())
                    {
                        message += QStringLiteral(" Integrity refresh failed: ") +
                                   refreshError;
                    }
                    (*failure)(message);
                    return;
                }

                (*failure)(QStringLiteral("Twitch API Error: ") + originalError);
            });
    };

    *runRequest = [=](const QString &integrity, bool allowIntegrityRefresh,
                      bool allowNoIntegrityRetry) {
        makeTwitchWebGqlDocumentRequest(
            QStringLiteral("MergerinoChatterGroups"), query, variables,
            oauthClient, oauthToken, integrity, deviceId)
            .onSuccess([=](const NetworkResult &result) {
                const auto root = result.parseJsonValue();
                if (root.isUndefined() || root.isNull())
                {
                    (*failure)(QStringLiteral("Failed to parse GQL response"));
                    return;
                }

                const auto gqlError = firstWebGqlError(root);
                if (!gqlError.isEmpty())
                {
                    if (allowNoIntegrityRetry && !integrity.trimmed().isEmpty() &&
                        isTwitchIntegrityError(gqlError))
                    {
                        (*runRequest)(QString(), allowIntegrityRefresh, false);
                        return;
                    }
                    if (allowIntegrityRefresh &&
                        isTwitchIntegrityError(gqlError))
                    {
                        refreshAndRetry(gqlError);
                        return;
                    }
                    if (isTwitchIntegrityError(gqlError))
                    {
                        (*failure)(twitchBrowserHelperTokenMessage());
                        return;
                    }

                    (*failure)(QStringLiteral("Twitch API Error: ") + gqlError);
                    return;
                }

                const auto chatters = webGqlData(root)
                                          .value(QStringLiteral("channel"))
                                          .toObject()
                                          .value(QStringLiteral("chatters"))
                                          .toObject();
                if (chatters.isEmpty())
                {
                    (*failure)(QStringLiteral(
                        "Twitch returned an empty chatter role response."));
                    return;
                }

                rememberTwitchWebIntegrity(oauthToken, deviceId, integrity);

                QJsonObject response;
                response.insert(QStringLiteral("chatters"), chatters);
                response.insert(QStringLiteral("chatter_count"),
                                chatters.value(QStringLiteral("count")).toInt());
                (*success)(HelixChatterGroups(response));
            })
            .onError([=](const NetworkResult &result) {
                const auto error = webGqlNetworkError(result);
                if (allowNoIntegrityRetry && !integrity.trimmed().isEmpty() &&
                    isTwitchIntegrityError(error))
                {
                    (*runRequest)(QString(), allowIntegrityRefresh, false);
                    return;
                }
                if (allowIntegrityRefresh && isTwitchIntegrityError(error))
                {
                    refreshAndRetry(error);
                    return;
                }
                if (isTwitchIntegrityError(error))
                {
                    (*failure)(twitchBrowserHelperTokenMessage());
                    return;
                }

                (*failure)(error);
            })
            .execute();
    };

    const auto initialIntegrity = clientIntegrity.trimmed();
    if (!initialIntegrity.isEmpty())
    {
        (*runRequest)(initialIntegrity, true, true);
        return;
    }

    (*runRequest)(QString(), true, false);
}

void TwitchWebApi::getResubNotification(
    const QString &channelLogin, const QString &oauthClient,
    const QString &oauthToken,
    std::function<void(std::optional<TwitchResubNotification>)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto login = channelLogin.trimmed().toLower();
    if (login.isEmpty())
    {
        failureCallback(QStringLiteral("Missing channel name."));
        return;
    }
    if (stripTwitchOAuthPrefix(oauthToken).isEmpty())
    {
        failureCallback(QStringLiteral("No Twitch auth token available."));
        return;
    }

    QJsonObject variables;
    variables.insert(QStringLiteral("channelLogin"), login);

    const auto query = QStringLiteral(
        "query Chat_ShareResub_ChannelData($channelLogin: String!) {"
        " user(login: $channelLogin) {"
        "  id"
        "  self {"
        "   resubNotification {"
        "    id cumulativeTenureMonths months streakTenureMonths token"
        "    isGiftSubscription"
        "    gifter { id login displayName }"
        "   }"
        "  }"
        " }"
        "}");

    executeTwitchWebGqlDocument(
        QStringLiteral("Chat_ShareResub_ChannelData"), query, variables,
        oauthClient, oauthToken,
        [successCallback =
             std::move(successCallback)](const QJsonValue &root) mutable {
            const auto value = webGqlData(root)
                                   .value(QStringLiteral("user"))
                                   .toObject()
                                   .value(QStringLiteral("self"))
                                   .toObject()
                                   .value(QStringLiteral("resubNotification"));
            if (!value.isObject())
            {
                successCallback(std::nullopt);
                return;
            }

            const auto object = value.toObject();
            TwitchResubNotification notification;
            notification.id = object.value(QStringLiteral("id")).toString();
            notification.cumulativeTenureMonths =
                object.value(QStringLiteral("cumulativeTenureMonths")).toInt();
            notification.months =
                object.value(QStringLiteral("months")).toInt();
            notification.streakTenureMonths =
                object.value(QStringLiteral("streakTenureMonths")).toInt();
            notification.token =
                object.value(QStringLiteral("token")).toString();
            notification.isGiftSubscription =
                object.value(QStringLiteral("isGiftSubscription")).toBool();
            notification.gifterDisplayName =
                object.value(QStringLiteral("gifter"))
                    .toObject()
                    .value(QStringLiteral("displayName"))
                    .toString();

            if (notification.id.isEmpty())
            {
                successCallback(std::nullopt);
                return;
            }
            successCallback(std::move(notification));
        },
        std::move(failureCallback));
}

void TwitchWebApi::shareResubNotification(
    const QString &channelLogin, const QString &tokenID, const QString &message,
    bool includeStreak, const QString &oauthClient, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    const auto login = channelLogin.trimmed().toLower();
    if (login.isEmpty() || tokenID.trimmed().isEmpty())
    {
        failureCallback(QStringLiteral("Missing subscription share details."));
        return;
    }
    if (stripTwitchOAuthPrefix(oauthToken).isEmpty())
    {
        failureCallback(QStringLiteral("No Twitch auth token available."));
        return;
    }
    if (message.size() > 255)
    {
        failureCallback(QStringLiteral(
            "Subscription message must be 255 characters or less."));
        return;
    }

    QJsonObject input;
    input.insert(QStringLiteral("message"), message);
    input.insert(QStringLiteral("channelLogin"), login);
    input.insert(QStringLiteral("includeStreak"), includeStreak);
    input.insert(QStringLiteral("tokenID"), tokenID);
    QJsonObject variables;
    variables.insert(QStringLiteral("input"), input);

    const auto query =
        QStringLiteral("mutation Chat_ShareResub_UseResubToken("
                       " $input: UseChatNotificationTokenInput!) {"
                       " useChatNotificationToken(input: $input) { isSuccess }"
                       "}");

    executeTwitchWebGqlDocument(
        QStringLiteral("Chat_ShareResub_UseResubToken"), query, variables,
        oauthClient, oauthToken,
        [successCallback = std::move(successCallback),
         failureCallback](const QJsonValue &root) mutable {
            const auto success =
                webGqlData(root)
                    .value(QStringLiteral("useChatNotificationToken"))
                    .toObject()
                    .value(QStringLiteral("isSuccess"))
                    .toBool();
            if (!success)
            {
                failureCallback(QStringLiteral(
                    "Twitch did not accept the subscription share."));
                return;
            }
            successCallback();
        },
        failureCallback);
}

void TwitchWebApi::getChannelModerationPermissions(
    const QString &channelId, const QString &userId,
    const QString &oauthClient, const QString &oauthToken,
    std::function<void(const TwitchChannelModerationPermissions &)>
        successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (channelId.trimmed().isEmpty() || userId.trimmed().isEmpty())
    {
        failureCallback(
            QStringLiteral("Missing channel or moderator account ID."));
        return;
    }
    if (stripTwitchOAuthPrefix(oauthToken).isEmpty())
    {
        failureCallback(QStringLiteral("No Twitch mod auth token saved."));
        return;
    }

    const auto query = QStringLiteral(
        "query MergerinoChannelModerationPermissions("
        "$channelID: ID!, $userID: ID!) {"
        " channel(id: $channelID) {"
        "  id"
        "  addModerator: hasPermission(userID: $userID,"
        "   permission: \"moderation.roles.mod:add\")"
        "  removeModerator: hasPermission(userID: $userID,"
        "   permission: \"moderation.roles.mod:remove\")"
        "  addVip: hasPermission(userID: $userID,"
        "   permission: \"moderation.roles.vip:add\")"
        "  removeVip: hasPermission(userID: $userID,"
        "   permission: \"moderation.roles.vip:remove\")"
        "  deleteModComments: hasPermission(userID: $userID,"
        "   permission: \"moderation.vmcl.mod_comments:delete\")"
        " }"
        "}");

    QJsonObject variables;
    variables.insert(QStringLiteral("channelID"), channelId);
    variables.insert(QStringLiteral("userID"), userId);

    executeTwitchWebGqlDocument(
        QStringLiteral("MergerinoChannelModerationPermissions"), query,
        variables, oauthClient, oauthToken,
        [successCallback = std::move(successCallback),
         failureCallback](const QJsonValue &root) mutable {
            const auto channel =
                webGqlData(root).value(QStringLiteral("channel")).toObject();
            if (channel.isEmpty())
            {
                failureCallback(QStringLiteral(
                    "Twitch did not return channel moderation permissions."));
                return;
            }

            TwitchChannelModerationPermissions permissions;
            permissions.addModerator =
                channel.value(QStringLiteral("addModerator")).toBool();
            permissions.removeModerator =
                channel.value(QStringLiteral("removeModerator")).toBool();
            permissions.addVip =
                channel.value(QStringLiteral("addVip")).toBool();
            permissions.removeVip =
                channel.value(QStringLiteral("removeVip")).toBool();
            permissions.deleteModComments =
                channel.value(QStringLiteral("deleteModComments")).toBool();
            successCallback(permissions);
        },
        failureCallback);
}

void TwitchWebApi::updateChannelRole(
    TwitchChannelRoleAction action, const QString &channelId,
    const QString &targetLogin, const QString &oauthClient,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (channelId.trimmed().isEmpty() || targetLogin.trimmed().isEmpty())
    {
        failureCallback(QStringLiteral("Missing channel or target user."));
        return;
    }
    if (stripTwitchOAuthPrefix(oauthToken).isEmpty())
    {
        failureCallback(QStringLiteral("No Twitch mod auth token saved."));
        return;
    }

    QString operationName;
    QString inputType;
    QString mutationField;
    QString targetKey;
    switch (action)
    {
        case TwitchChannelRoleAction::AddModerator:
            operationName = QStringLiteral("ModUser");
            inputType = QStringLiteral("ModUserInput");
            mutationField = QStringLiteral("modUser");
            targetKey = QStringLiteral("targetLogin");
            break;
        case TwitchChannelRoleAction::RemoveModerator:
            operationName = QStringLiteral("UnmodUser");
            inputType = QStringLiteral("UnmodUserInput");
            mutationField = QStringLiteral("unmodUser");
            targetKey = QStringLiteral("targetLogin");
            break;
        case TwitchChannelRoleAction::AddVip:
            operationName = QStringLiteral("VIPUser");
            inputType = QStringLiteral("GrantVIPInput");
            mutationField = QStringLiteral("grantVIP");
            targetKey = QStringLiteral("granteeLogin");
            break;
        case TwitchChannelRoleAction::RemoveVip:
            operationName = QStringLiteral("UnVIPUser");
            inputType = QStringLiteral("RevokeVIPInput");
            mutationField = QStringLiteral("revokeVIP");
            targetKey = QStringLiteral("revokeeLogin");
            break;
    }

    QJsonObject input;
    input.insert(QStringLiteral("channelID"), channelId);
    input.insert(targetKey, targetLogin.trimmed().toLower());
    QJsonObject variables;
    variables.insert(QStringLiteral("input"), input);

    const auto query =
        QStringLiteral("mutation %1($input: %2!) {"
                       " %3(input: $input) { error { code } }"
                       "}")
            .arg(operationName, inputType, mutationField);

    executeTwitchWebGqlDocument(
        operationName, query, variables, oauthClient, oauthToken,
        [mutationField, successCallback = std::move(successCallback),
         failureCallback](const QJsonValue &root) mutable {
            const auto payload =
                webGqlData(root).value(mutationField).toObject();
            if (payload.isEmpty())
            {
                failureCallback(
                    QStringLiteral("Twitch returned an empty role response."));
                return;
            }

            const auto error =
                payload.value(QStringLiteral("error")).toObject();
            const auto code = error.value(QStringLiteral("code"))
                                  .toString()
                                  .trimmed();
            if (!code.isEmpty())
            {
                failureCallback(QStringLiteral("Twitch API Error: ") + code);
                return;
            }
            successCallback();
        },
        failureCallback);
}

void TwitchWebApi::getModComments(
    const QString &channelId, const QString &targetId,
    const QString &oauthClient, const QString &oauthToken,
    std::function<void(const QVector<TwitchModComment> &)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (channelId.trimmed().isEmpty() || targetId.trimmed().isEmpty())
    {
        failureCallback(QStringLiteral("Missing channel or target user ID."));
        return;
    }
    if (stripTwitchOAuthPrefix(oauthToken).isEmpty())
    {
        failureCallback(QStringLiteral("No Twitch mod auth token saved."));
        return;
    }

    const auto query = QStringLiteral(
        "query ViewerCardModLogsComments("
        "$channelID: ID!, $targetID: ID!, $cursor: Cursor) {"
        " viewerCardModLogs(channelID: $channelID, targetID: $targetID) {"
        "  comments(first: 100, after: $cursor) {"
        "   ... on ModLogsCommentConnection {"
        "    edges {"
        "     cursor"
        "     node {"
        "      id timestamp text isShareable"
        "      channel { id login }"
        "      author { id login displayName }"
        "     }"
        "    }"
        "    pageInfo { hasNextPage }"
        "   }"
        "   ... on ModLogsCommentsError { code }"
        "   __typename"
        "  }"
        " }"
        "}");

    auto comments = std::make_shared<QVector<TwitchModComment>>();
    auto fetchPage =
        std::make_shared<std::function<void(const QString &, int)>>();
    *fetchPage = [=](const QString &cursor, int page) {
        QJsonObject variables;
        variables.insert(QStringLiteral("channelID"), channelId);
        variables.insert(QStringLiteral("targetID"), targetId);
        if (!cursor.isEmpty())
        {
            variables.insert(QStringLiteral("cursor"), cursor);
        }

        executeTwitchWebGqlDocument(
            QStringLiteral("ViewerCardModLogsComments"), query, variables,
            oauthClient, oauthToken,
            [=](const QJsonValue &root) {
                const auto commentsPayload =
                    webGqlData(root)
                        .value(QStringLiteral("viewerCardModLogs"))
                        .toObject()
                        .value(QStringLiteral("comments"))
                        .toObject();
                const auto typeName =
                    commentsPayload.value(QStringLiteral("__typename"))
                        .toString();
                if (typeName != QStringLiteral("ModLogsCommentConnection"))
                {
                    auto error =
                        commentsPayload.value(QStringLiteral("code"))
                            .toString()
                            .trimmed();
                    if (error.isEmpty())
                    {
                        error = QStringLiteral(
                            "Twitch did not return moderator comments.");
                    }
                    *fetchPage = {};
                    failureCallback(error);
                    return;
                }

                QString nextCursor;
                const auto edges =
                    commentsPayload.value(QStringLiteral("edges")).toArray();
                for (const auto &edgeValue : edges)
                {
                    const auto edge = edgeValue.toObject();
                    nextCursor =
                        edge.value(QStringLiteral("cursor")).toString();
                    const auto node =
                        edge.value(QStringLiteral("node")).toObject();
                    if (node.isEmpty())
                    {
                        continue;
                    }

                    const auto channel =
                        node.value(QStringLiteral("channel")).toObject();
                    const auto author =
                        node.value(QStringLiteral("author")).toObject();
                    TwitchModComment comment;
                    comment.id =
                        node.value(QStringLiteral("id")).toString();
                    comment.timestamp =
                        node.value(QStringLiteral("timestamp")).toString();
                    comment.text =
                        node.value(QStringLiteral("text")).toString();
                    comment.isShareable =
                        node.value(QStringLiteral("isShareable")).toBool();
                    comment.channelId =
                        channel.value(QStringLiteral("id")).toString();
                    comment.channelLogin =
                        channel.value(QStringLiteral("login")).toString();
                    comment.authorId =
                        author.value(QStringLiteral("id")).toString();
                    comment.authorLogin =
                        author.value(QStringLiteral("login")).toString();
                    comment.authorDisplayName =
                        author.value(QStringLiteral("displayName")).toString();
                    if (!comment.id.isEmpty())
                    {
                        comments->append(std::move(comment));
                    }
                }

                const bool hasNextPage =
                    commentsPayload.value(QStringLiteral("pageInfo"))
                        .toObject()
                        .value(QStringLiteral("hasNextPage"))
                        .toBool();
                if (hasNextPage && !nextCursor.isEmpty() && page < 19)
                {
                    (*fetchPage)(nextCursor, page + 1);
                    return;
                }

                *fetchPage = {};
                successCallback(*comments);
            },
            [=](const QString &error) {
                *fetchPage = {};
                failureCallback(error);
            });
    };

    (*fetchPage)(QString(), 0);
}

void TwitchWebApi::deleteModComment(
    const QString &channelId, const QString &commentId,
    const QString &oauthClient, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (channelId.trimmed().isEmpty() || commentId.trimmed().isEmpty())
    {
        failureCallback(QStringLiteral("Missing channel or comment ID."));
        return;
    }
    if (stripTwitchOAuthPrefix(oauthToken).isEmpty())
    {
        failureCallback(QStringLiteral("No Twitch mod auth token saved."));
        return;
    }

    QJsonObject input;
    input.insert(QStringLiteral("ID"), commentId);
    input.insert(QStringLiteral("channelID"), channelId);
    QJsonObject variables;
    variables.insert(QStringLiteral("input"), input);

    const auto query = QStringLiteral(
        "mutation deleteModeratorComment("
        "$input: DeleteModeratorCommentInput!) {"
        " deleteModeratorComment(input: $input) { comment { id } }"
        "}");

    executeTwitchWebGqlDocument(
        QStringLiteral("deleteModeratorComment"), query, variables,
        oauthClient, oauthToken,
        [commentId, successCallback = std::move(successCallback),
         failureCallback](const QJsonValue &root) mutable {
            const auto deletedId =
                webGqlData(root)
                    .value(QStringLiteral("deleteModeratorComment"))
                    .toObject()
                    .value(QStringLiteral("comment"))
                    .toObject()
                    .value(QStringLiteral("id"))
                    .toString();
            if (deletedId != commentId)
            {
                failureCallback(QStringLiteral(
                    "Twitch did not confirm the moderator comment deletion."));
                return;
            }
            successCallback();
        },
        failureCallback);
}

void TwitchWebApi::getModeratorLogins(
    const QString &broadcasterId, const QString &oauthClient,
    const QString &oauthToken,
    std::function<void(const QStringList &)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (stripTwitchOAuthPrefix(oauthToken).isEmpty())
    {
        failureCallback(QStringLiteral("No Twitch mod auth token saved."));
        return;
    }

    QUrlQuery urlQuery;
    urlQuery.addQueryItem(QStringLiteral("broadcaster_id"), broadcasterId);
    urlQuery.addQueryItem(QStringLiteral("first"), QStringLiteral("100"));

    fetchTwitchWebHelixUserLogins(
        QStringLiteral("moderation/moderators"), urlQuery, oauthClient,
        oauthToken, 500, std::make_shared<QStringList>(),
        std::move(successCallback), std::move(failureCallback));
}

void TwitchWebApi::getVipLogins(
    const QString &broadcasterId, const QString &oauthClient,
    const QString &oauthToken,
    std::function<void(const QStringList &)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (stripTwitchOAuthPrefix(oauthToken).isEmpty())
    {
        failureCallback(QStringLiteral("No Twitch mod auth token saved."));
        return;
    }

    QUrlQuery urlQuery;
    urlQuery.addQueryItem(QStringLiteral("broadcaster_id"), broadcasterId);
    urlQuery.addQueryItem(QStringLiteral("first"), QStringLiteral("100"));

    fetchTwitchWebHelixUserLogins(
        QStringLiteral("channels/vips"), urlQuery, oauthClient, oauthToken,
        500, std::make_shared<QStringList>(), std::move(successCallback),
        std::move(failureCallback));
}

void TwitchWebApi::startPoll(
    const QString &channelId, const QString &title, const QStringList &choices,
    int durationSeconds, std::optional<int> pointsPerVote,
    const QString &oauthClient, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert(QStringLiteral("title"), title);
    input.insert(QStringLiteral("durationSeconds"), durationSeconds);
    input.insert(QStringLiteral("ownedBy"), channelId);
    input.insert(QStringLiteral("multichoiceEnabled"), true);
    input.insert(QStringLiteral("isCommunityPointsVotingEnabled"),
                 pointsPerVote.has_value());
    input.insert(QStringLiteral("communityPointsCost"),
                 pointsPerVote.value_or(0));

    QJsonArray choicesArray;
    for (const auto &choiceTitle : choices)
    {
        QJsonObject choice;
        choice.insert(QStringLiteral("title"), choiceTitle);
        choicesArray.append(choice);
    }
    input.insert(QStringLiteral("choices"), choicesArray);
    variables.insert(QStringLiteral("input"), input);

    makeTwitchWebGqlRequest(QStringLiteral("CreatePoll"), CREATE_POLL_HASH,
                            variables, oauthClient, oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback(QStringLiteral("Failed to parse GQL response"));
                return;
            }

            const auto gqlError = firstWebGqlError(root);
            if (!gqlError.isEmpty())
            {
                failureCallback(QStringLiteral("Twitch API Error: ") + gqlError);
                return;
            }

            const auto payload =
                webGqlData(root)
                    .value(QStringLiteral("createPoll"))
                    .toObject();
            const auto payloadError = webGqlMutationErrorText(
                payload,
                QStringLiteral("Failed to create poll"));
            if (!payloadError.isEmpty())
            {
                failureCallback(QStringLiteral("Twitch API Error: ") +
                                payloadError);
                return;
            }

            const auto pollId = payload.value(QStringLiteral("poll"))
                                    .toObject()
                                    .value(QStringLiteral("id"))
                                    .toString();
            if (pollId.isEmpty())
            {
                failureCallback(QStringLiteral(
                    "Twitch API Error: Failed to create poll"));
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(webGqlNetworkError(result));
        })
        .execute();
}

void TwitchWebApi::getPolls(
    const QString &channelId, QStringList ids, int first, const QString &after,
    const QString &oauthClient, const QString &oauthToken,
    std::function<void(const HelixPolls &)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (stripTwitchOAuthPrefix(oauthToken).isEmpty())
    {
        failureCallback(QStringLiteral("No Twitch mod auth token saved."));
        return;
    }

    QUrlQuery urlQuery;
    urlQuery.addQueryItem(QStringLiteral("broadcaster_id"), channelId);
    urlQuery.addQueryItem(QStringLiteral("first"),
                          QString::number(std::max(first, 1)));

    if (!after.isEmpty())
    {
        urlQuery.addQueryItem(QStringLiteral("after"), after);
    }

    for (const auto &id : ids)
    {
        urlQuery.addQueryItem(QStringLiteral("id"), id);
    }

    makeTwitchWebHelixRequest(QStringLiteral("polls"), urlQuery,
                              NetworkRequestType::Get, oauthClient,
                              oauthToken)
        .onSuccess([successCallback](const NetworkResult &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting polls with web auth was"
                    << result.formatError() << "but we expected it to be 200";
            }

            successCallback(HelixPolls(result.parseJson()));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(webHelixNetworkError(result));
        })
        .execute();
}

void TwitchWebApi::endPoll(
    const QString &channelId, const QString &pollId,
    const QString &oauthClient, const QString &oauthToken,
    std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (stripTwitchOAuthPrefix(oauthToken).isEmpty())
    {
        failureCallback(QStringLiteral("No Twitch mod auth token saved."));
        return;
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("broadcaster_id"), channelId);
    payload.insert(QStringLiteral("id"), pollId);
    payload.insert(QStringLiteral("status"), QStringLiteral("TERMINATED"));

    makeTwitchWebHelixRequest(QStringLiteral("polls"), {},
                              NetworkRequestType::Patch, oauthClient,
                              oauthToken)
        .json(payload)
        .onSuccess([successCallback](const NetworkResult &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for ending a poll with web auth was"
                    << result.formatError() << "but we expected it to be 200";
            }
            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(webHelixNetworkError(result));
        })
        .execute();
}

void TwitchWebApi::startPrediction(
    const QString &channelId, const QString &title,
    const QStringList &outcomes, int predictionWindowSeconds,
    const QString &oauthClient, const QString &oauthToken,
    std::function<void(const QString &, const QJsonObject &)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    QJsonObject variables;
    QJsonObject input;
    input.insert(QStringLiteral("channelID"), channelId);
    input.insert(QStringLiteral("title"), title);
    input.insert(QStringLiteral("predictionWindowSeconds"),
                 predictionWindowSeconds);

    QJsonArray outcomesArray;
    const int outcomeCount = outcomes.size();
    for (int i = 0; i < outcomeCount; ++i)
    {
        QJsonObject outcome;
        outcome.insert(QStringLiteral("title"), outcomes.at(i));
        outcome.insert(QStringLiteral("color"),
                       predictionOutcomeColor(i, outcomeCount));
        outcomesArray.append(outcome);
    }
    input.insert(QStringLiteral("outcomes"), outcomesArray);
    variables.insert(QStringLiteral("input"), input);

    makeTwitchWebGqlRequest(QStringLiteral("createPredictionEvent"),
                            CREATE_PREDICTION_HASH, variables, oauthClient,
                            oauthToken)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback(QStringLiteral("Failed to parse GQL response"));
                return;
            }

            const auto gqlError = firstWebGqlError(root);
            if (!gqlError.isEmpty())
            {
                failureCallback(QStringLiteral("Twitch API Error: ") + gqlError);
                return;
            }

            const auto payload =
                webGqlData(root)
                    .value(QStringLiteral("createPredictionEvent"))
                    .toObject();
            const auto payloadError = webGqlMutationErrorText(
                payload,
                QStringLiteral("Failed to create prediction"));
            if (!payloadError.isEmpty())
            {
                failureCallback(QStringLiteral("Twitch API Error: ") +
                                payloadError);
                return;
            }

            const auto predictionObject =
                payload.value(QStringLiteral("predictionEvent")).toObject();
            const auto predictionId =
                predictionObject.value(QStringLiteral("id")).toString();
            if (predictionId.isEmpty())
            {
                failureCallback(QStringLiteral(
                    "Twitch API Error: Failed to create prediction"));
                return;
            }

            successCallback(predictionId, predictionObject);
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(webGqlNetworkError(result));
        })
        .execute();
}

void TwitchWebApi::getPredictions(
    const QString &channelId, QStringList ids, int first, const QString &after,
    const QString &oauthClient, const QString &oauthToken,
    std::function<void(const HelixPredictions &)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (stripTwitchOAuthPrefix(oauthToken).isEmpty())
    {
        failureCallback(QStringLiteral("No Twitch mod auth token saved."));
        return;
    }

    QUrlQuery urlQuery;
    urlQuery.addQueryItem(QStringLiteral("broadcaster_id"), channelId);
    urlQuery.addQueryItem(QStringLiteral("first"),
                          QString::number(std::max(first, 1)));

    if (!after.isEmpty())
    {
        urlQuery.addQueryItem(QStringLiteral("after"), after);
    }

    for (const auto &id : ids)
    {
        urlQuery.addQueryItem(QStringLiteral("id"), id);
    }

    makeTwitchWebHelixRequest(QStringLiteral("predictions"), urlQuery,
                              NetworkRequestType::Get, oauthClient,
                              oauthToken)
        .onSuccess([successCallback](const NetworkResult &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for getting predictions with web auth was"
                    << result.formatError() << "but we expected it to be 200";
            }

            successCallback(HelixPredictions(result.parseJson()));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(webHelixNetworkError(result));
        })
        .execute();
}

void TwitchWebApi::getActivePollAndPredictions(
    const QString &channelId, const QString &oauthClient,
    const QString &oauthToken,
    std::function<void(const HelixPolls &, const HelixPredictions &)>
        successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (stripTwitchOAuthPrefix(oauthToken).isEmpty())
    {
        failureCallback(QStringLiteral("No Twitch mod auth token saved."));
        return;
    }

    QJsonObject variables;
    variables.insert(QStringLiteral("channelId"), channelId);

    const auto query = QStringLiteral(
        "query MergerinoActivePollPrediction($channelId: ID!) {"
        "  channel(id: $channelId) {"
        "    activePredictionEvents {"
        "      createdAt endedAt id lockedAt predictionWindowSeconds status title"
        "      winningOutcome { id }"
        "      outcomes { id title totalPoints totalUsers }"
        "    }"
        "    lockedPredictionEvents {"
        "      createdAt endedAt id lockedAt predictionWindowSeconds status title"
        "      winningOutcome { id }"
        "      outcomes { id title totalPoints totalUsers }"
        "    }"
        "  }"
        "  user(id: $channelId) {"
        "    latestPoll {"
        "      id status title durationSeconds endedAt startedAt"
        "      choices { id title votes { total } }"
        "    }"
        "  }"
        "}");

    makeTwitchWebGqlDocumentRequest(
        QStringLiteral("MergerinoActivePollPrediction"), query, variables,
        oauthClient, oauthToken)
        .onSuccess([successCallback = std::move(successCallback),
                    failureCallback](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback(QStringLiteral("Failed to parse GQL response"));
                return;
            }

            const auto data = webGqlData(root);
            const auto gqlError = firstWebGqlError(root);
            if (!gqlError.isEmpty())
            {
                if (data.isEmpty())
                {
                    failureCallback(QStringLiteral("Twitch API Error: ") +
                                    gqlError);
                    return;
                }

                qCWarning(chatterinoTwitch)
                    << "Partial Twitch web active poll/prediction response:"
                    << gqlError;
            }

            QJsonArray pollData;
            const auto poll = data.value(QStringLiteral("user"))
                                  .toObject()
                                  .value(QStringLiteral("latestPoll"));
            if (poll.isObject())
            {
                auto normalizedPoll = gqlPollToHelixPoll(poll.toObject());
                if (normalizedPoll.value(QStringLiteral("status")).toString() ==
                    QStringLiteral("ACTIVE"))
                {
                    pollData.append(normalizedPoll);
                }
            }

            QJsonArray predictionData;
            const auto channel = data.value(QStringLiteral("channel")).toObject();
            auto appendPredictions = [&predictionData](const QJsonArray &events) {
                for (const auto &eventValue : events)
                {
                    if (eventValue.isObject())
                    {
                        predictionData.append(
                            gqlPredictionToHelixPrediction(
                                eventValue.toObject()));
                    }
                }
            };
            appendPredictions(
                channel.value(QStringLiteral("activePredictionEvents"))
                    .toArray());
            appendPredictions(
                channel.value(QStringLiteral("lockedPredictionEvents"))
                    .toArray());

            QJsonObject pollsRoot;
            pollsRoot.insert(QStringLiteral("data"), pollData);
            QJsonObject predictionsRoot;
            predictionsRoot.insert(QStringLiteral("data"), predictionData);

            successCallback(HelixPolls(pollsRoot),
                            HelixPredictions(predictionsRoot));
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(webGqlNetworkError(result));
        })
        .execute();
}

void TwitchWebApi::endPrediction(
    const QString &channelId, const QString &predictionId, bool refundPoints,
    const QString &winningOutcomeId, const QString &oauthClient,
    const QString &oauthToken,
    std::function<void(const HelixPrediction &)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (stripTwitchOAuthPrefix(oauthToken).isEmpty())
    {
        failureCallback(QStringLiteral("No Twitch mod auth token saved."));
        return;
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("broadcaster_id"), channelId);
    payload.insert(QStringLiteral("id"), predictionId);
    if (refundPoints)
    {
        payload.insert(QStringLiteral("status"), QStringLiteral("CANCELED"));
    }
    else if (winningOutcomeId.isEmpty())
    {
        payload.insert(QStringLiteral("status"), QStringLiteral("LOCKED"));
    }
    else
    {
        payload.insert(QStringLiteral("status"), QStringLiteral("RESOLVED"));
        payload.insert(QStringLiteral("winning_outcome_id"), winningOutcomeId);
    }

    makeTwitchWebHelixRequest(QStringLiteral("predictions"), {},
                              NetworkRequestType::Patch, oauthClient,
                              oauthToken)
        .json(payload)
        .onSuccess([successCallback, failureCallback](
                       const NetworkResult &result) {
            if (result.status() != 200)
            {
                qCWarning(chatterinoTwitch)
                    << "Success result for ending a prediction with web auth was"
                    << result.formatError() << "but we expected it to be 200";
            }

            const auto data = HelixPredictions(result.parseJson());
            if (data.predictions.empty())
            {
                failureCallback(QStringLiteral(
                    "Twitch API Error: Prediction update returned no data"));
                return;
            }

            successCallback(data.predictions.front());
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(webHelixNetworkError(result));
        })
        .execute();
}

void TwitchWebApi::endPredictionEvent(
    const QString &predictionId, bool refundPoints,
    const QString &winningOutcomeId, const QString &oauthClient,
    const QString &oauthToken, std::function<void()> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (stripTwitchOAuthPrefix(oauthToken).isEmpty())
    {
        failureCallback(QStringLiteral("No Twitch mod auth token saved."));
        return;
    }
    if (predictionId.trimmed().isEmpty())
    {
        failureCallback(QStringLiteral("Missing Twitch prediction ID."));
        return;
    }

    QString operationName;
    QString mutationField;
    QString inputType;
    QString failureText;
    QJsonObject input;

    if (refundPoints)
    {
        operationName = QStringLiteral("CancelPredictionEvent");
        mutationField = QStringLiteral("cancelPredictionEvent");
        inputType = QStringLiteral("CancelPredictionEventInput!");
        failureText = QStringLiteral("Failed to delete prediction");
        input.insert(QStringLiteral("id"), predictionId);
    }
    else if (winningOutcomeId.trimmed().isEmpty())
    {
        operationName = QStringLiteral("LockPredictionEvent");
        mutationField = QStringLiteral("lockPredictionEvent");
        inputType = QStringLiteral("LockPredictionEventInput!");
        failureText = QStringLiteral("Failed to end submissions");
        input.insert(QStringLiteral("id"), predictionId);
    }
    else
    {
        operationName = QStringLiteral("ResolvePredictionEvent");
        mutationField = QStringLiteral("resolvePredictionEvent");
        inputType = QStringLiteral("ResolvePredictionEventInput!");
        failureText = QStringLiteral("Failed to choose outcome");
        input.insert(QStringLiteral("eventID"), predictionId);
        input.insert(QStringLiteral("outcomeID"), winningOutcomeId.trimmed());
    }

    QJsonObject variables;
    variables.insert(QStringLiteral("input"), input);

    const auto query =
        QStringLiteral(
            "mutation %1($input: %2) { %3(input: $input) { error { "
            "__typename } } }")
            .arg(operationName, inputType, mutationField);

    makeTwitchWebGqlDocumentRequest(operationName, query, variables,
                                    oauthClient, oauthToken)
        .onSuccess([successCallback = std::move(successCallback),
                    failureCallback, mutationField,
                    failureText](const NetworkResult &result) {
            const auto root = result.parseJsonValue();
            if (root.isUndefined() || root.isNull())
            {
                failureCallback(QStringLiteral("Failed to parse GQL response"));
                return;
            }

            const auto gqlError = firstWebGqlError(root);
            if (!gqlError.isEmpty())
            {
                failureCallback(QStringLiteral("Twitch API Error: ") +
                                gqlError);
                return;
            }

            const auto payload =
                webGqlData(root).value(mutationField).toObject();
            const auto payloadError =
                webGqlMutationErrorText(payload, failureText);
            if (!payloadError.isEmpty())
            {
                failureCallback(QStringLiteral("Twitch API Error: ") +
                                payloadError);
                return;
            }

            successCallback();
        })
        .onError([failureCallback](const NetworkResult &result) {
            failureCallback(webGqlNetworkError(result));
        })
        .execute();
}

void Helix::update(QString clientId, QString oauthToken)
{
    this->clientId = std::move(clientId);
    this->oauthToken = std::move(oauthToken);
}

void Helix::initialize()
{
    assert(instance == nullptr);

    initializeHelix(new Helix());
}

void initializeHelix(IHelix *_instance)
{
    assert(_instance != nullptr);

    instance = _instance;
}

IHelix *getHelix()
{
    assert(instance != nullptr);

    return instance;
}

}  // namespace chatterino
