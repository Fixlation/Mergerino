#pragma once

#include "twitch-eventsub-ws/payloads/subscription.hpp"
#include "twitch-eventsub-ws/string.hpp"

#include <boost/json.hpp>

namespace chatterino::eventsub::lib::payload::channel_shoutout_create::v1 {

// https://dev.twitch.tv/docs/eventsub/eventsub-subscription-types/#channelshoutoutcreate
// The event also has started_at, viewer_count, cooldown_ends_at and
// target_cooldown_ends_at, which aren't used.

struct Event {
    // The channel that gave the shoutout
    String broadcasterUserID;
    String broadcasterUserLogin;
    String broadcasterUserName;

    // The moderator or broadcaster who sent the shoutout
    String moderatorUserID;
    String moderatorUserLogin;
    String moderatorUserName;

    // The channel that received the shoutout
    String toBroadcasterUserID;
    String toBroadcasterUserLogin;
    String toBroadcasterUserName;
};

struct Payload {
    subscription::Subscription subscription;

    Event event;
};

#include "twitch-eventsub-ws/payloads/channel-shoutout-create-v1.inc"

}  // namespace chatterino::eventsub::lib::payload::channel_shoutout_create::v1
