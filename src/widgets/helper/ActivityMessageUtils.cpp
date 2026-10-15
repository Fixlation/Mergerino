// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/ActivityMessageUtils.hpp"

#include "messages/MessageFlag.hpp"
#include "providers/twitch/TwitchBadge.hpp"

#include <QRegularExpression>
#include <QTime>

#include <limits>
#include <ranges>

namespace {

using namespace chatterino;

struct ActivityGiftBombSummary {
    QString name;
    int count = 0;
    QString unit;
};

const QRegularExpression ACTIVITY_COMMUNITY_GIFT_SUMMARY_REGEX(
    QStringLiteral(
        R"(^(.+?)\s+(?:is gifting|gifted)\s+(\d+)\s+.*?\b(?:gift\s+)?(subscriptions|memberships|subs)\b(?:\s+to\b.*)?(?:!|\.)?(?:\s+.*)?$)"),
    QRegularExpression::CaseInsensitiveOption);

const QRegularExpression ACTIVITY_SENT_GIFT_SUMMARY_REGEX(
    QStringLiteral(
        R"(^sent\s+(\d+)\s+.*?\b(?:gift\s+)?(subscriptions|memberships|subs)\b(?:!|\.)?(?:\s+.*)?$)"),
    QRegularExpression::CaseInsensitiveOption);

const QRegularExpression ACTIVITY_COUNTED_GIFT_SUMMARY_REGEX(
    QStringLiteral(
        R"(^(.+?)\s+gifted\s+(\d+)\s+(?:(?:tier)\s+\d+\s+)?(?:gift\s+)?(subscriptions?|memberships?|subs?)\b\s+to\b.*$)"),
    QRegularExpression::CaseInsensitiveOption);

const QRegularExpression ACTIVITY_GIFT_RECIPIENT_REGEX(
    QStringLiteral(
        R"((?:^.+\s+gifted\s+(?:(?:an?\s+|\d+\s+months?\s+of\s+an?\s+).*)?\b(?:sub|subscription|membership)\b\s+to\s+.+(?:!|\.)?(?:\s+.*)?$)|(?:^.+\s+gifted\s+\d+\s+months?\s+of\s+.+\s+to\s+.+(?:!|\.)?(?:\s+.*)?$)|(?:^(?:.+\s+)?(?:received|was gifted)\s+(?:a\s+gift\s+|an?\s+)?(?:sub|subscription|membership)\s+(?:from|by)\s+.+(?:!|\.)?(?:\s+.*)?$))"),
    QRegularExpression::CaseInsensitiveOption);

const QRegularExpression ACTIVITY_TWITCH_BITS_BADGE_REGEX(
    QStringLiteral(R"(^.+\s+just earned a new\s+.+\s+Bits badge!$)"),
    QRegularExpression::CaseInsensitiveOption);

QString normalizedActivityGiftUnit(const QString &matchedUnit, int count)
{
    if (matchedUnit.contains(QStringLiteral("membership"),
                             Qt::CaseInsensitive))
    {
        return count == 1 ? QStringLiteral("membership")
                          : QStringLiteral("memberships");
    }

    return count == 1 ? QStringLiteral("sub") : QStringLiteral("subs");
}

std::optional<ActivityGiftBombSummary> activityGiftBombSummary(
    const Message &message)
{
    if (!message.flags.has(MessageFlag::Subscription))
    {
        return std::nullopt;
    }

    if (message.giftedSubscriptionRecipientCount > 0)
    {
        auto name = message.displayName.trimmed();
        if (name.isEmpty())
        {
            name = message.loginName.trimmed();
        }

        const auto count =
            message.giftedSubscriptionRecipientCount >
                    static_cast<uint32_t>(std::numeric_limits<int>::max())
                ? std::numeric_limits<int>::max()
                : static_cast<int>(message.giftedSubscriptionRecipientCount);

        return ActivityGiftBombSummary{
            .name = name,
            .count = count,
            .unit = QStringLiteral("subs"),
        };
    }

    if (const auto match =
            ACTIVITY_COUNTED_GIFT_SUMMARY_REGEX.match(message.messageText);
        match.hasMatch())
    {
        return ActivityGiftBombSummary{
            .name = match.captured(1).trimmed(),
            .count = match.captured(2).toInt(),
            .unit = match.captured(3),
        };
    }

    if (const auto match =
            ACTIVITY_COMMUNITY_GIFT_SUMMARY_REGEX.match(message.messageText);
        match.hasMatch())
    {
        return ActivityGiftBombSummary{
            .name = match.captured(1).trimmed(),
            .count = match.captured(2).toInt(),
            .unit = match.captured(3),
        };
    }

    if (const auto match =
            ACTIVITY_SENT_GIFT_SUMMARY_REGEX.match(message.messageText);
        match.hasMatch())
    {
        return ActivityGiftBombSummary{
            .count = match.captured(1).toInt(),
            .unit = match.captured(2),
        };
    }

    return std::nullopt;
}

}  // namespace

namespace chatterino {

bool isActivityAlertMessage(const Message &message)
{
    return message.flags.hasAny({MessageFlag::Subscription,
                                 MessageFlag::ElevatedMessage,
                                 MessageFlag::CheerMessage,
                                 MessageFlag::Raid, MessageFlag::WatchStreak});
}

bool isActivityBotMessage(const Message &message)
{
    const auto hasTwitchBotBadge =
        std::ranges::any_of(message.twitchBadges, [](const auto &badge) {
            return badge.key_.compare("bot", Qt::CaseInsensitive) == 0 ||
                   badge.key_.compare("chatbot", Qt::CaseInsensitive) == 0;
        });
    if (hasTwitchBotBadge)
    {
        return true;
    }

    return std::ranges::any_of(message.externalBadges, [](const auto &badge) {
        return badge.endsWith(":bot", Qt::CaseInsensitive);
    });
}

bool isActivityDateSeparatorMessage(const Message &message)
{
    return message.flags.has(MessageFlag::System) &&
           message.flags.has(MessageFlag::DoNotLog) &&
           message.parseTime == QTime(0, 0);
}

bool isActivityKickRewardRedemptionMessage(const Message &message)
{
    return message.platform == MessagePlatform::Kick &&
           message.flags.has(MessageFlag::RedeemedChannelPointReward) &&
           message.kickGiftKicks == 0;
}

bool isActivityTwitchAnnouncementHeaderMessage(const Message &message)
{
    return message.platform == MessagePlatform::AnyOrTwitch &&
           message.flags.has(MessageFlag::Subscription) &&
           message.flags.has(MessageFlag::System) &&
           message.messageText == QStringLiteral("Announcement");
}

bool isActivityTwitchAnnouncementFollowupMessage(const Message &message)
{
    return message.platform == MessagePlatform::AnyOrTwitch &&
           message.flags.has(MessageFlag::Subscription) &&
           !message.flags.has(MessageFlag::System);
}

bool isActivityTwitchBitsBadgeMessage(const Message &message)
{
    return message.platform == MessagePlatform::AnyOrTwitch &&
           message.flags.has(MessageFlag::Subscription) &&
           message.flags.has(MessageFlag::System) &&
           ACTIVITY_TWITCH_BITS_BADGE_REGEX.match(message.messageText)
               .hasMatch();
}

bool isActivityTwitchBitsMessage(const Message &message)
{
    return message.platform == MessagePlatform::AnyOrTwitch &&
           message.flags.has(MessageFlag::CheerMessage) && message.bits > 0;
}

bool shouldShowTwitchBitsInActivityPane(const Message &message,
                                        uint32_t minimumBits)
{
    return isActivityTwitchBitsMessage(message) && message.bits >= minimumBits;
}

bool isActivityKickKicksGiftMessage(const Message &message)
{
    return message.platform == MessagePlatform::Kick &&
           message.kickGiftKicks > 0;
}

bool shouldShowKickKicksGiftInActivityPane(const Message &message,
                                           uint32_t minimumKicks)
{
    return isActivityKickKicksGiftMessage(message) &&
           message.kickGiftKicks >= minimumKicks;
}

bool isActivityTikTokGiftMessage(const Message &message)
{
    return message.platform == MessagePlatform::TikTok &&
           message.flags.has(MessageFlag::Subscription) &&
           message.tiktokGiftDiamondCount > 0;
}

bool shouldShowTikTokGiftInActivityPane(const Message &message,
                                        uint32_t minimumDiamondCount)
{
    return isActivityTikTokGiftMessage(message) &&
           message.tiktokGiftDiamondCount >= minimumDiamondCount;
}

std::optional<int> getActivityGiftBombRecipientCount(const Message &message)
{
    const auto summary = activityGiftBombSummary(message);
    if (!summary)
    {
        return std::nullopt;
    }

    return summary->count;
}

bool isActivityGiftRecipientMessage(const Message &message)
{
    return message.flags.has(MessageFlag::Subscription) &&
           ACTIVITY_GIFT_RECIPIENT_REGEX.match(message.messageText).hasMatch();
}

QString compactActivityGiftBombText(const Message &message)
{
    const auto summary = activityGiftBombSummary(message);
    if (!summary)
    {
        return message.messageText;
    }

    auto name = summary->name.trimmed();
    if (name.isEmpty())
    {
        name = message.displayName.trimmed();
    }
    if (name.isEmpty())
    {
        name = message.loginName.trimmed();
    }
    if (name.isEmpty())
    {
        name = QStringLiteral("Someone");
    }

    const int count = summary->count;
    const auto unit = normalizedActivityGiftUnit(summary->unit, count);
    return QStringLiteral("%1 gifted %2 %3")
        .arg(name, QString::number(count), unit);
}

bool shouldShowMessageInActivityPane(const Message &message,
                                     uint32_t twitchMinimumBits,
                                     uint32_t kickMinimumKicks,
                                     uint32_t tiktokGiftMinimumDiamonds,
                                     bool showRaidsAndWatchStreaks)
{
    if (isActivityBotMessage(message))
    {
        return false;
    }

    if (message.flags.has(MessageFlag::RecentMessage))
    {
        return false;
    }

    if (isActivityDateSeparatorMessage(message) ||
        isActivityKickRewardRedemptionMessage(message) ||
        isActivityTwitchAnnouncementHeaderMessage(message) ||
        isActivityTwitchBitsBadgeMessage(message))
    {
        return false;
    }

    if (isActivityGiftRecipientMessage(message) &&
        !getActivityGiftBombRecipientCount(message))
    {
        return false;
    }

    if (message.flags.hasAny({MessageFlag::Raid, MessageFlag::WatchStreak}))
    {
        return showRaidsAndWatchStreaks;
    }

    if (isActivityTwitchBitsMessage(message))
    {
        return shouldShowTwitchBitsInActivityPane(message, twitchMinimumBits);
    }

    if (isActivityKickKicksGiftMessage(message))
    {
        return shouldShowKickKicksGiftInActivityPane(message,
                                                    kickMinimumKicks);
    }

    if (isActivityTikTokGiftMessage(message))
    {
        return shouldShowTikTokGiftInActivityPane(
            message, tiktokGiftMinimumDiamonds);
    }

    if (message.flags.has(MessageFlag::CheerMessage))
    {
        return false;
    }

    return isActivityAlertMessage(message);
}

}  // namespace chatterino
