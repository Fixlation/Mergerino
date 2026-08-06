// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/completion/sources/CommandSource.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/completion/sources/Helpers.hpp"
#include "messages/Message.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "widgets/splits/InputCompletionItem.hpp"

#include <algorithm>

namespace chatterino::completion {

namespace {

void addCommand(
    const QString &command, std::vector<CommandItem> &out,
    std::vector<MessagePlatform> platforms = {})
{
    if (command.startsWith('/') || command.startsWith('.'))
    {
        out.push_back({
            .name = command.mid(1),
            .prefix = command.at(0),
            .platforms = std::move(platforms),
        });
    }
    else
    {
        out.push_back({
            .name = command,
            .prefix = "",
            .platforms = std::move(platforms),
        });
    }
}

void appendPlatform(std::vector<MessagePlatform> &platforms,
                    MessagePlatform platform)
{
    if (std::ranges::find(platforms, platform) == platforms.end())
    {
        platforms.push_back(platform);
    }
}

QString displayText(const CommandItem &command)
{
    return command.prefix + command.name;
}

QString commandKey(const CommandItem &command)
{
    return displayText(command).toLower();
}

bool commandLessThan(const CommandItem &a, const CommandItem &b)
{
    const auto nameCompare = QString::compare(a.name, b.name,
                                              Qt::CaseInsensitive);
    if (nameCompare != 0)
    {
        return nameCompare < 0;
    }

    return QString::compare(a.prefix, b.prefix, Qt::CaseInsensitive) < 0;
}

bool commandEquals(const CommandItem &a, const CommandItem &b)
{
    return QString::compare(commandKey(a), commandKey(b),
                            Qt::CaseInsensitive) == 0;
}

bool isTwitchCommandTarget(const Channel *channel)
{
    return channel != nullptr &&
           (channel->isTwitchChannel() || channel->isMergedChannel());
}

bool isKickCommandTarget(const Channel *channel)
{
    return channel != nullptr &&
           (channel->isKickChannel() || channel->isMergedChannel());
}

const QStringList &kickDefaultCommands()
{
    static const QStringList commands{
        "/ban",
        "/delete",
        "/streamlink",
        "/timeout",
        "/unban",
        "/untimeout",
    };

    return commands;
}

const QStringList &kickCompatibleChatterinoCommands()
{
    static const QStringList commands{
        "/ban",
        "/debug-kick-raw-event",
        "/delete",
        "/pin",
        "/streamlink",
        "/timeout",
        "/unban",
        "/untimeout",
        "/user",
    };

    return commands;
}

const QStringList &kickOnlyChatterinoCommands()
{
    static const QStringList commands{
        "/debug-kick-raw-event",
    };

    return commands;
}

const QStringList &clientOnlyChatterinoCommands()
{
    static const QStringList commands{
        "/c2-set-logging-rules",
        "/c2-theme-autoreload",
        "/clearmessages",
        "/copy",
        "/debug-args",
        "/debug-env",
        "/debug-force-image-gc",
        "/debug-force-image-unload",
        "/debug-force-layout-channel-views",
        "/debug-increment-image-generation",
        "/debug-invalidate-buffers",
        "/debug-relaunch-with-console",
        "/giveaway",
        "/openurl",
    };

    return commands;
}

const QStringList &moderatorCommands()
{
    static const QStringList commands{
        "/announce",
        "/announceblue",
        "/announcegreen",
        "/announceorange",
        "/announcepurple",
        "/ban",
        "/banid",
        "/chatters",
        "/clear",
        "/delete",
        "/emoteonly",
        "/emoteonlyoff",
        "/followers",
        "/followersoff",
        "/lowtrust",
        "/monitor",
        "/mod",
        "/pin",
        "/poll",
        "/prediction",
        "/requests",
        "/restrict",
        "/r9kbeta",
        "/r9kbetaoff",
        "/shield",
        "/shieldoff",
        "/shoutout",
        "/slow",
        "/slowoff",
        "/subscribers",
        "/subscribersoff",
        "/timeout",
        "/unban",
        "/uniquechat",
        "/uniquechatoff",
        "/unmonitor",
        "/unmod",
        "/unrestrict",
        "/untimeout",
        "/unvip",
        "/vip",
        "/warn",
    };

    return commands;
}

const QStringList &broadcasterCommands()
{
    static const QStringList commands{
        "/cancelprediction",
        "/cancelpoll",
        "/commercial",
        "/completeprediction",
        "/endpoll",
        "/host",
        "/lockprediction",
        "/marker",
        "/mods",
        "/raid",
        "/setgame",
        "/settitle",
        "/unhost",
        "/unraid",
        "/vips",
    };

    return commands;
}

bool canUseCommand(const CommandItem &command, const Channel *channel)
{
    const auto key = commandKey(command);

    if (broadcasterCommands().contains(key, Qt::CaseInsensitive))
    {
        return channel != nullptr && channel->isBroadcaster();
    }

    if (moderatorCommands().contains(key, Qt::CaseInsensitive))
    {
        return channel != nullptr && channel->hasModRights();
    }

    return true;
}

}  // namespace

CommandSource::CommandSource(
    std::unique_ptr<CommandStrategy> strategy, ActionCallback callback,
    const Channel *channel, bool slashCommandsOnly,
    std::vector<MessagePlatform> platformFilter,
    PlatformActionCallback platformCallback)
    : strategy_(std::move(strategy))
    , callback_(std::move(callback))
    , platformCallback_(std::move(platformCallback))
    , channel_(channel)
    , slashCommandsOnly_(slashCommandsOnly)
    , platformFilter_(std::move(platformFilter))
{
    this->initializeItems();
}

void CommandSource::update(const QString &query)
{
    this->output_.clear();
    if (this->strategy_)
    {
        this->strategy_->apply(this->items_, this->output_, query);
    }
}

void CommandSource::addToListModel(GenericListModel &model,
                                   size_t maxCount) const
{
    addVecToListModel(this->output_, model, maxCount,
                      [this](const CommandItem &command) {
                          auto platforms =
                              this->platformFilter_.size() > 1
                                  ? command.platforms
                                  : std::vector<MessagePlatform>{};
                          return std::make_unique<InputCompletionItem>(
                              nullptr, displayText(command), this->callback_,
                              std::move(platforms), this->platformCallback_);
                      });
}

void CommandSource::addToStringList(QStringList &list, size_t maxCount,
                                    bool /* isFirstWord */) const
{
    addVecToStringList(this->output_, list, maxCount,
                       [](const CommandItem &command) {
                           return command.prefix + command.name + " ";
                       });
}

void CommandSource::initializeItems()
{
    std::vector<CommandItem> commands;
    const bool canUseTwitchCommands = isTwitchCommandTarget(this->channel_);
    const bool canUseKickCommands = isKickCommandTarget(this->channel_);

    const auto filteredPlatforms =
        [this](std::vector<MessagePlatform> platforms) {
            if (this->platformFilter_.empty())
            {
                return platforms;
            }

            std::vector<MessagePlatform> filtered;
            for (const auto selected : this->platformFilter_)
            {
                if (std::ranges::find(platforms, selected) != platforms.end())
                {
                    filtered.push_back(selected);
                }
            }
            return filtered;
        };

    std::vector<MessagePlatform> customPlatforms = this->platformFilter_;
    if (customPlatforms.empty())
    {
        if (canUseTwitchCommands)
        {
            customPlatforms.push_back(MessagePlatform::AnyOrTwitch);
        }
        if (canUseKickCommands)
        {
            customPlatforms.push_back(MessagePlatform::Kick);
        }
    }

#ifdef CHATTERINO_HAVE_PLUGINS
    for (const auto &command : getApp()->getCommands()->pluginCommands())
    {
        addCommand(command, commands, customPlatforms);
    }
#endif

    // Custom Chatterino commands can be expanded for any selected platform.
    for (const auto &command : getApp()->getCommands()->items)
    {
        addCommand(command.name, commands, customPlatforms);
    }

    // Default Chatterino commands carry only the platforms where they work.
    auto defaultCommands =
        getApp()->getCommands()->getDefaultChatterinoCommandList();
    for (const auto &command : defaultCommands)
    {
        // These execute entirely inside Mergerino. The selected send target
        // neither changes their behavior nor limits where they can be used.
        if (clientOnlyChatterinoCommands().contains(command,
                                                    Qt::CaseInsensitive))
        {
            addCommand(command, commands);
            continue;
        }

        std::vector<MessagePlatform> platforms;
        if (canUseTwitchCommands &&
            !kickOnlyChatterinoCommands().contains(command,
                                                   Qt::CaseInsensitive))
        {
            appendPlatform(platforms, MessagePlatform::AnyOrTwitch);
        }
        if (canUseKickCommands &&
            kickCompatibleChatterinoCommands().contains(
                command, Qt::CaseInsensitive))
        {
            appendPlatform(platforms, MessagePlatform::Kick);
        }
        platforms = filteredPlatforms(std::move(platforms));
        if (!platforms.empty())
        {
            addCommand(command, commands, std::move(platforms));
        }
    }

    if (canUseTwitchCommands)
    {
        for (const auto &command : TWITCH_DEFAULT_COMMANDS)
        {
            auto platforms =
                filteredPlatforms({MessagePlatform::AnyOrTwitch});
            if (!platforms.empty())
            {
                addCommand(command, commands, std::move(platforms));
            }
        }
    }

    if (canUseKickCommands)
    {
        for (const auto &command : kickDefaultCommands())
        {
            auto platforms = filteredPlatforms({MessagePlatform::Kick});
            if (!platforms.empty())
            {
                addCommand(command, commands, std::move(platforms));
            }
        }
    }

    commands.erase(std::remove_if(commands.begin(), commands.end(),
                                  [this](const CommandItem &command) {
                                      if (this->slashCommandsOnly_ &&
                                          command.prefix != "/")
                                      {
                                          return true;
                                      }

                                      return !canUseCommand(command,
                                                            this->channel_);
                                  }),
                   commands.end());

    std::sort(commands.begin(), commands.end(), commandLessThan);
    std::vector<CommandItem> merged;
    merged.reserve(commands.size());
    for (auto &command : commands)
    {
        if (!merged.empty() && commandEquals(merged.back(), command))
        {
            for (const auto platform : command.platforms)
            {
                appendPlatform(merged.back().platforms, platform);
            }
            continue;
        }
        merged.push_back(std::move(command));
    }

    if (!this->platformFilter_.empty())
    {
        for (auto &command : merged)
        {
            command.platforms =
                filteredPlatforms(std::move(command.platforms));
        }
    }

    if (this->platformFilter_.size() > 1 && this->platformCallback_)
    {
        std::vector<CommandItem> expanded;
        expanded.reserve(merged.size() * 3);
        for (const auto &command : merged)
        {
            // Expose one row per platform first, then keep the combined target
            // last so the most specific actions are encountered first.
            if (command.platforms.size() > 1)
            {
                for (const auto platform : command.platforms)
                {
                    auto singlePlatform = command;
                    singlePlatform.platforms = {platform};
                    expanded.push_back(std::move(singlePlatform));
                }
            }
            expanded.push_back(command);
        }
        this->items_ = std::move(expanded);
        return;
    }

    this->items_ = std::move(merged);
}

const std::vector<CommandItem> &CommandSource::output() const
{
    return this->output_;
}

}  // namespace chatterino::completion
