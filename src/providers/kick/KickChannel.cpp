#include "providers/kick/KickChannel.hpp"

#include "Application.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "messages/MessageFlag.hpp"
#include "messages/MessageThread.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickBadges.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/kick/KickEmotes.hpp"
#include "providers/kick/KickLiveUpdates.hpp"
#include "providers/kick/KickMessageBuilder.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvEventAPI.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Settings.hpp"
#include "util/BoostJsonWrap.hpp"
#include "util/FormatTime.hpp"
#include "util/Helpers.hpp"
#include "util/PostToThread.hpp"

#include <boost/json/parse.hpp>

#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <atomic>
#include <cstdint>

using namespace Qt::Literals;
using namespace std::chrono_literals;

namespace chatterino {

namespace {

std::atomic_uint64_t NEXT_LOCAL_MESSAGE_ID = 1;

struct KickPollState {
    std::optional<KickPoll> poll;
};

std::unordered_map<const KickChannel *, std::unique_ptr<KickPollState>>
    KICK_POLL_STATES;

struct KickPredictionRefreshState {
    uint64_t revision = 0;
    uint64_t generation = 0;
};

std::unordered_map<const KickChannel *,
                   std::unique_ptr<KickPredictionRefreshState>>
    KICK_PREDICTION_REFRESH_STATES;

KickPollState &kickPollState(const KickChannel *channel)
{
    auto &state = KICK_POLL_STATES[channel];
    if (!state)
    {
        state = std::make_unique<KickPollState>();
    }
    return *state;
}

KickPredictionRefreshState &kickPredictionRefreshState(
    const KickChannel *channel)
{
    auto &state = KICK_PREDICTION_REFRESH_STATES[channel];
    if (!state)
    {
        state = std::make_unique<KickPredictionRefreshState>();
    }
    return *state;
}

constexpr uint16_t KICK_LEVEL_BADGE_IMAGE_SIZE = 17;
constexpr int LOCAL_SENT_MESSAGE_DELAY_MS = 250;

QString predictionID(const QJsonValue &value)
{
    const auto stringID = value.toString().trimmed();
    if (!stringID.isEmpty())
    {
        return stringID;
    }

    bool ok = false;
    const auto numericID = value.toVariant().toULongLong(&ok);
    return ok && numericID != 0 ? QString::number(numericID) : QString{};
}

QDateTime predictionDateTime(const QJsonValue &value)
{
    const auto parsed =
        QDateTime::fromString(value.toString().trimmed(), Qt::ISODate);
    return parsed.isValid() ? parsed.toUTC() : QDateTime{};
}

std::optional<KickPrediction> parseLatestPrediction(
    const QJsonObject &rawPrediction)
{
    KickPrediction prediction;
    prediction.id =
        predictionID(rawPrediction.value(QStringLiteral("id")));
    prediction.title =
        rawPrediction.value(QStringLiteral("title")).toString().trimmed();
    prediction.state = rawPrediction.value(QStringLiteral("state"))
                           .toString()
                           .trimmed()
                           .toUpper();
    prediction.durationSeconds =
        rawPrediction.value(QStringLiteral("duration")).toInt();
    if (prediction.durationSeconds <= 0)
    {
        prediction.durationSeconds =
            rawPrediction.value(QStringLiteral("duration_seconds")).toInt();
    }
    prediction.createdAt = predictionDateTime(
        rawPrediction.value(QStringLiteral("created_at")));
    prediction.updatedAt = predictionDateTime(
        rawPrediction.value(QStringLiteral("updated_at")));
    prediction.lockedAt = predictionDateTime(
        rawPrediction.value(QStringLiteral("locked_at")));
    prediction.winningOutcomeID = predictionID(
        rawPrediction.value(QStringLiteral("winning_outcome_id")));

    for (const auto &rawOutcome :
         rawPrediction.value(QStringLiteral("outcomes")).toArray())
    {
        const auto outcome = rawOutcome.toObject();
        const auto title =
            outcome.value(QStringLiteral("title")).toString().trimmed();
        if (title.isEmpty())
        {
            continue;
        }

        prediction.outcomes.push_back({
            .id = predictionID(outcome.value(QStringLiteral("id"))),
            .title = title,
            .totalVoteAmount =
                outcome.value(QStringLiteral("total_vote_amount")).toInt(),
            .voteCount =
                outcome.value(QStringLiteral("vote_count")).toInt(),
        });
    }

    if (prediction.state.isEmpty() ||
        (prediction.isOpen() &&
         (prediction.title.isEmpty() || prediction.outcomes.empty())))
    {
        return std::nullopt;
    }

    return prediction;
}

QString makeLocalKickMessageID()
{
    return u"local-kick-"_s %
           QString::number(QDateTime::currentMSecsSinceEpoch()) % u'-' %
           QString::number(NEXT_LOCAL_MESSAGE_ID.fetch_add(1));
}

bool isKickLevelBadge(const KickPrivateUserBadgeInfo &badge)
{
    auto type = badge.type.toLower();
    return type == u"level"_s || type == u"kick_level"_s ||
           type == u"kick-level"_s || type == u"level_badge"_s ||
           type == u"level-badge"_s || type.contains(u"level"_s) ||
           badge.level > 0 ||
           badge.imageUrl.contains(u"/chat/badges/"_s, Qt::CaseInsensitive);
}

std::unique_ptr<MessageElement> makeKickLevelBadgeElement(
    const KickPrivateUserBadgeInfo &badge, const QString &slug)
{
    if (badge.imageUrl.isEmpty())
    {
        return nullptr;
    }

    auto name = badge.text;
    if (name.isEmpty() || name.compare(u"level"_s, Qt::CaseInsensitive) == 0)
    {
        name = badge.level > 0 ? u"Level "_s % QString::number(badge.level)
                               : u"Kick Level"_s;
    }

    auto emote = std::make_shared<const Emote>(Emote{
        .name = {name},
        .images = ImageSet(Image::fromAutoscaledUrl(
            {badge.imageUrl}, KICK_LEVEL_BADGE_IMAGE_SIZE)),
        .tooltip = Tooltip{name},
        .homePage = {u"https://kick.com/" % slug},
    });
    return std::make_unique<BadgeElement>(emote,
                                          MessageElementFlag::BadgeKickLevel);
}

void normalizeRecentMessageMetadata(boost::json::object &message)
{
    auto metadataIt = message.find("metadata");
    if (metadataIt == message.end() || !metadataIt->value().is_string())
    {
        return;
    }

    auto &metadataValue = metadataIt->value();
    const auto &metadataString = metadataValue.as_string();
    if (metadataString.empty())
    {
        return;
    }

    boost::system::error_code ec;
    auto parsed = boost::json::parse(
        std::string_view(metadataString.data(), metadataString.size()), ec);
    if (!ec && parsed.is_object())
    {
        metadataValue = std::move(parsed);
    }
}

std::vector<MessagePtr> buildKickRecentMessages(
    KickChannel *channel, const std::vector<boost::json::object> &rawMessages)
{
    std::vector<MessagePtr> messages;
    messages.reserve(rawMessages.size());

    for (const auto &rawMessage : rawMessages)
    {
        auto raw = rawMessage;
        normalizeRecentMessageMetadata(raw);

        BoostJsonObject json(raw);
        auto result = KickMessageBuilder::makeChatMessage(channel, json);
        auto msg = std::move(result.first);
        if (!msg)
        {
            continue;
        }

        bool ok = false;
        const auto userID = msg->userID.toULongLong(&ok);
        if (ok)
        {
            getApp()->getKickChatServer()->requestSeventvCosmetics(
                userID, msg->displayName);
        }
        channel->updateOwnIdentityFromMessage(*msg);

        msg->flags.set(MessageFlag::RecentMessage);
        messages.emplace_back(std::move(msg));
    }

    std::sort(messages.begin(), messages.end(),
              [](const auto &lhs, const auto &rhs) {
                  return lhs->serverReceivedTime < rhs->serverReceivedTime;
              });

    return messages;
}

std::vector<MessagePtr> removeExistingMessages(KickChannel *channel,
                                               std::vector<MessagePtr> messages)
{
    std::vector<MessagePtr> newMessages;
    newMessages.reserve(messages.size());

    for (auto &msg : messages)
    {
        if (!msg)
        {
            continue;
        }

        if (!msg->id.isEmpty() && channel->findMessageByID(msg->id))
        {
            continue;
        }

        newMessages.emplace_back(std::move(msg));
    }

    return newMessages;
}

}  // namespace

KickChannel::KickChannel(const QString &name)
    : Channel(name.toLower(), Type::Kick)
    , ChannelChatters(static_cast<Channel &>(*this))
    , displayName_(name)
    , slug_(KickApi::slugify(this->getName()))
    , kickChannelEmotes_(std::make_shared<const EmoteMap>())
    , seventvEmotes_(std::make_shared<const EmoteMap>())
{
    this->setMentionFlag(MessageElementFlag::KickUsername);

    this->sendWaitTimer_.setInterval(1s);
    this->sendWaitTimer_.setSingleShot(false);
    QObject::connect(&this->sendWaitTimer_, &QTimer::timeout, [this] {
        this->emitSendWait();
    });
}

KickChannel::~KickChannel()
{
    KICK_POLL_STATES.erase(this);

    auto *app = getApp();
    if (app)
    {
        app->getKickChatServer()->liveUpdates().leaveRoom(this->roomID(),
                                                          this->channelID());
        auto *eventApi = app->getSeventvEventAPI();
        if (eventApi)
        {
            eventApi->unsubscribeKickChannel(QString::number(this->userID()));
        }
    }
}

void KickChannel::initialize(const UserInit &init)
{
    this->setUserInfo(init);
    this->resolveChannelInfo();
}

std::shared_ptr<KickChannel> KickChannel::sharedFromThis()
{
    return std::static_pointer_cast<KickChannel>(this->shared_from_this());
}

std::weak_ptr<KickChannel> KickChannel::weakFromThis()
{
    return this->sharedFromThis();
}

std::pair<std::shared_ptr<MessageThread>, MessagePtr>
    KickChannel::getOrCreateThread(const QString &messageID)
{
    auto existingIt = this->threads_.find(messageID);
    if (existingIt != this->threads_.end())
    {
        auto existing = existingIt->second.lock();
        if (existing)
        {
            return {existing, existing->root()};
        }
    }

    auto msg = this->findMessageByID(messageID);
    if (!msg)
    {
        return {nullptr, nullptr};
    }

    if (msg->replyThread)
    {
        return {msg->replyThread, msg};
    }

    auto thread = std::make_shared<MessageThread>(msg);
    this->threads_[messageID] = thread;
    return {thread, msg};
}

// FIXME: These are largely the same as in TwitchChannel. They should be
// combined. However, we also want to avoid merge conflicts as much as possible.

std::shared_ptr<const EmoteMap> KickChannel::kickChannelEmotes() const
{
    return this->kickChannelEmotes_.get();
}

void KickChannel::reloadSeventvEmotes(bool manualRefresh)
{
    bool cacheHit = readProviderEmotesCache(
        u"kick." % QString::number(this->userID()), "seventv",
        [this](const auto &jsonDoc) {
            const auto json = jsonDoc.object();
            const auto emoteSet = json["emote_set"].toObject();
            const auto parsedEmotes = emoteSet["emotes"].toArray();
            auto emoteMap = seventv::detail::parseEmotes(
                parsedEmotes, SeventvEmoteSetKind::Channel);
            this->seventvEmotes_.set(
                std::make_shared<const EmoteMap>(emoteMap));
        });

    SeventvEmotes::loadKickChannelEmotes(
        this->weakFromThis(), this->userID(),
        [weak = this->weakFromThis()](EmoteMap &&emotes,
                                      const SeventvEmotes::ChannelInfo &info) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }

            self->seventvEmotes_.set(
                std::make_shared<const EmoteMap>(std::move(emotes)));
            self->seventvKickConnectionIndex_ = info.twitchConnectionIndex;
            self->updateSeventvData(info.userID, info.emoteSetID);
        },
        manualRefresh, cacheHit);
}

std::shared_ptr<const EmoteMap> KickChannel::seventvEmotes() const
{
    return this->seventvEmotes_.get();
}

EmotePtr KickChannel::seventvEmote(const EmoteName &name) const
{
    auto emotes = this->seventvEmotes_.get();

    auto it = emotes->find(name);
    if (it != emotes->end())
    {
        return it->second;
    }
    return nullptr;
}

EmotePtr KickChannel::subscriberBadgeForMonths(uint64_t months) const
{
    if (this->subscriberBadges_.empty())
    {
        return nullptr;
    }

    const SubscriberBadge *best = nullptr;
    for (const auto &badge : this->subscriberBadges_)
    {
        if (!badge.emote)
        {
            continue;
        }

        if (months == 0)
        {
            return badge.emote;
        }

        if (!best || badge.months <= months)
        {
            best = &badge;
        }

        if (badge.months > months)
        {
            break;
        }
    }

    return best ? best->emote : nullptr;
}

void KickChannel::addSeventvEmote(
    const seventv::eventapi::EmoteAddDispatch &dispatch)
{
    if (!SeventvEmotes::addEmote(this->seventvEmotes_, dispatch))
    {
        return;
    }

    this->addOrReplaceSeventvAddRemove(true, dispatch.actorName,
                                       dispatch.emoteJson["name"].toString());
}

void KickChannel::updateSeventvEmote(
    const seventv::eventapi::EmoteUpdateDispatch &dispatch)
{
    if (!SeventvEmotes::updateEmote(this->seventvEmotes_, dispatch))
    {
        return;
    }

    auto builder =
        MessageBuilder(liveUpdatesUpdateEmoteMessage, "7TV", dispatch.actorName,
                       dispatch.emoteName, dispatch.oldEmoteName);
    this->addMessage(builder.release(), MessageContext::Original);
}

void KickChannel::removeSeventvEmote(
    const seventv::eventapi::EmoteRemoveDispatch &dispatch)
{
    auto removed = SeventvEmotes::removeEmote(this->seventvEmotes_, dispatch);
    if (!removed)
    {
        return;
    }

    this->addOrReplaceSeventvAddRemove(false, dispatch.actorName,
                                       (*removed)->name.string);
}

void KickChannel::updateSeventvUser(
    const seventv::eventapi::UserConnectionUpdateDispatch &dispatch)
{
    if (dispatch.connectionIndex != this->seventvKickConnectionIndex_)
    {
        // A different connection was updated
        return;
    }

    this->updateSeventvData(this->seventvUserID_, dispatch.emoteSetID);
    SeventvEmotes::getEmoteSet(
        dispatch.emoteSetID,
        [this, weak = weakOf<Channel>(this), dispatch](auto &&emotes,
                                                       const auto &name) {
            postToThread([this, weak, dispatch, emotes, name]() {
                if (auto shared = weak.lock())
                {
                    this->seventvEmotes_.set(
                        std::make_shared<EmoteMap>(emotes));
                    auto builder =
                        MessageBuilder(liveUpdatesUpdateEmoteSetMessage, "7TV",
                                       dispatch.actorName, name);
                    this->addMessage(builder.release(),
                                     MessageContext::Original);
                }
            });
        },
        [this, weak = weakOf<Channel>(this)](const auto &reason) {
            postToThread([this, weak, reason]() {
                if (auto shared = weak.lock())
                {
                    this->seventvEmotes_.set(EMPTY_EMOTE_MAP);
                    this->addSystemMessage(
                        QString("Failed updating 7TV emote set (%1).")
                            .arg(reason));
                }
            });
        });
}

const QString &KickChannel::seventvUserID() const
{
    return this->seventvUserID_;
}

const QString &KickChannel::seventvEmoteSetID() const
{
    return this->seventvEmoteSetID_;
}

bool KickChannel::canSendMessage() const
{
    return getApp()->getAccounts()->kick.isLoggedIn();
}

void KickChannel::sendMessage(const QString &message)
{
    this->sendReply(message, {}, {});
}

void KickChannel::sendMessageAndWaitForEcho(
    const QString &message, SentMessageCallback callback)
{
    this->sendReply(message, {}, std::move(callback));
}

void KickChannel::sendReply(const QString &message, const QString &replyToId)
{
    this->sendReply(message, replyToId, {});
}

void KickChannel::sendReply(const QString &message, const QString &replyToId,
                            SentMessageCallback callback)
{
    if (!getApp()->getAccounts()->kick.isLoggedIn())
    {
        this->addLoginMessage();
        return;
    }

    auto prepared = this->prepareMessage(message);
    if (prepared.isEmpty())
    {
        return;
    }

    if (!this->checkMessageRatelimit())
    {
        return;
    }

    QString localID;
    if (auto pending = KickMessageBuilder::makeSentMessage(
            this, prepared, makeLocalKickMessageID()))
    {
        localID = pending->id;
        this->pendingSentMessages_.push_back(PendingSentMessage{
            .localID = localID,
            .messageText = pending->messageText,
            .createdAt = pending->serverReceivedTime,
            .confirmedCallback = std::move(callback),
        });
        QTimer::singleShot(
            LOCAL_SENT_MESSAGE_DELAY_MS,
            [weak = this->weakFromThis(), localID, pending]() {
                auto self = weak.lock();
                if (!self)
                {
                    return;
                }

                const auto pendingIt = std::find_if(
                    self->pendingSentMessages_.begin(),
                    self->pendingSentMessages_.end(),
                    [&](const auto &pendingSentMessage) {
                        return pendingSentMessage.localID == localID;
                    });
                if (pendingIt == self->pendingSentMessages_.end() ||
                    self->findMessageByID(localID))
                {
                    return;
                }

                self->addMessage(pending, MessageContext::Original);
            });
    }

    this->updateSevenTVActivity();

    const bool sendWaitStarted =
        this->roomModes_.slowModeDuration && !this->hasHighRateLimit();
    if (sendWaitStarted)
    {
        this->setSendWait(*this->roomModes_.slowModeDuration);
    }

    getKickApi()->sendMessage(
        this->userID(), prepared, replyToId,
        [weak = this->weakFromThis(), localID,
         sendWaitStarted](const auto &res) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (res)
            {
                return;  // message sent
            }
            if (self)
            {
                if (sendWaitStarted)
                {
                    self->setSendWait(0s);
                }
                self->markPendingSentMessageFailed(localID);
                self->addSystemMessage(u"Failed to send message: " %
                                       res.error());
            }
        });
}

void KickChannel::updateOwnIdentityFromMessage(const Message &message)
{
    auto account = getApp()->getAccounts()->kick.current();
    if (!account || account->isAnonymous())
    {
        return;
    }

    bool ok = false;
    const auto messageUserID = message.userID.toULongLong(&ok);
    if (!ok || messageUserID != account->userID())
    {
        return;
    }

    CachedOwnIdentity identity;
    identity.displayName = message.displayName;
    identity.loginName = message.loginName;
    identity.userID = message.userID;
    identity.usernameColor = message.usernameColor;

    for (const auto &element : message.elements)
    {
        if (!element ||
            !element->getFlags().hasAny({MessageElementFlag::Badges}))
        {
            continue;
        }
        identity.badges.emplace_back(element->clone());
    }

    if (!identity.displayName.isEmpty())
    {
        this->ownIdentity_.emplace(std::move(identity));
    }
}

const KickChannel::CachedOwnIdentity *KickChannel::ownIdentity() const
{
    if (!this->ownIdentity_)
    {
        return nullptr;
    }
    return &*this->ownIdentity_;
}

bool KickChannel::tryReplacePendingSentMessage(const MessagePtr &message)
{
    auto account = getApp()->getAccounts()->kick.current();
    if (!account || account->isAnonymous() ||
        message->userID != QString::number(account->userID()))
    {
        return false;
    }

    this->prunePendingSentMessages(QDateTime::currentDateTime());

    for (auto it = this->pendingSentMessages_.begin();
         it != this->pendingSentMessages_.end(); ++it)
    {
        if (it->messageText != message->messageText)
        {
            continue;
        }

        auto confirmedCallback = std::move(it->confirmedCallback);
        if (auto pending = this->findMessageByID(it->localID))
        {
            this->replaceMessage(pending, message);
            this->pendingSentMessages_.erase(it);
            if (confirmedCallback)
            {
                confirmedCallback(message);
            }
            return true;
        }

        this->pendingSentMessages_.erase(it);
        if (confirmedCallback)
        {
            confirmedCallback(message);
        }
        return false;
    }

    return false;
}

void KickChannel::prunePendingSentMessages(const QDateTime &now)
{
    auto it = this->pendingSentMessages_.begin();
    while (it != this->pendingSentMessages_.end())
    {
        const auto age = it->createdAt.msecsTo(now);
        if (age > 30'000 ||
            (age > LOCAL_SENT_MESSAGE_DELAY_MS &&
             !this->findMessageByID(it->localID)))
        {
            it = this->pendingSentMessages_.erase(it);
            continue;
        }
        ++it;
    }
}

void KickChannel::markPendingSentMessageFailed(const QString &localID)
{
    if (localID.isEmpty())
    {
        return;
    }

    auto it = std::find_if(this->pendingSentMessages_.begin(),
                           this->pendingSentMessages_.end(),
                           [&](const auto &pending) {
                               return pending.localID == localID;
                           });
    if (it != this->pendingSentMessages_.end())
    {
        this->pendingSentMessages_.erase(it);
    }

    if (auto pending = this->findMessageByID(localID))
    {
        auto replacement = pending->clone();
        replacement->flags.set(MessageFlag::Disabled);
        this->replaceMessage(pending, replacement);
    }
}

void KickChannel::deleteMessage(const QString &messageID)
{
    getKickApi()->deleteChatMessage(
        messageID, [weak = this->weakFromThis()](const auto &res) {
            auto self = weak.lock();
            if (!self || res)
            {
                return;
            }
            self->addSystemMessage(u"Failed to delete message: " % res.error());
        });
}

bool KickChannel::isMod() const
{
    return this->isMod_;
}

void KickChannel::setMod(bool mod)
{
    if (this->isMod_ == mod)
    {
        return;
    }
    this->isMod_ = mod;
    this->userStateChanged.invoke();
}

bool KickChannel::isVip() const
{
    return this->isVip_;
}

void KickChannel::setVip(bool vip)
{
    if (this->isVip_ == vip)
    {
        return;
    }
    this->isVip_ = vip;
    this->userStateChanged.invoke();
}

bool KickChannel::isBroadcaster() const
{
    auto *app = tryGetApp();
    if (app == nullptr)
    {
        return false;
    }

    auto *accounts = app->getAccounts();
    if (accounts == nullptr)
    {
        return false;
    }

    auto currentUser = accounts->kick.current();
    if (!currentUser)
    {
        return false;
    }

    return this->userID() == currentUser->userID();
}

bool KickChannel::hasModRights() const
{
    return this->isMod() || this->isBroadcaster();
}

bool KickChannel::hasHighRateLimit() const
{
    return this->hasModRights() || this->isVip();
}

bool KickChannel::isLive() const
{
    return this->streamData_.isLive;
}

bool KickChannel::canReconnect() const
{
    return true;
}

void KickChannel::reconnect()
{
    this->resolveChannelInfo();

    if (this->roomID() == 0 || this->channelID() == 0)
    {
        this->loadRecentMessagesReconnect();
        return;
    }

    const auto roomID = this->roomID();
    const auto channelID = this->channelID();
    auto *server = getApp()->getKickChatServer();
    server->liveUpdates().reconnect();
    server->liveUpdates().leaveRoom(roomID, channelID);
    this->loadRecentMessagesReconnect([weak = this->weakFromThis(), roomID,
                                       channelID] {
        auto self = weak.lock();
        if (!self)
        {
            return;
        }
        getApp()->getKickChatServer()->liveUpdates().joinRoom(roomID,
                                                               channelID);
    });
}

QString KickChannel::getCurrentStreamID() const
{
    return this->currentStreamID_;
}

void KickChannel::applyStreamData(bool isLive, const QString &streamTitle,
                                  const QString &categoryName,
                                  const QString &thumbnailUrl,
                                  uint64_t viewerCount,
                                  const QDateTime &startTime)
{
    bool changed = false;
    bool liveStatusChanged = false;
    if (this->streamData_.isLive != isLive)
    {
        changed = true;
        liveStatusChanged = true;
        this->streamData_.isLive = isLive;
        this->currentStreamID_.clear();

        if (this->streamData_.isLive)
        {
            this->addMessage(
                MessageBuilder::makeLiveMessage(
                    this->getDisplayName(), QString::number(this->userID()),
                    streamTitle,
                    {MessageFlag::System,
                     MessageFlag::DoNotTriggerNotification}),
                MessageContext::Original);
        }
        else
        {
            this->addMessage(
                MessageBuilder::makeOfflineSystemMessage(
                    this->getDisplayName(), QString::number(this->userID())),
                MessageContext::Original);
        }
    }
    if (this->streamData_.title != streamTitle)
    {
        changed = true;
        this->streamData_.title = streamTitle;
    }
    if (this->streamData_.category != categoryName)
    {
        changed = true;
        this->streamData_.category = categoryName;
    }

    if (this->streamData_.isLive)
    {
        changed = true;
        this->currentStreamID_ =
            QString("kick:%1:%2")
                .arg(this->channelID())
                .arg(startTime.isValid()
                         ? startTime.toUTC().toString(Qt::ISODateWithMs)
                         : QString("live"));
        this->streamData_.thumbnailUrl = thumbnailUrl;
        this->streamData_.viewerCount = viewerCount;
        auto uptimeMinutes = startTime.secsTo(QDateTime::currentDateTime()) / 60;
        this->streamData_.uptime = QString::number(uptimeMinutes / 60) % u"h " %
                                   QString::number(uptimeMinutes % 60) % u"m";
    }
    else
    {
        this->currentStreamID_.clear();
        this->streamData_.thumbnailUrl.clear();
        this->streamData_.viewerCount = 0;
        this->streamData_.uptime.clear();
    }

    if (changed)
    {
        this->streamDataChanged.invoke();
    }

    if (liveStatusChanged)
    {
        this->liveStatusChanged.invoke();
    }
}

void KickChannel::updateStreamData(const KickChannelInfo &info)
{
    assert(info.userID == this->userID());
    this->applyStreamData(info.stream.isLive, info.streamTitle,
                          info.category.name, info.stream.thumbnailUrl,
                          info.stream.viewerCount, info.stream.startTime);
}

void KickChannel::updateStreamData(const KickPrivateChannelInfo &info)
{
    assert(info.user.userID == this->userID());
    this->applyStreamData(info.isLive, info.streamTitle, info.liveCategoryName,
                          info.thumbnailUrl, info.viewerCount, info.startTime);
}

const KickChannel::StreamData &KickChannel::streamData() const
{
    return this->streamData_;
}

const KickChannel::RoomModes &KickChannel::roomModes() const
{
    return this->roomModes_;
}

void KickChannel::updateRoomModes(const RoomModes &modes)
{
    if (modes == this->roomModes_)
    {
        return;
    }

    this->roomModes_ = modes;
    this->roomModesChanged.invoke();

    if (!modes.slowModeDuration || *modes.slowModeDuration == 0s)
    {
        this->setSendWait(0s);
    }
}

const std::optional<KickPrediction> &KickChannel::activePrediction() const
{
    return this->activePrediction_;
}

void KickChannel::refreshPrediction()
{
    const auto slug = this->slug_.trimmed();
    if (slug.isEmpty())
    {
        return;
    }

    auto &refreshState = kickPredictionRefreshState(this);
    const auto requestGeneration = ++refreshState.generation;
    const auto revision = refreshState.revision;
    const auto weak = this->weakFromThis();
    KickApi::privateLatestPrediction(
        slug, [weak, requestGeneration, revision](
                  const ExpectedStr<QJsonObject> &result) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            auto &refreshState = kickPredictionRefreshState(self.get());
            if (refreshState.generation != requestGeneration ||
                refreshState.revision != revision)
            {
                return;
            }

            if (!result)
            {
                qCWarning(chatterinoKick)
                    << *self << "Failed to refresh active prediction:"
                    << result.error();
                return;
            }

            if (result->isEmpty())
            {
                if (self->activePrediction_)
                {
                    refreshState.revision++;
                    self->activePrediction_.reset();
                    self->predictionChanged.invoke();
                }
                return;
            }

            auto prediction = parseLatestPrediction(*result);
            if (!prediction)
            {
                qCWarning(chatterinoKick)
                    << *self << "Ignored incomplete latest prediction";
                return;
            }

            self->updatePrediction(std::move(*prediction));
        });
}

void KickChannel::updatePrediction(KickPrediction prediction)
{
    kickPredictionRefreshState(this).revision++;
    prediction.state = prediction.state.trimmed().toUpper();

    std::optional<KickPrediction> next;
    if (prediction.isOpen())
    {
        next = std::move(prediction);
    }

    if (this->activePrediction_ == next)
    {
        return;
    }

    this->activePrediction_ = std::move(next);
    this->predictionChanged.invoke();
}

const std::optional<KickPoll> &KickChannel::activePoll() const
{
    return kickPollState(this).poll;
}

void KickChannel::updatePoll(std::optional<KickPoll> poll)
{
    const auto now = QDateTime::currentDateTimeUtc();
    if (poll)
    {
        poll->state = poll->state.trimmed().toUpper();
        if (!poll->isVisible() ||
            (poll->hideAt.isValid() && poll->hideAt <= now))
        {
            poll.reset();
        }
    }

    auto &state = kickPollState(this);
    if (state.poll == poll)
    {
        return;
    }

    state.poll = std::move(poll);
    this->predictionChanged.invoke();

    if (!state.poll || !state.poll->hideAt.isValid())
    {
        return;
    }

    const auto hideAt = state.poll->hideAt;
    const auto delayMs =
        std::clamp<qint64>(now.msecsTo(hideAt), 1, 24 * 60 * 60 * 1000);
    const auto weak = this->weakFromThis();
    QTimer::singleShot(static_cast<int>(delayMs), [weak, hideAt] {
        auto channel = weak.lock();
        if (channel == nullptr || !channel->activePoll() ||
            channel->activePoll()->hideAt != hideAt ||
            QDateTime::currentDateTimeUtc() < hideAt)
        {
            return;
        }

        channel->updatePoll(std::nullopt);
    });
}

void KickChannel::setSendWait(std::chrono::seconds waitTime)
{
    if (waitTime <= 0s)
    {
        if (this->sendWaitEnd_)
        {
            this->sendWaitEnd_ = std::nullopt;
            this->emitSendWait();
        }
        return;
    }

    this->sendWaitEnd_ = std::chrono::steady_clock::now() + waitTime;
    if (!this->sendWaitTimer_.isActive())
    {
        this->sendWaitTimer_.start();
        this->emitSendWait();
    }
}

void KickChannel::messageRemovedFromStart(const MessagePtr &msg)
{
    if (msg->replyThread)
    {
        if (msg->replyThread->liveCount(msg) == 0)
        {
            this->threads_.erase(msg->replyThread->rootId());
        }
    }
}

void KickChannel::resolveChannelInfo()
{
    auto weak = this->weakFromThis();
    KickApi::privateChannelInfo(
        this->getName(),
        [weak](const ExpectedStr<KickPrivateChannelInfo> &res) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }

            if (!res)
            {
                qCWarning(chatterinoKick)
                    << *self
                    << "Failed to resolve channel info:" << res.error();
                self->addSystemMessage(u"Failed to resolve channel info: "_s %
                                       res.error());
                return;
            }

            self->slug_ = res->slug;
            self->setSubscriberBadges(res->subscriberBadges);
            self->setUserInfo(UserInit{
                .roomID = res->chatroom.roomID,
                .userID = res->user.userID,
                .channelID = res->channelID,
            });
            auto oldDisplayName =
                std::exchange(self->displayName_, res->user.username);
            if (oldDisplayName != self->displayName_)
            {
                self->displayNameChanged.invoke();
            }

            self->updateRoomModes(RoomModes{
                .subscribersMode = res->chatroom.subscribersMode,
                .emotesMode = res->chatroom.emotesMode,
                // Kick's legacy channel payload reports a stale interval.
                // The current chat-settings request below is authoritative.
                .slowModeDuration = std::nullopt,
                .followersModeDuration = res->chatroom.followersModeDuration,
            });
            KickApi::privateChatSettings(
                res->channelID,
                [weak](const ExpectedStr<KickPrivateChatSettings> &settings) {
                    auto self = weak.lock();
                    if (!self)
                    {
                        return;
                    }
                    if (!settings)
                    {
                        qCWarning(chatterinoKick)
                            << *self << "Failed to fetch chat settings:"
                            << settings.error();
                        return;
                    }

                    self->updateRoomModes(RoomModes{
                        .subscribersMode = settings->subscribersMode,
                        .emotesMode = settings->emotesMode,
                        .slowModeDuration = settings->slowModeDuration,
                        .followersModeDuration =
                            settings->followersModeDuration,
                    });
                });
            self->updateStreamData(*res);
            self->reloadKickChannelEmotes();
            self->refreshOwnIdentity();
        });
}

void KickChannel::reloadKickChannelEmotes()
{
    auto weak = this->weakFromThis();
    KickApi::privateEmotesInChannel(
        this->slug_,
        [weak](const ExpectedStr<std::vector<KickPrivateEmoteSetInfo>> &res) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }

            if (!res)
            {
                qCWarning(chatterinoKick)
                    << *self << "Failed to fetch channel emotes:"
                    << res.error();
                self->kickChannelEmotes_.set(EMPTY_EMOTE_MAP);
                return;
            }

            auto emotes = std::make_shared<EmoteMap>();
            for (const auto &set : *res)
            {
                if (!set.userID)
                {
                    continue;  // global set
                }

                for (const auto &emoteInfo : set.emotes)
                {
                    if (emoteInfo.emoteID == 0 || emoteInfo.name.isEmpty())
                    {
                        continue;
                    }

                    auto id = QString::number(emoteInfo.emoteID);
                    auto emote = KickEmotes::emoteForID(id, emoteInfo.name);
                    (*emotes)[emote->name] = emote;
                }
            }

            self->kickChannelEmotes_.set(std::move(emotes));
            qCDebug(chatterinoKick)
                << "Loaded" << self->kickChannelEmotes_.get()->size()
                << "channel emotes for" << *self;
        });
}

void KickChannel::refreshOwnIdentity()
{
    auto account = getApp()->getAccounts()->kick.current();
    if (!account || account->isAnonymous())
    {
        this->ownIdentity_.reset();
        return;
    }

    auto weak = this->weakFromThis();
    KickApi::privateUserInChannelInfo(
        account->username(), this->getName(),
        [weak](const ExpectedStr<KickPrivateUserInChannelInfo> &res) {
            auto self = weak.lock();
            if (!self || !res)
            {
                return;
            }
            self->cacheOwnIdentityFromUserInfo(*res);
        });
}

void KickChannel::cacheOwnIdentityFromUserInfo(
    const KickPrivateUserInChannelInfo &info)
{
    auto account = getApp()->getAccounts()->kick.current();
    if (!account || account->isAnonymous())
    {
        return;
    }
    if (info.userID != 0 && info.userID != account->userID())
    {
        return;
    }

    CachedOwnIdentity identity;
    identity.displayName =
        info.username.isEmpty() ? account->username() : info.username;
    identity.loginName = identity.displayName.toLower();
    const auto ownUserID = info.userID == 0 ? account->userID() : info.userID;
    identity.userID = QString::number(ownUserID);
    identity.usernameColor = this->getUserColor(identity.displayName);
    getApp()->getKickChatServer()->requestSeventvCosmetics(
        ownUserID, identity.displayName);

    bool hasMod = false;
    bool hasVip = false;
    bool hasSubscriberBadge = false;
    std::vector<std::unique_ptr<MessageElement>> levelBadges;
    for (const auto &badge : info.badges)
    {
        if (!badge.active || !badge.selected)
        {
            continue;
        }

        const auto type = badge.type.toLower();
        if (type == u"moderator"_s)
        {
            hasMod = true;
        }
        else if (type == u"vip"_s)
        {
            hasVip = true;
        }

        if (isKickLevelBadge(badge))
        {
            if (auto element = makeKickLevelBadgeElement(badge, this->slug_))
            {
                levelBadges.emplace_back(std::move(element));
            }
            continue;
        }

        auto [emote, flag] = [&] {
            if (type == u"subscriber"_s)
            {
                hasSubscriberBadge = true;
                if (auto subscriberBadge =
                        this->subscriberBadgeForMonths(badge.count))
                {
                    return std::pair{subscriberBadge,
                                     MessageElementFlag::BadgeSubscription};
                }
            }
            return KickBadges::lookup(type.toStdString());
        }();
        if (emote)
        {
            identity.badges.emplace_back(
                std::make_unique<BadgeElement>(emote, flag));
        }
    }

    if (!hasSubscriberBadge && info.subscriptionMonths)
    {
        if (auto subscriberBadge =
                this->subscriberBadgeForMonths(*info.subscriptionMonths))
        {
            identity.badges.emplace_back(std::make_unique<BadgeElement>(
                subscriberBadge, MessageElementFlag::BadgeSubscription));
        }
    }

    for (auto &levelBadge : levelBadges)
    {
        identity.badges.emplace_back(std::move(levelBadge));
    }

    this->setMod(hasMod);
    this->setVip(hasVip);
    this->ownIdentity_.emplace(std::move(identity));
}

void KickChannel::setSubscriberBadges(
    const std::vector<KickPrivateSubscriberBadgeInfo> &badges)
{
    this->subscriberBadges_.clear();
    this->subscriberBadges_.reserve(badges.size());

    for (const auto &badge : badges)
    {
        if (badge.imageUrl.isEmpty())
        {
            continue;
        }

        QString tooltip = u"Subscriber"_s;
        if (badge.months > 0)
        {
            tooltip = u"Subscriber ("_s % QString::number(badge.months) %
                      (badge.months == 1 ? u" month)"_s : u" months)"_s);
        }

        auto emote = std::make_shared<const Emote>(Emote{
            .name = {u"Subscriber"_s},
            .images = ImageSet(
                Image::fromAutoscaledUrl({badge.imageUrl}, 18)),
            .tooltip = Tooltip{std::move(tooltip)},
            .homePage = {u"https://kick.com/" % this->slug_},
        });
        this->subscriberBadges_.push_back(SubscriberBadge{
            .months = badge.months,
            .emote = std::move(emote),
        });
    }

    std::sort(this->subscriberBadges_.begin(), this->subscriberBadges_.end(),
              [](const auto &lhs, const auto &rhs) {
                  return lhs.months < rhs.months;
              });
}

void KickChannel::setUserInfo(UserInit init)
{
    const auto oldUserID = this->userID_;
    const auto oldChannelID = this->channelID_;
    const auto oldRoomID = this->roomID_;

    if ((oldChannelID != 0 && init.channelID != 0 &&
         oldChannelID != init.channelID) ||
        (oldRoomID != 0 && init.roomID != 0 && oldRoomID != init.roomID))
    {
        qCWarning(chatterinoKick)
            << *this << "Unexpected room/channel ID change - oldChannelID:"
            << oldChannelID << "channelID:" << init.channelID
            << "oldRoomID:" << oldRoomID << "roomID:" << init.roomID;
        return;
    }

    if (init.userID != 0)
    {
        this->userID_ = init.userID;
    }
    if (init.channelID != 0)
    {
        this->channelID_ = init.channelID;
    }
    if (init.roomID != 0)
    {
        this->roomID_ = init.roomID;
    }

    if (oldChannelID != this->channelID() || oldRoomID != this->roomID())
    {
        if (this->roomID() != 0 && this->channelID() != 0)
        {
            auto *srv = getApp()->getKickChatServer();
            srv->registerRoomID(this->roomID(), this->channelID(),
                                this->weakFromThis());
            const auto roomID = this->roomID();
            const auto channelID = this->channelID();
            QTimer::singleShot(0, [weak = this->weakFromThis(), roomID,
                                   channelID] {
                auto self = weak.lock();
                if (!self)
                {
                    return;
                }
                self->loadRecentMessages([weak, roomID, channelID] {
                    auto self = weak.lock();
                    if (!self)
                    {
                        return;
                    }
                    getApp()->getKickChatServer()->liveUpdates().joinRoom(
                        roomID, channelID);
                });
            });
        }
        else
        {
            QTimer::singleShot(0, [weak = this->weakFromThis()] {
                auto self = weak.lock();
                if (!self)
                {
                    return;
                }
                self->loadRecentMessages();
            });
        }
    }

    if (oldUserID != this->userID())
    {
        this->reloadSeventvEmotes(false);
        this->userIDChanged.invoke();
    }
}

void KickChannel::loadRecentMessages(std::function<void()> onDone)
{
    if (!getSettings()->loadTwitchMessageHistoryOnConnect)
    {
        if (onDone)
        {
            onDone();
        }
        return;
    }

    if (this->roomID() == 0)
    {
        if (onDone)
        {
            onDone();
        }
        return;
    }

    if (this->loadedRecentMessages_.load())
    {
        if (onDone)
        {
            onDone();
        }
        return;
    }

    if (this->loadingRecentMessages_.test_and_set())
    {
        if (onDone)
        {
            onDone();
        }
        return;
    }

    auto weak = this->weakFromThis();
    KickApi::privateRecentMessages(
        this->roomID(), getSettings()->twitchMessageHistoryLimit.getValue(),
        [weak, onDone = std::move(onDone)](
            const ExpectedStr<std::vector<boost::json::object>> &res) mutable {
            auto finish = [&onDone] {
                if (onDone)
                {
                    onDone();
                }
            };
            auto self = weak.lock();
            if (!self)
            {
                finish();
                return;
            }

            if (!res)
            {
                qCDebug(chatterinoKick)
                    << *self
                    << "Failed to load Kick message history:" << res.error();
                self->loadingRecentMessages_.clear();
                finish();
                return;
            }

            auto messages = removeExistingMessages(
                self.get(), buildKickRecentMessages(self.get(), *res));
            if (!messages.empty())
            {
                self->addMessagesAtStart(messages);
                self->loadedRecentMessages_.store(true);
            }
            self->loadingRecentMessages_.clear();

            std::vector<MessagePtr> mentionMessages;
            for (const auto &msg : messages)
            {
                const auto highlighted =
                    msg->flags.has(MessageFlag::Highlighted);
                const auto showInMentions =
                    msg->flags.has(MessageFlag::ShowInMentions);
                if (highlighted && showInMentions)
                {
                    mentionMessages.push_back(msg);
                }
            }

            if (!mentionMessages.empty())
            {
                getApp()->getTwitch()->getMentionsChannel()
                    ->fillInMissingMessages(mentionMessages);
            }

            finish();
        });
}

void KickChannel::loadRecentMessagesReconnect(std::function<void()> onDone)
{
    if (!getSettings()->loadTwitchMessageHistoryOnConnect)
    {
        if (onDone)
        {
            onDone();
        }
        return;
    }

    if (this->roomID() == 0)
    {
        if (onDone)
        {
            onDone();
        }
        return;
    }

    if (this->loadingRecentMessages_.test_and_set())
    {
        if (onDone)
        {
            onDone();
        }
        return;
    }

    auto weak = this->weakFromThis();
    KickApi::privateRecentMessages(
        this->roomID(), getSettings()->twitchMessageHistoryLimit.getValue(),
        [weak, onDone = std::move(onDone)](
            const ExpectedStr<std::vector<boost::json::object>> &res) mutable {
            auto finish = [&onDone] {
                if (onDone)
                {
                    onDone();
                }
            };
            auto self = weak.lock();
            if (!self)
            {
                finish();
                return;
            }

            if (!res)
            {
                qCDebug(chatterinoKick)
                    << *self
                    << "Failed to refresh Kick message history:" << res.error();
                self->loadingRecentMessages_.clear();
                finish();
                return;
            }

            auto messages = removeExistingMessages(
                self.get(), buildKickRecentMessages(self.get(), *res));
            if (!messages.empty())
            {
                self->fillInMissingMessages(messages);
            }
            self->loadingRecentMessages_.clear();

            std::vector<MessagePtr> mentionMessages;
            for (const auto &msg : messages)
            {
                const auto highlighted =
                    msg->flags.has(MessageFlag::Highlighted);
                const auto showInMentions =
                    msg->flags.has(MessageFlag::ShowInMentions);
                if (highlighted && showInMentions)
                {
                    mentionMessages.push_back(msg);
                }
            }

            if (!mentionMessages.empty())
            {
                getApp()->getTwitch()->getMentionsChannel()
                    ->fillInMissingMessages(mentionMessages);
            }

            finish();
        });
}

size_t KickChannel::maxBurstMessages() const
{
    // FIXME: this isn't fully tested (maybe these are higher?)
    if (this->hasHighRateLimit())
    {
        return 20;
    }
    return 5;
}

std::chrono::milliseconds KickChannel::minMessageOffset() const
{
    // FIXME: this isn't fully tested
    if (this->hasHighRateLimit())
    {
        return 50ms;
    }
    if (this->roomModes().slowModeDuration)
    {
        return 500ms;
    }
    return 100ms;
}

bool KickChannel::checkMessageRatelimit()
{
    auto now = std::chrono::steady_clock::now();
    auto &timestamps = this->lastMessageTimestamps_;

    // FIXME: haven't tested this fully
    const auto cooldown = 5s;

    // This is mostly identical to the logic in TwitchIrcServer
    if (!timestamps.empty() &&
        timestamps.back() + this->minMessageOffset() > now)
    {
        if (this->lastMessageSpeedErrorTs_ + 30s < now)
        {
            this->addSystemMessage(u"You are sending messages too quickly."_s);
            this->lastMessageSpeedErrorTs_ = now;
        }
        return false;
    }

    // remove messages older than `cooldown`
    while (!timestamps.empty() && timestamps.front() + cooldown < now)
    {
        timestamps.pop();
    }

    // check if you are sending too many messages
    if (timestamps.size() >= this->maxBurstMessages())
    {
        if (this->lastMessageAmountErrorTs_ + 30s < now)
        {
            this->addSystemMessage(u"You are sending too many messages."_s);

            this->lastMessageAmountErrorTs_ = now;
        }
        return false;
    }

    timestamps.push(now);
    return true;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static) -- might need some state later
QString KickChannel::prepareMessage(const QString &message) const
{
    const QString baseMessage = getApp()
                                    ->getEmotes()
                                    ->getEmojis()
                                    ->replaceShortCodes(message)
                                    .simplified();

    // We need to manually add the emotes. They're in the format
    // "[emote:{id}:{name}]". If the name doesn't match the emote name, Kick
    // will reject the message.
    auto channelEmotes = this->kickChannelEmotes();
    auto globalEmotes = getApp()->getKickChatServer()->globalEmotes();
    QString outMessage;
    const QChar *lastEnd = nullptr;
    for (QStringView word : baseMessage.tokenize(u' '))
    {
        EmoteName emote{word.toString()};  // FIXME: get rid of this
        EmotePtr kickEmote;
        if (auto it = channelEmotes->find(emote); it != channelEmotes->end())
        {
            kickEmote = it->second;
        }
        else if (auto it = globalEmotes->find(emote); it != globalEmotes->end())
        {
            kickEmote = it->second;
        }

        if (!kickEmote)
        {
            continue;
        }

        if (lastEnd)
        {
            outMessage += QStringView(lastEnd, word.begin());
        }
        else if (word.begin() != baseMessage.begin())
        {
            outMessage += QStringView(baseMessage.begin(), word.begin());
        }

        lastEnd = word.end();
        outMessage += u"[emote:";
        outMessage += kickEmote->id.string;
        outMessage += ':';
        outMessage += kickEmote->name.string;
        outMessage += ']';
    }

    if (lastEnd)
    {
        outMessage += QStringView(lastEnd, baseMessage.end());
    }
    else
    {
        // no emote added
        outMessage = baseMessage;
    }
    return outMessage;
}

void KickChannel::updateSevenTVActivity()
{
    const auto currentSeventvUserID =
        getApp()->getAccounts()->kick.current()->seventvUserID();
    if (currentSeventvUserID.isEmpty())
    {
        return;
    }

    if (!getSettings()->enableSevenTVEventAPI ||
        !getSettings()->sendSevenTVActivity)
    {
        return;
    }

    if (this->nextSeventvActivity_.isValid() &&
        QDateTime::currentDateTimeUtc() < this->nextSeventvActivity_)
    {
        return;
    }
    // Make sure to not send activity again before receiving the response
    this->nextSeventvActivity_ = this->nextSeventvActivity_.addSecs(300);

    qCDebug(chatterinoSeventv) << "Sending activity in" << this->getName();

    getApp()->getSeventvAPI()->updateKickPresence(
        this->userID(), currentSeventvUserID,
        [weak = this->weakFromThis()]() {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            self->nextSeventvActivity_ =
                QDateTime::currentDateTimeUtc().addSecs(60);
        },
        [](const auto &result) {
            qCDebug(chatterinoSeventv)
                << "Failed to update 7TV activity:" << result.formatError();
        });
}

void KickChannel::addLoginMessage()
{
    auto builder = MessageBuilder();
    builder->flags.set(MessageFlag::System);
    builder->flags.set(MessageFlag::DoNotTriggerNotification);

    builder.emplace<TimestampElement>();
    builder.emplace<TextElement>(
        u"You need to log in to send messages. You can link your "_s
        "Kick account",
        MessageElementFlag::Text, MessageColor::System);
    builder
        .emplace<TextElement>(u"in the settings."_s, MessageElementFlag::Text,
                              MessageColor::Link)
        ->setLink({Link::OpenAccountsPage, {}});

    this->addMessage(builder.release(), MessageContext::Original);
}

void KickChannel::updateSeventvData(const QString &newUserID,
                                    const QString &newEmoteSetID)
{
    if (this->seventvUserID_ == newUserID &&
        this->seventvEmoteSetID_ == newEmoteSetID)
    {
        return;
    }

    const auto oldUserID = makeConditionedOptional(
        !this->seventvUserID_.isEmpty() && this->seventvUserID_ != newUserID,
        this->seventvUserID_);
    const auto oldEmoteSetID =
        makeConditionedOptional(!this->seventvEmoteSetID_.isEmpty() &&
                                    this->seventvEmoteSetID_ != newEmoteSetID,
                                this->seventvEmoteSetID_);

    this->seventvUserID_ = newUserID;
    this->seventvEmoteSetID_ = newEmoteSetID;
    runInGuiThread([this, oldUserID, oldEmoteSetID]() {
        if (getApp()->getSeventvEventAPI())
        {
            getApp()->getSeventvEventAPI()->subscribeUser(
                this->seventvUserID_, this->seventvEmoteSetID_);

            if (oldUserID || oldEmoteSetID)
            {
                // FIXME: make sure no TwitchChannel is listenting to this
                getApp()->getTwitch()->dropSeventvChannel(
                    oldUserID.value_or(QString()),
                    oldEmoteSetID.value_or(QString()));
            }
        }
    });
}

void KickChannel::addOrReplaceSeventvAddRemove(bool isEmoteAdd,
                                               const QString &actor,
                                               const QString &emoteName)
{
    if (this->tryReplaceLastSeventvAddOrRemove(
            isEmoteAdd ? MessageFlag::LiveUpdatesAdd
                       : MessageFlag::LiveUpdatesRemove,
            actor, emoteName))
    {
        return;
    }

    this->lastSeventvEmoteNames_ = {emoteName};

    MessagePtr msg;
    if (isEmoteAdd)
    {
        msg = MessageBuilder(liveUpdatesAddEmoteMessage, "7TV", actor,
                             this->lastSeventvEmoteNames_)
                  .release();
    }
    else
    {
        msg = MessageBuilder(liveUpdatesRemoveEmoteMessage, "7TV", actor,
                             this->lastSeventvEmoteNames_)
                  .release();
    }
    this->lastSeventvMessage_ = msg;
    this->lastSeventvEmoteActor_ = actor;
    this->addMessage(msg, MessageContext::Original);
}

bool KickChannel::tryReplaceLastSeventvAddOrRemove(MessageFlag op,
                                                   const QString &actor,
                                                   const QString &emoteName)
{
    auto last = this->lastSeventvMessage_.lock();
    if (!last || !last->flags.has(op) ||
        last->parseTime < QTime::currentTime().addSecs(-5) ||
        last->loginName != actor)
    {
        return false;
    }
    // Update the message
    this->lastSeventvEmoteNames_.push_back(emoteName);

    auto makeReplacement = [&](MessageFlag op) -> MessageBuilder {
        if (op == MessageFlag::LiveUpdatesAdd)
        {
            return {
                liveUpdatesAddEmoteMessage,
                "7TV",
                last->loginName,
                this->lastSeventvEmoteNames_,
            };
        }

        // op == RemoveEmoteMessage
        return {
            liveUpdatesRemoveEmoteMessage,
            "7TV",
            last->loginName,
            this->lastSeventvEmoteNames_,
        };
    };

    auto replacement = makeReplacement(op);

    replacement->flags = last->flags;

    auto msg = replacement.release();
    this->lastSeventvMessage_ = msg;
    this->replaceMessage(last, msg);

    return true;
}

void KickChannel::emitSendWait()
{
    auto now = std::chrono::steady_clock::now();
    std::chrono::milliseconds remaining{0};
    if (this->sendWaitEnd_)
    {
        remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            *this->sendWaitEnd_ - now);
    }
    if (remaining <= std::chrono::milliseconds{0})
    {
        this->sendWaitTimer_.stop();
        this->sendWaitUpdate.invoke(0);
    }
    else
    {
        const auto rounded = std::chrono::duration_cast<std::chrono::seconds>(
            remaining + std::chrono::milliseconds{999});
        this->sendWaitUpdate.invoke(static_cast<int>(rounded.count()));
    }
}

QDebug operator<<(QDebug dbg, const KickChannel &chan)
{
    QDebugStateSaver s(dbg);
    dbg.nospace().noquote() << "[KickChannel " << chan.getName() << ']';
    return dbg;
}

}  // namespace chatterino
