// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "Test.hpp"

#include "common/WindowDescriptors.hpp"

#include "messages/Message.hpp"
#include "messages/MessageFlag.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "widgets/helper/ActivityMessageUtils.hpp"

#include <QJsonDocument>
#include <QJsonObject>

using namespace chatterino;

TEST(ActivityMessageUtils, AllowsNativeAlertMessages)
{
    Message message;
    message.flags.set(MessageFlag::Subscription);

    EXPECT_TRUE(isActivityAlertMessage(message));
    EXPECT_TRUE(shouldShowMessageInActivityPane(message));
}

TEST(ActivityMessageUtils, RaidsAndWatchStreaksShareDefaultEnabledSetting)
{
    for (const auto flag : {MessageFlag::Raid, MessageFlag::WatchStreak})
    {
        Message message;
        message.flags.set(MessageFlag::System);
        message.flags.set(flag);

        EXPECT_TRUE(isActivityAlertMessage(message));
        EXPECT_TRUE(shouldShowMessageInActivityPane(message));
        EXPECT_FALSE(shouldShowMessageInActivityPane(message, 100, 100, 0, false));
        EXPECT_TRUE(shouldShowMessageInActivityPane(message, 100, 100, 0, true));
    }

    Message kickHost;
    kickHost.platform = MessagePlatform::Kick;
    kickHost.flags.set(MessageFlag::System);
    kickHost.flags.set(MessageFlag::Raid);
    EXPECT_TRUE(shouldShowMessageInActivityPane(kickHost));
    EXPECT_FALSE(shouldShowMessageInActivityPane(kickHost, 100, 100, 0, false));

    Message subscription;
    subscription.flags.set(MessageFlag::Subscription);
    EXPECT_TRUE(shouldShowMessageInActivityPane(subscription, 100, 100, 0, false));
}

TEST(ActivityMessageUtils, RaidsAndWatchStreaksStillRespectHistoryAndBotExclusions)
{
    for (const auto flag : {MessageFlag::Raid, MessageFlag::WatchStreak})
    {
        Message message;
        message.flags.set(flag);
        message.flags.set(MessageFlag::RecentMessage);
        EXPECT_FALSE(shouldShowMessageInActivityPane(message));

        message.flags.unset(MessageFlag::RecentMessage);
        message.twitchBadges.emplace_back("bot", "1");
        EXPECT_FALSE(shouldShowMessageInActivityPane(message));
    }
}

TEST(ActivityMessageUtils, ActivityEventSettingDefaultsOnAndRestoresSavedValue)
{
    const QJsonObject data{{"type", "twitch"}, {"name", "pajlada"}};
    SplitDescriptor legacy;
    SplitDescriptor::loadFromJSON(legacy, {}, data);
    EXPECT_TRUE(legacy.showRaidsAndWatchStreaks_);

    for (const bool enabled : {false, true})
    {
        const QJsonObject saved{{"showRaidsAndWatchStreaks", enabled}};
        const auto restored =
            QJsonDocument::fromJson(QJsonDocument(saved).toJson()).object();
        SplitDescriptor descriptor;
        SplitDescriptor::loadFromJSON(descriptor, restored, data);
        EXPECT_EQ(descriptor.showRaidsAndWatchStreaks_, enabled);
    }
}

TEST(ActivityMessageUtils, RejectsBotBadgedMessages)
{
    Message twitchBadgedMessage;
    twitchBadgedMessage.flags.set(MessageFlag::CheerMessage);
    twitchBadgedMessage.twitchBadges.emplace_back("bot", "1");

    EXPECT_TRUE(isActivityBotMessage(twitchBadgedMessage));
    EXPECT_FALSE(shouldShowMessageInActivityPane(twitchBadgedMessage));

    Message externalBadgedMessage;
    externalBadgedMessage.flags.set(MessageFlag::ElevatedMessage);
    externalBadgedMessage.externalBadges = {"frankerfacez:bot"};

    EXPECT_TRUE(isActivityBotMessage(externalBadgedMessage));
    EXPECT_FALSE(shouldShowMessageInActivityPane(externalBadgedMessage));
}

TEST(ActivityMessageUtils, RejectsDateSeparators)
{
    Message separator;
    separator.flags.set(MessageFlag::System);
    separator.flags.set(MessageFlag::DoNotLog);
    separator.parseTime = QTime(0, 0);

    EXPECT_TRUE(isActivityDateSeparatorMessage(separator));
    EXPECT_FALSE(shouldShowMessageInActivityPane(separator));

    Message otherSystemMessage;
    otherSystemMessage.flags.set(MessageFlag::System);
    otherSystemMessage.flags.set(MessageFlag::DoNotLog);
    otherSystemMessage.parseTime = QTime(12, 0);

    EXPECT_FALSE(isActivityDateSeparatorMessage(otherSystemMessage));
    EXPECT_FALSE(shouldShowMessageInActivityPane(otherSystemMessage));
}

TEST(ActivityMessageUtils, RejectsPlainChatMessages)
{
    Message message;
    message.messageText = "Thanks for the follow!";

    EXPECT_FALSE(isActivityAlertMessage(message));
    EXPECT_FALSE(isActivityBotMessage(message));
    EXPECT_FALSE(shouldShowMessageInActivityPane(message));
}

TEST(ActivityMessageUtils, FiltersTwitchBitsByMinimum)
{
    Message message;
    message.platform = MessagePlatform::AnyOrTwitch;
    message.flags.set(MessageFlag::CheerMessage);
    message.bits = 5;

    EXPECT_TRUE(isActivityTwitchBitsMessage(message));
    EXPECT_FALSE(shouldShowMessageInActivityPane(message, 100));
    EXPECT_TRUE(shouldShowMessageInActivityPane(message, 5));
}

TEST(ActivityMessageUtils, RejectsTwitchCheerWithoutParsedBits)
{
    Message message;
    message.platform = MessagePlatform::AnyOrTwitch;
    message.flags.set(MessageFlag::CheerMessage);

    EXPECT_FALSE(isActivityTwitchBitsMessage(message));
    EXPECT_FALSE(shouldShowMessageInActivityPane(message, 100));
}

TEST(ActivityMessageUtils, FiltersKickKicksByMinimum)
{
    Message message;
    message.platform = MessagePlatform::Kick;
    message.flags.set(MessageFlag::CheerMessage);
    message.kickGiftKicks = 5;

    EXPECT_TRUE(isActivityKickKicksGiftMessage(message));
    EXPECT_FALSE(shouldShowMessageInActivityPane(message, 100, 100));
    EXPECT_TRUE(shouldShowMessageInActivityPane(message, 100, 5));
}

TEST(ActivityMessageUtils, RejectsKickCheerWithoutParsedKicks)
{
    Message message;
    message.platform = MessagePlatform::Kick;
    message.flags.set(MessageFlag::CheerMessage);

    EXPECT_FALSE(isActivityKickKicksGiftMessage(message));
    EXPECT_FALSE(shouldShowMessageInActivityPane(message, 100, 100));
}

TEST(ActivityMessageUtils, CompactsGiftBombMembershipsToSubs)
{
    Message message;
    message.flags.set(MessageFlag::Subscription);
    message.messageText = "GiftLord gifted 5 memberships to viewers!";

    ASSERT_EQ(getActivityGiftBombRecipientCount(message), 5);
    EXPECT_EQ(compactActivityGiftBombText(message),
              "GiftLord gifted 5 memberships");
}

TEST(ActivityMessageUtils, CompactsTwitchCommunityGiftBombs)
{
    Message message;
    message.flags.set(MessageFlag::Subscription);
    message.messageText = "GiftLord is gifting 10 Tier 1 Subs to the community!";

    ASSERT_EQ(getActivityGiftBombRecipientCount(message), 10);
    EXPECT_EQ(compactActivityGiftBombText(message), "GiftLord gifted 10 subs");
}

TEST(ActivityMessageUtils, CompactsYouTubeGiftPurchaseAnnouncements)
{
    Message message;
    message.flags.set(MessageFlag::Subscription);
    message.displayName = "Halfman";
    message.messageText = "Sent 20 Coconut B gift memberships";

    ASSERT_EQ(getActivityGiftBombRecipientCount(message), 20);
    EXPECT_EQ(compactActivityGiftBombText(message),
              "Halfman gifted 20 memberships");
}

TEST(ActivityMessageUtils, CompactsKickGiftedSubscriptionLists)
{
    Message message;
    message.platform = MessagePlatform::Kick;
    message.flags.set(MessageFlag::Subscription);
    message.flags.set(MessageFlag::Collapsed);
    message.messageText =
        "GiftLord gifted 3 subscriptions to ViewerA, ViewerB, and ViewerC. "
        "They gifted 99 subs in total. ";

    ASSERT_EQ(getActivityGiftBombRecipientCount(message), 3);
    EXPECT_EQ(compactActivityGiftBombText(message), "GiftLord gifted 3 subs");
    EXPECT_TRUE(shouldShowMessageInActivityPane(message));
}

TEST(ActivityMessageUtils, CompactsStructuredKickGiftedSubscriptionLists)
{
    Message message;
    message.platform = MessagePlatform::Kick;
    message.flags.set(MessageFlag::Subscription);
    message.flags.set(MessageFlag::Collapsed);
    message.loginName = "GiftLord";
    message.giftedSubscriptionRecipientCount = 10;
    message.messageText = "GiftLord gifted subscriptions to a list of viewers";

    ASSERT_EQ(getActivityGiftBombRecipientCount(message), 10);
    EXPECT_EQ(compactActivityGiftBombText(message), "GiftLord gifted 10 subs");
    EXPECT_TRUE(shouldShowMessageInActivityPane(message));
}

TEST(ActivityMessageUtils, CompactsSingleCountedGiftWithoutRecipientName)
{
    Message message;
    message.platform = MessagePlatform::Kick;
    message.flags.set(MessageFlag::Subscription);
    message.messageText =
        "GiftLord gifted 1 subscription to ViewerA. They gifted 99 subs in "
        "total.";

    ASSERT_EQ(getActivityGiftBombRecipientCount(message), 1);
    EXPECT_EQ(compactActivityGiftBombText(message), "GiftLord gifted 1 sub");
    EXPECT_TRUE(shouldShowMessageInActivityPane(message));
}

TEST(ActivityMessageUtils, DetectsTwitchAndYouTubeGiftRecipients)
{
    Message twitchRecipient;
    twitchRecipient.flags.set(MessageFlag::Subscription);
    twitchRecipient.flags.set(MessageFlag::System);
    twitchRecipient.messageText = "GiftLord gifted a Tier 1 sub to Viewer!";

    EXPECT_TRUE(isActivityGiftRecipientMessage(twitchRecipient));

    Message twitchMultiMonthRecipient;
    twitchMultiMonthRecipient.flags.set(MessageFlag::Subscription);
    twitchMultiMonthRecipient.flags.set(MessageFlag::System);
    twitchMultiMonthRecipient.messageText =
        "GiftLord gifted 6 months of Tier 1 to Viewer. They've gifted 334 months in the channel!";

    EXPECT_TRUE(isActivityGiftRecipientMessage(twitchMultiMonthRecipient));
    const auto twitchMultiMonthCount =
        getActivityGiftBombRecipientCount(twitchMultiMonthRecipient);
    EXPECT_FALSE(twitchMultiMonthCount.has_value());

    Message twitchMultiMonthRecipientWithSub;
    twitchMultiMonthRecipientWithSub.flags.set(MessageFlag::Subscription);
    twitchMultiMonthRecipientWithSub.flags.set(MessageFlag::System);
    twitchMultiMonthRecipientWithSub.messageText =
        "An anonymous user gifted 6 months of a Tier 1 sub to Viewer!";

    EXPECT_TRUE(isActivityGiftRecipientMessage(twitchMultiMonthRecipientWithSub));
    const auto twitchMultiMonthWithSubCount =
        getActivityGiftBombRecipientCount(twitchMultiMonthRecipientWithSub);
    EXPECT_FALSE(twitchMultiMonthWithSubCount.has_value());

    Message twitchFirstGiftRecipient;
    twitchFirstGiftRecipient.flags.set(MessageFlag::Subscription);
    twitchFirstGiftRecipient.flags.set(MessageFlag::System);
    twitchFirstGiftRecipient.messageText =
        "GiftLord gifted a Tier 1 sub to Viewer! This is their first Gift Sub "
        "in the channel!";

    EXPECT_TRUE(isActivityGiftRecipientMessage(twitchFirstGiftRecipient));

    Message youtubeRecipient;
    youtubeRecipient.flags.set(MessageFlag::Subscription);
    youtubeRecipient.messageText =
        "Viewer received a gift membership from GiftLord!";

    EXPECT_TRUE(isActivityGiftRecipientMessage(youtubeRecipient));

    Message youtubeRecipientBy;
    youtubeRecipientBy.flags.set(MessageFlag::Subscription);
    youtubeRecipientBy.messageText =
        "Viewer received a gift membership by GiftLord";

    EXPECT_TRUE(isActivityGiftRecipientMessage(youtubeRecipientBy));

    Message giftedByRecipient;
    giftedByRecipient.flags.set(MessageFlag::Subscription);
    giftedByRecipient.messageText = "Viewer was gifted a membership by GiftLord";

    EXPECT_TRUE(isActivityGiftRecipientMessage(giftedByRecipient));
}

TEST(ActivityMessageUtils, RejectsKickRewardRedemptions)
{
    Message message;
    message.platform = MessagePlatform::Kick;
    message.flags.set(MessageFlag::CheerMessage);
    message.flags.set(MessageFlag::RedeemedChannelPointReward);
    message.messageText = "Viewer redeemed Highlight My Message";

    EXPECT_TRUE(isActivityKickRewardRedemptionMessage(message));
    EXPECT_FALSE(shouldShowMessageInActivityPane(message));
}

TEST(ActivityMessageUtils, RejectsTwitchAnnouncementHeader)
{
    Message message;
    message.platform = MessagePlatform::AnyOrTwitch;
    message.flags.set(MessageFlag::Subscription);
    message.flags.set(MessageFlag::System);
    message.messageText = "Announcement";

    EXPECT_TRUE(isActivityTwitchAnnouncementHeaderMessage(message));
    EXPECT_FALSE(shouldShowMessageInActivityPane(message));
}

TEST(ActivityMessageUtils, DetectsTwitchAnnouncementFollowup)
{
    Message message;
    message.platform = MessagePlatform::AnyOrTwitch;
    message.flags.set(MessageFlag::Subscription);
    message.flags.set(MessageFlag::Highlighted);
    message.messageText = "StreamElements says hello";

    EXPECT_TRUE(isActivityTwitchAnnouncementFollowupMessage(message));
}

TEST(ActivityMessageUtils, RejectsTwitchBitsBadgeMessages)
{
    Message message;
    message.platform = MessagePlatform::AnyOrTwitch;
    message.flags.set(MessageFlag::Subscription);
    message.flags.set(MessageFlag::System);
    message.messageText = "whoopiix just earned a new 1K Bits badge!";

    EXPECT_TRUE(isActivityTwitchBitsBadgeMessage(message));
    EXPECT_FALSE(shouldShowMessageInActivityPane(message));
}
