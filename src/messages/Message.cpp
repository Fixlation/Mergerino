// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/Message.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "messages/MessageThread.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Settings.hpp"
#include "util/DebugCount.hpp"
#include "util/QMagicEnum.hpp"
#include "widgets/helper/ScrollbarHighlight.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>

namespace chatterino {

namespace {

QColor defaultPlatformAccent(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QColor(83, 252, 24);
        case MessagePlatform::YouTube:
            return QColor(255, 48, 64);
        case MessagePlatform::TikTok:
            return QColor(37, 244, 238);
        case MessagePlatform::AnyOrTwitch:
        default:
            return QColor(145, 70, 255);
    }
}

QColor activityPlatformHighlightColor(const Message &message)
{
    const auto accent = defaultPlatformAccent(message.platform);
    const auto hsv = accent.toHsv();
    const int hue = hsv.hsvHue() >= 0 ? hsv.hsvHue() : 0;
    const int saturation =
        std::clamp(std::max(hsv.hsvSaturation(), 150) - 10, 0, 180);
    const int value = std::clamp(std::min(220, std::max(hsv.value(), 198)), 0,
                                 220);
    constexpr int alpha = 102;

    QColor popped;
    popped.setHsv(hue, saturation, value, alpha);
    return popped;
}

std::shared_ptr<QColor> platformHighlightColor(const Message &message,
                                               ColorType colorType,
                                               PlatformIndicatorMode
                                                   platformIndicatorMode,
                                               bool useActivityPlatformHighlightColors)
{
    auto base = ColorProvider::instance().color(colorType);

    if (useActivityPlatformHighlightColors)
    {
        if (!mergedPlatformIndicatorShowsLineColor(platformIndicatorMode))
        {
            return {};
        }

        return std::make_shared<QColor>(activityPlatformHighlightColor(message));
    }

    if (!base || !base->isValid())
    {
        return base;
    }

    const auto style = getSettings()->platformEventHighlightStyle.getEnum();
    if (style == PlatformEventHighlightStyle::None)
    {
        return {};
    }

    if (style == PlatformEventHighlightStyle::CustomColor)
    {
        QColor custom(getSettings()->platformEventHighlightCustomColor);
        if (custom.isValid())
        {
            return std::make_shared<QColor>(custom);
        }
    }

    if (message.platform == MessagePlatform::AnyOrTwitch &&
        (colorType == ColorType::FirstMessageHighlight ||
         colorType == ColorType::Subscription))
    {
        return std::make_shared<QColor>(*base);
    }

    auto color =
        message.platformAccentColor.value_or(defaultPlatformAccent(message.platform));
    color.setAlpha(base->alpha());
    return std::make_shared<QColor>(color);
}

std::shared_ptr<QColor> platformAlertHighlightColor(
    const Message &message, PlatformIndicatorMode platformIndicatorMode,
    bool useActivityPlatformHighlightColors)
{
    const auto style = platformAlertHighlightStyleSetting().getEnum();
    if (style == PlatformEventHighlightStyle::None)
    {
        return {};
    }

    if (useActivityPlatformHighlightColors)
    {
        if (!mergedPlatformIndicatorShowsLineColor(platformIndicatorMode))
        {
            return {};
        }

        return std::make_shared<QColor>(activityPlatformHighlightColor(message));
    }

    if (style == PlatformEventHighlightStyle::CustomColor)
    {
        QColor custom(platformAlertHighlightCustomColorSetting());
        if (custom.isValid())
        {
            return std::make_shared<QColor>(custom);
        }
    }

    auto color =
        message.platformAccentColor.value_or(defaultPlatformAccent(message.platform));
    int alpha = 72;
    if (message.platformAccentColor && message.platformAccentColor->alpha() > 0)
    {
        alpha = std::max(message.platformAccentColor->alpha(), alpha);
    }
    color.setAlpha(alpha);
    return std::make_shared<QColor>(color);
}

bool isPlatformAlertMessage(const Message &message)
{
    return message.flags.has(MessageFlag::ModerationAction) &&
           !message.flags.has(MessageFlag::AutoMod) &&
           !message.flags.has(MessageFlag::LowTrustUsers);
}

}  // namespace

using namespace literals;

Message::Message()
    : parseTime(QTime::currentTime())
{
    DebugCount::increase(DebugObject::Message);
}

Message::~Message()
{
    DebugCount::decrease(DebugObject::Message);
}

ScrollbarHighlight Message::getScrollBarHighlight(
    PlatformIndicatorMode platformIndicatorMode,
    bool useActivityPlatformHighlightColors) const
{
    if (this->flags.has(MessageFlag::Highlighted) ||
        this->flags.has(MessageFlag::HighlightedWhisper))
    {
        return {
            this->highlightColor,
        };
    }

    if (this->flags.has(MessageFlag::WatchStreak) &&
        getSettings()->enableWatchStreakHighlight)
    {
        auto color = platformHighlightColor(
            *this, ColorType::WatchStreak, platformIndicatorMode,
            useActivityPlatformHighlightColors);
        if (!color)
        {
            return {};
        }
        return {
            color,
        };
    }

    if (this->flags.has(MessageFlag::Subscription) &&
        getSettings()->enableSubHighlight)
    {
        auto color = platformHighlightColor(
            *this, ColorType::Subscription, platformIndicatorMode,
            useActivityPlatformHighlightColors);
        if (!color)
        {
            return {};
        }
        return {
            color,
        };
    }

    if (useActivityPlatformHighlightColors &&
        this->flags.has(MessageFlag::CheerMessage))
    {
        if (!mergedPlatformIndicatorShowsLineColor(platformIndicatorMode))
        {
            return {};
        }

        return {
            std::make_shared<QColor>(activityPlatformHighlightColor(*this)),
        };
    }

    if (this->flags.has(MessageFlag::RedeemedHighlight) ||
        this->flags.has(MessageFlag::RedeemedChannelPointReward))
    {
        auto color = platformHighlightColor(
            *this, ColorType::RedeemedHighlight, platformIndicatorMode,
            useActivityPlatformHighlightColors);
        if (!color)
        {
            return {};
        }
        return {
            color,
            ScrollbarHighlight::Default,
            true,
        };
    }

    if (this->flags.has(MessageFlag::ElevatedMessage))
    {
        auto color = platformHighlightColor(
            *this, ColorType::ElevatedMessageHighlight,
            platformIndicatorMode, useActivityPlatformHighlightColors);
        if (!color)
        {
            return {};
        }
        return {
            color,
            ScrollbarHighlight::Default,
            false,
            false,
            true,
        };
    }

    if (this->flags.has(MessageFlag::FirstMessage) ||
        this->flags.has(MessageFlag::FirstMessageSession))
    {
        auto color = platformHighlightColor(
            *this, ColorType::FirstMessageHighlight, platformIndicatorMode,
            useActivityPlatformHighlightColors);
        if (!color)
        {
            return {};
        }
        return {
            color,
            ScrollbarHighlight::Default,
            false,
            this->flags.has(MessageFlag::FirstMessage),
            false,
            this->flags.has(MessageFlag::FirstMessageSession),
        };
    }

    if (isPlatformAlertMessage(*this))
    {
        auto color = platformAlertHighlightColor(
            *this, platformIndicatorMode, useActivityPlatformHighlightColors);
        if (!color)
        {
            return {};
        }
        return {
            color,
        };
    }

    if (this->flags.has(MessageFlag::AutoModOffendingMessage) ||
        this->flags.has(MessageFlag::AutoModOffendingMessageHeader))
    {
        return {
            ColorProvider::instance().color(ColorType::AutomodHighlight),
        };
    }

    return {};
}

std::shared_ptr<Message> Message::clone() const
{
    auto cloned = std::make_shared<Message>();
    cloned->flags = this->flags;
    cloned->parseTime = this->parseTime;
    cloned->id = this->id;
    cloned->searchText = this->searchText;
    cloned->messageText = this->messageText;
    cloned->loginName = this->loginName;
    cloned->displayName = this->displayName;
    cloned->localizedName = this->localizedName;
    cloned->userID = this->userID;
    cloned->timeoutUser = this->timeoutUser;
    cloned->channelName = this->channelName;
    cloned->usernameColor = this->usernameColor;
    cloned->serverReceivedTime = this->serverReceivedTime;
    cloned->twitchBadges = this->twitchBadges;
    cloned->twitchBadgeInfos = this->twitchBadgeInfos;
    cloned->externalBadges = this->externalBadges;
    cloned->highlightColor = this->highlightColor;
    cloned->platformAccentColor = this->platformAccentColor;
    cloned->replyThread = this->replyThread;
    cloned->replyParent = this->replyParent;
    cloned->count = this->count;
    cloned->reward = this->reward;
    cloned->platform = this->platform;
    cloned->kickMessageJson = this->kickMessageJson;
    cloned->bits = this->bits;
    cloned->giftedSubscriptionRecipientCount =
        this->giftedSubscriptionRecipientCount;
    cloned->kickGiftKicks = this->kickGiftKicks;
    cloned->tiktokGiftDiamondCount = this->tiktokGiftDiamondCount;
    std::ranges::transform(this->elements, std::back_inserter(cloned->elements),
                           [](const auto &element) {
                               return element->clone();
                           });
    return cloned;
}

QJsonObject Message::toJson() const
{
    QJsonObject msg{
        {"flags"_L1, qmagicenum::enumFlagsName(this->flags.value())},
        {"id"_L1, this->id},
        {"searchText"_L1, this->searchText},
        {"messageText"_L1, this->messageText},
        {"loginName"_L1, this->loginName},
        {"displayName"_L1, this->displayName},
        {"localizedName"_L1, this->localizedName},
        {"userID"_L1, this->userID},
        {"timeoutUser"_L1, this->timeoutUser},
        {"channelName"_L1, this->channelName},
        {"usernameColor"_L1, this->usernameColor.name(QColor::HexArgb)},
        {"count"_L1, static_cast<qint64>(this->count)},
        {"serverReceivedTime"_L1,
         this->serverReceivedTime.toString(Qt::ISODate)},
        {"frozen"_L1, this->frozen},
    };

    QJsonArray twitchBadges;
    for (const auto &badge : this->twitchBadges)
    {
        twitchBadges.append(badge.key_);
    }
    msg["twitchBadges"_L1] = twitchBadges;

    QJsonObject twitchBadgeInfos;
    for (const auto &[key, value] : this->twitchBadgeInfos)
    {
        twitchBadgeInfos.insert(key, value);
    }
    msg["twitchBadgeInfos"_L1] = twitchBadgeInfos;

    msg["externalBadges"_L1] = QJsonArray::fromStringList(this->externalBadges);

    if (this->highlightColor)
    {
        msg["highlightColor"_L1] = this->highlightColor->name(QColor::HexArgb);
    }
    if (this->platformAccentColor)
    {
        msg["platformAccentColor"_L1] =
            this->platformAccentColor->name(QColor::HexArgb);
    }

    if (this->replyThread)
    {
        msg["replyThread"_L1] = this->replyThread->toJson();
    }

    if (this->replyParent)
    {
        msg["replyParent"_L1] = this->replyParent->id;
    }

    if (this->reward)
    {
        msg["reward"_L1] = this->reward->toJson();
    }

    if (this->bits > 0)
    {
        msg["bits"_L1] = static_cast<qint64>(this->bits);
    }
    if (this->giftedSubscriptionRecipientCount > 0)
    {
        msg["giftedSubscriptionRecipientCount"_L1] =
            static_cast<qint64>(this->giftedSubscriptionRecipientCount);
    }
    if (this->kickGiftKicks > 0)
    {
        msg["kickGiftKicks"_L1] =
            static_cast<qint64>(this->kickGiftKicks);
    }
    if (this->tiktokGiftDiamondCount > 0)
    {
        msg["tiktokGiftDiamondCount"_L1] =
            static_cast<qint64>(this->tiktokGiftDiamondCount);
    }

    // XXX: figure out if we can add this in tests
    if (!getApp()->isTest())
    {
        msg["parseTime"_L1] = this->parseTime.toString(Qt::ISODate);
    }

    QJsonArray elements;
    for (const auto &element : this->elements)
    {
        elements.append(element->toJson());
    }
    msg["elements"_L1] = elements;

    if (this->platform != MessagePlatform::AnyOrTwitch)
    {
        msg["platform"_L1] = qmagicenum::enumNameString(this->platform);
    }

    return msg;
}

Message::ReplyStatus Message::isReplyable() const
{
    if (this->loginName.isEmpty())
    {
        // no replies can happen
        return ReplyStatus::NotReplyable;
    }

    constexpr int oneDayInSeconds = 24 * 60 * 60;
    bool messageReplyable = true;
    if (this->flags.hasAny({MessageFlag::System, MessageFlag::Subscription,
                            MessageFlag::Timeout, MessageFlag::Whisper,
                            MessageFlag::ModerationAction,
                            MessageFlag::InvalidReplyTarget}) ||
        this->serverReceivedTime.secsTo(QDateTime::currentDateTime()) >
            oneDayInSeconds)
    {
        messageReplyable = false;
    }

    if (this->replyThread != nullptr)
    {
        if (const auto &rootPtr = this->replyThread->root(); rootPtr != nullptr)
        {
            assert(this != rootPtr.get());
            if (rootPtr->isReplyable() == ReplyStatus::NotReplyable)
            {
                // thread parent must be replyable to be replyable
                return ReplyStatus::NotReplyableDueToThread;
            }

            return messageReplyable ? ReplyStatus::ReplyableWithThread
                                    : ReplyStatus::NotReplyableWithThread;
        }
    }

    return messageReplyable ? ReplyStatus::Replyable
                            : ReplyStatus::NotReplyable;
}

}  // namespace chatterino
