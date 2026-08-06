#pragma once

#include "common/Atomic.hpp"
#include "common/Channel.hpp"
#include "common/ChannelChatters.hpp"
#include "messages/MessageElement.hpp"

#include <pajlada/signals/signal.hpp>

#include <QDateTime>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

namespace chatterino {

namespace seventv::eventapi {
struct EmoteAddDispatch;
struct EmoteRemoveDispatch;
struct EmoteUpdateDispatch;
struct UserConnectionUpdateDispatch;
}  // namespace seventv::eventapi

class MessageThread;
class EmoteMap;

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

struct EmoteName;

struct KickChannelInfo;
struct KickPrivateChannelInfo;
struct KickPrivateSubscriberBadgeInfo;
struct KickPrivateUserInChannelInfo;

struct KickPredictionOutcome {
    QString id;
    QString title;
    int totalVoteAmount = 0;
    int voteCount = 0;

    bool operator==(const KickPredictionOutcome &other) const = default;
};

struct KickPrediction {
    QString id;
    QString title;
    QString state;
    std::vector<KickPredictionOutcome> outcomes;
    int durationSeconds = 0;
    QDateTime createdAt;
    QDateTime updatedAt;
    QDateTime lockedAt;
    QString winningOutcomeID;

    [[nodiscard]] bool isOpen() const
    {
        return this->state == QStringLiteral("ACTIVE") ||
               this->state == QStringLiteral("LOCKED");
    }

    bool operator==(const KickPrediction &other) const = default;
};

struct KickPollOption {
    QString id;
    QString title;
    int votes = 0;

    bool operator==(const KickPollOption &other) const = default;
};

struct KickPoll {
    QString id;
    QString title;
    QString state;
    std::vector<KickPollOption> options;
    QDateTime createdAt;
    QDateTime endsAt;
    QDateTime hideAt;

    [[nodiscard]] bool isVisible() const
    {
        return (this->state == QStringLiteral("ACTIVE") ||
                this->state == QStringLiteral("RESULTS")) &&
               !this->title.isEmpty() && !this->options.empty();
    }

    bool operator==(const KickPoll &other) const = default;
};

class KickChannel : public Channel, public ChannelChatters
{
public:
    struct UserInit {
        uint64_t roomID = 0;
        uint64_t userID = 0;
        uint64_t channelID = 0;
    };

    struct RoomModes {
        bool subscribersMode = false;
        bool emotesMode = false;
        std::optional<std::chrono::seconds> slowModeDuration;
        std::optional<std::chrono::minutes> followersModeDuration;

        auto operator<=>(const RoomModes &other) const = default;
    };

    KickChannel(const QString &name);
    ~KickChannel() override;

    void initialize(const UserInit &init);
    void loadRecentMessages(std::function<void()> onDone = {});
    void loadRecentMessagesReconnect(std::function<void()> onDone = {});

    std::shared_ptr<KickChannel> sharedFromThis();
    std::weak_ptr<KickChannel> weakFromThis();

    const QString &getDisplayName() const override
    {
        return this->displayName_;
    }

    const QString &slug() const
    {
        return this->slug_;
    }

    uint64_t roomID() const
    {
        return this->roomID_;
    }
    uint64_t userID() const
    {
        return this->userID_;
    }
    uint64_t channelID() const
    {
        return this->channelID_;
    }

    /// Get the thread for the given message.
    /// If no thread can be found for the message, create one.
    /// Additionally, this returns the reply parent.
    std::pair<std::shared_ptr<MessageThread>, MessagePtr> getOrCreateThread(
        const QString &messageID);

    void reloadSeventvEmotes(bool manualRefresh);

    std::shared_ptr<const EmoteMap> kickChannelEmotes() const;
    std::shared_ptr<const EmoteMap> seventvEmotes() const;
    EmotePtr seventvEmote(const EmoteName &name) const;
    EmotePtr subscriberBadgeForMonths(uint64_t months) const;

    void addSeventvEmote(const seventv::eventapi::EmoteAddDispatch &dispatch);

    void updateSeventvEmote(
        const seventv::eventapi::EmoteUpdateDispatch &dispatch);
    void removeSeventvEmote(
        const seventv::eventapi::EmoteRemoveDispatch &dispatch);
    void updateSeventvUser(
        const seventv::eventapi::UserConnectionUpdateDispatch &dispatch);

    const QString &seventvUserID() const;
    const QString &seventvEmoteSetID() const;

    bool canSendMessage() const override;
    void sendMessage(const QString &message) override;
    using SentMessageCallback = std::function<void(const MessagePtr &)>;
    void sendMessageAndWaitForEcho(const QString &message,
                                   SentMessageCallback callback);
    void sendReply(const QString &message, const QString &replyToID);

    bool tryReplacePendingSentMessage(const MessagePtr &message);
    void updateOwnIdentityFromMessage(const Message &message);

    struct CachedOwnIdentity {
        QString displayName;
        QString loginName;
        QString userID;
        QColor usernameColor;
        std::vector<std::unique_ptr<MessageElement>> badges;
    };
    const CachedOwnIdentity *ownIdentity() const;

    void deleteMessage(const QString &messageID);

    bool isMod() const override;
    void setMod(bool mod);

    bool isVip() const;
    void setVip(bool vip);

    bool isBroadcaster() const override;
    bool hasModRights() const override;
    bool hasHighRateLimit() const override;
    bool isLive() const override;
    bool canReconnect() const override;
    void reconnect() override;
    QString getCurrentStreamID() const override;

    struct StreamData {
        bool isLive = false;
        QString title;
        QString category;

        QString thumbnailUrl;
        QString uptime;
        uint64_t viewerCount = 0;
    };
    void updateStreamData(const KickChannelInfo &info);
    void updateStreamData(const KickPrivateChannelInfo &info);
    const StreamData &streamData() const;
    pajlada::Signals::NoArgSignal streamDataChanged;
    pajlada::Signals::NoArgSignal liveStatusChanged;

    pajlada::Signals::NoArgSignal userIDChanged;
    pajlada::Signals::NoArgSignal userStateChanged;

    const RoomModes &roomModes() const;
    void updateRoomModes(const RoomModes &modes);
    pajlada::Signals::NoArgSignal roomModesChanged;

    const std::optional<KickPrediction> &activePrediction() const;
    void refreshPrediction();
    void updatePrediction(KickPrediction prediction);
    const std::optional<KickPoll> &activePoll() const;
    void updatePoll(std::optional<KickPoll> poll);
    pajlada::Signals::Signal<int> sendWaitUpdate;
    void setSendWait(std::chrono::seconds waitTime);

    friend QDebug operator<<(QDebug dbg, const KickChannel &chan);

protected:
    void messageRemovedFromStart(const MessagePtr &msg) override;

private:
    /// Message ID -> thread
    std::unordered_map<QString, std::weak_ptr<MessageThread>> threads_;

    uint64_t roomID_ = 0;
    uint64_t userID_ = 0;
    uint64_t channelID_ = 0;

    void resolveChannelInfo();
    void reloadKickChannelEmotes();
    void refreshOwnIdentity();
    void cacheOwnIdentityFromUserInfo(const KickPrivateUserInChannelInfo &info);
    void setUserInfo(UserInit init);
    void setSubscriberBadges(
        const std::vector<KickPrivateSubscriberBadgeInfo> &badges);

    size_t maxBurstMessages() const;
    std::chrono::milliseconds minMessageOffset() const;
    bool checkMessageRatelimit();

    QString prepareMessage(const QString &message) const;
    void sendReply(const QString &message, const QString &replyToID,
                   SentMessageCallback callback);
    void updateSevenTVActivity();
    void prunePendingSentMessages(const QDateTime &now);
    void markPendingSentMessageFailed(const QString &localID);
    void applyStreamData(bool isLive, const QString &streamTitle,
                         const QString &categoryName,
                         const QString &thumbnailUrl, uint64_t viewerCount,
                         const QDateTime &startTime);

    void addLoginMessage();

    void updateSeventvData(const QString &newUserID,
                           const QString &newEmoteSetID);
    void addOrReplaceSeventvAddRemove(bool isEmoteAdd, const QString &actor,
                                      const QString &emoteName);
    bool tryReplaceLastSeventvAddOrRemove(MessageFlag op, const QString &actor,
                                          const QString &emoteName);

    void emitSendWait();

    // Kick usually calls this username
    QString displayName_;
    // The name in the URL (replaces non-alphanumeric characters with dashes)
    QString slug_;

    Atomic<std::shared_ptr<const EmoteMap>> kickChannelEmotes_;
    Atomic<std::shared_ptr<const EmoteMap>> seventvEmotes_;

    struct SubscriberBadge {
        uint64_t months = 0;
        EmotePtr emote;
    };
    std::vector<SubscriberBadge> subscriberBadges_;

    QString seventvUserID_;
    QString seventvEmoteSetID_;
    size_t seventvKickConnectionIndex_ = 0;
    /// The actor name of the last 7TV emote update.
    QString lastSeventvEmoteActor_;
    /// A weak reference to the last 7TV emote update message.
    std::weak_ptr<const Message> lastSeventvMessage_;
    /// A list of the emotes listed in the lat 7TV emote update message.
    std::vector<QString> lastSeventvEmoteNames_;
    QDateTime nextSeventvActivity_;

    std::queue<std::chrono::steady_clock::time_point> lastMessageTimestamps_;
    std::chrono::steady_clock::time_point lastMessageSpeedErrorTs_;
    std::chrono::steady_clock::time_point lastMessageAmountErrorTs_;

    struct PendingSentMessage {
        QString localID;
        QString messageText;
        QDateTime createdAt;
        SentMessageCallback confirmedCallback;
    };
    std::deque<PendingSentMessage> pendingSentMessages_;
    std::optional<CachedOwnIdentity> ownIdentity_;

    QTimer sendWaitTimer_;
    // Timepoint at which the user can send messages again
    std::optional<std::chrono::steady_clock::time_point> sendWaitEnd_;
    std::atomic_bool loadedRecentMessages_ = false;
    std::atomic_flag loadingRecentMessages_ = ATOMIC_FLAG_INIT;

    RoomModes roomModes_;
    bool isMod_ = false;
    bool isVip_ = false;

    StreamData streamData_;
    QString currentStreamID_;
    std::optional<KickPrediction> activePrediction_;

public:
    pajlada::Signals::NoArgSignal predictionChanged;
};

}  // namespace chatterino
