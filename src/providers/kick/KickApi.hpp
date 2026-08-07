#pragma once

#include "util/Expected.hpp"

#include <boost/json/object.hpp>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace chatterino {

class BoostJsonObject;
class NetworkRequest;

// Private API

struct KickPrivateUserInfo {
    KickPrivateUserInfo(BoostJsonObject obj);

    uint64_t userID = 0;
    QString username;
    std::optional<QString> profilePictureURL;
};

struct KickPrivateChatroomInfo {
    KickPrivateChatroomInfo(BoostJsonObject obj);

    uint64_t roomID = 0;
    QDateTime createdAt;
    bool subscribersMode = false;
    bool emotesMode = false;
    std::optional<std::chrono::seconds> slowModeDuration;
    std::optional<std::chrono::minutes> followersModeDuration;
};

struct KickPrivateChatSettings {
    KickPrivateChatSettings(BoostJsonObject obj);

    bool subscribersMode = false;
    bool emotesMode = false;
    std::optional<std::chrono::seconds> slowModeDuration;
    std::optional<std::chrono::minutes> followersModeDuration;
};

struct KickPrivateSubscriberBadgeInfo {
    KickPrivateSubscriberBadgeInfo(BoostJsonObject obj);

    uint64_t months = 0;
    QString imageUrl;
};

struct KickPrivateUserBadgeInfo {
    KickPrivateUserBadgeInfo(BoostJsonObject obj);

    QString type;
    QString text;
    QString imageUrl;
    uint64_t count = 0;
    uint64_t level = 0;
    uint64_t sortOrder = 1000;
    bool active = true;
    bool selected = true;
};

struct KickChatIdentityBadge {
    QString name;
    QString title;
    QString badgeType;
    QString imageUrl;
    uint64_t count = 0;
    uint64_t level = 0;
    uint64_t sortOrder = 1000;
    bool selected = false;
    bool legacy = false;
};

struct KickChatIdentity {
    QString color;
    std::vector<KickChatIdentityBadge> badges;
};

QString kickIdentityAuthHelper();
QString parseKickIdentityToken(const QString &clipboardText);

struct KickPrivateChannelInfo {
    KickPrivateChannelInfo(BoostJsonObject obj);

    uint64_t channelID = 0;
    uint64_t followersCount = 0;
    QString slug;
    KickPrivateUserInfo user;
    KickPrivateChatroomInfo chatroom;
    bool isLive = false;
    QString streamTitle;
    QString liveCategoryName;
    uint64_t viewerCount = 0;
    QDateTime startTime;
    QString thumbnailUrl;
    std::vector<KickPrivateSubscriberBadgeInfo> subscriberBadges;
};

struct KickPrivateUserInChannelInfo {
    KickPrivateUserInChannelInfo(BoostJsonObject obj);

    uint64_t userID = 0;
    QString username;
    bool isModerator = false;
    bool isChannelOwner = false;
    std::optional<QDateTime> followingSince;
    std::optional<uint16_t> subscriptionMonths;
    std::optional<QString> profilePictureURL;
    std::vector<KickPrivateUserBadgeInfo> badges;
};

struct KickPrivateEmoteInfo {
    KickPrivateEmoteInfo(BoostJsonObject obj);

    uint64_t emoteID = 0;
    QString name;
    bool subscribersOnly = false;
};

struct KickPrivateEmoteSetInfo {
    KickPrivateEmoteSetInfo(BoostJsonObject obj);

    // if this is set, it's a user set - otherwise it's global
    std::optional<uint64_t> userID;
    std::vector<KickPrivateEmoteInfo> emotes;
};

// Public API

struct KickCategoryInfo {
    KickCategoryInfo(BoostJsonObject obj);

    QString name;
};

struct KickStreamInfo {
    KickStreamInfo(BoostJsonObject obj);

    bool isLive = false;
    uint64_t viewerCount = 0;
    QDateTime startTime;
    QString thumbnailUrl;
};

struct KickChannelInfo {
    KickChannelInfo(BoostJsonObject obj);

    uint64_t userID = 0;
    KickCategoryInfo category;
    KickStreamInfo stream;
    QString streamTitle;
};

class KickApi
{
public:
    template <typename T>
    using Callback = std::function<void(ExpectedStr<T>)>;

    static KickApi *instance();

    static QString slugify(const QString &usernameOrSlug);

    static void privateChannelInfo(const QString &username,
                                   Callback<KickPrivateChannelInfo> cb);

    static void privateLatestPrediction(const QString &username,
                                        Callback<QJsonObject> cb);

    static void privateChatSettings(uint64_t channelID,
                                    Callback<KickPrivateChatSettings> cb);

    static void privateUserInChannelInfo(
        const QString &userUsername, const QString &channelUsername,
        Callback<KickPrivateUserInChannelInfo> cb);

    static void privateEmotesInChannel(
        const QString &username,
        Callback<std::vector<KickPrivateEmoteSetInfo>> cb);

    static void privateRecentMessages(
        uint64_t chatroomID, int limit,
        Callback<std::vector<boost::json::object>> cb);

    void sendMessage(uint64_t broadcasterUserID, const QString &message,
                     const QString &replyToMessageID, Callback<void> cb);

    void getChannels(std::span<uint64_t> userIDs,
                     Callback<std::vector<KickChannelInfo>> cb);

    void getChannelByName(const QString &usernameOrSlug,
                          Callback<KickChannelInfo> cb);

    void banUser(uint64_t broadcasterUserID, uint64_t userID,
                 std::optional<std::chrono::minutes> duration,
                 const QString &reason, Callback<void> cb);

    void unbanUser(uint64_t broadcasterUserID, uint64_t userID,
                   Callback<void> cb);

    void deleteChatMessage(const QString &messageID, Callback<void> cb);

    void pinChatMessage(const QString &channelSlug,
                        const QString &chatIdentityToken,
                        const QJsonObject &message, int durationSeconds,
                        Callback<void> cb);
    void unpinChatMessage(const QString &channelSlug,
                          const QString &chatIdentityToken,
                          Callback<void> cb);
    void createPoll(const QString &channelSlug,
                    const QString &chatIdentityToken, const QString &title,
                    const QStringList &choices, int durationSeconds,
                    int resultDisplayDurationSeconds, Callback<void> cb);
    void deletePoll(const QString &channelSlug,
                    const QString &chatIdentityToken, Callback<void> cb);
    void createPrediction(const QString &channelSlug,
                          const QString &chatIdentityToken,
                          const QString &title, const QStringList &outcomes,
                          int durationSeconds, Callback<void> cb);
    void updatePrediction(const QString &channelSlug,
                          const QString &chatIdentityToken,
                          const QString &predictionID, const QString &state,
                          const QString &winningOutcomeID, Callback<void> cb);
    void votePrediction(const QString &channelSlug,
                        const QString &chatIdentityToken,
                        const QString &outcomeID, int amount,
                        Callback<void> cb);
    void validateChatIdentityToken(const QString &chatIdentityToken,
                                   uint64_t expectedUserID,
                                   Callback<void> cb);
    void getChatIdentity(uint64_t channelID, uint64_t userID,
                         const QString &chatIdentityToken,
                         Callback<KickChatIdentity> cb);
    void updateChatIdentity(uint64_t channelID, uint64_t userID,
                            const QString &chatIdentityToken,
                            const KickChatIdentity &identity,
                            Callback<KickChatIdentity> cb);

    void setAuth(const QString &authToken);

private:
    KickApi();

    template <typename T>
    void getJson(const QString &endpoint, Callback<T> cb);

    template <typename T>
    void postJson(const QString &endpoint, const QJsonObject &json,
                  Callback<T> cb);

    template <typename T>
    void deleteJson(const QString &endpoint, const QJsonObject &json,
                    Callback<T> cb);

    template <typename T>
    void deleteEmptyBody(const QString &endpoint, Callback<T> cb);

    template <typename T>
    void doRequest(NetworkRequest &&req, Callback<T> cb);

    QByteArray authToken;
};

KickApi *getKickApi();

}  // namespace chatterino
