#include "providers/kick/KickChatServer.hpp"

#include "Application.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickEmotes.hpp"
#include "providers/kick/KickMessageBuilder.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvEventAPI.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Settings.hpp"
#include "util/BoostJsonWrap.hpp"
#include "util/PostToThread.hpp"

#include <QPointer>

#include <algorithm>
#include <utility>

namespace {

using namespace Qt::Literals;

// fallback case
template <typename T>
T stringSwitch(std::string_view /* provided */)
{
    return {};
}

template <typename T>
T stringSwitch(std::string_view provided, std::string_view match, T &&value,
               auto &&...rest)
{
    if (provided == match)
    {
        return std::forward<T>(value);
    }
    return stringSwitch<T>(provided, std::forward<decltype(rest)>(rest)...);
}

}  // namespace

namespace chatterino {

KickChatServer::KickChatServer()
    : liveController_(*this)
{
}

KickChatServer::~KickChatServer() = default;

void KickChatServer::initialize()
{
    this->initializeSeventvEventApi(getApp()->getSeventvEventAPI());
}

std::shared_ptr<KickChannel> KickChatServer::findByRoomID(uint64_t roomID) const
{
    auto it = this->channelsByRoomID.find(roomID);
    if (it != this->channelsByRoomID.end())
    {
        return it->second.lock();
    }
    return nullptr;
}

std::shared_ptr<KickChannel> KickChatServer::findByChannelID(
    uint64_t channelID) const
{
    auto it = this->channelsByChannelID.find(channelID);
    if (it != this->channelsByChannelID.end())
    {
        return it->second.lock();
    }
    return nullptr;
}

std::shared_ptr<KickChannel> KickChatServer::findByUserID(uint64_t userID) const
{
    auto it = this->channelsByUserID.find(userID);
    if (it != this->channelsByUserID.end())
    {
        return it->second.lock();
    }
    return nullptr;
}

std::shared_ptr<KickChannel> KickChatServer::findBySlug(
    const QString &slug) const
{
    QString searchString = slug.toLower();
    if (searchString.startsWith('#'))
    {
        searchString.removeFirst();
    }
    auto it = this->channelsBySlug.find(searchString);
    if (it != this->channelsBySlug.end())
    {
        return it->second.lock();
    }
    return nullptr;
}

void KickChatServer::forEachChannel(FunctionRef<void(KickChannel &channel)> cb)
{
    for (const auto &[id, weak] : this->channelsByRoomID)
    {
        auto chan = weak.lock();
        if (chan)
        {
            cb(*chan);
        }
    }
}

void KickChatServer::forEachSeventvEmoteSet(
    const QString &emoteSetID, FunctionRef<void(KickChannel &channel)> cb)
{
    for (const auto &[id, weak] : this->channelsByRoomID)
    {
        auto chan = weak.lock();
        if (chan && chan->seventvEmoteSetID() == emoteSetID)
        {
            cb(*chan);
        }
    }
}

void KickChatServer::forEachSeventvUser(
    const QString &seventvUserID, FunctionRef<void(KickChannel &channel)> cb)
{
    for (const auto &[id, weak] : this->channelsByRoomID)
    {
        auto chan = weak.lock();
        if (chan && chan->seventvUserID() == seventvUserID)
        {
            cb(*chan);
        }
    }
}

std::shared_ptr<Channel> KickChatServer::getOrCreate(
    const QString &slug, const KickChannel::UserInit &init)
{
    auto lower = slug.toLower();
    if (lower.startsWith(u":kick:"))
    {
        lower = std::move(lower).mid(6);
    }

    auto existing = this->findBySlug(lower);
    if (existing)
    {
        existing->loadRecentMessages();
        if (existing->userID() != 0)
        {
            this->liveController_.queueNewChannel(existing->userID());
            this->subscribeSeventvCosmetics(existing);
        }
        return existing;
    }
    auto chan = std::make_shared<KickChannel>(lower);
    this->channelsBySlug[lower] = chan;
    if (init.roomID != 0)
    {
        this->channelsByRoomID[init.roomID] = chan;
    }
    if (init.channelID != 0)
    {
        this->channelsByChannelID[init.channelID] = chan;
    }
    this->signalHolder_.managedConnect(
        chan->userIDChanged, [this, weak{chan->weakFromThis()}] {
            auto chan = weak.lock();
            if (chan && chan->userID() != 0)
            {
                this->channelsByUserID[chan->userID()] = weak;
                this->liveController_.queueNewChannel(chan->userID());
                this->subscribeSeventvCosmetics(chan);
            }
        });
    chan->initialize(init);
    this->loadGlobalEmotesIfNeeded();
    return chan;
}

bool KickChatServer::onAppEvent(uint64_t roomID, uint64_t channelID,
                                std::string_view event, BoostJsonObject data)
{
    using Fn = void (KickChatServer::*)(KickChannel *, BoostJsonObject);
    auto fn = stringSwitch<Fn>(
        event,                                                      //
        "ChatMessageEvent", &KickChatServer::onChatMessage,         //
        "MessageDeletedEvent", &KickChatServer::onMessageDeleted,   //
        "ChatroomClearEvent", &KickChatServer::onChatroomClear,     //
        "UserBannedEvent", &KickChatServer::onUserBanned,           //
        "UserUnbannedEvent", &KickChatServer::onUserUnbanned,       //
        "SubscriptionEvent", &KickChatServer::onSubscriptionEvent,  //
        "GiftedSubscriptionsEvent",
        &KickChatServer::onGiftedSubscriptionEvent,  //
        "PinnedMessageCreatedEvent",
        &KickChatServer::onPinnedMessageCreatedEvent,  //
        "PinnedMessageDeletedEvent",
        &KickChatServer::onPinnedMessageDeletedEvent,                     //
        "RewardRedeemedEvent", &KickChatServer::onRewardRedeemedEvent,    //
        "KicksGifted", &KickChatServer::onKicksGiftedEvent,               //
        "StreamHostEvent", &KickChatServer::onStreamHostEvent,            //
        "ChatroomUpdatedEvent", &KickChatServer::onChatroomUpdatedEvent,  //
        "ChatSettingsChanged", &KickChatServer::onChatroomUpdatedEvent,   //
        "PredictionCreated", &KickChatServer::onPredictionEvent,          //
        "PredictionUpdated", &KickChatServer::onPredictionEvent,          //
        "PollUpdateEvent", &KickChatServer::onPollUpdateEvent,            //
        "PollDeleteEvent", &KickChatServer::onPollDeleteEvent,            //

        // ignored
        "KicksLeaderboardUpdated", &KickChatServer::onKnownIgnoredMessage,  //
        "GiftsLeaderboardUpdated", &KickChatServer::onKnownIgnoredMessage,  //
        // old sub events
        "ChannelSubscriptionEvent", &KickChatServer::onKnownIgnoredMessage,  //
        "LuckyUsersWhoGotGiftSubscriptionsEvent",
        &KickChatServer::onKnownIgnoredMessage,  //
        // v1 stream host event
        "StreamHostedEvent", &KickChatServer::onKnownIgnoredMessage,  //
        // seems to be for subscriptions too
        "ChatMessageSentEvent", &KickChatServer::onKnownIgnoredMessage  //
    );

    if (!fn)
    {
        return false;  // no handler
    }

    std::shared_ptr<KickChannel> channel;
    if (roomID != 0)
    {
        channel = this->findByRoomID(roomID);
    }
    else
    {
        channel = this->findByChannelID(channelID);
    }

    if (!channel)
    {
        qCWarning(chatterinoKick)
            << "No channel found for room" << roomID << "channel" << channelID;
        return true;  // technically it's handled, we just don't have a channel
    }

    (this->*fn)(channel.get(), data);
    return true;
}

// NOLINTBEGIN(readability-convert-member-functions-to-static)
void KickChatServer::onChatMessage(KickChannel *channel, BoostJsonObject data)
{
    auto [msg, highlight] = KickMessageBuilder::makeChatMessage(channel, data);
    if (msg)
    {
        bool ok = false;
        const auto userID = msg->userID.toULongLong(&ok);
        if (ok)
        {
            this->requestSeventvCosmetics(userID, msg->displayName);
        }
        channel->updateOwnIdentityFromMessage(*msg);

        if (channel->tryReplacePendingSentMessage(msg))
        {
            return;
        }

        channel->applySimilarityFilters(msg);

        if (!msg->flags.has(MessageFlag::Similar) ||
            (!getSettings()->hideSimilar &&
             getSettings()->shownSimilarTriggerHighlights))
        {
            MessageBuilder::triggerHighlights(channel, highlight);
        }

        const auto highlighted = msg->flags.has(MessageFlag::Highlighted);
        const auto showInMentions = msg->flags.has(MessageFlag::ShowInMentions);

        if (highlighted && showInMentions)
        {
            // yes, we add this to the Twitch channel
            getApp()->getTwitch()->getMentionsChannel()->addMessage(
                msg, MessageContext::Original);
        }
        channel->addMessage(msg, MessageContext::Original);
    }
}

void KickChatServer::requestSeventvCosmetics(uint64_t userID,
                                             const QString &userName)
{
    if (userID == 0)
    {
        return;
    }

    {
        std::lock_guard lock(this->requestedSeventvCosmeticUsersMutex_);
        if (!this->requestedSeventvCosmeticUsers_.emplace(userID).second)
        {
            return;
        }
    }

    auto *api = getApp()->getSeventvAPI();
    if (!api)
    {
        std::lock_guard lock(this->requestedSeventvCosmeticUsersMutex_);
        this->requestedSeventvCosmeticUsers_.erase(userID);
        return;
    }

    const QPointer<KickChatServer> weakThis(this);
    const auto normalizedUserName = userName.toLower();
    const QJsonObject preferredConnection{
        {"id", QString::number(userID)},
        {"platform", "KICK"},
        {"username", normalizedUserName},
        {"display_name", userName},
    };
    api->getUserByKickID(
        userID,
        [weakThis, preferredConnection](const QJsonObject &json) {
            if (!weakThis)
            {
                return;
            }
            SeventvEmotes::applyUserCosmetics(json, preferredConnection);
        },
        [weakThis, userID](const NetworkResult &result) {
            if (!weakThis)
            {
                return;
            }
            const auto status = result.status();
            if (!status || *status >= 500)
            {
                std::lock_guard lock(
                    weakThis->requestedSeventvCosmeticUsersMutex_);
                weakThis->requestedSeventvCosmeticUsers_.erase(userID);
            }
        });
}

void KickChatServer::onUserBanned(KickChannel *channel, BoostJsonObject data)
{
    auto now = QDateTime::currentDateTime();
    auto msg = KickMessageBuilder::makeTimeoutMessage(channel, now, data);
    if (msg)
    {
        channel->addOrReplaceTimeout(msg, now);
    }
    auto duration = data["duration"].toInt64();
    auto cur = getApp()->getAccounts()->kick.current();
    if (!cur->isAnonymous() && duration > 0)
    {
        auto userID = data["user"]["id"].toUint64();
        if (cur->userID() == userID)
        {
            channel->setSendWait(std::chrono::minutes{duration});
        }
    }
}

void KickChatServer::onUserUnbanned(KickChannel *channel, BoostJsonObject data)
{
    auto msg = KickMessageBuilder::makeUntimeoutMessage(channel, data);
    if (msg)
    {
        channel->addMessage(msg, MessageContext::Original);
    }

    auto cur = getApp()->getAccounts()->kick.current();
    if (!cur->isAnonymous())
    {
        auto userID = data["user"]["id"].toUint64();
        if (cur->userID() == userID)
        {
            channel->setSendWait(std::chrono::seconds{0});
        }
    }
}

void KickChatServer::onMessageDeleted(KickChannel *channel,
                                      BoostJsonObject data)
{
    auto messageID = data["message"]["id"].toQString();
    auto msg = channel->findMessageByID(messageID);
    if (!msg)
    {
        return;
    }

    msg->flags.set(MessageFlag::Disabled, MessageFlag::InvalidReplyTarget);
    if (!getSettings()->hideDeletionActions)
    {
        channel->addMessage(MessageBuilder::makeDeletionMessageFromIRC(msg),
                            MessageContext::Original);
    }
}

void KickChatServer::onChatroomClear(KickChannel *channel,
                                     BoostJsonObject /* data */)
{
    auto now = QDateTime::currentDateTime();
    auto clear = KickMessageBuilder::makeClearChatMessage(now, {});
    channel->disableAllMessages();
    channel->addOrReplaceClearChat(clear, now);
}

void KickChatServer::onPinnedMessageCreatedEvent(KickChannel *channel,
                                                 BoostJsonObject data)
{
    const auto message = data["message"].toObject();
    const auto sender = message["sender"].toObject();
    const auto now = QDateTime::currentDateTimeUtc();

    PinnedChatMessage pinned;
    pinned.platform = PinnedChatMessage::Platform::Kick;
    pinned.messageID = message["id"].toQString();
    pinned.messageText = message["content"].toQString(
        message["text"].toQString()).simplified();
    pinned.senderUserID = QString::number(sender["id"].toUint64());
    pinned.senderLogin = sender["slug"].toQString(
        sender["username"].toQString().toLower());
    pinned.senderDisplayName = sender["username"].toQString();
    pinned.pinnedByDisplayName =
        data["pinnedBy"]["username"].toQString();
    pinned.startsAt = now;

    bool durationOk = false;
    auto duration = data["duration"].toQString().toInt(&durationOk);
    if (!durationOk)
    {
        duration = static_cast<int>(data["duration"].toInt64());
    }
    if (duration > 0)
    {
        pinned.endsAt = now.addSecs(duration);
    }
    channel->setPinnedMessage(std::move(pinned));
}

void KickChatServer::onPinnedMessageDeletedEvent(KickChannel *channel,
                                                 BoostJsonObject /*data*/)
{
    channel->setPinnedMessage(std::nullopt);
}

void KickChatServer::onStreamHostEvent(KickChannel *channel,
                                       BoostJsonObject data)
{
    channel->addMessage(KickMessageBuilder::makeHostMessage(channel, data),
                        MessageContext::Original);
}

void KickChatServer::onSubscriptionEvent(KickChannel *channel,
                                         BoostJsonObject data)
{
    auto [first, second, alert] =
        KickMessageBuilder::makeSubscriptionMessage(channel, data);
    if (first)
    {
        MessageBuilder::triggerHighlights(channel, alert);
        channel->addMessage(first, MessageContext::Original);
    }
    channel->addMessage(second, MessageContext::Original);
}

void KickChatServer::onGiftedSubscriptionEvent(KickChannel *channel,
                                               BoostJsonObject data)
{
    auto msg = KickMessageBuilder::makeGiftedSubscriptionMessage(channel, data);
    if (msg)
    {
        channel->addMessage(msg, MessageContext::Original);
    }
}

void KickChatServer::onRewardRedeemedEvent(KickChannel *channel,
                                           BoostJsonObject data)
{
    auto msg = KickMessageBuilder::makeRewardRedeemedMessage(channel, data);
    if (msg)
    {
        channel->addMessage(msg, MessageContext::Original);
    }
}

void KickChatServer::onKicksGiftedEvent(KickChannel *channel,
                                        BoostJsonObject data)
{
    auto msg = KickMessageBuilder::makeKicksGiftedMessage(channel, data);
    if (msg)
    {
        channel->addMessage(msg, MessageContext::Original);
    }
}

void KickChatServer::onChatroomUpdatedEvent(KickChannel *channel,
                                            BoostJsonObject data)
{
    auto settings = data;
    if (data["settings"].isObject())
    {
        settings = data["settings"].toObject();
    }

    auto subscribersMode = settings["subscribers_only_mode"].toObject();
    if (subscribersMode.empty())
    {
        subscribersMode = settings["subscribers_mode"].toObject();
    }
    auto emotesMode = settings["emotes_only_mode"].toObject();
    if (emotesMode.empty())
    {
        emotesMode = settings["emotes_mode"].toObject();
    }

    KickChannel::RoomModes newMode{
        .subscribersMode = subscribersMode["enabled"].toBool(),
        .emotesMode = emotesMode["enabled"].toBool(),
    };
    auto slowMode = settings["slow_mode"].toObject();
    if (slowMode["enabled"].toBool())
    {
        auto duration = slowMode["duration_seconds"].toInt64();
        if (duration == 0)
        {
            duration = slowMode["message_interval"].toInt64();
        }
        newMode.slowModeDuration = std::chrono::seconds{duration};
    }
    auto followersMode = settings["followers_only_mode"].toObject();
    const bool currentSettings = !followersMode.empty();
    if (!currentSettings)
    {
        followersMode = settings["followers_mode"].toObject();
    }
    if (followersMode["enabled"].toBool())
    {
        if (currentSettings)
        {
            newMode.followersModeDuration =
                std::chrono::duration_cast<std::chrono::minutes>(
                    std::chrono::seconds{
                        followersMode["duration_seconds"].toInt64()});
        }
        else
        {
            newMode.followersModeDuration = std::chrono::minutes{
                followersMode["min_duration"].toInt64()};
        }
    }

    const auto &oldMode = channel->roomModes();

    if (newMode.subscribersMode != oldMode.subscribersMode)
    {
        channel->addMessage(KickMessageBuilder::makeRoomModeMessage(
                                channel, u"Subscribers"_s,
                                newMode.subscribersMode, std::nullopt),
                            MessageContext::Original);
    }
    if (newMode.emotesMode != oldMode.emotesMode)
    {
        channel->addMessage(
            KickMessageBuilder::makeRoomModeMessage(
                channel, u"Emote-only"_s, newMode.emotesMode, std::nullopt),
            MessageContext::Original);
    }
    if (newMode.slowModeDuration != oldMode.slowModeDuration)
    {
        channel->addMessage(
            KickMessageBuilder::makeRoomModeMessage(
                channel, u"Slow"_s, newMode.slowModeDuration.has_value(),
                newMode.slowModeDuration),
            MessageContext::Original);
    }
    if (newMode.followersModeDuration != oldMode.followersModeDuration)
    {
        channel->addMessage(KickMessageBuilder::makeRoomModeMessage(
                                channel, u"Followers-only"_s,
                                newMode.followersModeDuration.has_value(),
                                newMode.followersModeDuration),
                            MessageContext::Original);
    }

    channel->updateRoomModes(newMode);
}

void KickChatServer::onPredictionEvent(KickChannel *channel,
                                             BoostJsonObject data)
{
    auto rawPrediction = data["prediction"].toObject();
    if (rawPrediction.empty())
    {
        rawPrediction = data;
    }

    const auto readID = [](BoostJsonValue value) {
        auto id = value.toQString().trimmed();
        if (!id.isEmpty())
        {
            return id;
        }

        const auto numericID = value.toUint64();
        return numericID == 0 ? QString{} : QString::number(numericID);
    };
    const auto readDateTime = [](BoostJsonValue value) {
        const auto parsed =
            QDateTime::fromString(value.toQString().trimmed(), Qt::ISODate);
        return parsed.isValid() ? parsed.toUTC() : QDateTime{};
    };

    KickPrediction prediction;
    prediction.id = readID(rawPrediction["id"]);
    prediction.title = rawPrediction["title"].toQString().trimmed();
    prediction.state = rawPrediction["state"].toQString().trimmed().toUpper();
    prediction.durationSeconds =
        static_cast<int>(rawPrediction["duration"].toInt64());
    if (prediction.durationSeconds <= 0)
    {
        prediction.durationSeconds =
            static_cast<int>(rawPrediction["duration_seconds"].toInt64());
    }
    prediction.createdAt = readDateTime(rawPrediction["created_at"]);
    prediction.updatedAt = readDateTime(rawPrediction["updated_at"]);
    prediction.lockedAt = readDateTime(rawPrediction["locked_at"]);
    prediction.winningOutcomeID =
        readID(rawPrediction["winning_outcome_id"]);

    for (const auto rawOutcome : rawPrediction["outcomes"].toArray())
    {
        const auto outcome = rawOutcome.toObject();
        const auto title = outcome["title"].toQString().trimmed();
        if (title.isEmpty())
        {
            continue;
        }

        prediction.outcomes.push_back({
            .id = readID(outcome["id"]),
            .title = title,
            .totalVoteAmount =
                static_cast<int>(outcome["total_vote_amount"].toInt64()),
            .voteCount = static_cast<int>(outcome["vote_count"].toInt64()),
        });
    }

    if (prediction.state.isEmpty())
    {
        return;
    }
    if (prediction.isOpen() &&
        (prediction.title.isEmpty() || prediction.outcomes.empty()))
    {
        qCWarning(chatterinoKick)
            << "Ignored incomplete Kick prediction event for"
            << channel->getName();
        return;
    }

    channel->updatePrediction(std::move(prediction));
}

void KickChatServer::onPollUpdateEvent(KickChannel *channel,
                                           BoostJsonObject data)
{
    auto rawPoll = data["poll"].toObject();
    if (rawPoll.empty())
    {
        rawPoll = data;
    }

    const auto readID = [](BoostJsonValue value) {
        auto id = value.toQString().trimmed();
        if (!id.isEmpty())
        {
            return id;
        }

        const auto numericID = value.toUint64();
        return numericID == 0 ? QString{} : QString::number(numericID);
    };

    KickPoll poll;
    poll.id = readID(rawPoll["id"]);
    poll.title = rawPoll["title"].toQString().trimmed();
    if (poll.title.isEmpty())
    {
        poll.title = rawPoll["subject"].toQString().trimmed();
    }
    if (poll.title.isEmpty())
    {
        poll.title = rawPoll["question"].toQString().trimmed();
    }

    for (const auto rawOption : rawPoll["options"].toArray())
    {
        const auto option = rawOption.toObject();
        auto title = option["title"].toQString().trimmed();
        if (title.isEmpty())
        {
            title = option["name"].toQString().trimmed();
        }
        if (title.isEmpty())
        {
            title = rawOption.toQString().trimmed();
        }
        if (title.isEmpty())
        {
            continue;
        }

        auto votes = static_cast<int>(option["votes"].toInt64());
        if (votes == 0)
        {
            votes = static_cast<int>(option["votes_count"].toInt64());
        }
        if (votes == 0)
        {
            votes = static_cast<int>(option["vote_count"].toInt64());
        }
        poll.options.push_back({
            .id = readID(option["id"]),
            .title = title,
            .votes = std::max(votes, 0),
        });
    }

    if (poll.title.isEmpty() || poll.options.size() < 2)
    {
        qCWarning(chatterinoKick)
            << "Ignored incomplete Kick poll event for" << channel->getName();
        return;
    }

    auto remaining = rawPoll.contains("remaining")
                         ? static_cast<int>(rawPoll["remaining"].toInt64())
                         : static_cast<int>(rawPoll["duration"].toInt64());
    auto resultDisplayDuration =
        static_cast<int>(rawPoll["result_display_duration"].toInt64());
    if (resultDisplayDuration <= 0)
    {
        resultDisplayDuration = 15;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    poll.state = remaining > 0 ? QStringLiteral("ACTIVE")
                               : QStringLiteral("RESULTS");
    poll.createdAt =
        QDateTime::fromString(rawPoll["created_at"].toQString(), Qt::ISODate)
            .toUTC();
    if (!poll.createdAt.isValid())
    {
        poll.createdAt = now;
    }
    poll.endsAt = now.addSecs(std::max(remaining, 0));
    poll.hideAt = poll.endsAt.addSecs(resultDisplayDuration);
    channel->updatePoll(std::move(poll));
}

void KickChatServer::onPollDeleteEvent(KickChannel *channel,
                                           BoostJsonObject /*data*/)
{
    channel->updatePoll(std::nullopt);
}

void KickChatServer::onKnownIgnoredMessage(KickChannel * /*channel*/,
                                           BoostJsonObject /*data*/)
{
    // nop
}

// NOLINTEND(readability-convert-member-functions-to-static)

void KickChatServer::onJoin(uint64_t roomID) const
{
    auto existing = this->findByRoomID(roomID);
    if (!existing)
    {
        qCWarning(chatterinoKick) << "No channel found for room" << roomID;
        return;
    }
    existing->addSystemMessage("joined");
}

std::shared_ptr<const EmoteMap> KickChatServer::globalEmotes() const
{
    if (!this->globalEmotes_)
    {
        return EMPTY_EMOTE_MAP;
    }
    return this->globalEmotes_;
}

void KickChatServer::loadGlobalEmotesIfNeeded()
{
    if (this->globalEmotes_ || this->loadingGlobalEmotes_)
    {
        return;
    }
    this->loadingGlobalEmotes_ = true;
    KickApi::privateEmotesInChannel(
        u"kick"_s,
        [self = QPointer(this)](
            const ExpectedStr<std::vector<KickPrivateEmoteSetInfo>> &res) {
            if (!self)
            {
                return;
            }
            self->loadingGlobalEmotes_ = false;
            if (!res)
            {
                qCWarning(chatterinoKick)
                    << "Failed to fetch global emotes:" << res.error();
                self->globalEmotes_ = EMPTY_EMOTE_MAP;
                return;
            }
            auto emotes = std::make_shared<EmoteMap>();
            for (const auto &set : *res)
            {
                if (set.userID)
                {
                    continue;  // local set
                }
                for (const auto &emoteInfo : set.emotes)
                {
                    if (emoteInfo.subscribersOnly)
                    {
                        continue;
                    }
                    auto id = QString::number(emoteInfo.emoteID);
                    auto emote = KickEmotes::emoteForID(id, emoteInfo.name);
                    (*emotes)[emote->name] = emote;
                }
            }
            self->globalEmotes_ = std::move(emotes);
            qCDebug(chatterinoKick)
                << "Loaded" << self->globalEmotes_->size() << "global emotes";
        });
}

void KickChatServer::registerRoomID(uint64_t roomID, uint64_t channelID,
                                    std::weak_ptr<KickChannel> chan)
{
    this->channelsByRoomID[roomID] = chan;
    this->channelsByChannelID[channelID] = std::move(chan);
}

void KickChatServer::initializeSeventvEventApi(SeventvEventAPI *api)
{
    if (!api)
    {
        return;
    }

    this->signalHolder_.managedConnect(
        api->signals_.emoteAdded, [&](const auto &data) {
            postToThread(
                [this, data] {
                    this->forEachSeventvEmoteSet(data.emoteSetID,
                                                 [data](KickChannel &chan) {
                                                     chan.addSeventvEmote(data);
                                                 });
                },
                this);
        });
    this->signalHolder_.managedConnect(
        api->signals_.emoteUpdated, [&](const auto &data) {
            postToThread(
                [this, data] {
                    this->forEachSeventvEmoteSet(
                        data.emoteSetID, [data](KickChannel &chan) {
                            chan.updateSeventvEmote(data);
                        });
                },
                this);
        });
    this->signalHolder_.managedConnect(
        api->signals_.emoteRemoved, [&](const auto &data) {
            postToThread(
                [this, data] {
                    this->forEachSeventvEmoteSet(
                        data.emoteSetID, [data](KickChannel &chan) {
                            chan.removeSeventvEmote(data);
                        });
                },
                this);
        });
    this->signalHolder_.managedConnect(
        api->signals_.userUpdated, [&](const auto &data) {
            this->forEachSeventvUser(data.userID, [data](KickChannel &chan) {
                chan.updateSeventvUser(data);
            });
        });
    this->signalHolder_.managedConnect(
        api->signals_.personalEmoteSetAdded,
        [&](const seventv::eventapi::PersonalEmoteSetAdded &data) {
            QVarLengthArray<QString, 1> names;
            for (const auto &user : data.connections)
            {
                if (const auto *u =
                        std::get_if<seventv::eventapi::KickUser>(&user))
                {
                    names.emplace_back(u->userName);
                }
            }
            if (names.empty())
            {
                return;
            }

            postToThread(
                [this, emoteSet = data.emoteSet, names{std::move(names)}] {
                    this->forEachChannel([&](auto &chan) {
                        for (const auto &name : names)
                        {
                            chan.upsertPersonalSeventvEmotes(name, emoteSet);
                        }
                    });
                },
                this);
        });

    this->forEachChannel([api](KickChannel &channel) {
        if (channel.userID() != 0)
        {
            api->subscribeKickChannel(QString::number(channel.userID()));
        }
    });
}

void KickChatServer::subscribeSeventvCosmetics(
    const std::shared_ptr<KickChannel> &channel) const
{
    if (channel == nullptr || channel->userID() == 0)
    {
        return;
    }

    if (auto *api = getApp()->getSeventvEventAPI())
    {
        api->subscribeKickChannel(QString::number(channel->userID()));
    }
}

}  // namespace chatterino
