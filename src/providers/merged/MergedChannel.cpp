// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/merged/MergedChannel.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/tiktok/TikTokLiveChat.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/youtube/YouTubeLiveChat.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/QStringHash.hpp"

#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>
#include <QUrlQuery>
#include <QStringList>

namespace {

using namespace chatterino;

QString normalizeChannelName(QString value)
{
    value = value.trimmed();
    if (value.startsWith('#'))
    {
        value.remove(0, 1);
    }
    if (value.startsWith(u":kick:"))
    {
        value.remove(0, 6);
    }

    return value.toLower();
}

QString trimTrailingSlash(QString value)
{
    while (value.size() > 1 && value.endsWith('/'))
    {
        value.chop(1);
    }
    return value;
}

bool isLikelyYouTubeChannelId(const QString &value)
{
    const auto trimmed = value.trimmed();
    if (!trimmed.startsWith("UC") || trimmed.size() != 24)
    {
        return false;
    }

    for (const auto ch : trimmed)
    {
        if (!ch.isLetterOrNumber() && ch != '_' && ch != '-')
        {
            return false;
        }
    }
    return true;
}

QUrl youtubeChannelBrowserUrl(QString source)
{
    source = source.trimmed();
    if (source.isEmpty())
    {
        return {};
    }

    if (source.startsWith('@'))
    {
        return QUrl(QString("https://www.youtube.com/%1")
                        .arg(source.section('/', 0, 0).trimmed()));
    }

    if (isLikelyYouTubeChannelId(source))
    {
        return QUrl(QString("https://www.youtube.com/channel/%1").arg(source));
    }

    if (!source.contains("://") && !source.contains('/') &&
        !source.contains('\\'))
    {
        return QUrl(QString("https://www.youtube.com/@%1").arg(source));
    }

    if (source.startsWith("//"))
    {
        source.prepend("https:");
    }
    else if (source.startsWith('/'))
    {
        source.prepend("https://www.youtube.com");
    }
    else if (!source.contains("://") &&
             (source.startsWith("www.", Qt::CaseInsensitive) ||
              source.startsWith("youtube.com/", Qt::CaseInsensitive) ||
              source.startsWith("m.youtube.com/", Qt::CaseInsensitive)))
    {
        source.prepend("https://");
    }

    const QUrl url(source);
    if (!url.isValid() || url.host().isEmpty())
    {
        return {};
    }

    const auto host = url.host().toLower();
    const auto path = trimTrailingSlash(url.path());
    const QUrlQuery query(url);

    if (host.endsWith("youtube.com") && path == "/embed/live_stream")
    {
        const auto channelId = query.queryItemValue("channel").trimmed();
        if (isLikelyYouTubeChannelId(channelId))
        {
            return QUrl(QString("https://www.youtube.com/channel/%1")
                            .arg(channelId));
        }
    }

    if (host.endsWith("youtube.com") && path.startsWith("/@"))
    {
        return QUrl(QString("https://www.youtube.com/%1")
                        .arg(path.section('/', 1, 1).trimmed()));
    }

    if (host.endsWith("youtube.com") && path.startsWith("/channel/"))
    {
        const auto channelId = path.section('/', 2, 2).trimmed();
        if (isLikelyYouTubeChannelId(channelId))
        {
            return QUrl(QString("https://www.youtube.com/channel/%1")
                            .arg(channelId));
        }
    }

    if (host.endsWith("youtube.com"))
    {
        const auto firstSegment = path.section('/', 1, 1).trimmed();
        if (firstSegment == "c" || firstSegment == "user")
        {
            const auto name = path.section('/', 2, 2).trimmed();
            if (!name.isEmpty())
            {
                return QUrl(QString("https://www.youtube.com/%1/%2")
                                .arg(firstSegment, name));
            }
        }
    }

    return {};
}

QString platformName(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return "Kick";
        case MessagePlatform::YouTube:
            return "YouTube";
        case MessagePlatform::TikTok:
            return "TikTok";
        case MessagePlatform::AnyOrTwitch:
        default:
            return "Twitch";
    }
}

QString platformIconPath(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return ":/platforms/kick.svg";
        case MessagePlatform::YouTube:
            return ":/platforms/youtube.svg";
        case MessagePlatform::TikTok:
            return ":/platforms/tiktok.svg";
        case MessagePlatform::AnyOrTwitch:
        default:
            return ":/platforms/twitch.svg";
    }
}

QPixmap renderPlatformBadge(MessagePlatform platform)
{
    QPixmap pixmap(18, 18);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    QSvgRenderer renderer(platformIconPath(platform));
    renderer.render(&painter);
    return pixmap;
}

HighlightAlert processMergedPlatformHighlights(Message &message)
{
    if (getSettings()->isBlacklistedUser(message.loginName))
    {
        return {};
    }

    const MessageParseArgs args;
    auto [highlighted, highlightResult] = getApp()->getHighlights()->check(
        args, {}, message.loginName, message.messageText, message.flags,
        message.platform);

    if (!highlighted)
    {
        return {};
    }

    message.flags.set(MessageFlag::Highlighted);
    message.highlightColor = highlightResult.color;

    if (highlightResult.showInMentions)
    {
        message.flags.set(MessageFlag::ShowInMentions);
    }

    return {
        .customSound = highlightResult.customSoundUrl.value_or(QUrl{}),
        .playSound = highlightResult.playSound,
        .windowAlert = highlightResult.alert,
    };
}

void triggerHighlightsAndAddMention(Channel *channel,
                                    const std::shared_ptr<Message> &message,
                                    const HighlightAlert &alert)
{
    if (!channel || !message)
    {
        return;
    }

    MessageBuilder::triggerHighlights(channel, alert);

    if (message->flags.has(MessageFlag::Highlighted) &&
        message->flags.has(MessageFlag::ShowInMentions))
    {
        getApp()->getTwitch()->getMentionsChannel()->addMessage(
            message, MessageContext::Original);
    }
}

}  // namespace

namespace chatterino {

QString MergedChannelConfig::displayName() const
{
    const auto trimmed = this->tabName.trimmed();
    if (!trimmed.isEmpty())
    {
        return trimmed;
    }

    if (this->twitchEnabled)
    {
        const auto twitch = this->effectiveTwitchChannelName();
        if (!twitch.isEmpty())
        {
            return twitch;
        }
    }

    if (this->kickEnabled)
    {
        const auto kick = this->effectiveKickChannelName();
        if (!kick.isEmpty())
        {
            return kick;
        }
    }

    return "Merged chat";
}

QString MergedChannelConfig::effectiveTwitchChannelName() const
{
    const auto overrideName = normalizeChannelName(this->twitchChannelName);
    if (!overrideName.isEmpty())
    {
        return overrideName;
    }
    return normalizeChannelName(this->tabName);
}

QString MergedChannelConfig::effectiveKickChannelName() const
{
    const auto overrideName = normalizeChannelName(this->kickChannelName);
    if (!overrideName.isEmpty())
    {
        return overrideName;
    }
    return normalizeChannelName(this->tabName);
}

MergedChannel::MergedChannel(MergedChannelConfig config)
    : Channel(config.displayName(), Channel::Type::Merged)
    , ChannelChatters(static_cast<Channel &>(*this))
    , config_(std::move(config))
    , displayName_(this->config_.displayName())
{
    this->platform_ = "merged";
    this->initializeSources();
    this->refreshStatusText();
}

MergedChannel::~MergedChannel() = default;

const MergedChannelConfig &MergedChannel::config() const
{
    return this->config_;
}

void MergedChannel::setTikTokShowJoinMessages(bool enabled)
{
    if (this->config_.tiktokShowJoinMessages == enabled)
    {
        return;
    }

    this->config_.tiktokShowJoinMessages = enabled;
    if (this->tiktokLiveChat_ != nullptr)
    {
        this->tiktokLiveChat_->setShowJoinMessages(enabled);
    }

    if (!enabled)
    {
        for (const auto &message : this->getMessageSnapshot())
        {
            if (message->flags.has(MessageFlag::TikTokJoinMessage))
            {
                this->removeMessage(message);
            }
        }
    }
}

const QString &MergedChannel::getDisplayName() const
{
    return this->displayName_;
}

const QString &MergedChannel::getLocalizedName() const
{
    return this->displayName_;
}

bool MergedChannel::isMergedChannel() const
{
    return true;
}

bool MergedChannel::canSendMessage() const
{
    return this->isWritable();
}

bool MergedChannel::isWritable() const
{
    return (this->config_.twitchEnabled && this->twitchChannel_ != nullptr) ||
           (this->config_.kickEnabled && this->kickChannel_ != nullptr) ||
           (this->config_.youtubeEnabled &&
            this->youtubeLiveChat_ != nullptr);
}

void MergedChannel::sendMessage(const QString &message)
{
    if (message.trimmed().isEmpty())
    {
        return;
    }

    QStringList unavailablePlatforms;

    if (this->config_.twitchEnabled && this->twitchChannel_)
    {
        if (getApp()->getAccounts()->twitch.isLoggedIn())
        {
            this->twitchChannel_->sendMessage(message);
            return;
        }
        unavailablePlatforms.append("Twitch");
    }

    if (this->config_.kickEnabled && this->kickChannel_)
    {
        if (getApp()->getAccounts()->kick.isLoggedIn())
        {
            this->kickChannel_->sendMessage(message);
            return;
        }
        unavailablePlatforms.append("Kick");
    }

    if (this->config_.youtubeEnabled && this->youtubeLiveChat_)
    {
        if (getApp()->getAccounts()->youtube.isLoggedIn())
        {
            this->youtubeLiveChat_->sendMessage(message);
            return;
        }
        unavailablePlatforms.append("YouTube");
    }

    if (!unavailablePlatforms.isEmpty())
    {
        this->addSystemStatusMessage(
            QString("Log in to %1 to send merged chat.")
                .arg(unavailablePlatforms.join(" or ")));
    }
}

bool MergedChannel::isMod() const
{
    return (this->twitchChannel_ && this->twitchChannel_->isMod()) ||
           (this->kickChannel_ && this->kickChannel_->isMod());
}

bool MergedChannel::isBroadcaster() const
{
    return (this->twitchChannel_ && this->twitchChannel_->isBroadcaster()) ||
           (this->kickChannel_ && this->kickChannel_->isBroadcaster());
}

bool MergedChannel::hasModRights() const
{
    return (this->twitchChannel_ && this->twitchChannel_->hasModRights()) ||
           (this->kickChannel_ && this->kickChannel_->hasModRights());
}

bool MergedChannel::isLive() const
{
    return this->twitchLive_ || this->kickLive_ || this->youtubeLive_ ||
           this->tiktokLive_;
}

bool MergedChannel::isRerun() const
{
    if (auto *twitch = dynamic_cast<TwitchChannel *>(this->twitchChannel_.get()))
    {
        return twitch->isRerun();
    }
    return false;
}

bool MergedChannel::canReconnect() const
{
    return (this->twitchChannel_ && this->twitchChannel_->canReconnect()) ||
           (this->kickChannel_ && this->kickChannel_->canReconnect()) ||
           this->youtubeLiveChat_ != nullptr || this->tiktokLiveChat_ != nullptr;
}

void MergedChannel::reconnect()
{
    bool announceTwitchJoin = false;
    QString twitchTitle;
    bool announceKickJoin = false;
    QString kickTitle;

    if (this->twitchChannel_ && this->twitchChannel_->canReconnect())
    {
        this->twitchChannel_->reconnect();
        this->twitchLiveJoinAnnounced_ = false;
        if (auto *twitch = dynamic_cast<TwitchChannel *>(this->twitchChannel_.get()))
        {
            this->twitchLive_ = twitch->isLive();
            if (this->twitchLive_)
            {
                announceTwitchJoin = true;
                twitchTitle = twitch->accessStreamStatus()->title;
            }
        }
    }

    if (this->kickChannel_ && this->kickChannel_->canReconnect())
    {
        this->kickChannel_->reconnect();
        this->kickLiveJoinAnnounced_ = false;
        if (auto *kick = dynamic_cast<KickChannel *>(this->kickChannel_.get()))
        {
            this->kickLive_ = kick->isLive();
            if (this->kickLive_)
            {
                announceKickJoin = true;
                kickTitle = kick->streamData().title;
            }
        }
    }

    if (this->youtubeLiveChat_ != nullptr)
    {
        this->restartYouTubeLiveChat();
    }

    if (this->tiktokLiveChat_ != nullptr)
    {
        this->tiktokLive_ = false;
        this->tiktokLiveChat_->stop();
        this->tiktokLiveChat_->start();
    }
    if (announceTwitchJoin)
    {
        this->announceJoinedLiveChat(MessagePlatform::AnyOrTwitch, twitchTitle);
    }
    if (announceKickJoin)
    {
        this->announceJoinedLiveChat(MessagePlatform::Kick, kickTitle);
    }

    this->refreshStatusText();
    this->streamStatusChanged.invoke();
}

QString MergedChannel::getCurrentStreamID() const
{
    if (this->youtubeLiveChat_ && !this->youtubeLiveChat_->videoId().isEmpty())
    {
        return this->youtubeLiveChat_->videoId();
    }
    if (this->tiktokLiveChat_ && !this->tiktokLiveChat_->roomId().isEmpty())
    {
        return this->tiktokLiveChat_->roomId();
    }
    if (this->twitchChannel_)
    {
        auto id = this->twitchChannel_->getCurrentStreamID();
        if (!id.isEmpty())
        {
            return id;
        }
    }
    if (this->kickChannel_)
    {
        return this->kickChannel_->getCurrentStreamID();
    }
    return {};
}

QString MergedChannel::getCurrentStreamIDForMessage(const Message &message) const
{
    switch (message.platform)
    {
        case MessagePlatform::YouTube:
            if (this->youtubeLiveChat_ &&
                !this->youtubeLiveChat_->videoId().isEmpty())
            {
                return "youtube:" + this->youtubeLiveChat_->videoId();
            }
            break;

        case MessagePlatform::TikTok:
            if (this->tiktokLiveChat_ &&
                !this->tiktokLiveChat_->roomId().isEmpty())
            {
                return "tiktok:" + this->tiktokLiveChat_->roomId();
            }
            break;

        case MessagePlatform::Kick:
            if (this->kickChannel_)
            {
                return this->kickChannel_->getCurrentStreamID();
            }
            break;

        case MessagePlatform::AnyOrTwitch:
        default:
            if (this->twitchChannel_)
            {
                auto id = this->twitchChannel_->getCurrentStreamID();
                if (!id.isEmpty())
                {
                    return "twitch:" + id;
                }
            }
            break;
    }

    return this->getCurrentStreamID();
}

QString MergedChannel::statusSuffix() const
{
    if (!this->isLive())
    {
        return {};
    }

    QStringList livePlatforms;
    if (this->twitchLive_)
    {
        livePlatforms.append("Twitch");
    }
    if (this->kickLive_)
    {
        livePlatforms.append("Kick");
    }
    if (this->youtubeLive_)
    {
        livePlatforms.append("YouTube");
    }
    if (this->tiktokLive_)
    {
        livePlatforms.append("TikTok");
    }

    if (livePlatforms.isEmpty())
    {
        return " (live)";
    }

    return QString(" (%1 live)").arg(livePlatforms.join(" + "));
}

QString MergedChannel::tooltipText() const
{
    return this->tooltipText_;
}

std::vector<MergedChannel::BrowserUrl>
    MergedChannel::liveStreamBrowserUrls() const
{
    std::vector<BrowserUrl> urls;

    if (this->config_.twitchEnabled && this->twitchLive_ &&
        this->twitchChannel_)
    {
        const auto channelName = this->config_.effectiveTwitchChannelName();
        if (!channelName.isEmpty())
        {
            urls.push_back(
                {"Twitch",
                 QUrl(QString("https://www.twitch.tv/%1").arg(channelName))});
        }
    }

    if (this->config_.kickEnabled && this->kickLive_ && this->kickChannel_)
    {
        auto slug = this->config_.effectiveKickChannelName();
        if (auto *kick = dynamic_cast<KickChannel *>(this->kickChannel_.get()))
        {
            slug = kick->slug();
        }
        if (!slug.isEmpty())
        {
            urls.push_back(
                {"Kick", QUrl(QString("https://kick.com/%1").arg(slug))});
        }
    }

    if (this->config_.youtubeEnabled && this->youtubeLive_ &&
        this->youtubeLiveChat_ && !this->youtubeLiveChat_->videoId().isEmpty())
    {
        urls.push_back({"YouTube",
                        QUrl(QString("https://www.youtube.com/watch?v=%1")
                                 .arg(this->youtubeLiveChat_->videoId()))});
    }

    if (this->config_.tiktokEnabled && this->tiktokLive_ &&
        this->tiktokLiveChat_)
    {
        auto source = this->tiktokLiveChat_->resolvedSource();
        if (source.isEmpty())
        {
            source = TikTokLiveChat::normalizeSource(this->config_.tiktokSource);
        }
        if (!source.isEmpty())
        {
            urls.push_back({"TikTok",
                            QUrl(QString("https://www.tiktok.com/%1/live")
                                     .arg(source))});
        }
    }

    return urls;
}

std::vector<MergedChannel::BrowserUrl> MergedChannel::channelBrowserUrls() const
{
    std::vector<BrowserUrl> urls;

    if (this->config_.twitchEnabled && this->twitchChannel_)
    {
        const auto channelName = this->config_.effectiveTwitchChannelName();
        if (!channelName.isEmpty())
        {
            urls.push_back(
                {"Twitch",
                 QUrl(QString("https://www.twitch.tv/%1").arg(channelName))});
        }
    }

    if (this->config_.kickEnabled && this->kickChannel_)
    {
        auto slug = this->config_.effectiveKickChannelName();
        if (auto *kick = dynamic_cast<KickChannel *>(this->kickChannel_.get()))
        {
            slug = kick->slug();
        }
        if (!slug.isEmpty())
        {
            urls.push_back(
                {"Kick", QUrl(QString("https://kick.com/%1").arg(slug))});
        }
    }

    if (this->config_.youtubeEnabled)
    {
        QString source;
        if (this->youtubeLiveChat_)
        {
            source = this->youtubeLiveChat_->resolvedSource();
        }
        if (source.isEmpty())
        {
            source = this->config_.youtubeStreamUrl;
        }

        const auto url = youtubeChannelBrowserUrl(source);
        if (!url.isEmpty() && url.isValid())
        {
            urls.push_back({"YouTube", url});
        }
    }

    if (this->config_.tiktokEnabled)
    {
        QString source;
        if (this->tiktokLiveChat_)
        {
            source = this->tiktokLiveChat_->resolvedSource();
        }
        if (source.isEmpty())
        {
            source = TikTokLiveChat::normalizeSource(this->config_.tiktokSource);
        }
        if (!source.isEmpty())
        {
            urls.push_back({"TikTok",
                            QUrl(QString("https://www.tiktok.com/%1")
                                     .arg(source))});
        }
    }

    return urls;
}

ChannelPtr MergedChannel::twitchChannel() const
{
    return this->twitchChannel_;
}

ChannelPtr MergedChannel::kickChannel() const
{
    return this->kickChannel_;
}

YouTubeLiveChat *MergedChannel::youtubeLiveChat() const
{
    return this->youtubeLiveChat_.get();
}

void MergedChannel::setYouTubeDiscoveryForeground(bool foreground)
{
    this->youtubeDiscoveryForeground_ = foreground;
    if (this->youtubeLiveChat_)
    {
        this->youtubeLiveChat_->setDiscoveryForeground(foreground);
    }
}

void MergedChannel::createYouTubeLiveChat()
{
    if (!this->config_.youtubeEnabled ||
        this->config_.youtubeStreamUrl.trimmed().isEmpty())
    {
        return;
    }

    this->youtubeLive_ = false;
    this->youtubeLiveChat_ =
        std::make_unique<YouTubeLiveChat>(this->config_.youtubeStreamUrl);
    this->youtubeLiveChat_->setDiscoveryForeground(
        this->youtubeDiscoveryForeground_);
    this->youtubeConnections_.managedConnect(
        this->youtubeLiveChat_->messageReceived,
        [this](const MessagePtr &message) { this->addYouTubeMessage(message); });
    this->youtubeConnections_.managedConnect(
        this->youtubeLiveChat_->sourceResolved, [this](const QString &source) {
            if (!source.isEmpty())
            {
                this->config_.youtubeStreamUrl = source;
                getApp()->getWindows()->queueSave();
            }
        });
    this->youtubeConnections_.managedConnect(
        this->youtubeLiveChat_->systemMessageReceived,
        [this](const MessagePtr &message) {
            this->addSystemStatusMessage(message);
            this->refreshStatusText();
        });
    this->youtubeConnections_.managedConnect(
        this->youtubeLiveChat_->liveStatusChanged, [this] {
            this->youtubeLive_ = this->youtubeLiveChat_->isLive();
            this->refreshStatusText();
            this->streamStatusChanged.invoke();
        });
    this->youtubeConnections_.managedConnect(
        this->youtubeLiveChat_->moderationStatusChanged,
        [this] { this->streamStatusChanged.invoke(); });
    this->youtubeLiveChat_->start();
}

void MergedChannel::restartYouTubeLiveChat()
{
    this->youtubeLive_ = false;
    if (this->youtubeLiveChat_)
    {
        this->youtubeLiveChat_->stop();
    }
    this->youtubeConnections_.clear();
    this->youtubeLiveChat_.reset();
    this->createYouTubeLiveChat();
}

void MergedChannel::initializeSources()
{
    if (this->config_.twitchEnabled)
    {
        const auto twitchName = this->config_.effectiveTwitchChannelName();
        if (!twitchName.isEmpty())
        {
            this->twitchChannel_ =
                getApp()->getTwitch()->getOrAddChannel(twitchName);
            this->connectSourceSignals(this->twitchChannel_,
                                       MessagePlatform::AnyOrTwitch,
                                       this->twitchConnections_);
            this->appendInitialMessages(this->twitchChannel_,
                                        MessagePlatform::AnyOrTwitch);
            this->twitchLive_ = this->twitchChannel_->isLive();
            if (this->twitchLive_)
            {
                if (auto *twitch =
                        dynamic_cast<TwitchChannel *>(this->twitchChannel_.get()))
                {
                    this->announceJoinedLiveChat(
                        MessagePlatform::AnyOrTwitch,
                        twitch->accessStreamStatus()->title);
                }
            }
        }
    }

    if (this->config_.kickEnabled)
    {
        const auto kickName = this->config_.effectiveKickChannelName();
        if (!kickName.isEmpty())
        {
            this->kickChannel_ =
                getApp()->getKickChatServer()->getOrCreate(kickName);
            this->connectSourceSignals(this->kickChannel_, MessagePlatform::Kick,
                                       this->kickConnections_);
            this->appendInitialMessages(this->kickChannel_, MessagePlatform::Kick);
            this->kickLive_ = this->kickChannel_->isLive();
            if (this->kickLive_)
            {
                if (auto *kick =
                        dynamic_cast<KickChannel *>(this->kickChannel_.get()))
                {
                    this->announceJoinedLiveChat(MessagePlatform::Kick,
                                                 kick->streamData().title);
                }
            }
        }
    }

    this->createYouTubeLiveChat();

    if (this->config_.tiktokEnabled &&
        !this->config_.tiktokSource.trimmed().isEmpty())
    {
        this->tiktokLiveChat_ =
            std::make_unique<TikTokLiveChat>(
                this->config_.tiktokSource,
                this->config_.tiktokShowJoinMessages);
        this->tiktokConnections_.managedConnect(
            this->tiktokLiveChat_->messageReceived,
            [this](const MessagePtr &message) { this->addTikTokMessage(message); });
        this->tiktokConnections_.managedConnect(
            this->tiktokLiveChat_->sourceResolved, [this](const QString &source) {
                if (!source.isEmpty())
                {
                    this->config_.tiktokSource = source;
                }
            });
        this->tiktokConnections_.managedConnect(
            this->tiktokLiveChat_->systemMessageReceived,
            [this](const MessagePtr &message) {
                this->addSystemStatusMessage(message);
                this->refreshStatusText();
            });
        this->tiktokConnections_.managedConnect(
            this->tiktokLiveChat_->liveStatusChanged, [this] {
                this->tiktokLive_ = this->tiktokLiveChat_->isLive();
                this->refreshStatusText();
                this->streamStatusChanged.invoke();
            });
        this->tiktokLiveChat_->start();
    }
}

void MergedChannel::connectSourceSignals(
    const ChannelPtr &source, MessagePlatform platform,
    pajlada::Signals::SignalHolder &connections)
{
    if (!source)
    {
        return;
    }

    connections.managedConnect(
        source->messageAppended,
        [this, platform](MessagePtr &message,
                         std::optional<MessageFlags> overridingFlags) {
            this->appendMergedMessage(message, platform,
                                      std::move(overridingFlags));
        });
    connections.managedConnect(
        source->messagesAddedAtStart,
        [this, platform](const std::vector<MessagePtr> &messages) {
            this->addMergedMessagesAtStart(messages, platform);
        });
    connections.managedConnect(
        source->filledInMessages,
        [this, platform](const std::vector<MessagePtr> &messages) {
            this->fillInMergedMessages(messages, platform);
        });
    connections.managedConnect(
        source->messageReplaced,
        [this, platform](size_t, const MessagePtr &previous,
                         const MessagePtr &replacement) {
            this->replaceMergedMessage(previous, replacement, platform);
        });
    connections.managedConnect(
        Channel::messageFlagsChanged,
        [this, sourceChannel = source.get(), platform](
            Channel *changedChannel, const MessagePtr &message) {
            if (changedChannel != sourceChannel)
            {
                return;
            }

            const auto key = messageKey(message, platform);
            const auto it = this->mirroredMessages_.find(key);
            if (it == this->mirroredMessages_.end())
            {
                return;
            }

            it->second->flags.set(
                MessageFlag::Disabled,
                message->flags.has(MessageFlag::Disabled));
            it->second->flags.set(
                MessageFlag::InvalidReplyTarget,
                message->flags.has(MessageFlag::InvalidReplyTarget));
        });

    if (auto *twitch = dynamic_cast<TwitchChannel *>(source.get()))
    {
        connections.managedConnect(twitch->streamStatusChanged, [this, twitch] {
            this->twitchLive_ = twitch->isLive();
            if (this->twitchLive_)
            {
                if (!this->twitchLiveJoinAnnounced_)
                {
                    this->announceJoinedLiveChat(
                        MessagePlatform::AnyOrTwitch,
                        twitch->accessStreamStatus()->title);
                }
            }
            else
            {
                this->twitchLiveJoinAnnounced_ = false;
            }
            this->refreshStatusText();
            this->streamStatusChanged.invoke();
        });
    }
    else if (auto *kick = dynamic_cast<KickChannel *>(source.get()))
    {
        connections.managedConnect(kick->liveStatusChanged, [this, kick] {
            this->kickLive_ = kick->isLive();
            if (this->kickLive_)
            {
                if (!this->kickLiveJoinAnnounced_)
                {
                    this->announceJoinedLiveChat(MessagePlatform::Kick,
                                                 kick->streamData().title);
                }
            }
            else
            {
                this->kickLiveJoinAnnounced_ = false;
            }
            this->refreshStatusText();
            this->streamStatusChanged.invoke();
        });
    }
}

void MergedChannel::appendInitialMessages(const ChannelPtr &source,
                                          MessagePlatform platform)
{
    if (!source)
    {
        return;
    }

    const auto snapshot = source->getMessageSnapshot(150);
    for (const auto &message : snapshot)
    {
        auto merged = this->createAndTrackMergedMessage(message, platform, true);
        if (!merged)
        {
            continue;
        }

        this->addMessage(merged, MessageContext::Repost);
    }
}

void MergedChannel::addMergedMessagesAtStart(
    const std::vector<MessagePtr> &messages, MessagePlatform platform)
{
    std::vector<MessagePtr> mergedMessages;
    mergedMessages.reserve(messages.size());

    for (const auto &message : messages)
    {
        auto merged = this->createAndTrackMergedMessage(message, platform, true);
        if (merged)
        {
            mergedMessages.emplace_back(std::move(merged));
        }
    }

    if (!mergedMessages.empty())
    {
        this->addMessagesAtStart(mergedMessages);
    }
}

void MergedChannel::fillInMergedMessages(
    const std::vector<MessagePtr> &messages, MessagePlatform platform)
{
    std::vector<MessagePtr> mergedMessages;
    mergedMessages.reserve(messages.size());

    for (const auto &message : messages)
    {
        auto merged = this->createAndTrackMergedMessage(message, platform, true);
        if (merged)
        {
            mergedMessages.emplace_back(std::move(merged));
        }
    }

    if (!mergedMessages.empty())
    {
        this->fillInMissingMessages(mergedMessages);
    }
}

void MergedChannel::appendMergedMessage(const MessagePtr &source,
                                        MessagePlatform platform,
                                        std::optional<MessageFlags>
                                            overridingFlags)
{
    auto merged = this->createAndTrackMergedMessage(source, platform);
    if (!merged)
    {
        return;
    }

    this->addMessage(merged, MessageContext::Repost,
                     std::move(overridingFlags));
}

void MergedChannel::replaceMergedMessage(const MessagePtr &previous,
                                         const MessagePtr &replacement,
                                         MessagePlatform platform)
{
    const auto key = messageKey(previous, platform);
    if (key.isEmpty())
    {
        return;
    }

    auto it = this->mirroredMessages_.find(key);
    if (it == this->mirroredMessages_.end())
    {
        return;
    }

    auto updated = this->createMergedMessage(
        replacement, platform,
        it->second->flags.has(MessageFlag::RecentMessage));
    if (!updated)
    {
        return;
    }

    this->replaceMessage(it->second, updated);
    const auto updatedKey = messageKey(replacement, platform);
    if (!updatedKey.isEmpty() && updatedKey != key)
    {
        this->mirroredMessages_.erase(it);
        this->mirroredMessages_[updatedKey] = updated;
        return;
    }

    it->second = updated;
}

std::shared_ptr<Message> MergedChannel::createAndTrackMergedMessage(
    const MessagePtr &source, MessagePlatform platform, bool markAsRecent)
{
    if (!shouldMirrorSourceMessage(source))
    {
        return nullptr;
    }

    const auto key = messageKey(source, platform);
    if (!key.isEmpty() && this->mirroredMessages_.contains(key))
    {
        return nullptr;
    }

    auto merged = this->createMergedMessage(source, platform, markAsRecent);
    if (!merged)
    {
        return nullptr;
    }

    if (!key.isEmpty())
    {
        this->mirroredMessages_[key] = merged;
    }

    const auto chatterName =
        !merged->loginName.isEmpty() ? merged->loginName : merged->displayName;
    if (!chatterName.isEmpty())
    {
        this->addRecentChatter(chatterName);
    }

    return merged;
}

std::shared_ptr<Message> MergedChannel::createMergedMessage(
    const MessagePtr &source, MessagePlatform platform, bool markAsRecent) const
{
    if (!source)
    {
        return nullptr;
    }

    auto merged = source->clone();
    merged->platform = platform;
    merged->platformAccentColor = platformAccent(platform);
    if (markAsRecent)
    {
        merged->flags.set(MessageFlag::RecentMessage);
    }

    if (platform == MessagePlatform::AnyOrTwitch)
    {
        merged->channelName = this->config_.effectiveTwitchChannelName();
    }
    else if (platform == MessagePlatform::Kick)
    {
        merged->channelName = this->config_.effectiveKickChannelName();
    }

    merged->searchText =
        platformName(platform) + " " + merged->searchText.trimmed();

    const auto badge = platformBadge(platform);
    if (badge)
    {
        auto badgePosition = merged->elements.begin();
        while (badgePosition != merged->elements.end())
        {
            const auto &element = *badgePosition;
            if (!element ||
                !element->getFlags().has(MessageElementFlag::RepliedMessage))
            {
                break;
            }

            ++badgePosition;
        }

        merged->elements.insert(
            badgePosition,
            std::make_unique<BadgeElement>(badge,
                                           MessageElementFlag::PlatformBadge));
    }

    return merged;
}

void MergedChannel::addYouTubeMessage(const MessagePtr &message)
{
    const auto key = messageKey(message, MessagePlatform::YouTube);
    if (!key.isEmpty() && this->mirroredMessages_.contains(key))
    {
        return;
    }

    auto merged = this->createMergedMessage(message, MessagePlatform::YouTube);
    if (!merged)
    {
        return;
    }

    if (!key.isEmpty())
    {
        this->mirroredMessages_[key] = merged;
    }

    const auto chatterName =
        !merged->loginName.isEmpty() ? merged->loginName : merged->displayName;
    if (!chatterName.isEmpty())
    {
        this->addRecentChatter(chatterName);
    }

    const auto highlight = processMergedPlatformHighlights(*merged);
    triggerHighlightsAndAddMention(this, merged, highlight);

    this->addMessage(merged, MessageContext::Repost);
}

void MergedChannel::addTikTokMessage(const MessagePtr &message)
{
    const auto key = messageKey(message, MessagePlatform::TikTok);
    if (!key.isEmpty() && this->mirroredMessages_.contains(key))
    {
        return;
    }

    auto merged = this->createMergedMessage(message, MessagePlatform::TikTok);
    if (!merged)
    {
        return;
    }

    if (!key.isEmpty())
    {
        this->mirroredMessages_[key] = merged;
    }

    if (!merged->flags.has(MessageFlag::System))
    {
        const auto chatterName = !merged->loginName.isEmpty()
                                     ? merged->loginName
                                     : merged->displayName;
        if (!chatterName.isEmpty())
        {
            this->addRecentChatter(chatterName);
        }

        const auto highlight = processMergedPlatformHighlights(*merged);
        triggerHighlightsAndAddMention(this, merged, highlight);
    }

    this->addMessage(merged, MessageContext::Repost);
}

bool MergedChannel::shouldMirrorSourceMessage(const MessagePtr &message)
{
    if (!message)
    {
        return false;
    }

    if (message->flags.has(MessageFlag::ConnectedMessage))
    {
        return false;
    }

    if (!message->flags.has(MessageFlag::System))
    {
        return true;
    }

    const auto text = message->messageText.trimmed().toLower();
    if (text.contains(" is live!") || text.contains(" is live:"))
    {
        return false;
    }

    return text != "joined" && text != "joined channel" &&
           text != "connected" && text != "reconnected";
}

void MergedChannel::announceJoinedLiveChat(MessagePlatform platform,
                                           const QString &title)
{
    auto message = makeSystemMessage(
                       title.trimmed().isEmpty()
                           ? QString("Joined %1 live chat")
                                 .arg(platformName(platform))
                           : QString("Joined %1 live chat: %2")
                                 .arg(platformName(platform), title.trimmed()))
                       ->clone();
    message->platform = platform;
    message->platformAccentColor = platformAccent(platform);

    if (platform == MessagePlatform::Kick)
    {
        this->kickLiveJoinAnnounced_ = true;
    }
    else if (platform == MessagePlatform::AnyOrTwitch)
    {
        this->twitchLiveJoinAnnounced_ = true;
    }

    this->addSystemStatusMessage(message);
}

void MergedChannel::addSystemStatusMessage(const QString &message)
{
    if (message.isEmpty())
    {
        return;
    }

    this->addMessage(makeSystemMessage(message), MessageContext::Repost);
}

void MergedChannel::addSystemStatusMessage(const MessagePtr &message)
{
    if (!message)
    {
        return;
    }

    this->addMessage(message, MessageContext::Repost);
}

void MergedChannel::refreshStatusText()
{
    QStringList lines;

    if (this->config_.twitchEnabled)
    {
        lines.append(QString("Twitch: %1 (%2)")
                         .arg(this->config_.effectiveTwitchChannelName(),
                              this->twitchLive_ ? "live" : "offline"));
    }
    if (this->config_.kickEnabled)
    {
        lines.append(QString("Kick: %1 (%2)")
                         .arg(this->config_.effectiveKickChannelName(),
                              this->kickLive_ ? "live" : "offline"));
    }
    if (this->config_.youtubeEnabled)
    {
        QString youtubeStatus =
            this->youtubeLive_ ? QStringLiteral("live")
                               : QStringLiteral("waiting for live chat");
        if (this->youtubeLiveChat_ &&
            !this->youtubeLiveChat_->statusText().trimmed().isEmpty())
        {
            youtubeStatus = this->youtubeLiveChat_->statusText();
        }
        lines.append(QString("YouTube: %1").arg(youtubeStatus));
    }
    if (this->config_.tiktokEnabled)
    {
        QString tiktokStatus =
            this->tiktokLive_ ? QStringLiteral("live")
                              : QStringLiteral("waiting for live chat");
        if (this->tiktokLiveChat_ &&
            !this->tiktokLiveChat_->statusText().trimmed().isEmpty())
        {
            tiktokStatus = this->tiktokLiveChat_->statusText();
        }
        lines.append(QString("TikTok: %1").arg(tiktokStatus));
    }

    this->tooltipText_ = lines.join("<br>");
}

QColor MergedChannel::platformAccent(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QColor(83, 252, 24, 36);
        case MessagePlatform::YouTube:
            return QColor(255, 48, 64, 60);
        case MessagePlatform::TikTok:
            return QColor(37, 244, 238, 48);
        case MessagePlatform::AnyOrTwitch:
        default:
            return QColor(145, 70, 255, 36);
    }
}

EmotePtr MergedChannel::platformBadge(MessagePlatform platform)
{
    static EmoteMap cache;

    const auto name = EmoteName{platformName(platform).toLower()};
    auto it = cache.find(name);
    if (it != cache.end())
    {
        return it->second;
    }

    Emote emote{
        .name = name,
        .images = ImageSet{Image::fromResourcePixmap(renderPlatformBadge(platform))},
        .tooltip = Tooltip{platformName(platform)},
        .homePage = Url{},
    };
    auto badge = cachedOrMakeEmotePtr(std::move(emote), cache);
    cache[badge->name] = badge;
    return badge;
}

QString MergedChannel::messageKey(const MessagePtr &message,
                                  MessagePlatform platform)
{
    if (!message)
    {
        return {};
    }

    auto key = message->id;
    if (key.isEmpty())
    {
        key = QString("%1|%2|%3|%4")
                  .arg(message->channelName, message->loginName,
                       message->messageText,
                       message->serverReceivedTime.toString(Qt::ISODateWithMs));
    }

    return QString("%1|%2").arg(static_cast<int>(platform)).arg(key);
}

}  // namespace chatterino
