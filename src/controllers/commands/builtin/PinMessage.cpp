// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/PinMessage.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "messages/Message.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace chatterino::commands {
namespace {

constexpr int DEFAULT_PIN_DURATION_SECONDS = 20 * 60;
constexpr int MIN_PIN_DURATION_MINUTES = 1;
constexpr int MAX_PIN_DURATION_MINUTES = 30;

struct ParsedDuration {
    bool valid = false;
    std::optional<int> seconds;
};

ParsedDuration parseDuration(QString value)
{
    value = value.trimmed().toLower();
    if (value == u"stream" || value == u"streamend" ||
        value == u"untilstreamend")
    {
        return {true, std::nullopt};
    }

    if (value.endsWith(u'm'))
    {
        value.chop(1);
    }

    bool ok = false;
    const int minutes = value.toInt(&ok);
    if (!ok || minutes < MIN_PIN_DURATION_MINUTES ||
        minutes > MAX_PIN_DURATION_MINUTES)
    {
        return {};
    }
    return {true, minutes * 60};
}

QString pinUsage()
{
    return QStringLiteral(
        "Usage: reply to a message and use \"/pin [minutes|stream]\", or "
        "use \"/pin <message-id> [minutes|stream]\". Twitch durations must "
        "be 1-30 minutes; \"stream\" keeps it pinned until stream end.");
}

PinnedChatMessage stateFromMessage(
    const Message &message, PinnedChatMessage::Platform platform,
    const QString &pinnedBy, std::optional<int> durationSeconds)
{
    const auto now = QDateTime::currentDateTimeUtc();
    PinnedChatMessage state;
    state.platform = platform;
    state.messageID = message.id;
    state.messageText = message.messageText;
    state.senderUserID = message.userID;
    state.senderLogin = message.loginName;
    state.senderDisplayName =
        message.displayName.isEmpty() ? message.loginName : message.displayName;
    state.pinnedByDisplayName = pinnedBy;
    state.startsAt = now;
    if (durationSeconds)
    {
        state.endsAt = now.addSecs(*durationSeconds);
    }
    return state;
}

PinnedChatMessage stateFromHelix(const HelixPinnedChatMessage &message)
{
    PinnedChatMessage state;
    state.platform = PinnedChatMessage::Platform::Twitch;
    state.messageID = message.messageID;
    state.messageText = message.messageText;
    state.senderUserID = message.senderUserID;
    state.senderLogin = message.senderLogin;
    state.senderDisplayName = message.senderDisplayName;
    state.pinnedByDisplayName = message.pinnedByDisplayName;
    state.startsAt = message.startsAt;
    state.endsAt = message.endsAt;
    return state;
}

QJsonObject kickMessagePayload(const Message &message,
                               const KickChannel &channel)
{
    const auto document = QJsonDocument::fromJson(message.kickMessageJson);
    if (document.isObject())
    {
        return document.object();
    }

    QJsonObject identity{
        {"color", message.usernameColor.name()},
        {"badges", QJsonArray{}},
    };
    QJsonObject sender{
        {"username", message.displayName},
        {"slug", message.loginName},
        {"identity", identity},
    };
    bool userIDOk = false;
    const auto userID = message.userID.toLongLong(&userIDOk);
    sender.insert("id", userIDOk ? QJsonValue(userID)
                                 : QJsonValue(message.userID));

    return {
        {"id", message.id},
        {"chatroom_id", static_cast<qint64>(channel.roomID())},
        {"content", message.messageText},
        {"type", "message"},
        {"created_at",
         message.serverReceivedTime.toUTC().toString(Qt::ISODateWithMs)},
        {"sender", sender},
    };
}

void sendPinnedTwitchChatMessage(const ChannelPtr &channel,
                                 const QString &description)
{
    auto target = std::dynamic_pointer_cast<TwitchChannel>(
        channelForPinnedMessage(channel,
                                PinnedChatMessage::Platform::Twitch));
    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!target || !target->hasModRights())
    {
        channel->addSystemMessage(
            "Sending a pinned message requires moderator access in a Twitch "
            "channel.");
        return;
    }
    if (!account || account->isAnon())
    {
        channel->addSystemMessage(
            "Log in to Twitch before sending pinned messages.");
        return;
    }
    if (target->roomId().isEmpty())
    {
        channel->addSystemMessage(
            "Sending messages in this channel isn't possible.");
        return;
    }

    getHelix()->sendChatMessage(
        {
            .broadcasterID = target->roomId(),
            .senderID = account->getUserId(),
            .message = description,
            .pin = true,
        },
        [weak = std::weak_ptr<TwitchChannel>(target),
         channel](const HelixSentMessage &result) {
            if (!result.isSent)
            {
                const auto reason =
                    result.dropReason
                        ? result.dropReason->message
                        : QStringLiteral("Twitch did not send the message.");
                channel->addSystemMessage(
                    QStringLiteral("Pinned message was not sent - %1")
                        .arg(reason));
                return;
            }
            if (auto self = weak.lock())
            {
                refreshPinnedChatMessage(self);
            }
        },
        [channel](HelixSendMessageError, const QString &error) {
            channel->addSystemMessage(
                QStringLiteral("Failed to send pinned message - %1")
                    .arg(error.isEmpty() ? QStringLiteral("Unknown error")
                                         : error));
        });
}

void sendPinnedKickChatMessage(const ChannelPtr &channel,
                               const QString &description)
{
    auto target = std::dynamic_pointer_cast<KickChannel>(
        channelForPinnedMessage(channel, PinnedChatMessage::Platform::Kick));
    if (!target || !target->hasModRights())
    {
        channel->addSystemMessage(
            "Sending a pinned message requires moderator access in a Kick "
            "channel.");
        return;
    }
    if (!target->canSendMessage())
    {
        channel->addSystemMessage(
            "Log in to Kick before sending pinned messages.");
        return;
    }

    target->sendMessageAndWaitForEcho(
        description, [channel](const MessagePtr &message) {
            pinChatMessage(channel, message, DEFAULT_PIN_DURATION_SECONDS);
        });
}

}  // namespace

ChannelPtr channelForPinnedMessage(const ChannelPtr &channel,
                                   PinnedChatMessage::Platform platform)
{
    if (!channel)
    {
        return nullptr;
    }

    if (auto merged = std::dynamic_pointer_cast<MergedChannel>(channel))
    {
        return platform == PinnedChatMessage::Platform::Kick
                   ? merged->kickChannel()
                   : merged->twitchChannel();
    }

    if (platform == PinnedChatMessage::Platform::Kick)
    {
        return channel->isKickChannel() ? channel : nullptr;
    }
    return channel->isTwitchChannel() ? channel : nullptr;
}

bool canPinChatMessage(const ChannelPtr &channel, const MessagePtr &message)
{
    if (!message || message->id.isEmpty() ||
        message->flags.hasAny({MessageFlag::System, MessageFlag::Disabled,
                               MessageFlag::Timeout, MessageFlag::ClearChat}))
    {
        return false;
    }

    const auto platform =
        message->platform == MessagePlatform::Kick
            ? PinnedChatMessage::Platform::Kick
            : PinnedChatMessage::Platform::Twitch;
    auto target = channelForPinnedMessage(channel, platform);
    if (!target || !target->hasModRights())
    {
        return false;
    }
    return message->platform == MessagePlatform::AnyOrTwitch ||
           message->platform == MessagePlatform::Kick;
}

void pinChatMessage(const ChannelPtr &channel, const MessagePtr &message,
                    std::optional<int> durationSeconds)
{
    if (!canPinChatMessage(channel, message))
    {
        if (channel)
        {
            channel->addSystemMessage(
                "This message cannot be pinned, or you do not have "
                "moderator access in its channel.");
        }
        return;
    }

    if (message->platform == MessagePlatform::Kick)
    {
        auto target = std::dynamic_pointer_cast<KickChannel>(
            channelForPinnedMessage(channel, PinnedChatMessage::Platform::Kick));
        if (!target)
        {
            return;
        }
        if (!durationSeconds)
        {
            target->addSystemMessage(
                "Keeping a pin until stream end is only supported on Twitch.");
            return;
        }

        const auto account = getApp()->getAccounts()->kick.current();
        if (!account || account->isAnonymous() ||
            account->chatIdentityToken().isEmpty())
        {
            target->addSystemMessage(
                "Connect Kick's website session under Settings > Accounts "
                "before pinning messages.");
            return;
        }
        const auto payload = kickMessagePayload(*message, *target);
        const auto pinnedBy = account->username();
        getKickApi()->pinChatMessage(
            target->slug(), account->chatIdentityToken(), payload,
            *durationSeconds,
            [weak = std::weak_ptr<KickChannel>(target), message, pinnedBy,
             durationSeconds](const auto &result) {
                auto self = weak.lock();
                if (!self)
                {
                    return;
                }
                if (!result)
                {
                    self->addSystemMessage("Failed to pin Kick message - " +
                                           result.error());
                    return;
                }
                self->setPinnedMessage(stateFromMessage(
                    *message, PinnedChatMessage::Platform::Kick, pinnedBy,
                    durationSeconds));
            });
        return;
    }

    auto target = std::dynamic_pointer_cast<TwitchChannel>(
        channelForPinnedMessage(channel, PinnedChatMessage::Platform::Twitch));
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!target || account->isAnon())
    {
        channel->addSystemMessage("Log in to Twitch before pinning messages.");
        return;
    }

    getHelix()->pinChatMessage(
        target->roomId(), account->getUserId(), message->id, durationSeconds,
        [weak = std::weak_ptr<TwitchChannel>(target), message,
         pinnedBy = account->getUserName(), durationSeconds] {
            if (auto self = weak.lock())
            {
                self->setPinnedMessage(stateFromMessage(
                    *message, PinnedChatMessage::Platform::Twitch, pinnedBy,
                    durationSeconds));
            }
        },
        [weak = std::weak_ptr<TwitchChannel>(target)](const QString &error) {
            if (auto self = weak.lock())
            {
                self->addSystemMessage("Failed to pin Twitch message - " +
                                       error);
            }
        });
}

void updatePinnedChatMessageDuration(
    const ChannelPtr &channel, std::optional<int> durationSeconds)
{
    auto target = std::dynamic_pointer_cast<TwitchChannel>(
        channelForPinnedMessage(channel, PinnedChatMessage::Platform::Twitch));
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!target || !target->pinnedMessage() || account->isAnon())
    {
        return;
    }

    const auto messageID = target->pinnedMessage()->messageID;
    getHelix()->updatePinnedChatMessage(
        target->roomId(), account->getUserId(), messageID, durationSeconds,
        [weak = std::weak_ptr<TwitchChannel>(target), durationSeconds] {
            auto self = weak.lock();
            if (!self || !self->pinnedMessage())
            {
                return;
            }
            auto state = *self->pinnedMessage();
            if (durationSeconds)
            {
                state.endsAt =
                    QDateTime::currentDateTimeUtc().addSecs(*durationSeconds);
            }
            else
            {
                state.endsAt.reset();
            }
            self->setPinnedMessage(std::move(state));
        },
        [weak = std::weak_ptr<TwitchChannel>(target)](const QString &error) {
            if (auto self = weak.lock())
            {
                self->addSystemMessage(
                    "Failed to update pinned message duration - " + error);
            }
        });
}

void unpinChatMessage(const ChannelPtr &channel)
{
    if (!channel || !channel->pinnedMessage())
    {
        return;
    }

    if (channel->pinnedMessage()->platform ==
        PinnedChatMessage::Platform::Kick)
    {
        auto target = std::dynamic_pointer_cast<KickChannel>(channel);
        if (!target)
        {
            return;
        }
        const auto account = getApp()->getAccounts()->kick.current();
        if (!account || account->isAnonymous() ||
            account->chatIdentityToken().isEmpty())
        {
            target->addSystemMessage(
                "Connect Kick's website session under Settings > Accounts "
                "before unpinning messages.");
            return;
        }
        getKickApi()->unpinChatMessage(
            target->slug(), account->chatIdentityToken(),
            [weak = std::weak_ptr<KickChannel>(target)](const auto &result) {
                auto self = weak.lock();
                if (!self)
                {
                    return;
                }
                if (!result)
                {
                    self->addSystemMessage("Failed to unpin Kick message - " +
                                           result.error());
                    return;
                }
                self->setPinnedMessage(std::nullopt);
            });
        return;
    }

    auto target = std::dynamic_pointer_cast<TwitchChannel>(channel);
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!target || account->isAnon())
    {
        return;
    }

    getHelix()->unpinChatMessage(
        target->roomId(), account->getUserId(),
        target->pinnedMessage()->messageID,
        [weak = std::weak_ptr<TwitchChannel>(target)] {
            if (auto self = weak.lock())
            {
                self->setPinnedMessage(std::nullopt);
            }
        },
        [weak = std::weak_ptr<TwitchChannel>(target)](const QString &error) {
            if (auto self = weak.lock())
            {
                self->addSystemMessage("Failed to unpin Twitch message - " +
                                       error);
            }
        });
}

void refreshPinnedChatMessage(const ChannelPtr &channel)
{
    auto target = std::dynamic_pointer_cast<TwitchChannel>(
        channelForPinnedMessage(channel, PinnedChatMessage::Platform::Twitch));
    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!target || account->isAnon() || target->roomId().isEmpty())
    {
        return;
    }

    getHelix()->getPinnedChatMessage(
        target->roomId(), account->getUserId(),
        [weak = std::weak_ptr<TwitchChannel>(target)](
            const std::optional<HelixPinnedChatMessage> &message) {
            auto self = weak.lock();
            if (!self)
            {
                return;
            }
            if (!message)
            {
                self->setPinnedMessage(std::nullopt);
                return;
            }
            self->setPinnedMessage(stateFromHelix(*message));
        },
        [](const QString &) {
            // Non-moderators cannot read this endpoint. This background
            // refresh is intentionally silent.
        });
}

QString pinChatMessageCommand(const CommandContext &ctx)
{
    if (!ctx.channel)
    {
        return {};
    }

    MessagePtr targetMessage;
    int durationWord = 1;
    if (ctx.message != nullptr && !ctx.message->id.isEmpty())
    {
        targetMessage = ctx.channel->findMessageByID(ctx.message->id);
    }
    else
    {
        if (ctx.words.size() < 2)
        {
            ctx.channel->addSystemMessage(
                "Usage: /pin <message>. This sends and pins the message for "
                "20 minutes.");
            return {};
        }
        targetMessage = ctx.channel->findMessageByID(ctx.words.at(1));
        if (!targetMessage)
        {
            if (ctx.channel->isKickChannel())
            {
                sendPinnedKickChatMessage(
                    ctx.channel,
                    ctx.words.mid(1).join(QLatin1Char(' ')).trimmed());
                return {};
            }
            sendPinnedTwitchChatMessage(
                ctx.channel, ctx.words.mid(1).join(QLatin1Char(' ')).trimmed());
            return {};
        }
        durationWord = 2;
    }

    if (!targetMessage)
    {
        ctx.channel->addSystemMessage(
            "Could not find that message in this channel. " + pinUsage());
        return {};
    }

    std::optional<int> durationSeconds = DEFAULT_PIN_DURATION_SECONDS;
    if (ctx.words.size() > durationWord)
    {
        const auto duration = parseDuration(ctx.words.at(durationWord));
        if (!duration.valid)
        {
            ctx.channel->addSystemMessage(pinUsage());
            return {};
        }
        durationSeconds = duration.seconds;
    }
    if (ctx.words.size() > durationWord + 1)
    {
        ctx.channel->addSystemMessage(pinUsage());
        return {};
    }

    if (targetMessage->platform == MessagePlatform::Kick && !durationSeconds)
    {
        ctx.channel->addSystemMessage(
            "Keeping a pin until stream end is only supported on Twitch.");
        return {};
    }

    pinChatMessage(ctx.channel, targetMessage, durationSeconds);
    return {};
}

}  // namespace chatterino::commands
