// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"

#include <optional>

namespace chatterino {

struct CommandContext;
struct Message;
using MessagePtr = std::shared_ptr<const Message>;

namespace commands {

bool canPinChatMessage(const ChannelPtr &channel, const MessagePtr &message);
ChannelPtr channelForPinnedMessage(const ChannelPtr &channel,
                                   PinnedChatMessage::Platform platform);

void pinChatMessage(const ChannelPtr &channel, const MessagePtr &message,
                    std::optional<int> durationSeconds = 1200);
void updatePinnedChatMessageDuration(
    const ChannelPtr &channel, std::optional<int> durationSeconds);
void unpinChatMessage(const ChannelPtr &channel);
void refreshPinnedChatMessage(const ChannelPtr &channel);

QString pinChatMessageCommand(const CommandContext &ctx);

}  // namespace commands
}  // namespace chatterino
