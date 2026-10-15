#pragma once

#include "providers/merged/MergedChannel.hpp"

#include <utility>

namespace chatterino {
class MergedChannelTestAccess
{
public:
    static void append(MergedChannel &channel, const MessagePtr &msg,
                       MessagePlatform platform)
    {
        channel.appendMergedMessage(msg, platform);
    }
    static void replace(MergedChannel &channel, const MessagePtr &old,
                        const MessagePtr &next, MessagePlatform platform)
    {
        channel.replaceMergedMessage(old, next, platform);
    }
    static void connect(MergedChannel &channel, const ChannelPtr &source,
                        MessagePlatform platform)
    {
        auto &connections = platform == MessagePlatform::Kick
                                ? channel.kickConnections_
                                : channel.twitchConnections_;
        channel.connectSourceSignals(source, platform, connections);
    }
    static void setSources(MergedChannel &channel, ChannelPtr twitch,
                           ChannelPtr kick)
    {
        channel.twitchChannel_ = std::move(twitch);
        channel.kickChannel_ = std::move(kick);
    }
};
}  // namespace chatterino
