// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Message.hpp"

#include <optional>

namespace chatterino {

bool isActivityAlertMessage(const Message &message);
bool isActivityBotMessage(const Message &message);
bool isActivityDateSeparatorMessage(const Message &message);
bool isActivityKickRewardRedemptionMessage(const Message &message);
bool isActivityTwitchAnnouncementHeaderMessage(const Message &message);
bool isActivityTwitchAnnouncementFollowupMessage(const Message &message);
bool isActivityTwitchBitsBadgeMessage(const Message &message);
bool isActivityTwitchBitsMessage(const Message &message);
bool shouldShowTwitchBitsInActivityPane(const Message &message,
                                        uint32_t minimumBits);
bool isActivityKickKicksGiftMessage(const Message &message);
bool shouldShowKickKicksGiftInActivityPane(const Message &message,
                                           uint32_t minimumKicks);
bool isActivityTikTokGiftMessage(const Message &message);
bool shouldShowTikTokGiftInActivityPane(const Message &message,
                                        uint32_t minimumDiamondCount);
std::optional<int> getActivityGiftBombRecipientCount(const Message &message);
bool isActivityGiftRecipientMessage(const Message &message);
QString compactActivityGiftBombText(const Message &message);
bool shouldShowMessageInActivityPane(const Message &message,
                                     uint32_t twitchMinimumBits = 100,
                                     uint32_t kickMinimumKicks = 100,
                                     uint32_t tiktokGiftMinimumDiamonds = 0,
                                     bool showRaidsAndWatchStreaks = true);

}  // namespace chatterino
