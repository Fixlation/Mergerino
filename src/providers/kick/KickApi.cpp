#include "providers/kick/KickApi.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "util/BoostJsonWrap.hpp"

#include <boost/json/parse.hpp>
#include <QJsonDocument>
#include <QList>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace {

using namespace chatterino;
using namespace Qt::Literals;

std::vector<std::pair<QByteArray, QByteArray>> kickNoAuthHeaders()
{
    return {
        {"User-Agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
         "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"},
        {"Accept", "application/json, text/plain, */*"},
        {"Accept-Language", "en-US,en;q=0.9"},
        {"Origin", "https://kick.com"},
        {"Referer", "https://kick.com/"},
    };
}

struct KickBrowserSession {
    QString sessionToken;
    QString xsrfToken;
};

KickBrowserSession parseKickBrowserSession(const QString &credential)
{
    QJsonParseError error;
    const auto document =
        QJsonDocument::fromJson(credential.trimmed().toUtf8(), &error);
    if (error.error == QJsonParseError::NoError && document.isObject())
    {
        const auto object = document.object();
        return {
            .sessionToken =
                object.value(QStringLiteral("sessionToken")).toString().trimmed(),
            .xsrfToken =
                object.value(QStringLiteral("xsrfToken")).toString().trimmed(),
        };
    }

    return {
        .sessionToken = credential.trimmed(),
        .xsrfToken = {},
    };
}

std::vector<std::pair<QByteArray, QByteArray>> kickBrowserSessionHeaders(
    const QString &credential, QString channelSlug = {})
{
    const auto session = parseKickBrowserSession(credential);
    auto referer = QByteArray{"https://kick.com/"};
    channelSlug = channelSlug.trimmed();
    if (!channelSlug.isEmpty())
    {
        referer += channelSlug.toUtf8();
    }

    auto cookie = "session_token=" + session.sessionToken.toUtf8();
    if (!session.xsrfToken.isEmpty())
    {
        cookie += "; XSRF-TOKEN=" +
                  QUrl::toPercentEncoding(session.xsrfToken);
    }

    std::vector<std::pair<QByteArray, QByteArray>> headers{
        {"User-Agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
         "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"},
        {"Accept", "application/json"},
        {"Accept-Language", "en-US,en;q=0.9"},
        {"Origin", "https://kick.com"},
        {"Referer", referer},
        {"x-app-platform", "web"},
        {"Authorization", "Bearer " + session.sessionToken.toUtf8()},
        {"Cookie", cookie},
    };
    if (!session.xsrfToken.isEmpty())
    {
        headers.emplace_back("X-XSRF-TOKEN", session.xsrfToken.toUtf8());
    }
    return headers;
}

template <typename T>
struct IsCollectionS : std::false_type {
};
template <typename T, typename Alloc>
struct IsCollectionS<std::vector<T, Alloc>> : std::true_type {
};
template <typename T>
struct IsCollectionS<QList<T>> : std::true_type {
};

template <typename T>
concept IsCollection = IsCollectionS<T>::value;

template <typename T>
void callDeserialize(auto &&cb, BoostJsonValue data)
{
    if (data.isObject())
    {
        cb(T(data.toObject()));
    }
    else if (data.isArray())
    {
        auto arr = data.toArray();
        if (arr.empty())
        {
            cb(makeUnexpected(u"Not found (no item returned)"_s));
            return;
        }
        if (!arr[0].isObject())
        {
            cb(makeUnexpected(u"'data[0]' is not an object"_s));
            return;
        }
        cb(T(arr[0].toObject()));
    }
    else
    {
        cb(makeUnexpected(u"'data' is not an object"_s));
    }
}

template <std::same_as<void> T>
void callDeserialize(auto &&cb, BoostJsonValue /* data */)
{
    cb(ExpectedStr<void>{});
}

template <IsCollection T>
void callDeserialize(auto &&cb, BoostJsonValue data)
{
    if (!data.isArray())
    {
        cb(makeUnexpected(u"'data' is not an array"_s));
        return;
    }
    auto arr = data.toArray();
    T coll;
    coll.reserve(arr.size());

    for (auto val : arr)
    {
        if (!val.isObject())
        {
            cb(makeUnexpected(u"Array element was not an object"_s));
            return;
        }
        coll.emplace_back(typename T::value_type(val.toObject()));
    }

    cb(std::move(coll));
}

template <typename T>
void getJsonNoAuth(const QString &url, std::function<void(ExpectedStr<T>)> cb,
                   bool unwrapData = false)
{
    NetworkRequest(url)
        .headerList(kickNoAuthHeaders())
        .onError([cb](const NetworkResult &res) {
            cb(makeUnexpected(res.formatError()));
        })
        .onSuccess([cb = std::move(cb), unwrapData](
                       const NetworkResult &res) {
            const auto &ba = res.getData();
            boost::system::error_code ec;
            auto jv =
                boost::json::parse(std::string_view(ba.data(), ba.size()), ec);
            if (ec)
            {
                qCWarning(chatterinoKick)
                    << "Failed to parse API response:" << ec.message();
                cb(makeUnexpected(u"Failed to parse API response: "_s %
                                  QString::fromStdString(ec.message())));
                return;
            }

            BoostJsonValue ref(jv);
            if (unwrapData)
            {
                if (!ref.isObject())
                {
                    cb(makeUnexpected(u"Root value was not an object"_s));
                    return;
                }
                callDeserialize<T>(cb, ref["data"]);
                return;
            }
            callDeserialize<T>(cb, ref);
        })
        .execute();
}

QString makePublicV1Url(QStringView endpoint)
{
    return u"https://api.kick.com/public/v1/" % endpoint;
}

QString kickIdentityUrl(uint64_t channelID, uint64_t userID)
{
    return u"https://kick.com/api/v2/channels/" %
           QString::number(channelID) % u"/users/" %
           QString::number(userID) % u"/identity";
}

QString kickIdentityError(const NetworkResult &result)
{
    const auto root = result.parseJson();
    auto message = root.value(QStringLiteral("message")).toString();
    if (message.isEmpty())
    {
        message = root.value(QStringLiteral("status"))
                      .toObject()
                      .value(QStringLiteral("message"))
                      .toString();
    }
    if (result.status() == 419 ||
        message.contains(QStringLiteral("csrf"), Qt::CaseInsensitive))
    {
        return QStringLiteral(
            "Kick website session security data expired. Reconnect it under "
            "Settings > Accounts.");
    }
    if (result.status() == 401)
    {
        return QStringLiteral(
            "Kick website session authorization expired. Reconnect it under "
            "Settings > Accounts.");
    }
    if (result.status() == 403)
    {
        return message.isEmpty()
                   ? QStringLiteral(
                         "Kick denied this action for the connected account.")
                   : message;
    }
    return message.isEmpty() ? result.formatError() : message;
}

QString kickPredictionManagementError(const NetworkResult &result)
{
    if (result.status() == 403)
    {
        return QStringLiteral(
            "Kick does not allow moderators who voted in this prediction to "
            "manage or delete it.");
    }

    return kickIdentityError(result);
}

uint64_t kickWebsiteUserID(const QJsonObject &root)
{
    const auto readID = [](const QJsonObject &object) {
        for (const auto &key :
             {QStringLiteral("id"), QStringLiteral("user_id")})
        {
            bool ok = false;
            const auto id = object.value(key).toVariant().toULongLong(&ok);
            if (ok && id != 0)
            {
                return id;
            }
        }
        return uint64_t{0};
    };

    const auto data = root.value(QStringLiteral("data")).toObject();
    for (const auto &candidate :
         {root, data, root.value(QStringLiteral("user")).toObject(),
          data.value(QStringLiteral("user")).toObject()})
    {
        if (const auto id = readID(candidate); id != 0)
        {
            return id;
        }
    }
    return 0;
}

QString kickIdentityBadgeImageUrl(const QJsonObject &badge)
{
    const auto fromValue = [](const QJsonValue &value) {
        if (value.isString())
        {
            return value.toString().trimmed();
        }
        const auto object = value.toObject();
        for (const auto &key :
             {QStringLiteral("url"), QStringLiteral("src"),
              QStringLiteral("image_url")})
        {
            const auto candidate = object.value(key).toString().trimmed();
            if (!candidate.isEmpty())
            {
                return candidate;
            }
        }
        return QString{};
    };

    for (const auto &key :
         {QStringLiteral("image_url"), QStringLiteral("image"),
          QStringLiteral("badge_image"), QStringLiteral("src")})
    {
        const auto candidate = fromValue(badge.value(key));
        if (!candidate.isEmpty())
        {
            return candidate;
        }
    }

    const auto metadata = badge.value(QStringLiteral("metadata")).toObject();
    for (const auto &key :
         {QStringLiteral("image_url"), QStringLiteral("image"),
          QStringLiteral("badge_image"), QStringLiteral("src")})
    {
        const auto candidate = fromValue(metadata.value(key));
        if (!candidate.isEmpty())
        {
            return candidate;
        }
    }
    return {};
}

std::optional<KickChatIdentity> parseKickChatIdentity(const QJsonObject &root)
{
    auto identity = root.value(QStringLiteral("data"))
                        .toObject()
                        .value(QStringLiteral("identity"))
                        .toObject();
    if (identity.isEmpty())
    {
        identity = root.value(QStringLiteral("identity")).toObject();
    }
    if (identity.isEmpty())
    {
        return std::nullopt;
    }

    KickChatIdentity result;
    result.color = identity.value(QStringLiteral("color")).toString();
    const QStringList globalLegacy{
        QStringLiteral("trainwreckstv"), QStringLiteral("staff"),
        QStringLiteral("sidekick"), QStringLiteral("verified"),
        QStringLiteral("bot")};

    for (const auto &value :
         identity.value(QStringLiteral("badges")).toArray())
    {
        const auto object = value.toObject();
        KickChatIdentityBadge badge{
            .name = object.value(QStringLiteral("type"))
                        .toString(object.value(QStringLiteral("name"))
                                      .toString()),
            .title = object.value(QStringLiteral("text"))
                         .toString(object.value(QStringLiteral("label"))
                                       .toString()),
            .badgeType = {},
            .imageUrl = kickIdentityBadgeImageUrl(object),
            .count = object.value(QStringLiteral("count"))
                         .toVariant()
                         .toULongLong(),
            .level = object.value(QStringLiteral("metadata"))
                         .toObject()
                         .value(QStringLiteral("level"))
                         .toVariant()
                         .toULongLong(),
            .sortOrder =
                object.value(QStringLiteral("sort_order"))
                    .toVariant()
                    .toULongLong(),
            .selected =
                object.value(QStringLiteral("active")).toBool(true),
            .legacy = true,
        };
        badge.badgeType =
            globalLegacy.contains(badge.name, Qt::CaseInsensitive)
                ? QStringLiteral("global")
                : QStringLiteral("channel");
        if (!badge.name.isEmpty())
        {
            result.badges.push_back(std::move(badge));
        }
    }

    for (const auto &value :
         identity.value(QStringLiteral("badges_v2")).toArray())
    {
        const auto object = value.toObject();
        KickChatIdentityBadge badge{
            .name = object.value(QStringLiteral("name"))
                        .toString(object.value(QStringLiteral("type"))
                                      .toString()),
            .title = object.value(QStringLiteral("text"))
                         .toString(object.value(QStringLiteral("label"))
                                       .toString()),
            .badgeType =
                object.value(QStringLiteral("badge_type")).toString(),
            .imageUrl = kickIdentityBadgeImageUrl(object),
            .count = object.value(QStringLiteral("count"))
                         .toVariant()
                         .toULongLong(),
            .level = object.value(QStringLiteral("metadata"))
                         .toObject()
                         .value(QStringLiteral("level"))
                         .toVariant()
                         .toULongLong(),
            .sortOrder =
                object.value(QStringLiteral("sort_order"))
                    .toVariant()
                    .toULongLong(),
            .selected =
                object.value(QStringLiteral("selected")).toBool(false),
            .legacy = false,
        };
        if (!badge.name.isEmpty())
        {
            result.badges.push_back(std::move(badge));
        }
    }

    std::stable_sort(result.badges.begin(), result.badges.end(),
                     [](const auto &left, const auto &right) {
                         return left.sortOrder < right.sortOrder;
                     });
    return result;
}

}  // namespace

namespace chatterino {

QString kickIdentityAuthHelper()
{
    return QStringLiteral(
        "(() => {"
        "const readCookie = name => {"
        "const entry = document.cookie.split(';').map(item => item.trim()).find("
        "item => item.startsWith(name + '='));"
        "return entry ? decodeURIComponent(entry.slice(name.length + 1)) : '';"
        "};"
        "const sessionToken = readCookie('session_token');"
        "if (!sessionToken) { console.error('Mergerino: no signed-in Kick session "
        "was found.'); return; }"
        "const xsrfToken = readCookie('XSRF-TOKEN');"
        "if (!xsrfToken) { console.error('Mergerino: Kick security data was "
        "not found. Refresh kick.com and run the helper again.'); return; }"
        "copy(JSON.stringify({sessionToken, xsrfToken}));"
        "console.log('Mergerino Kick token copied. Return to Mergerino and "
        "click Paste Token.');"
        "})()");
}

QString parseKickIdentityToken(const QString &clipboardText)
{
    QJsonParseError error;
    const auto document =
        QJsonDocument::fromJson(clipboardText.toUtf8(), &error);
    if (error.error == QJsonParseError::NoError && document.isObject())
    {
        const auto object = document.object();
        const auto sessionToken =
            object.value(QStringLiteral("sessionToken")).toString().trimmed();
        const auto xsrfToken =
            object.value(QStringLiteral("xsrfToken")).toString().trimmed();
        if (sessionToken.isEmpty())
        {
            return {};
        }
        if (xsrfToken.isEmpty())
        {
            return sessionToken;
        }
        return QString::fromUtf8(
            QJsonDocument(QJsonObject{
                              {QStringLiteral("sessionToken"), sessionToken},
                              {QStringLiteral("xsrfToken"), xsrfToken},
                          })
                .toJson(QJsonDocument::Compact));
    }

    if (!clipboardText.startsWith(QLatin1Char('{')) &&
        !clipboardText.contains(QStringLiteral("document.cookie")))
    {
        return clipboardText.trimmed();
    }
    return {};
}

KickPrivateUserInfo::KickPrivateUserInfo(BoostJsonObject obj)
    : userID(obj["id"].toUint64())
    , username(obj["username"].toQString())
{
    auto pictureUrl = obj["profile_pic"];
    if (pictureUrl.isString())
    {
        this->profilePictureURL = pictureUrl.toQString();
    }
}

KickPrivateChatroomInfo::KickPrivateChatroomInfo(BoostJsonObject obj)
    : roomID(obj["id"].toUint64())
    , createdAt(QDateTime::fromString(obj["created_at"].toQString(),
                                      Qt::ISODateWithMs))
    , subscribersMode(obj["subscribers_mode"].toBool())
    , emotesMode(obj["emotes_mode"].toBool())
{
    bool slowMode = obj["slow_mode"].toBool();
    if (slowMode)
    {
        this->slowModeDuration =
            std::chrono::seconds{obj["message_interval"].toInt64()};
    }
    bool followersMode = obj["followers_mode"].toBool();
    if (followersMode)
    {
        this->followersModeDuration =
            std::chrono::minutes{obj["following_min_duration"].toInt64()};
    }
}

KickPrivateChatSettings::KickPrivateChatSettings(BoostJsonObject obj)
    : subscribersMode(obj["subscribers_only_mode"]["enabled"].toBool())
    , emotesMode(obj["emotes_only_mode"]["enabled"].toBool())
{
    auto slowMode = obj["slow_mode"].toObject();
    if (slowMode["enabled"].toBool())
    {
        this->slowModeDuration =
            std::chrono::seconds{slowMode["duration_seconds"].toInt64()};
    }

    auto followersMode = obj["followers_only_mode"].toObject();
    if (followersMode["enabled"].toBool())
    {
        this->followersModeDuration =
            std::chrono::duration_cast<std::chrono::minutes>(
                std::chrono::seconds{
                    followersMode["duration_seconds"].toInt64()});
    }
}

KickPrivateSubscriberBadgeInfo::KickPrivateSubscriberBadgeInfo(
    BoostJsonObject obj)
    : months(obj["months"].toUint64())
{
    auto badgeImage = obj["badge_image"].toObject();
    this->imageUrl = badgeImage["src"].toQString();
}

KickPrivateUserBadgeInfo::KickPrivateUserBadgeInfo(BoostJsonObject obj)
{
    this->type = obj["type"].toQString(obj["name"].toQString());
    if (this->type.isEmpty())
    {
        this->type = obj["badge_type"].toQString();
    }
    this->text = obj["text"].toQString(obj["label"].toQString());
    this->imageUrl = obj["image_url"].toQString(obj["src"].toQString());
    this->count = obj["count"].toUint64();
    this->sortOrder = obj["sort_order"].toUint64(1000);
    this->active = obj["active"].toBool(true);
    this->selected = obj["selected"].toBool(true);

    auto metadata = obj["metadata"].toObject();
    this->level = metadata["level"].toUint64(obj["level"].toUint64());
}

KickPrivateChannelInfo::KickPrivateChannelInfo(BoostJsonObject obj)
    : channelID(obj["id"].toUint64())
    , followersCount(obj["followers_count"].toUint64())
    , slug(obj["slug"].toQString())
    , user(obj["user"].toObject())
    , chatroom(obj["chatroom"].toObject())
{
    auto badgeArray = obj["subscriber_badges"].toArray();
    this->subscriberBadges.reserve(badgeArray.size());
    for (auto badgeValue : badgeArray)
    {
        if (badgeValue.isObject())
        {
            this->subscriberBadges.emplace_back(badgeValue.toObject());
        }
    }

    auto livestream = obj["livestream"];
    if (!livestream.isObject())
    {
        return;
    }

    auto livestreamObj = livestream.toObject();
    this->isLive = livestreamObj["is_live"].toBool();
    this->streamTitle = livestreamObj["session_title"].toQString();
    this->viewerCount = livestreamObj["viewer_count"].toUint64();
    this->startTime = QDateTime::fromString(
        livestreamObj["start_time"].toQString(), Qt::ISODate);

    auto thumbnail = livestreamObj["thumbnail"];
    if (thumbnail.isObject())
    {
        this->thumbnailUrl = thumbnail.toObject()["url"].toQString();
    }

    auto categories = livestreamObj["categories"].toArray();
    if (!categories.empty())
    {
        this->liveCategoryName =
            categories[0].toObject()["name"].toQString();
    }
}

KickPrivateUserInChannelInfo::KickPrivateUserInChannelInfo(BoostJsonObject obj)
    : userID(obj["id"].toUint64())
    , username(obj["username"].toQString())
    , isModerator(obj["is_moderator"].toBool())
    , isChannelOwner(obj["is_channel_owner"].toBool())

{
    auto badgesV2 = obj["badges_v2"].toArray();
    this->badges.reserve(badgesV2.size() + obj["badges"].toArray().size());
    for (auto badgeValue : badgesV2)
    {
        if (badgeValue.isObject())
        {
            this->badges.emplace_back(badgeValue.toObject());
        }
    }

    auto badges = obj["badges"].toArray();
    for (auto badgeValue : badges)
    {
        if (badgeValue.isObject())
        {
            this->badges.emplace_back(badgeValue.toObject());
        }
    }

    std::stable_sort(this->badges.begin(), this->badges.end(),
                     [](const auto &lhs, const auto &rhs) {
                         return lhs.sortOrder < rhs.sortOrder;
                     });

    auto followingSinceStr = obj["following_since"].toQString();
    if (!followingSinceStr.isEmpty())
    {
        this->followingSince =
            QDateTime::fromString(followingSinceStr, Qt::ISODateWithMs);
    }

    auto months = obj["subscribed_for"].toUint64();
    if (months > 0 && months < std::numeric_limits<uint16_t>::max())
    {
        this->subscriptionMonths = static_cast<uint16_t>(months);
    }

    auto pictureUrl = obj["profile_pic"];
    if (pictureUrl.isString())
    {
        this->profilePictureURL = pictureUrl.toQString();
    }
}

KickCategoryInfo::KickCategoryInfo(BoostJsonObject obj)
    : name(obj["name"].toQString())
{
}

KickStreamInfo::KickStreamInfo(BoostJsonObject obj)
    : isLive(obj["is_live"].toBool())
    , viewerCount(obj["viewer_count"].toUint64())
    , startTime(
          QDateTime::fromString(obj["start_time"].toQString(), Qt::ISODate))
    , thumbnailUrl(obj["thumbnail"].toQString())
{
}

KickChannelInfo::KickChannelInfo(BoostJsonObject obj)
    : userID(obj["broadcaster_user_id"].toUint64())
    , category(obj["category"].toObject())
    , stream(obj["stream"].toObject())
    , streamTitle(obj["stream_title"].toQString())
{
}

KickPrivateEmoteInfo::KickPrivateEmoteInfo(BoostJsonObject obj)
    : emoteID(obj["id"].toUint64())
    , name(obj["name"].toQString())
    , subscribersOnly(obj["subscribers_only"].toBool())
{
}

KickPrivateEmoteSetInfo::KickPrivateEmoteSetInfo(BoostJsonObject obj)
{
    auto userIDVal = obj["user_id"];
    if (userIDVal.isString())
    {
        bool ok = false;
        auto userID = userIDVal.toQString().toULongLong(&ok);
        if (ok)
        {
            this->userID = userID;
        }
    }
    else if (userIDVal.isInt64())
    {
        this->userID = userIDVal.toUint64();
    }
    auto emotesArr = obj["emotes"].toArray();
    this->emotes.reserve(emotesArr.size());
    for (auto emoteVal : emotesArr)
    {
        this->emotes.emplace_back(emoteVal.toObject());
    }
}

KickApi *KickApi::instance()
{
    static std::unique_ptr<KickApi> api;
    if (!api)
    {
        api = std::unique_ptr<KickApi>{new KickApi};
    }
    return api.get();
}

QString KickApi::slugify(const QString &usernameOrSlug)
{
    auto slugified = usernameOrSlug;
    slugified.replace('_', '-');
    return slugified;
}

void KickApi::privateChannelInfo(const QString &username,
                                 Callback<KickPrivateChannelInfo> cb)
{
    getJsonNoAuth<KickPrivateChannelInfo>(
        u"https://kick.com/api/v2/channels/" % slugify(username),
        std::move(cb));
}

void KickApi::privateLatestPrediction(const QString &username,
                                      Callback<QJsonObject> cb)
{
    const auto slug = slugify(username).trimmed();
    if (slug.isEmpty())
    {
        cb(makeUnexpected(u"Missing channel slug"_s));
        return;
    }

    const QString url = u"https://kick.com/api/v2/channels/" % slug %
                        u"/predictions/latest";
    NetworkRequest(url)
        .headerList(kickNoAuthHeaders())
        .onError([cb](const NetworkResult &res) {
            cb(makeUnexpected(res.formatError()));
        })
        .onSuccess([cb = std::move(cb)](const NetworkResult &res) {
            const auto root = res.parseJson();
            const auto data = root.value(QStringLiteral("data"));
            if (!data.isObject())
            {
                cb(makeUnexpected(u"'data' is not an object"_s));
                return;
            }

            const auto prediction =
                data.toObject().value(QStringLiteral("prediction"));
            if (prediction.isNull() || prediction.isUndefined())
            {
                cb(QJsonObject{});
                return;
            }
            if (!prediction.isObject())
            {
                cb(makeUnexpected(u"'data.prediction' is not an object"_s));
                return;
            }

            cb(prediction.toObject());
        })
        .execute();
}

void KickApi::privateChatSettings(
    uint64_t channelID, Callback<KickPrivateChatSettings> cb)
{
    if (channelID == 0)
    {
        cb(makeUnexpected(u"Missing channel ID"_s));
        return;
    }

    getJsonNoAuth<KickPrivateChatSettings>(
        u"https://web.kick.com/api/v1/channels/" %
            QString::number(channelID) % u"/chat/settings",
        std::move(cb), true);
}

void KickApi::privateUserInChannelInfo(
    const QString &userUsername, const QString &channelUsername,
    Callback<KickPrivateUserInChannelInfo> cb)
{
    getJsonNoAuth<KickPrivateUserInChannelInfo>(
        u"https://kick.com/api/v2/channels/" % slugify(channelUsername) %
            "/users/" % slugify(userUsername),
        std::move(cb));
}

void KickApi::privateEmotesInChannel(
    const QString &username, Callback<std::vector<KickPrivateEmoteSetInfo>> cb)
{
    getJsonNoAuth(u"https://kick.com/emotes/" % slugify(username),
                  std::move(cb));
}

void KickApi::privateRecentMessages(
    uint64_t chatroomID, int limit,
    Callback<std::vector<boost::json::object>> cb)
{
    if (chatroomID == 0)
    {
        cb(makeUnexpected(u"Missing chatroom ID"_s));
        return;
    }

    const auto boundedLimit = std::max(1, limit);
    const QString url = u"https://kick.com/api/v2/channels/" %
                        QString::number(chatroomID) %
                        u"/messages?limit=" % QString::number(boundedLimit);

    NetworkRequest(url)
        .headerList(kickNoAuthHeaders())
        .onError([cb](const NetworkResult &res) {
            cb(makeUnexpected(res.formatError()));
        })
        .onSuccess([cb = std::move(cb)](const NetworkResult &res) {
            const auto &ba = res.getData();
            boost::system::error_code ec;
            auto jv =
                boost::json::parse(std::string_view(ba.data(), ba.size()), ec);
            if (ec)
            {
                qCWarning(chatterinoKick)
                    << "Failed to parse recent messages response:"
                    << ec.message();
                cb(makeUnexpected(u"Failed to parse API response: "_s %
                                  QString::fromStdString(ec.message())));
                return;
            }

            if (!jv.is_object())
            {
                qCWarning(chatterinoKick)
                    << "Recent messages root value was not an object";
                cb(makeUnexpected(u"Root value was not an object"_s));
                return;
            }

            const auto &root = jv.as_object();
            const auto dataIt = root.find("data");
            if (dataIt == root.end() || !dataIt->value().is_object())
            {
                cb(makeUnexpected(u"'data' is not an object"_s));
                return;
            }

            const auto &data = dataIt->value().as_object();
            const auto messagesIt = data.find("messages");
            if (messagesIt == data.end() || !messagesIt->value().is_array())
            {
                cb(makeUnexpected(u"'data.messages' is not an array"_s));
                return;
            }

            const auto &messageValues = messagesIt->value().as_array();
            std::vector<boost::json::object> messages;
            messages.reserve(messageValues.size());
            for (const auto &messageValue : messageValues)
            {
                if (!messageValue.is_object())
                {
                    cb(makeUnexpected(
                        u"'data.messages' contained a non-object value"_s));
                    return;
                }

                messages.emplace_back(messageValue.as_object());
            }

            cb(std::move(messages));
        })
        .execute();
}

void KickApi::sendMessage(uint64_t broadcasterUserID, const QString &message,
                          const QString &replyToMessageID, Callback<void> cb)
{
    struct Response {
        Response(BoostJsonObject obj)
            : isSent(obj["is_sent"].toBool())
        {
        }
        bool isSent = false;
    };

    QJsonObject json{
        {"broadcaster_user_id"_L1, static_cast<qint64>(broadcasterUserID)},
        {"content"_L1, message},
        {"type"_L1, "user"_L1},
    };
    if (!replyToMessageID.isEmpty())
    {
        json.insert("reply_to_message_id"_L1, replyToMessageID);
    }
    this->postJson<Response>(
        u"chat"_s, json,
        [cb = std::move(cb)](const ExpectedStr<Response> &res) {
            cb(res.and_then([](Response res) {
                if (res.isSent)
                {
                    return ExpectedStr<void>{};
                }
                return ExpectedStr<void>{
                    makeUnexpected(u"Message was not sent"_s)};
            }));
        });
}

void KickApi::getChannels(std::span<uint64_t> userIDs,
                          Callback<std::vector<KickChannelInfo>> cb)
{
    QString path = u"channels?"_s;
    for (auto id : userIDs)
    {
        path += u"broadcaster_user_id=";
        path += QString::number(id);
        path += '&';
    }
    path.removeLast();

    this->getJson(path, std::move(cb));
}

void KickApi::getChannelByName(const QString &usernameOrSlug,
                               Callback<KickChannelInfo> cb)
{
    QString path =
        u"channels?slug=" % QUrl::toPercentEncoding(slugify(usernameOrSlug));
    this->getJson(path, std::move(cb));
}

void KickApi::banUser(uint64_t broadcasterUserID, uint64_t userID,
                      std::optional<std::chrono::minutes> duration,
                      const QString &reason, Callback<void> cb)
{
    QJsonObject json{
        {"broadcaster_user_id"_L1, static_cast<qint64>(broadcasterUserID)},
        {"user_id"_L1, static_cast<qint64>(userID)},
    };
    if (duration)
    {
        json.insert("duration"_L1, static_cast<qint64>(duration->count()));
    }
    if (!reason.isEmpty())
    {
        json.insert("reason"_L1, reason);
    }
    this->postJson(u"moderation/bans"_s, json, std::move(cb));
}

void KickApi::unbanUser(uint64_t broadcasterUserID, uint64_t userID,
                        Callback<void> cb)
{
    this->deleteJson(
        u"moderation/bans"_s,
        {
            {"broadcaster_user_id"_L1, static_cast<qint64>(broadcasterUserID)},
            {"user_id"_L1, static_cast<qint64>(userID)},
        },
        std::move(cb));
}

void KickApi::deleteChatMessage(const QString &messageID, Callback<void> cb)
{
    QString path = u"chat/" % QUrl::toPercentEncoding(messageID);
    this->deleteEmptyBody(path, std::move(cb));
}

void KickApi::pinChatMessage(const QString &channelSlug,
                             const QString &chatIdentityToken,
                             const QJsonObject &message, int durationSeconds,
                             Callback<void> cb)
{
    const auto token = chatIdentityToken.trimmed();
    const auto session = parseKickBrowserSession(token);
    if (session.sessionToken.isEmpty())
    {
        cb(makeUnexpected(
            u"Connect Kick's website session under Settings > Accounts "
            u"before pinning messages."_s));
        return;
    }
    if (session.xsrfToken.isEmpty())
    {
        cb(makeUnexpected(
            u"Reconnect Kick's website session under Settings > Accounts "
            u"before pinning messages."_s));
        return;
    }

    QJsonObject payload{
        {"message", message},
        {"duration", durationSeconds},
    };
    const QString url =
        u"https://kick.com/api/v2/channels/" % slugify(channelSlug) %
        u"/pinned-message";
    NetworkRequest request(QUrl{url}, NetworkRequestType::Post);
    std::move(request)
        .headerList(kickBrowserSessionHeaders(token, channelSlug))
        .json(payload)
        .timeout(20000)
        .onError([cb](const NetworkResult &result) {
            cb(makeUnexpected(kickIdentityError(result)));
        })
        .onSuccess([cb = std::move(cb)](const NetworkResult &) {
            cb(ExpectedStr<void>{});
        })
        .execute();
}

void KickApi::unpinChatMessage(const QString &channelSlug,
                               const QString &chatIdentityToken,
                               Callback<void> cb)
{
    const auto token = chatIdentityToken.trimmed();
    const auto session = parseKickBrowserSession(token);
    if (session.sessionToken.isEmpty())
    {
        cb(makeUnexpected(
            u"Connect Kick's website session under Settings > Accounts "
            u"before unpinning messages."_s));
        return;
    }
    if (session.xsrfToken.isEmpty())
    {
        cb(makeUnexpected(
            u"Reconnect Kick's website session under Settings > Accounts "
            u"before unpinning messages."_s));
        return;
    }

    const QString url =
        u"https://kick.com/api/v2/channels/" % slugify(channelSlug) %
        u"/pinned-message";
    NetworkRequest request(QUrl{url}, NetworkRequestType::Delete);
    std::move(request)
        .headerList(kickBrowserSessionHeaders(token, channelSlug))
        .timeout(20000)
        .onError([cb](const NetworkResult &result) {
            cb(makeUnexpected(kickIdentityError(result)));
        })
        .onSuccess([cb = std::move(cb)](const NetworkResult &) {
            cb(ExpectedStr<void>{});
        })
        .execute();
}

void KickApi::createPoll(const QString &channelSlug,
                         const QString &chatIdentityToken,
                         const QString &title, const QStringList &choices,
                         int durationSeconds,
                         int resultDisplayDurationSeconds, Callback<void> cb)
{
    const auto token = chatIdentityToken.trimmed();
    const auto slug = slugify(channelSlug);
    if (token.isEmpty())
    {
        cb(makeUnexpected(u"Connect Kick’s website session under Settings > "
                          u"Accounts before creating polls."_s));
        return;
    }
    const auto trimmedTitle = title.trimmed();
    if (slug.isEmpty() || choices.size() < 2 || durationSeconds <= 0 ||
        resultDisplayDurationSeconds <= 0)
    {
        cb(makeUnexpected(u"Kick poll details are incomplete."_s));
        return;
    }
    if (trimmedTitle.size() < 2 || trimmedTitle.size() > 64)
    {
        cb(makeUnexpected(
            u"Kick poll questions must be between 2 and 64 characters."_s));
        return;
    }

    QJsonArray options;
    for (const auto &choice : choices)
    {
        const auto name = choice.trimmed();
        if (name.isEmpty())
        {
            continue;
        }
        if (name.size() < 2 || name.size() > 64)
        {
            cb(makeUnexpected(
                u"Kick poll responses must be between 2 and 64 characters."_s));
            return;
        }
        options.append(name);
    }
    if (options.size() < 2 || options.size() > 6)
    {
        cb(makeUnexpected(
            u"Kick polls need between two and six responses."_s));
        return;
    }

    const QJsonObject payload{
        {QStringLiteral("duration"), durationSeconds},
        {QStringLiteral("result_display_duration"),
         resultDisplayDurationSeconds},
        {QStringLiteral("options"), options},
        {QStringLiteral("title"), trimmedTitle},
    };
    const auto url = u"https://kick.com/api/v2/channels/" % slug % u"/polls";
    NetworkRequest request(QUrl{url}, NetworkRequestType::Post);
    std::move(request)
        .headerList(kickBrowserSessionHeaders(token, slug))
        .json(payload)
        .timeout(20000)
        .onError([cb](const NetworkResult &result) {
            cb(makeUnexpected(kickIdentityError(result)));
        })
        .onSuccess([cb = std::move(cb)](const NetworkResult &) {
            cb(ExpectedStr<void>{});
        })
        .execute();
}

void KickApi::deletePoll(const QString &channelSlug,
                         const QString &chatIdentityToken, Callback<void> cb)
{
    const auto token = chatIdentityToken.trimmed();
    const auto slug = slugify(channelSlug);
    if (token.isEmpty())
    {
        cb(makeUnexpected(u"Connect Kick's website session under Settings > "
                          u"Accounts before ending polls."_s));
        return;
    }
    if (slug.isEmpty())
    {
        cb(makeUnexpected(u"The Kick channel is unavailable."_s));
        return;
    }

    const auto url = u"https://kick.com/api/v2/channels/" % slug % u"/polls";
    NetworkRequest request(QUrl{url}, NetworkRequestType::Delete);
    std::move(request)
        .headerList(kickBrowserSessionHeaders(token, slug))
        .timeout(20000)
        .onError([cb](const NetworkResult &result) {
            cb(makeUnexpected(kickIdentityError(result)));
        })
        .onSuccess([cb = std::move(cb)](const NetworkResult &) {
            cb(ExpectedStr<void>{});
        })
        .execute();
}

void KickApi::createPrediction(const QString &channelSlug,
                               const QString &chatIdentityToken,
                               const QString &title,
                               const QStringList &outcomes,
                               int durationSeconds, Callback<void> cb)
{
    const auto token = chatIdentityToken.trimmed();
    const auto slug = slugify(channelSlug);
    if (token.isEmpty())
    {
        cb(makeUnexpected(
            u"Connect Kick’s website session under Settings > Accounts "
            u"before creating predictions."_s));
        return;
    }
    if (slug.isEmpty() || title.trimmed().isEmpty() ||
        outcomes.size() != 2 || durationSeconds <= 0)
    {
        cb(makeUnexpected(u"Kick prediction details are incomplete."_s));
        return;
    }

    const auto firstOutcome = outcomes.at(0).trimmed();
    const auto secondOutcome = outcomes.at(1).trimmed();
    if (firstOutcome.isEmpty() || secondOutcome.isEmpty())
    {
        cb(makeUnexpected(u"Kick prediction details are incomplete."_s));
        return;
    }
    if (firstOutcome.compare(secondOutcome, Qt::CaseInsensitive) == 0)
    {
        cb(makeUnexpected(
            u"The two prediction outcomes must have different names."_s));
        return;
    }

    const QJsonArray outcomeArray{firstOutcome, secondOutcome};
    const QJsonObject payload{
        {QStringLiteral("title"), title.trimmed()},
        {QStringLiteral("outcomes"), outcomeArray},
        {QStringLiteral("duration"), durationSeconds},
    };
    const auto url =
        u"https://kick.com/api/v2/channels/" % slug % u"/predictions";
    NetworkRequest request(QUrl{url}, NetworkRequestType::Post);
    std::move(request)
        .headerList(kickBrowserSessionHeaders(token, slug))
        .json(payload)
        .timeout(20000)
        .onError([cb](const NetworkResult &result) {
            cb(makeUnexpected(kickIdentityError(result)));
        })
        .onSuccess([cb = std::move(cb)](const NetworkResult &) {
            cb(ExpectedStr<void>{});
        })
        .execute();
}

void KickApi::updatePrediction(const QString &channelSlug,
                               const QString &chatIdentityToken,
                               const QString &predictionID,
                               const QString &state,
                               const QString &winningOutcomeID,
                               Callback<void> cb)
{
    const auto token = chatIdentityToken.trimmed();
    const auto slug = slugify(channelSlug);
    const auto id = predictionID.trimmed();
    const auto nextState = state.trimmed().toUpper();
    if (token.isEmpty())
    {
        cb(makeUnexpected(u"Connect Kick's website session under Settings > "
                          u"Accounts before managing predictions."_s));
        return;
    }
    if (slug.isEmpty() || id.isEmpty())
    {
        cb(makeUnexpected(u"The Kick prediction details are incomplete."_s));
        return;
    }
    if (nextState != u"LOCKED" && nextState != u"RESOLVED" &&
        nextState != u"CANCELLED")
    {
        cb(makeUnexpected(
            u"The requested Kick prediction state is invalid."_s));
        return;
    }
    if (nextState == u"RESOLVED" && winningOutcomeID.trimmed().isEmpty())
    {
        cb(makeUnexpected(u"Choose a winning Kick prediction outcome."_s));
        return;
    }

    QJsonObject payload{{QStringLiteral("state"), nextState}};
    if (nextState == u"RESOLVED")
    {
        payload.insert(QStringLiteral("winning_outcome_id"),
                       winningOutcomeID.trimmed());
    }

    const auto url = u"https://kick.com/api/v2/channels/" % slug %
                     u"/predictions/" % id;
    NetworkRequest request(QUrl{url}, NetworkRequestType::Patch);
    std::move(request)
        .headerList(kickBrowserSessionHeaders(token, slug))
        .json(payload)
        .timeout(20000)
        .onError([cb](const NetworkResult &result) {
            cb(makeUnexpected(kickPredictionManagementError(result)));
        })
        .onSuccess([cb = std::move(cb)](const NetworkResult &) {
            cb(ExpectedStr<void>{});
        })
        .execute();
}

void KickApi::votePrediction(const QString &channelSlug,
                             const QString &chatIdentityToken,
                             const QString &outcomeID, int amount,
                             Callback<void> cb)
{
    const auto token = chatIdentityToken.trimmed();
    const auto slug = slugify(channelSlug);
    const auto outcome = outcomeID.trimmed();
    if (token.isEmpty())
    {
        cb(makeUnexpected(
            u"Connect Kick's website session under Settings > Accounts "
            u"before making a prediction."_s));
        return;
    }
    if (slug.isEmpty() || outcome.isEmpty() || amount < 10 || amount > 250000)
    {
        cb(makeUnexpected(
            u"Choose an outcome and wager between 10 and 250,000 Channel "
            u"Points."_s));
        return;
    }

    const QJsonObject payload{
        {QStringLiteral("amount"), amount},
        {QStringLiteral("outcome_id"), outcome},
    };
    const auto url = u"https://kick.com/api/v2/channels/" % slug %
                     u"/predictions/vote";
    NetworkRequest request(QUrl{url}, NetworkRequestType::Post);
    std::move(request)
        .headerList(kickBrowserSessionHeaders(token, slug))
        .json(payload)
        .timeout(20000)
        .onError([cb](const NetworkResult &result) {
            cb(makeUnexpected(kickIdentityError(result)));
        })
        .onSuccess([cb = std::move(cb)](const NetworkResult &) {
            cb(ExpectedStr<void>{});
        })
        .execute();
}

void KickApi::validateChatIdentityToken(const QString &chatIdentityToken,
                                        uint64_t expectedUserID,
                                        Callback<void> cb)
{
    const auto token = chatIdentityToken.trimmed();
    if (token.isEmpty() || expectedUserID == 0)
    {
        cb(makeUnexpected(
            u"Kick chat identity token or account ID is unavailable."_s));
        return;
    }

    NetworkRequest request(QUrl{QStringLiteral("https://kick.com/api/v1/user")});
    std::move(request)
        .headerList(kickBrowserSessionHeaders(token))
        .timeout(20000)
        .onError([cb](const NetworkResult &result) {
            cb(makeUnexpected(kickIdentityError(result)));
        })
        .onSuccess([expectedUserID, cb = std::move(cb)](
                       const NetworkResult &result) {
            const auto userID = kickWebsiteUserID(result.parseJson());
            if (userID == 0)
            {
                cb(makeUnexpected(
                    u"Kick returned an invalid signed-in account response."_s));
                return;
            }
            if (userID != expectedUserID)
            {
                cb(makeUnexpected(
                    u"That Kick website session belongs to a different "
                    u"account."_s));
                return;
            }
            cb(ExpectedStr<void>{});
        })
        .execute();
}

void KickApi::getChatIdentity(uint64_t channelID, uint64_t userID,
                              const QString &chatIdentityToken,
                              Callback<KickChatIdentity> cb)
{
    const auto token = chatIdentityToken.trimmed();
    if (token.isEmpty())
    {
        cb(makeUnexpected(u"Connect Kick chat identity under Settings > "
                          u"Accounts before changing it."_s));
        return;
    }
    if (channelID == 0 || userID == 0)
    {
        cb(makeUnexpected(u"Kick channel or account ID is unavailable."_s));
        return;
    }

    NetworkRequest request(QUrl{kickIdentityUrl(channelID, userID)});
    std::move(request)
        .headerList(kickBrowserSessionHeaders(token))
        .timeout(20000)
        .onError([cb](const NetworkResult &result) {
            cb(makeUnexpected(kickIdentityError(result)));
        })
        .onSuccess([cb = std::move(cb)](const NetworkResult &result) {
            const auto identity = parseKickChatIdentity(result.parseJson());
            if (!identity)
            {
                cb(makeUnexpected(
                    u"Kick returned an invalid chat identity response."_s));
                return;
            }
            cb(*identity);
        })
        .execute();
}

void KickApi::updateChatIdentity(uint64_t channelID, uint64_t userID,
                                 const QString &chatIdentityToken,
                                 const KickChatIdentity &identity,
                                 Callback<KickChatIdentity> cb)
{
    const auto token = chatIdentityToken.trimmed();
    if (token.isEmpty())
    {
        cb(makeUnexpected(u"Connect Kick chat identity under Settings > "
                          u"Accounts before changing it."_s));
        return;
    }
    if (channelID == 0 || userID == 0)
    {
        cb(makeUnexpected(u"Kick channel or account ID is unavailable."_s));
        return;
    }

    QJsonArray badges;
    QJsonArray badgesV2;
    for (const auto &badge : identity.badges)
    {
        if (!badge.selected || badge.name.trimmed().isEmpty())
        {
            continue;
        }
        if (badge.legacy)
        {
            badges.append(badge.name);
        }
        else
        {
            badgesV2.append(
                QJsonObject{{QStringLiteral("name"), badge.name}});
        }
    }

    QJsonObject payload{
        {QStringLiteral("badges"), badges},
        {QStringLiteral("badges_v2"), badgesV2},
        {QStringLiteral("color"), identity.color},
    };
    NetworkRequest request(QUrl{kickIdentityUrl(channelID, userID)},
                           NetworkRequestType::Put);
    std::move(request)
        .headerList(kickBrowserSessionHeaders(token))
        .json(payload)
        .timeout(20000)
        .onError([cb](const NetworkResult &result) {
            cb(makeUnexpected(kickIdentityError(result)));
        })
        .onSuccess([identity, cb = std::move(cb)](
                       const NetworkResult &) {
            // Keep the complete set fetched for the picker. Kick's mutation
            // response may contain only the newly selected badges.
            cb(identity);
        })
        .execute();
}

void KickApi::setAuth(const QString &authToken)
{
    this->authToken = authToken.toUtf8();
}

template <typename T>
void KickApi::getJson(const QString &endpoint, Callback<T> cb)
{
    this->doRequest(NetworkRequest(makePublicV1Url(endpoint)), std::move(cb));
}

template <typename T>
void KickApi::postJson(const QString &endpoint, const QJsonObject &json,
                       Callback<T> cb)
{
    this->doRequest(
        NetworkRequest(makePublicV1Url(endpoint), NetworkRequestType::Post)
            .json(json),
        std::move(cb));
}

template <typename T>
void KickApi::deleteJson(const QString &endpoint, const QJsonObject &json,
                         Callback<T> cb)
{
    this->doRequest(
        NetworkRequest(makePublicV1Url(endpoint), NetworkRequestType::Delete)
            .json(json),
        std::move(cb));
}

template <typename T>
void KickApi::deleteEmptyBody(const QString &endpoint, Callback<T> cb)
{
    this->doRequest(
        NetworkRequest(makePublicV1Url(endpoint), NetworkRequestType::Delete),
        std::move(cb));
}

template <typename T>
void KickApi::doRequest(NetworkRequest &&req, Callback<T> cb)
{
    auto request = std::move(req);
    if (!this->authToken.isEmpty())
    {
        request = std::move(request).header("Authorization"_ba,
                                            "Bearer "_ba + this->authToken);
    }

    std::move(request)
        .onError([cb](const NetworkResult &res) {
            auto message = res.parseJson().value("message").toString();
            if (!message.isEmpty())
            {
                cb(makeUnexpected(message));
            }
            else
            {
                cb(makeUnexpected(res.formatError()));
            }
        })
        .onSuccess([cb = std::move(cb)](const NetworkResult &res) {
            if constexpr (std::is_void_v<T>)
            {
                cb(ExpectedStr<T>{});
                return;
            }

            const auto &ba = res.getData();
            boost::system::error_code ec;
            auto jv =
                boost::json::parse(std::string_view(ba.data(), ba.size()), ec);
            if (ec)
            {
                qCWarning(chatterinoKick)
                    << "Failed to parse API response:" << ec.message();
                cb(makeUnexpected(u"Failed to parse API response: "_s %
                                  QString::fromStdString(ec.message())));
                return;
            }

            BoostJsonValue ref(jv);
            if (!ref.isObject())
            {
                qCWarning(chatterinoKick) << "Root value was not an object";
                cb(makeUnexpected(u"Root value was not an object"_s));
                return;
            }
            auto data = ref["data"];
            callDeserialize<T>(cb, data);
        })
        .execute();
}

KickApi::KickApi() = default;

KickApi *getKickApi()
{
    return KickApi::instance();
}

}  // namespace chatterino
