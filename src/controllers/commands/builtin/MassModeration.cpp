// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/MassModeration.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/dialogs/MassModerationDialog.hpp"
#include "widgets/Window.hpp"

#include <QPointer>
#include <QTimer>

#include <memory>
#include <utility>

namespace chatterino {
namespace {

using Queue = MassModerationQueue;

// Always return to the dialog's thread before checking Pause/Stop or the account.
void onReceiver(QPointer<QObject> receiver, std::function<void()> fn)
{
    if (receiver)
    {
        QTimer::singleShot(0, receiver.data(), std::move(fn));
    }
}

Queue::Result kickFailure(const QString &message)
{
    const auto lower = message.toLower();
    if (lower.contains("429") || lower.contains("too many requests") ||
        lower.contains("rate limit") || lower.contains("ratelimit"))
    {
        return {
            Queue::Outcome::RateLimited,
            message + " Queue paused; increase the interval and resume later."};
    }
    if (lower.contains("401") || lower.contains("403") ||
        lower.contains("unauthoriz") || lower.contains("forbidden") ||
        lower.contains("permission") || lower.contains("scope"))
    {
        return {Queue::Outcome::Fatal, message + " Queue stopped."};
    }
    return {Queue::Outcome::Failed, message};
}

template <typename Error>
Queue::Result twitchFailure(Error error, QString message)
{
    if (error == Error::Ratelimited)
    {
        return {
            Queue::Outcome::RateLimited,
            "Twitch rate limit reached. Queue paused; increase the interval "
            "and resume later."};
    }
    if (error == Error::UserMissingScope || error == Error::UserNotAuthorized)
    {
        return {Queue::Outcome::Fatal,
                "Twitch authorization or moderation permission is missing. "
                "Queue stopped; check your account and channel permissions."};
    }
    if (message.isEmpty())
    {
        message = "Twitch could not complete this action.";
    }
    return {Queue::Outcome::Failed, std::move(message)};
}

QString openDialog(const CommandContext &ctx, bool ban)
{
    if (!ctx.channel)
    {
        return {};
    }
    auto channel = ctx.channel;
    if (auto merged = std::dynamic_pointer_cast<MergedChannel>(channel))
    {
        if (ctx.twitchChannel && ctx.kickChannel)
        {
            channel->addSystemMessage("Choose Twitch or Kick as the send "
                                      "target before using mass moderation.");
            return {};
        }
        channel =
            ctx.kickChannel ? merged->kickChannel() : merged->twitchChannel();
    }
    if (!channel || !channel->isTwitchOrKickChannel())
    {
        ctx.channel->addSystemMessage(
            "Mass moderation only works in Twitch or Kick channels.");
        return {};
    }
    if (!channel->hasModRights())
    {
        ctx.channel->addSystemMessage(
            "Mass moderation requires moderator or broadcaster rights "
            "on the selected platform.");
        return {};
    }
    auto *dialog = MassModerationDialog::find(channel, ban);
    if (!dialog)
    {
        dialog = new MassModerationDialog(
            channel, ban, &getApp()->getWindows()->getMainWindow());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
    }
    dialog->reopen();
    return {};
}

}  // namespace

ExpectedStr<Queue::Action> makeMassModerationAction(const ChannelPtr &channel,
                                                    bool ban,
                                                    const QString &reason,
                                                    QObject *receiver)
{
    if (!channel || !channel->hasModRights())
    {
        return makeUnexpected(QStringLiteral(
            "Mass moderation requires moderator or broadcaster rights "
            "on the selected platform."));
    }
    const QPointer<QObject> context(receiver);
    if (auto kick = std::dynamic_pointer_cast<KickChannel>(channel))
    {
        const auto account = getApp()->getAccounts()->kick.current();
        if (!account || account->isAnonymous() ||
            account->authToken().isEmpty())
        {
            return makeUnexpected(
                QStringLiteral("Log in to Kick before starting."));
        }
        const auto broadcaster = kick->userID();
        if (broadcaster == 0)
        {
            return makeUnexpected(
                QStringLiteral("Kick channel information is still loading. Try "
                               "again shortly."));
        }
        const auto accountID = account->userID();
        auto authorized = [account, accountID, weak = std::weak_ptr(kick)] {
            auto target = weak.lock();
            auto current = getApp()->getAccounts()->kick.current();
            return target && target->hasModRights() && current == account &&
                   !current->isAnonymous() && current->userID() == accountID &&
                   !current->authToken().isEmpty();
        };
        return Queue::Action([context, authorized, broadcaster, ban, reason](
                                 const QString &name, Queue::Guard canAct,
                                 Queue::Completion done) {
            if (!authorized())
            {
                done({Queue::Outcome::Fatal,
                      "Kick account changed, logged out, or moderation rights "
                      "lost. Queue stopped."});
                return;
            }
            getKickApi()->getChannelByName(name, [context, authorized,
                                                  broadcaster, ban, reason,
                                                  canAct, done](auto result) {
                onReceiver(context, [authorized, broadcaster, ban, reason,
                                     canAct, done, result = std::move(result)] {
                    if (!canAct())
                    {
                        done({Queue::Outcome::Cancelled, {}});
                        return;
                    }
                    if (!authorized())
                    {
                        done({Queue::Outcome::Fatal,
                              "Kick account changed, logged out, or moderation "
                              "rights lost. Queue "
                              "stopped."});
                        return;
                    }
                    if (!result || result->userID == 0)
                    {
                        done(result ? Queue::Result{Queue::Outcome::Failed,
                                                    "Kick user not found."}
                                    : kickFailure(result.error()));
                        return;
                    }
                    auto completed = [done](auto response) {
                        done(response
                                 ? Queue::Result{Queue::Outcome::Success, {}}
                                 : kickFailure(response.error()));
                    };
                    if (ban)
                    {
                        getKickApi()->banUser(broadcaster, result->userID,
                                              std::nullopt, reason, completed);
                    }
                    else
                    {
                        getKickApi()->unbanUser(broadcaster, result->userID,
                                                completed);
                    }
                });
            });
        });
    }
    if (auto twitch = std::dynamic_pointer_cast<TwitchChannel>(channel))
    {
        const auto account = getApp()->getAccounts()->twitch.getCurrent();
        if (!account || account->isAnon() || account->getOAuthToken().isEmpty())
        {
            return makeUnexpected(
                QStringLiteral("Log in to Twitch before starting."));
        }
        const auto broadcaster = twitch->roomId();
        if (broadcaster.isEmpty())
        {
            return makeUnexpected(
                QStringLiteral("Twitch channel information is still loading. "
                               "Try again shortly."));
        }
        const auto moderator = account->getUserId();
        auto authorized = [account, moderator, weak = std::weak_ptr(twitch)] {
            auto target = weak.lock();
            auto current = getApp()->getAccounts()->twitch.getCurrent();
            return target && target->hasModRights() && current == account &&
                   !current->isAnon() && current->getUserId() == moderator &&
                   !current->getOAuthToken().isEmpty();
        };
        return Queue::Action([context, authorized, broadcaster, moderator, ban,
                              reason](const QString &name, Queue::Guard canAct,
                                      Queue::Completion done) {
            if (!authorized())
            {
                done({Queue::Outcome::Fatal,
                      "Twitch account changed, logged out, or moderation "
                      "rights lost. Queue stopped."});
                return;
            }
            getHelix()->getUserByName(
                name,
                [context, authorized, broadcaster, moderator, ban, reason,
                 canAct, done](const HelixUser &user) {
                    onReceiver(context, [authorized, broadcaster, moderator,
                                         ban, reason, canAct, done, user] {
                        if (!canAct())
                        {
                            done({Queue::Outcome::Cancelled, {}});
                            return;
                        }
                        if (!authorized())
                        {
                            done({Queue::Outcome::Fatal,
                                  "Twitch account changed, logged out, or "
                                  "moderation rights lost. Queue "
                                  "stopped."});
                            return;
                        }
                        if (user.id.isEmpty())
                        {
                            done({Queue::Outcome::Failed,
                                  "Twitch user not found."});
                            return;
                        }
                        auto success = [done] {
                            done({Queue::Outcome::Success, {}});
                        };
                        if (ban)
                        {
                            getHelix()->banUser(
                                broadcaster, moderator, user.id, std::nullopt,
                                reason, success,
                                [done](auto error, auto message) {
                                    done(twitchFailure(error, message));
                                });
                        }
                        else
                        {
                            getHelix()->unbanUser(
                                broadcaster, moderator, user.id, success,
                                [done](auto error, auto message) {
                                    done(twitchFailure(error, message));
                                });
                        }
                    });
                },
                [done] {
                    done({Queue::Outcome::Failed,
                          "Could not look up this Twitch username."});
                });
        });
    }
    return makeUnexpected(QStringLiteral("Choose a Twitch or Kick channel."));
}

namespace commands {
QString massBan(const CommandContext &ctx)
{
    return openDialog(ctx, true);
}
QString massUnban(const CommandContext &ctx)
{
    return openDialog(ctx, false);
}
}  // namespace commands
}  // namespace chatterino
