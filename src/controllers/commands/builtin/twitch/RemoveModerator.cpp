// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/twitch/RemoveModerator.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchModerationAuth.hpp"
#include "providers/twitch/api/TwitchWebApi.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "util/Twitch.hpp"

namespace chatterino::commands {

QString removeModerator(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "The /unmod command only works in Twitch channels.");
        return "";
    }
    if (ctx.words.size() < 2)
    {
        ctx.channel->addSystemMessage(
            "Usage: \"/unmod <username>\" - Remove a regular moderator "
            "(Broadcaster or Lead Moderator). Use \"/mods\" to list this "
            "channel's moderators.");
        return "";
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon())
    {
        ctx.channel->addSystemMessage(
            "You must be logged in to unmod someone!");
        return "";
    }

    auto target = ctx.words.at(1);
    stripChannelName(target);

    const bool isBroadcaster = ctx.twitchChannel->isBroadcaster();
    TwitchModerationAuth::Account moderationAccount;
    if (!isBroadcaster)
    {
        QString authError;
        moderationAccount = TwitchModerationAuth::resolveForCurrentUser(
            currentUser->getUserId(), &authError);
        if (!moderationAccount.supportsWebGql())
        {
            ctx.channel->addSystemMessage(
                "Lead Moderator access for /unmod requires Twitch mod actions "
                "to be connected in Settings -> Accounts. " +
                authError);
            return "";
        }
    }

    getHelix()->getUserByName(
        target,
        [twitchChannel{ctx.twitchChannel}, channel{ctx.channel},
         currentUserId{currentUser->getUserId()}, isBroadcaster,
         moderationAccount](const HelixUser &targetUser) {
            if (!isBroadcaster)
            {
                TwitchWebApi::getChannelModerationPermissions(
                    twitchChannel->roomId(), currentUserId,
                    moderationAccount.clientId, moderationAccount.oauthToken,
                    [=](const TwitchChannelModerationPermissions &permissions) {
                        if (!permissions.removeModerator)
                        {
                            channel->addSystemMessage(
                                "/unmod can only be used by the broadcaster or "
                                "a Lead Moderator in this channel.");
                            return;
                        }

                        TwitchWebApi::updateChannelRole(
                            TwitchChannelRoleAction::RemoveModerator,
                            twitchChannel->roomId(), targetUser.login,
                            moderationAccount.clientId,
                            moderationAccount.oauthToken,
                            [] {},
                            [channel](const QString &error) {
                                channel->addSystemMessage(
                                    "Failed to remove channel moderator using "
                                    "Lead Moderator access - " +
                                    error);
                            });
                    },
                    [channel](const QString &error) {
                        channel->addSystemMessage(
                            "Failed to check Lead Moderator permission for "
                            "/unmod - " +
                            error);
                    });
                return;
            }

            getHelix()->removeChannelModerator(
                twitchChannel->roomId(), targetUser.id,
                [channel, targetUser] {
                    channel->addSystemMessage(
                        QString("You have removed %1 as a moderator of "
                                "this channel.")
                            .arg(targetUser.displayName));
                },
                [channel, targetUser](auto error, auto message) {
                    QString errorMessage =
                        QString("Failed to remove channel moderator - ");

                    using Error = HelixRemoveChannelModeratorError;

                    switch (error)
                    {
                        case Error::UserMissingScope: {
                            // TODO(pajlada): Phrase MISSING_REQUIRED_SCOPE
                            errorMessage += "Missing required scope. "
                                            "Re-login with your "
                                            "account and try again.";
                        }
                        break;

                        case Error::UserNotAuthorized: {
                            // TODO(pajlada): Phrase MISSING_PERMISSION
                            errorMessage += "You don't have permission to "
                                            "perform that action.";
                        }
                        break;

                        case Error::Ratelimited: {
                            errorMessage +=
                                "You are being ratelimited by Twitch. Try "
                                "again in a few seconds.";
                        }
                        break;

                        case Error::TargetNotModded: {
                            // Equivalent irc error
                            errorMessage +=
                                QString("%1 is not a moderator of this "
                                        "channel.")
                                    .arg(targetUser.displayName);
                        }
                        break;

                        case Error::Forwarded: {
                            errorMessage += message;
                        }
                        break;

                        case Error::Unknown:
                        default: {
                            errorMessage += "An unknown error has occurred.";
                        }
                        break;
                    }
                    channel->addSystemMessage(errorMessage);
                });
        },
        [channel{ctx.channel}, target] {
            // Equivalent error from IRC
            channel->addSystemMessage(
                QString("Invalid username: %1").arg(target));
        });

    return "";
}

}  // namespace chatterino::commands
