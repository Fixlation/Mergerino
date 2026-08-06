// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/ObsBrowserDockServer.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Emote.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/seventv/SeventvBadges.hpp"
#include "providers/seventv/SeventvPaints.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/youtube/YoutubeAccount.hpp"
#include "providers/youtube/YouTubeLiveChat.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "singletons/Theme.hpp"
#include "util/HttpServer.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/Window.hpp"

#include <QBuffer>
#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringBuilder>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>

namespace {

using namespace chatterino;

constexpr int OBS_DOCK_MESSAGE_LIMIT = 120;

QString normalizedDockView(const QString &view)
{
    if (view.compare(QStringLiteral("activity"), Qt::CaseInsensitive) == 0)
    {
        return QStringLiteral("activity");
    }

    return QStringLiteral("chat");
}

QString normalizedOverlayPlatformStyle(QString style)
{
    style = style.trimmed().toLower();
    if (style == QStringLiteral("accent-line") ||
        style == QStringLiteral("none"))
    {
        return style;
    }
    return QStringLiteral("logos");
}

struct OverlayLinkedAccounts {
    QString twitchChannelName;
    QString kickChannelName;
    QString youtubeChannelID;
    QString youtubeDisplayName;

    bool hasAny() const
    {
        return !this->twitchChannelName.isEmpty() ||
               !this->kickChannelName.isEmpty() ||
               !this->youtubeChannelID.isEmpty();
    }

    QString preferredTabName() const
    {
        if (!this->twitchChannelName.isEmpty())
        {
            return this->twitchChannelName;
        }
        if (!this->kickChannelName.isEmpty())
        {
            return this->kickChannelName;
        }
        if (!this->youtubeDisplayName.isEmpty())
        {
            return this->youtubeDisplayName;
        }
        return this->youtubeChannelID;
    }
};

OverlayLinkedAccounts linkedOverlayAccounts()
{
    OverlayLinkedAccounts linked;
    auto *accounts = getApp()->getAccounts();
    if (accounts == nullptr)
    {
        return linked;
    }

    const auto twitch = accounts->twitch.getCurrent();
    if (accounts->twitch.isLoggedIn() && twitch && !twitch->isAnon())
    {
        linked.twitchChannelName = twitch->getUserName().trimmed();
    }

    const auto kick = accounts->kick.current();
    if (accounts->kick.isLoggedIn() && kick && !kick->isAnonymous())
    {
        linked.kickChannelName = kick->username().trimmed();
    }

    const auto youtube = accounts->youtube.current();
    if (accounts->youtube.isLoggedIn() && youtube && !youtube->isAnonymous())
    {
        linked.youtubeChannelID = youtube->channelID().trimmed();
        linked.youtubeDisplayName = youtube->displayName().trimmed();
    }

    return linked;
}

bool namesMatch(const QString &left, const QString &right)
{
    return !left.trimmed().isEmpty() && !right.trimmed().isEmpty() &&
           left.trimmed().compare(right.trimmed(), Qt::CaseInsensitive) == 0;
}

bool channelNameMatches(const Channel &channel, const QString &name)
{
    return namesMatch(channel.getName(), name) ||
           namesMatch(channel.getDisplayName(), name) ||
           namesMatch(channel.getLocalizedName(), name);
}

QString overlayPageTitle(SplitContainer *page)
{
    auto *tab = page != nullptr ? page->getTab() : nullptr;
    return tab != nullptr ? tab->getTitle().trimmed() : QString();
}

bool hasChatSplit(SplitContainer *page)
{
    if (page == nullptr)
    {
        return false;
    }

    for (auto *split : page->getSplits())
    {
        if (split != nullptr && !split->isActivityPane())
        {
            return true;
        }
    }
    return false;
}

int linkedAccountMatchPriority(SplitContainer *page,
                               const OverlayLinkedAccounts &linked)
{
    int priority = 0;
    if (page == nullptr)
    {
        return priority;
    }

    for (auto *split : page->getSplits())
    {
        if (split == nullptr || split->isActivityPane())
        {
            continue;
        }

        const auto channel = split->getChannel();
        if (!channel)
        {
            continue;
        }

        if (auto *merged = dynamic_cast<MergedChannel *>(channel.get()))
        {
            const auto &config = merged->config();
            if (!linked.twitchChannelName.isEmpty() && config.twitchEnabled &&
                (namesMatch(config.twitchChannelName,
                            linked.twitchChannelName) ||
                 namesMatch(config.effectiveTwitchChannelName(),
                            linked.twitchChannelName)))
            {
                priority = std::max(priority, 300);
            }
            if (!linked.kickChannelName.isEmpty() && config.kickEnabled &&
                (namesMatch(config.kickChannelName, linked.kickChannelName) ||
                 namesMatch(config.effectiveKickChannelName(),
                            linked.kickChannelName)))
            {
                priority = std::max(priority, 200);
            }
            if (!linked.youtubeChannelID.isEmpty() &&
                config.youtubeEnabled &&
                namesMatch(config.youtubeStreamUrl,
                           linked.youtubeChannelID))
            {
                priority = std::max(priority, 100);
            }
            continue;
        }

        if (!linked.twitchChannelName.isEmpty() &&
            channel->isTwitchChannel() &&
            channelNameMatches(*channel, linked.twitchChannelName))
        {
            priority = std::max(priority, 300);
        }
        if (!linked.kickChannelName.isEmpty() && channel->isKickChannel() &&
            channelNameMatches(*channel, linked.kickChannelName))
        {
            priority = std::max(priority, 200);
        }
    }

    return priority;
}

SplitContainer *findLinkedAccountPage(
    Window *window, const OverlayLinkedAccounts &linked)
{
    if (window == nullptr || !linked.hasAny())
    {
        return nullptr;
    }

    SplitContainer *bestPage = nullptr;
    int bestScore = 0;
    auto &notebook = window->getNotebook();
    for (int index = 0; index < notebook.getPageCount(); ++index)
    {
        auto *page =
            dynamic_cast<SplitContainer *>(notebook.getPageAt(index));
        const auto priority = linkedAccountMatchPriority(page, linked);
        if (priority == 0)
        {
            continue;
        }

        const auto title = overlayPageTitle(page);
        const auto nonLegacyBonus =
            title.compare(QStringLiteral("OBS Chat"),
                          Qt::CaseInsensitive) == 0
                ? 0
                : 10;
        const auto score = priority + nonLegacyBonus;
        if (score > bestScore)
        {
            bestPage = page;
            bestScore = score;
        }
    }

    return bestPage;
}

QString platformName(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QStringLiteral("kick");
        case MessagePlatform::YouTube:
            return QStringLiteral("youtube");
        case MessagePlatform::TikTok:
            return QStringLiteral("tiktok");
        case MessagePlatform::AnyOrTwitch:
        default:
            return QStringLiteral("twitch");
    }
}

QString platformIconPath(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QStringLiteral(":/platforms/kick.svg");
        case MessagePlatform::YouTube:
            return QStringLiteral(":/platforms/youtube.svg");
        case MessagePlatform::TikTok:
            return QStringLiteral(":/platforms/tiktok.svg");
        case MessagePlatform::AnyOrTwitch:
        default:
            return QStringLiteral(":/platforms/twitch.svg");
    }
}

QString resourceDataUrl(const QString &resourcePath)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }

    return QStringLiteral("data:image/svg+xml;base64,%1")
        .arg(QString::fromLatin1(file.readAll().toBase64()));
}

QString platformIconDataUrl(MessagePlatform platform)
{
    return resourceDataUrl(platformIconPath(platform));
}

QColor defaultPlatformAccent(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QColor(83, 252, 24);
        case MessagePlatform::YouTube:
            return QColor(255, 48, 64);
        case MessagePlatform::TikTok:
            return QColor(37, 244, 238);
        case MessagePlatform::AnyOrTwitch:
        default:
            return QColor(145, 70, 255);
    }
}

QColor softenedDockOverlay(QColor color)
{
    if (!color.isValid())
    {
        return {};
    }

    if (color.alpha() >= 250)
    {
        color.setAlpha(92);
    }
    else if (color.alpha() < 72)
    {
        color.setAlpha(72);
    }

    return color;
}

QColor dockEventHighlightColor(const Message &message)
{
    const auto &colors = ColorProvider::instance();

    if (message.highlightColor && message.highlightColor->isValid())
    {
        return softenedDockOverlay(*message.highlightColor);
    }

    if (message.flags.has(MessageFlag::Highlighted))
    {
        return softenedDockOverlay(*colors.color(ColorType::SelfHighlight));
    }

    if (message.flags.has(MessageFlag::ElevatedMessage))
    {
        return softenedDockOverlay(
            *colors.color(ColorType::ElevatedMessageHighlight));
    }

    if (message.flags.has(MessageFlag::FirstMessage) ||
        message.flags.has(MessageFlag::FirstMessageSession))
    {
        return softenedDockOverlay(
            *colors.color(ColorType::FirstMessageHighlight));
    }

    if (message.flags.has(MessageFlag::WatchStreak))
    {
        return softenedDockOverlay(*colors.color(ColorType::WatchStreak));
    }

    if (message.flags.has(MessageFlag::Subscription))
    {
        const auto subscriptionColor = colors.color(ColorType::Subscription);
        if (message.platform == MessagePlatform::AnyOrTwitch)
        {
            return softenedDockOverlay(*subscriptionColor);
        }

        auto color =
            message.platformAccentColor.value_or(defaultPlatformAccent(message.platform));
        if (subscriptionColor && subscriptionColor->isValid())
        {
            color.setAlpha(subscriptionColor->alpha());
        }
        return softenedDockOverlay(color);
    }

    if (message.flags.has(MessageFlag::RedeemedHighlight) ||
        message.flags.has(MessageFlag::RedeemedChannelPointReward))
    {
        return softenedDockOverlay(*colors.color(ColorType::RedeemedHighlight));
    }

    if (message.flags.has(MessageFlag::Whisper) ||
        message.flags.has(MessageFlag::HighlightedWhisper))
    {
        return softenedDockOverlay(*colors.color(ColorType::Whisper));
    }

    if (message.flags.has(MessageFlag::AutoMod) ||
        message.flags.has(MessageFlag::AutoModOffendingMessageHeader) ||
        message.flags.has(MessageFlag::AutoModOffendingMessage))
    {
        return softenedDockOverlay(*colors.color(ColorType::AutomodHighlight));
    }

    if (message.flags.has(MessageFlag::CheerMessage) ||
        message.flags.has(MessageFlag::ModerationAction))
    {
        auto color =
            message.platformAccentColor.value_or(defaultPlatformAccent(message.platform));
        color.setAlpha(102);
        return color;
    }

    return {};
}

QString cssColor(const QColor &color)
{
    if (!color.isValid())
    {
        return QStringLiteral("rgba(0, 0, 0, 0)");
    }

    if (color.alpha() == 255)
    {
        return color.name(QColor::HexRgb);
    }

    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(QString::number(color.alphaF(), 'f', 3));
}

QColor readableObsUsernameColor(QColor color)
{
    if (color.isValid() && color.red() == 0 && color.green() == 0 &&
        color.blue() == 0)
    {
        color.setRgb(36, 36, 36, color.alpha());
    }
    return color;
}

bool isSevenTVImage(const Emote &emote)
{
    if (emote.homePage.string.contains(QStringLiteral("7tv.app"),
                                       Qt::CaseInsensitive))
    {
        return true;
    }

    const auto images = emote.images.toJson();
    for (const auto &key : {QStringLiteral("1x"), QStringLiteral("2x"),
                            QStringLiteral("3x")})
    {
        const auto url = images.value(key).toString();
        if (url.contains(QStringLiteral("cdn.7tv"),
                         Qt::CaseInsensitive) ||
            url.contains(QStringLiteral("7tv.io"), Qt::CaseInsensitive))
        {
            return true;
        }
    }
    return false;
}

QJsonObject browserImageMetadata(const Emote &emote, bool badge,
                                 int fallbackIndex)
{
    const auto images = emote.images.toJson();
    const auto image1x = images.value(QStringLiteral("1x")).toString();
    if (image1x.isEmpty())
    {
        return {};
    }

    auto id = emote.id.string.trimmed();
    if (id.isEmpty())
    {
        id = QStringLiteral("browser-%1-%2")
                 .arg(badge ? QStringLiteral("badge")
                            : QStringLiteral("emote"))
                 .arg(fallbackIndex);
    }

    auto name = emote.name.string.trimmed();
    if (name.isEmpty())
    {
        name = badge ? QStringLiteral("Chat badge")
                     : QStringLiteral("Chat emote %1").arg(fallbackIndex);
    }

    QJsonObject metadata{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("title"),
         emote.tooltip.string.isEmpty() ? name : emote.tooltip.string},
        {QStringLiteral("image1x"), image1x},
        {QStringLiteral("zeroWidth"), emote.zeroWidth},
    };
    if (isSevenTVImage(emote))
    {
        metadata.insert(QStringLiteral("provider"),
                        QStringLiteral("seventv"));
    }

    const auto image2x = images.value(QStringLiteral("2x")).toString();
    if (!image2x.isEmpty())
    {
        metadata.insert(QStringLiteral("image2x"), image2x);
    }

    const auto image3x = images.value(QStringLiteral("3x")).toString();
    if (!image3x.isEmpty())
    {
        metadata.insert(QStringLiteral("image3x"), image3x);
    }

    return metadata;
}

QString browserPaintShadowFilter(const Paint &paint)
{
    const auto &settings = *getSettings();
    if (!settings.displaySevenTVPaintShadows.getValue())
    {
        return {};
    }

    const auto radiusScale =
        settings.largeSevenTVPaintShadows.getValue() ? 3.0F : 1.0F;
    QStringList filters;
    for (const auto &shadow : paint.getDropShadows())
    {
        if (!shadow.isValid())
        {
            continue;
        }

        filters.push_back(
            QStringLiteral("drop-shadow(%1px %2px %3px %4)")
                .arg(QString::number(shadow.xOffset(), 'f', 3))
                .arg(QString::number(shadow.yOffset(), 'f', 3))
                .arg(QString::number(shadow.radius() * radiusScale, 'f', 3))
                .arg(cssColor(shadow.color())));
    }
    return filters.join(QLatin1Char(' '));
}

QJsonObject browserUsernamePaint(const Message &message,
                                 const QString &author)
{
    if (author.isEmpty() ||
        (message.platform != MessagePlatform::AnyOrTwitch &&
         message.platform != MessagePlatform::Kick))
    {
        return {};
    }

    auto userName = message.loginName.trimmed();
    if (userName.isEmpty())
    {
        userName = author;
    }

    const bool kick = message.platform == MessagePlatform::Kick;
    auto *paints = getApp()->getSeventvPaints();
    auto paint = paints->getPaint(userName, kick);
    if (!paint)
    {
        return {};
    }

    const auto &settings = *getSettings();
    QFont font(settings.obsOverlayFontFamily.getValue());
    font.setPixelSize(
        std::clamp(settings.obsOverlayFontSize.getValue(), 8, 96));
    font.setWeight(static_cast<QFont::Weight>(
        std::clamp(settings.obsOverlayFontWeight.getValue(), 100, 900)));

    const QFontMetricsF metrics(font);
    const QSizeF textSize{
        std::max(1.0, std::ceil(metrics.horizontalAdvance(author))),
        std::max(1.0, std::ceil(metrics.height())),
    };
    const auto userColor =
        paints->getUserStyleColor(userName, kick)
            .value_or(message.usernameColor.isValid() ? message.usernameColor
                                                      : QColor(Qt::white));
    const auto pixmap =
        paint->getPixmap(author, font, userColor, textSize, 1.0F, 1.0F);
    if (pixmap.isNull())
    {
        return {};
    }

    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !pixmap.save(&buffer, "PNG"))
    {
        return {};
    }

    const auto animated = paint->animated();
    QJsonObject serialized{
        {QStringLiteral("image"),
         QStringLiteral("data:image/png;base64,") +
             QString::fromLatin1(png.toBase64())},
        {QStringLiteral("width"), pixmap.width()},
        {QStringLiteral("height"), pixmap.height()},
        {QStringLiteral("paintId"), paint->id},
        {QStringLiteral("animated"), animated},
    };

    if (animated)
    {
        const auto animationLayers =
            seventvPaintBrowserLayers(paint, userColor);
        if (!animationLayers.isEmpty())
        {
            serialized.insert(QStringLiteral("animationLayers"),
                              animationLayers);
            serialized.insert(QStringLiteral("filter"),
                              browserPaintShadowFilter(*paint));
        }
    }

    return serialized;
}

bool isOverlaySystemOrEventMessage(const Message &message)
{
    return message.flags.hasAny(
        {MessageFlag::System, MessageFlag::ConnectedMessage,
         MessageFlag::DisconnectedMessage, MessageFlag::Subscription,
         MessageFlag::ElevatedMessage, MessageFlag::CheerMessage,
         MessageFlag::RedeemedHighlight,
         MessageFlag::RedeemedChannelPointReward, MessageFlag::WatchStreak,
         MessageFlag::LiveUpdatesAdd, MessageFlag::LiveUpdatesRemove,
         MessageFlag::LiveUpdatesUpdate});
}

QJsonObject browserCurrentSevenTVBadge(const Message &message,
                                       int fallbackIndex)
{
    if (message.userID.isEmpty())
    {
        return {};
    }

    auto *badges = getApp()->getSeventvBadges();
    EmotePtr badge;
    if (message.platform == MessagePlatform::AnyOrTwitch)
    {
        if (auto current = badges->getBadge({message.userID}))
        {
            badge = *current;
        }
    }
    else if (message.platform == MessagePlatform::Kick)
    {
        bool ok = false;
        const auto userID = message.userID.toULongLong(&ok);
        if (ok)
        {
            if (auto current = badges->getKickBadge(userID))
            {
                badge = *current;
            }
        }
    }

    if (!badge)
    {
        return {};
    }

    auto metadata = browserImageMetadata(*badge, true, fallbackIndex);
    if (!metadata.isEmpty())
    {
        metadata.insert(QStringLiteral("provider"),
                        QStringLiteral("seventv"));
    }
    return metadata;
}

QJsonObject browserMessageMetadata(const Message &message)
{
    QJsonArray badges;
    QJsonArray runs;
    int badgeIndex = 0;
    int emoteIndex = 0;

    for (const auto &elementPtr : message.elements)
    {
        const auto *element = elementPtr.get();
        if (element == nullptr)
        {
            continue;
        }

        const auto flags = element->getFlags();

        if (const auto *badge = dynamic_cast<const BadgeElement *>(element))
        {
            if (flags.has(MessageElementFlag::BadgeSevenTV))
            {
                continue;
            }

            const auto emote = badge->getEmote();
            if (!emote)
            {
                continue;
            }

            auto metadata = browserImageMetadata(*emote, true, badgeIndex++);
            if (const auto *ffzBadge =
                    dynamic_cast<const FfzBadgeElement *>(element))
            {
                const auto backgroundColor = QColor(
                    ffzBadge->toJson().value(QStringLiteral("color")).toString());
                if (backgroundColor.isValid())
                {
                    metadata.insert(QStringLiteral("backgroundColor"),
                                    cssColor(backgroundColor));
                }
            }
            if (!metadata.isEmpty())
            {
                badges.push_back(std::move(metadata));
            }
            continue;
        }

        if (flags.hasAny({MessageElementFlag::Username,
                          MessageElementFlag::Timestamp,
                          MessageElementFlag::PlatformBadge,
                          MessageElementFlag::RepliedMessage,
                          MessageElementFlag::ReplyButton,
                          MessageElementFlag::ModeratorTools,
                          MessageElementFlag::ChannelName}))
        {
            continue;
        }

        if (const auto *layered =
                dynamic_cast<const LayeredEmoteElement *>(element))
        {
            QJsonArray layers;
            for (const auto &layer : layered->getEmotes())
            {
                if (!layer.ptr)
                {
                    continue;
                }

                auto metadata =
                    browserImageMetadata(*layer.ptr, false, emoteIndex++);
                if (!metadata.isEmpty())
                {
                    layers.push_back(std::move(metadata));
                }
            }

            if (!layers.isEmpty())
            {
                runs.push_back(QJsonObject{
                    {QStringLiteral("layers"), std::move(layers)},
                    {QStringLiteral("trailingSpace"),
                     element->hasTrailingSpace()},
                });
            }
            continue;
        }

        if (const auto *emote = dynamic_cast<const EmoteElement *>(element))
        {
            if (!flags.hasAny({MessageElementFlag::Emote,
                               MessageElementFlag::EmojiAll}))
            {
                continue;
            }

            const auto emoteData = emote->getEmote();
            if (!emoteData)
            {
                continue;
            }

            auto metadata =
                browserImageMetadata(*emoteData, false, emoteIndex++);
            if (!metadata.isEmpty())
            {
                runs.push_back(QJsonObject{
                    {QStringLiteral("emote"), std::move(metadata)},
                    {QStringLiteral("trailingSpace"),
                     element->hasTrailingSpace()},
                });
            }
            continue;
        }

        if (!flags.has(MessageElementFlag::Text))
        {
            continue;
        }

        QString value;
        if (const auto *text = dynamic_cast<const TextElement *>(element))
        {
            value = text->words().join(QLatin1Char(' '));
        }
        else if (const auto *text =
                     dynamic_cast<const SingleLineTextElement *>(element))
        {
            value = text->words().join(QLatin1Char(' '));
        }

        if (element->hasTrailingSpace())
        {
            value += QLatin1Char(' ');
        }
        if (!value.isEmpty())
        {
            runs.push_back(QJsonObject{{QStringLiteral("text"), value}});
        }
    }

    auto currentSevenTVBadge =
        browserCurrentSevenTVBadge(message, badgeIndex);
    if (!currentSevenTVBadge.isEmpty())
    {
        badges.push_back(std::move(currentSevenTVBadge));
    }

    return QJsonObject{
        {QStringLiteral("badges"), badges},
        {QStringLiteral("runs"), runs},
    };
}

QString browserMessageId(const Message &message)
{
    if (!message.id.isEmpty())
    {
        return message.id;
    }

    return QStringLiteral("local-%1").arg(
        reinterpret_cast<quintptr>(&message), 0, 16);
}

QJsonObject serializeBrowserMessage(const Message &message)
{
    QString author = message.displayName.trimmed();
    if (author.isEmpty())
    {
        author = message.loginName.trimmed();
    }

    const bool isAlert = message.flags.hasAny(
        {MessageFlag::Subscription, MessageFlag::ElevatedMessage,
         MessageFlag::CheerMessage});
    const auto highlightColor = dockEventHighlightColor(message);
    const auto platformAccentColor = cssColor(
        message.platformAccentColor.value_or(
            defaultPlatformAccent(message.platform)));

    return QJsonObject{
        {QStringLiteral("id"), browserMessageId(message)},
        {QStringLiteral("timestamp"),
         message.parseTime.isValid()
             ? message.parseTime.toString(QStringLiteral("H:mm"))
             : QString()},
        {QStringLiteral("author"), author},
        {QStringLiteral("authorColor"),
         message.usernameColor.isValid()
             ? cssColor(readableObsUsernameColor(message.usernameColor))
             : QString()},
        {QStringLiteral("authorPaint"),
         browserUsernamePaint(message, author)},
        {QStringLiteral("text"), message.messageText},
        {QStringLiteral("platform"), platformName(message.platform)},
        {QStringLiteral("platformAccentColor"), platformAccentColor},
        {QStringLiteral("highlightColor"),
         highlightColor.isValid() ? cssColor(highlightColor) : QString()},
        {QStringLiteral("system"), message.flags.has(MessageFlag::System)},
        {QStringLiteral("alert"), isAlert || highlightColor.isValid()},
        {QStringLiteral("action"), message.flags.has(MessageFlag::Action)},
        {QStringLiteral("moderation"),
         message.flags.has(MessageFlag::ModerationAction)},
        {QStringLiteral("metadata"), browserMessageMetadata(message)},
    };
}

QString configuredCssColor(const QString &value, const QColor &fallback)
{
    auto color = QColor(value);
    if (!color.isValid())
    {
        color = fallback;
    }
    return cssColor(color);
}

QString configuredCssColor(const QString &value, int opacity,
                           const QColor &fallback)
{
    auto color = QColor(value);
    if (!color.isValid())
    {
        color = fallback;
    }
    color.setAlpha(std::clamp(opacity, 0, 255));
    return cssColor(color);
}

int normalizedDockTabIndex(int requestedTab, int pageCount, int fallbackIndex)
{
    if (requestedTab >= 0 && requestedTab < pageCount)
    {
        return requestedTab;
    }

    if (fallbackIndex >= 0 && fallbackIndex < pageCount)
    {
        return fallbackIndex;
    }

    if (pageCount > 0)
    {
        return 0;
    }

    return -1;
}

QString splitLabel(Split *split)
{
    if (split == nullptr)
    {
        return QStringLiteral("No split selected");
    }

    if (split->isActivityPane())
    {
        return split->activityPaneTitle();
    }

    const auto channel = split->getChannel();
    if (channel && !channel->getLocalizedName().isEmpty())
    {
        return channel->getLocalizedName();
    }
    if (channel && !channel->getDisplayName().isEmpty())
    {
        return channel->getDisplayName();
    }
    if (channel && !channel->getName().isEmpty())
    {
        return channel->getName();
    }

    return QStringLiteral("Selected split");
}

void appendPlatform(QJsonArray &platforms, QSet<QString> &seen,
                    MessagePlatform platform)
{
    const auto name = platformName(platform);
    if (seen.contains(name))
    {
        return;
    }

    seen.insert(name);
    platforms.push_back(name);
}

QJsonArray pagePlatforms(SplitContainer *page)
{
    QJsonArray platforms;
    QSet<QString> seen;

    if (page == nullptr)
    {
        return platforms;
    }

    for (auto *split : page->getSplits())
    {
        if (split == nullptr || split->isActivityPane())
        {
            continue;
        }

        const auto channel = split->getChannel();
        if (!channel)
        {
            continue;
        }

        if (auto *merged = dynamic_cast<MergedChannel *>(channel.get()))
        {
            const auto &config = merged->config();
            if (config.twitchEnabled)
            {
                appendPlatform(platforms, seen, MessagePlatform::AnyOrTwitch);
            }
            if (config.kickEnabled)
            {
                appendPlatform(platforms, seen, MessagePlatform::Kick);
            }
            if (config.youtubeEnabled)
            {
                appendPlatform(platforms, seen, MessagePlatform::YouTube);
            }
            if (config.tiktokEnabled)
            {
                appendPlatform(platforms, seen, MessagePlatform::TikTok);
            }
            continue;
        }

        if (channel->isKickChannel())
        {
            appendPlatform(platforms, seen, MessagePlatform::Kick);
        }
        else if (channel->isTwitchChannel())
        {
            appendPlatform(platforms, seen, MessagePlatform::AnyOrTwitch);
        }
    }

    return platforms;
}

}  // namespace

namespace chatterino {

ObsBrowserDockServer::ObsBrowserDockServer(QObject *parent)
    : QObject(parent)
    , server_(std::make_unique<HttpServer>(ObsBrowserDockServer::PORT, this))
{
    this->server_->setHandler([this](const HttpServer::Request &request) {
        const auto requestUrl =
            QUrl(QStringLiteral("http://127.0.0.1") + request.target);
        const auto path = requestUrl.path();
        const auto query = QUrlQuery(requestUrl);
        const auto view =
            normalizedDockView(query.queryItemValue(QStringLiteral("view")));
        bool hasRequestedTab = false;
        const auto requestedTab = query.queryItemValue(QStringLiteral("tab"))
                                      .toInt(&hasRequestedTab);

        if (request.method.compare(QStringLiteral("GET"),
                                   Qt::CaseInsensitive) != 0)
        {
            return HttpServer::Response{
                .status = 405,
                .body = QByteArrayLiteral("Method Not Allowed"),
            };
        }

        if (path == QStringLiteral("/obs-dock") ||
            path == QStringLiteral("/obs-dock/"))
        {
            return HttpServer::Response{
                .body = this->dockPageHtml(),
                .contentType = QByteArrayLiteral("text/html; charset=utf-8"),
            };
        }

        if (path == QStringLiteral("/obs-dock/state"))
        {
            return HttpServer::Response{
                .body = this->dockStateJson(view,
                                            hasRequestedTab ? requestedTab : -1),
                .contentType =
                    QByteArrayLiteral("application/json; charset=utf-8"),
            };
        }

        if (path == QStringLiteral("/obs-overlay") ||
            path == QStringLiteral("/obs-overlay/"))
        {
            return HttpServer::Response{
                .body = this->overlayPageHtml(),
                .contentType = QByteArrayLiteral("text/html; charset=utf-8"),
            };
        }

        if (path == QStringLiteral("/obs-overlay/state"))
        {
            return HttpServer::Response{
                .body = this->overlayStateJson(),
                .contentType =
                    QByteArrayLiteral("application/json; charset=utf-8"),
            };
        }

        return HttpServer::Response{
            .status = 404,
            .body = QByteArrayLiteral("Not Found"),
        };
    });

    QTimer::singleShot(0, this, [this] {
        auto &settings = *getSettings();
        if (settings.obsOverlayEnabled.getValue())
        {
            this->resolveOverlayPage(true);
        }
    });
}

QString ObsBrowserDockServer::dockUrl(const QString &view, int tabIndex)
{
    auto url = QStringLiteral("http://127.0.0.1:%1/obs-dock?view=%2")
                   .arg(ObsBrowserDockServer::PORT)
                   .arg(normalizedDockView(view));

    if (tabIndex >= 0)
    {
        url += QStringLiteral("&tab=%1").arg(tabIndex);
    }

    return url;
}

QString ObsBrowserDockServer::overlayUrl()
{
    return QStringLiteral("http://127.0.0.1:%1/obs-overlay")
        .arg(ObsBrowserDockServer::PORT);
}

QStringList ObsBrowserDockServer::availableOverlayTabNames()
{
    QStringList names;
    auto *windows = getApp()->getWindows();
    if (windows == nullptr)
    {
        return names;
    }

    auto &notebook = windows->getMainWindow().getNotebook();
    for (int index = 0; index < notebook.getPageCount(); ++index)
    {
        auto *page = dynamic_cast<SplitContainer *>(notebook.getPageAt(index));
        const auto title = overlayPageTitle(page);
        if (hasChatSplit(page) && !title.isEmpty() &&
            !names.contains(title, Qt::CaseInsensitive))
        {
            names.push_back(title);
        }
    }
    return names;
}

Window *ObsBrowserDockServer::dockWindow() const
{
    auto *windows = getApp()->getWindows();
    if (windows == nullptr)
    {
        return nullptr;
    }

    return &windows->getMainWindow();
}

SplitContainer *ObsBrowserDockServer::selectedPage(int tabIndex) const
{
    auto *window = this->dockWindow();
    if (window == nullptr)
    {
        return nullptr;
    }

    auto &notebook = window->getNotebook();
    const auto pageCount = notebook.getPageCount();
    const auto fallbackIndex = notebook.getSelectedIndex();
    const auto resolvedTabIndex =
        (tabIndex >= 0 && tabIndex < pageCount) ? tabIndex
        : (fallbackIndex >= 0 && fallbackIndex < pageCount) ? fallbackIndex
        : (pageCount > 0)                                 ? 0
                                                           : -1;

    if (resolvedTabIndex < 0)
    {
        return nullptr;
    }

    return dynamic_cast<SplitContainer *>(notebook.getPageAt(resolvedTabIndex));
}

SplitContainer *ObsBrowserDockServer::findOverlayPage(
    const QString &tabName) const
{
    auto *window = this->dockWindow();
    if (window == nullptr)
    {
        return nullptr;
    }

    auto &notebook = window->getNotebook();
    for (int index = 0; index < notebook.getPageCount(); ++index)
    {
        auto *page =
            dynamic_cast<SplitContainer *>(notebook.getPageAt(index));
        auto *tab = page != nullptr ? page->getTab() : nullptr;
        if (tab != nullptr &&
            tab->getTitle().trimmed().compare(tabName, Qt::CaseInsensitive) ==
                0)
        {
            return page;
        }
    }
    return nullptr;
}

SplitContainer *ObsBrowserDockServer::ensureOverlayPage(
    const QString &tabName) const
{
    if (auto *existing = this->findOverlayPage(tabName))
    {
        return existing;
    }

    auto *accounts = getApp()->getAccounts();
    auto *window = this->dockWindow();
    if (accounts == nullptr || window == nullptr)
    {
        return nullptr;
    }

    MergedChannelConfig config{
        .tabName = tabName,
        .twitchEnabled = false,
        .kickEnabled = false,
        .youtubeEnabled = false,
        .tiktokEnabled = false,
    };
    bool hasLinkedAccount = false;

    const auto twitch = accounts->twitch.getCurrent();
    if (accounts->twitch.isLoggedIn() && twitch && !twitch->isAnon() &&
        !twitch->getUserName().trimmed().isEmpty())
    {
        config.twitchEnabled = true;
        config.twitchChannelName = twitch->getUserName().trimmed();
        hasLinkedAccount = true;
    }

    const auto kick = accounts->kick.current();
    if (accounts->kick.isLoggedIn() && kick && !kick->isAnonymous() &&
        !kick->username().trimmed().isEmpty())
    {
        config.kickEnabled = true;
        config.kickChannelName = kick->username().trimmed();
        hasLinkedAccount = true;
    }

    const auto youtube = accounts->youtube.current();
    if (accounts->youtube.isLoggedIn() && youtube &&
        !youtube->isAnonymous() && !youtube->channelID().trimmed().isEmpty())
    {
        config.youtubeEnabled = true;
        config.youtubeStreamUrl = youtube->channelID().trimmed();
        hasLinkedAccount = true;
    }

    if (!hasLinkedAccount)
    {
        return nullptr;
    }

    auto *page = window->getNotebook().addPage(false);
    page->getTab()->setCustomTitle(tabName);
    auto *split = page->appendNewSplit(false);
    split->setChannel(IndirectChannel(
        std::make_shared<MergedChannel>(std::move(config))));
    getApp()->getWindows()->queueSave();
    return page;
}

SplitContainer *ObsBrowserDockServer::resolveOverlayPage(
    bool createIfMissing) const
{
    auto &setting = getSettings()->obsOverlayTabName;
    const auto requestedName = setting.getValue().trimmed();
    const auto legacyAutomatic =
        requestedName.isEmpty() ||
        requestedName.compare(QStringLiteral("OBS Chat"),
                              Qt::CaseInsensitive) == 0;

    if (!legacyAutomatic)
    {
        if (auto *selected = this->findOverlayPage(requestedName))
        {
            return selected;
        }
    }

    const auto linked = linkedOverlayAccounts();
    if (auto *existing = findLinkedAccountPage(this->dockWindow(), linked))
    {
        auto title = overlayPageTitle(existing);
        if ((title.isEmpty() || title.compare(QStringLiteral("OBS Chat"),
                                              Qt::CaseInsensitive) == 0) &&
            !linked.preferredTabName().isEmpty())
        {
            title = linked.preferredTabName();
            existing->getTab()->setCustomTitle(title);
            getApp()->getWindows()->queueSave();
        }

        if (!title.isEmpty() && setting.getValue() != title)
        {
            setting = title;
        }
        return existing;
    }

    if (!createIfMissing || !linked.hasAny())
    {
        return nullptr;
    }

    const auto tabName = linked.preferredTabName();
    if (tabName.isEmpty())
    {
        return nullptr;
    }

    auto *created = this->ensureOverlayPage(tabName);
    if (created != nullptr)
    {
        const auto title = overlayPageTitle(created);
        setting = title.isEmpty() ? tabName : title;
    }
    return created;
}

Split *ObsBrowserDockServer::resolveSplit(SplitContainer *page,
                                          const QString &view) const
{
    if (page == nullptr)
    {
        return nullptr;
    }

    if (view == QStringLiteral("activity"))
    {
        for (auto *split : page->getSplits())
        {
            if (split != nullptr && split->isActivityPane())
            {
                return split;
            }
        }

        return nullptr;
    }

    for (auto *split : page->getSplits())
    {
        if (split != nullptr && !split->isActivityPane())
        {
            return split;
        }
    }

    return nullptr;
}

Split *ObsBrowserDockServer::resolveOverlaySplit(SplitContainer *page) const
{
    if (page == nullptr)
    {
        return nullptr;
    }

    auto *selected = page->getSelectedSplit();
    if (selected != nullptr && !selected->isActivityPane())
    {
        return selected;
    }

    return this->resolveSplit(page, QStringLiteral("chat"));
}

QByteArray ObsBrowserDockServer::dockPageHtml() const
{
    auto *theme = getTheme();

    const auto muted = [&] {
        auto color = theme->window.text;
        color.setAlpha(150);
        return color;
    }();
    const auto divider = [&] {
        auto color = theme->splits.header.border;
        if (!color.isValid() || color.alpha() == 0)
        {
            color = theme->splits.messageSeperator;
        }
        if (!color.isValid() || color.alpha() == 0)
        {
            color = theme->window.text;
            color.setAlpha(40);
        }
        return color;
    }();
    const auto selectedLine = [&] {
        auto color = theme->tabs.selected.line.regular;
        if (!color.isValid() || color.alpha() == 0)
        {
            color = theme->accent;
        }
        return color;
    }();
    const auto twitchIcon = platformIconDataUrl(MessagePlatform::AnyOrTwitch);
    const auto kickIcon = platformIconDataUrl(MessagePlatform::Kick);
    const auto youtubeIcon = platformIconDataUrl(MessagePlatform::YouTube);
    const auto tiktokIcon = platformIconDataUrl(MessagePlatform::TikTok);
    const QString html = QStringLiteral(R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Mergerino OBS Dock</title>
  <style>
    :root {
      color-scheme: dark;
      --window-bg: %1;
      --split-bg: %2;
      --header-bg: %3;
      --header-focused-bg: %4;
      --header-text: %5;
      --header-focused-text: %6;
      --divider: %7;
      --text: %8;
      --muted: %9;
      --msg-bg: %10;
      --msg-alt-bg: %11;
      --msg-text: %12;
      --msg-system: %13;
      --tab-bg: %14;
      --tab-text: %15;
      --tab-line: %16;
      --tab-selected-bg: %17;
      --tab-selected-text: %18;
      --tab-selected-line: %19;
      --scrollbar-bg: %20;
      --scrollbar-thumb: %21;
      --scrollbar-thumb-selected: %22;
      --twitch: #a970ff;
      --kick: #53fc18;
      --youtube: #ff4b4b;
      --tiktok: #3ed7ff;
      font-family: "Segoe UI", "Helvetica Neue", sans-serif;
      font-size: 12px;
    }

    * {
      box-sizing: border-box;
    }

    html, body {
      margin: 0;
      height: 100%;
      background: var(--window-bg);
      color: var(--text);
    }

    body {
      overflow: hidden;
    }

    .shell {
      height: 100%;
      display: grid;
      grid-template-rows: 31px 32px 32px 30px 1fr;
    }

    .windowbar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 10px;
      padding: 0 10px;
      background: var(--window-bg);
      border-bottom: 1px solid var(--divider);
      font-size: 12px;
    }

    .windowbar-title {
      font-weight: 600;
      color: var(--text);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .windowbar-meta {
      color: var(--muted);
      font-size: 11px;
      white-space: nowrap;
    }

    .windowbar-meta.ready {
      color: var(--muted);
    }

    .windowbar-meta.waiting,
    .windowbar-meta.offline {
      color: var(--msg-system);
    }

    .page-toolbar,
    .tabstrip {
      display: flex;
      align-items: center;
      padding: 0 6px;
      background: var(--window-bg);
      border-bottom: 1px solid var(--divider);
    }

    .page-toolbar {
      gap: 6px;
    }

    .tabstrip {
      gap: 1px;
      overflow-x: auto;
      scrollbar-width: none;
    }

    .tabstrip::-webkit-scrollbar {
      display: none;
    }

    .page-tabstrip {
      flex: 1 1 auto;
      min-width: 0;
      white-space: nowrap;
      overflow: hidden;
    }

    .tab {
      display: inline-flex;
      align-items: center;
      padding: 0 10px;
      height: 100%;
      background: var(--tab-bg);
      color: var(--tab-text);
      border-top: 1px solid var(--divider);
      border-left: 1px solid var(--divider);
      border-right: 1px solid var(--divider);
      position: relative;
      user-select: none;
      font-size: 12px;
      text-decoration: none;
      cursor: pointer;
      transition:
        max-width 75ms cubic-bezier(0.55, 0.055, 0.675, 0.19),
        padding 75ms cubic-bezier(0.55, 0.055, 0.675, 0.19),
        background-color 75ms linear,
        color 75ms linear,
        opacity 75ms cubic-bezier(0.55, 0.055, 0.675, 0.19),
        transform 75ms cubic-bezier(0.55, 0.055, 0.675, 0.19);
    }

    .tab::before {
      content: "";
      position: absolute;
      top: 0;
      left: 0;
      right: 0;
      height: 1px;
      background: var(--tab-line);
    }

    .tab.selected {
      background: var(--tab-selected-bg);
      color: var(--tab-selected-text);
    }

    .tab.selected::before {
      height: 2px;
      background: var(--tab-selected-line);
    }

    .page-tab {
      max-width: 200px;
      min-width: 0;
      flex: 0 0 auto;
      overflow: hidden;
    }

    .page-tab-label {
      display: block;
      min-width: 0;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
      transition:
        opacity 75ms cubic-bezier(0.55, 0.055, 0.675, 0.19),
        transform 75ms cubic-bezier(0.55, 0.055, 0.675, 0.19),
        max-width 75ms cubic-bezier(0.55, 0.055, 0.675, 0.19);
    }

    .page-tab-slat {
      display: none;
      width: 2px;
      height: 15px;
      border-radius: 1px;
      background: var(--tab-line);
      flex: 0 0 auto;
    }

    .page-tabstrip.collapsed .page-tab:not(.selected) {
      max-width: 14px;
      padding-left: 0;
      padding-right: 0;
      justify-content: center;
    }

    .page-tabstrip.collapsed .page-tab:not(.selected) .page-tab-label {
      opacity: 0;
      transform: translateX(-6px);
      max-width: 0;
    }

    .page-tabstrip.collapsed .page-tab:not(.selected) .page-tab-slat {
      display: block;
    }

    .tabs-toggle {
      flex: 0 0 auto;
      border: 1px solid var(--divider);
      border-top: 1px solid var(--divider);
      background: var(--tab-bg);
      color: var(--tab-text);
      height: 22px;
      padding: 0 9px;
      font: inherit;
      cursor: pointer;
      position: relative;
    }

    .tabs-toggle::before {
      content: "";
      position: absolute;
      top: 0;
      left: 0;
      right: 0;
      height: 1px;
      background: var(--tab-line);
    }

    .tabs-toggle:hover {
      background: var(--tab-selected-bg);
      color: var(--tab-selected-text);
    }

    .tab:visited,
    .tab:hover,
    .tab:active {
      color: inherit;
    }

    .header {
      display: grid;
      grid-template-columns: 1fr auto;
      align-items: center;
      gap: 8px;
      padding: 0 10px;
      background: var(--header-focused-bg);
      color: var(--header-focused-text);
      border-bottom: 1px solid var(--divider);
    }

    .header-main {
      min-width: 0;
      display: flex;
      align-items: center;
      gap: 6px;
    }

    .header-platforms {
      display: inline-flex;
      align-items: center;
      gap: 4px;
      flex: 0 0 auto;
    }

    .header-platform {
      width: 14px;
      height: 14px;
      display: block;
      opacity: 0.95;
    }

    .pane-title {
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
      font-size: 13px;
      font-weight: 600;
      line-height: 1.2;
    }

    .header-side {
      display: flex;
      align-items: center;
      gap: 8px;
      color: var(--muted);
      font-size: 11px;
      white-space: nowrap;
    }

    .clear-activity {
      display: none;
      border: 1px solid var(--divider);
      background: transparent;
      color: var(--muted);
      height: 20px;
      padding: 0 7px;
      font: inherit;
      cursor: pointer;
    }

    .clear-activity.visible {
      display: inline-flex;
      align-items: center;
    }

    .clear-activity:hover {
      color: var(--header-focused-text);
      border-color: var(--muted);
    }

    .feed {
      min-height: 0;
      overflow: auto;
      background: var(--split-bg);
      scrollbar-width: thin;
      scrollbar-color: var(--scrollbar-thumb) var(--scrollbar-bg);
    }

    .feed::-webkit-scrollbar {
      width: 10px;
    }

    .feed::-webkit-scrollbar-track {
      background: var(--scrollbar-bg);
    }

    .feed::-webkit-scrollbar-thumb {
      background: var(--scrollbar-thumb);
    }

    .msg {
      display: grid;
      grid-template-columns: auto auto auto 1fr;
      align-items: start;
      column-gap: 6px;
      row-gap: 3px;
      padding: 6px 8px 6px 10px;
      position: relative;
      border-bottom: 1px solid var(--divider);
      background: var(--msg-bg);
      overflow: hidden;
      isolation: isolate;
    }

    .msg:nth-child(even) {
      background: var(--msg-alt-bg);
    }

    .msg-top {
      grid-column: 1 / -1;
      display: flex;
      align-items: flex-start;
      gap: 6px;
      min-width: 0;
    }

    .msg::before {
      content: "";
      position: absolute;
      inset: 0 auto 0 0;
      width: 2px;
      background: var(--platform-line, transparent);
      z-index: 3;
    }

    .msg::after {
      content: "";
      position: absolute;
      inset: 0;
      background:
        linear-gradient(90deg, var(--overlay-color, rgba(0, 0, 0, 0)) 0%,
        rgba(0, 0, 0, 0) 58%);
      opacity: 0;
      pointer-events: none;
      z-index: 1;
    }

    .msg.has-overlay::after {
      opacity: 1;
    }

    .msg-top,
    .body {
      position: relative;
      z-index: 2;
    }

    .time,
    .platform-badge,
    .author {
      white-space: nowrap;
    }

    .time {
      color: var(--muted);
      font-size: 11px;
      font-weight: 500;
      line-height: 1.35;
    }

    .platform-badge {
      width: 14px;
      height: 14px;
      min-width: 14px;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      opacity: 0.95;
      line-height: 1;
    }

    .platform-icon {
      width: 14px;
      height: 14px;
      display: block;
    }

    .author {
      font-size: 12px;
      font-weight: 700;
      line-height: 1.35;
      min-width: 0;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    .body {
      grid-column: 2 / -1;
      font-size: 13px;
      line-height: 1.4;
      word-break: break-word;
      color: var(--msg-text);
    }

    .msg.system .body {
      color: var(--msg-system);
    }

    .msg.action .body {
      font-style: italic;
    }

    .msg.moderation .body {
      color: var(--msg-system);
    }

    .empty {
      min-height: 100%;
      display: grid;
      place-items: center;
      padding: 18px;
      color: var(--muted);
      text-align: center;
      line-height: 1.45;
      font-size: 12px;
    }
  </style>
</head>
<body>
  <div class="shell">
    <div class="windowbar">
      <div class="windowbar-title">Mergerino</div>
      <div class="windowbar-meta" id="refresh-state">localhost</div>
    </div>

    <div class="page-toolbar">
      <div class="tabstrip page-tabstrip" id="page-tabs"></div>
      <button class="tabs-toggle" id="tabs-toggle" type="button">Hide Tabs</button>
    </div>

    <div class="tabstrip">
      <a class="tab" id="chat-tab" href="/obs-dock?view=chat">Chat</a>
      <a class="tab" id="activity-tab" href="/obs-dock?view=activity">Activity</a>
    </div>

    <div class="header">
      <div class="header-main">
        <div class="header-platforms" id="header-platforms"></div>
        <div class="pane-title" id="pane-title">Connecting...</div>
      </div>
      <div class="header-side">
        <button class="clear-activity" id="clear-activity" type="button">Clear</button>
        <span id="message-count">0 messages</span>
      </div>
    </div>

    <section class="feed" id="feed"></section>
  </div>
)HTML")
        + QStringLiteral(R"HTML(
  <script>
    const platformIcons = {
      twitch: '%23',
      kick: '%24',
      youtube: '%25',
      tiktok: '%26',
    };
    const tabsCollapsedStorageKey = 'mergerino.obsDockTabsCollapsed';
    const feed = document.getElementById('feed');
    const pageTabs = document.getElementById('page-tabs');
    const tabsToggle = document.getElementById('tabs-toggle');
    const headerPlatforms = document.getElementById('header-platforms');
    const paneTitle = document.getElementById('pane-title');
    const messageCount = document.getElementById('message-count');
    const clearActivity = document.getElementById('clear-activity');
    const refreshState = document.getElementById('refresh-state');
    const chatTab = document.getElementById('chat-tab');
    const activityTab = document.getElementById('activity-tab');
    let tabsCollapsed = localStorage.getItem(tabsCollapsedStorageKey) === '1';
    const clearedActivityIdsByTab = new Map();
    let latestState = null;

    function escapePlatform(platform) {
      return (platform || 'twitch').toLowerCase();
    }

    function setMode(view) {
      chatTab.classList.toggle('selected', view !== 'activity');
      activityTab.classList.toggle('selected', view === 'activity');
    }

    function dockUrl(view, tabIndex) {
      const params = new URLSearchParams();
      params.set('view', view || 'chat');
      if (Number.isInteger(tabIndex) && tabIndex >= 0) {
        params.set('tab', String(tabIndex));
      }
      return `/obs-dock?${params.toString()}`;
    }

    function platformIcon(platform) {
      return platformIcons[escapePlatform(platform)] || platformIcons.twitch;
    }

    function applyTabsCollapsedState() {
      pageTabs.classList.toggle('collapsed', tabsCollapsed);
      tabsToggle.textContent = tabsCollapsed ? 'Show Tabs' : 'Hide Tabs';
      tabsToggle.setAttribute('aria-pressed', tabsCollapsed ? 'true' : 'false');
    }

    function navigateTo(view, tabIndex) {
      history.replaceState(null, '', dockUrl(view, tabIndex));
      refresh();
    }

    function renderHeaderPlatforms(state) {
      headerPlatforms.innerHTML = '';
      if (!state.platforms || state.platforms.length === 0) {
        return;
      }

      for (const platform of state.platforms) {
        const icon = document.createElement('img');
        icon.className = 'header-platform';
        icon.src = platformIcon(platform);
        icon.alt = platform;
        icon.title = platform;
        headerPlatforms.appendChild(icon);
      }
    }

    function renderPageTabs(state) {
      pageTabs.innerHTML = '';

      if (!state.tabs || state.tabs.length === 0) {
        applyTabsCollapsedState();
        return;
      }

      for (const tab of state.tabs) {
        const link = document.createElement('a');
        link.className = 'tab page-tab';
        if (tab.index === state.selectedTab) {
          link.classList.add('selected');
        }
        link.href = dockUrl(state.view || 'chat', tab.index);
        link.title = tab.title || `Tab ${tab.index + 1}`;
        link.addEventListener('click', (event) => {
          event.preventDefault();
          navigateTo(state.view || 'chat', tab.index);
        });

        const label = document.createElement('span');
        label.className = 'page-tab-label';
        label.textContent = tab.title || `Tab ${tab.index + 1}`;
        link.appendChild(label);

        const slat = document.createElement('span');
        slat.className = 'page-tab-slat';
        link.appendChild(slat);

        pageTabs.appendChild(link);
      }

      applyTabsCollapsedState();
    }

    function renderEmpty(text) {
      feed.innerHTML = `<div class="empty">${text}</div>`;
    }

    function activityClearKey(state) {
      return String(Number.isInteger(state.selectedTab) ? state.selectedTab : -1);
    }

    function visibleMessages(state) {
      const messages = state.messages || [];
      if (state.view !== 'activity') {
        return messages;
      }

      const cleared = clearedActivityIdsByTab.get(activityClearKey(state));
      if (!cleared || cleared.size === 0) {
        return messages;
      }

      return messages.filter((message) => !cleared.has(message.id || ''));
    }

    function renderMessages(state) {
      if (!state.ready) {
        renderEmpty(state.emptyMessage || 'Select a split in Mergerino.');
        return;
      }

      const messages = visibleMessages(state);
      if (messages.length === 0) {
        renderEmpty('This pane is live, but there are no visible messages yet.');
        return;
      }

      const shouldStickToBottom =
        feed.scrollTop + feed.clientHeight >= feed.scrollHeight - 28;

      feed.innerHTML = '';

      for (const message of messages) {
        const item = document.createElement('article');
        const platform = escapePlatform(message.platform);
        item.className =
          `msg` +
          (message.system ? ' system' : '') +
          (message.alert ? ' alert' : '') +
          (message.action ? ' action' : '') +
          (message.moderation ? ' moderation' : '') +
          (message.highlightColor ? ' has-overlay' : '');
        item.style.setProperty('--platform-line', message.platformAccentColor || '');
        if (message.highlightColor) {
          item.style.setProperty('--overlay-color', message.highlightColor);
        }

        const top = document.createElement('div');
        top.className = 'msg-top';

        const time = document.createElement('span');
        time.className = 'time';
        time.textContent = message.timestamp || '';
        top.appendChild(time);

        const badge = document.createElement('span');
        badge.className = 'platform-badge';
        const badgeIcon = document.createElement('img');
        badgeIcon.className = 'platform-icon';
        badgeIcon.src = platformIcon(platform);
        badgeIcon.alt = platform;
        badgeIcon.title = platform;
        badge.appendChild(badgeIcon);
        top.appendChild(badge);

        if (message.author) {
          const author = document.createElement('span');
          author.className = 'author';
          author.textContent = message.author;
          if (message.authorColor) {
            author.style.color = message.authorColor;
          }
          top.appendChild(author);
        }

        const body = document.createElement('div');
        body.className = 'body';
        body.textContent = message.text || '';

        item.appendChild(top);
        item.appendChild(body);
        feed.appendChild(item);
      }

      if (shouldStickToBottom) {
        feed.scrollTop = feed.scrollHeight;
      }
    }

    )HTML")
        + QStringLiteral(R"HTML(
    async function refresh() {
      try {
        const response = await fetch(`/obs-dock/state${window.location.search}`, {
          cache: 'no-store',
        });
        if (!response.ok) {
          throw new Error(`HTTP ${response.status}`);
        }

        const state = await response.json();
        latestState = state;
        setMode(state.view || 'chat');
        clearActivity.classList.toggle('visible', state.view === 'activity');
        renderPageTabs(state);
        renderHeaderPlatforms(state);
        chatTab.href = dockUrl('chat', state.selectedTab);
        activityTab.href = dockUrl('activity', state.selectedTab);
        chatTab.onclick = (event) => {
          event.preventDefault();
          navigateTo('chat', state.selectedTab);
        };
        activityTab.onclick = (event) => {
          event.preventDefault();
          navigateTo('activity', state.selectedTab);
        };
        paneTitle.textContent = state.paneTitle || 'Mergerino OBS Dock';
        messageCount.textContent = `${visibleMessages(state).length} messages`;
        refreshState.textContent = state.ready ? 'localhost' : 'waiting';
        refreshState.className = `windowbar-meta ${state.ready ? 'ready' : 'waiting'}`;

        renderMessages(state);
      } catch (error) {
        setMode(new URLSearchParams(window.location.search).get('view') || 'chat');
        clearActivity.classList.remove('visible');
        pageTabs.innerHTML = '';
        headerPlatforms.innerHTML = '';
        paneTitle.textContent = 'Mergerino OBS Dock';
        messageCount.textContent = '0 messages';
        refreshState.textContent = 'offline';
        refreshState.className = 'windowbar-meta offline';
        renderEmpty('Mergerino is not responding on the local dock URL yet.');
      }
    }

    tabsToggle.addEventListener('click', () => {
      tabsCollapsed = !tabsCollapsed;
      localStorage.setItem(tabsCollapsedStorageKey, tabsCollapsed ? '1' : '0');
      applyTabsCollapsedState();
    });

    clearActivity.addEventListener('click', () => {
      if (!latestState || latestState.view !== 'activity') {
        return;
      }

      const key = activityClearKey(latestState);
      const cleared = clearedActivityIdsByTab.get(key) || new Set();
      for (const message of latestState.messages || []) {
        if (message.id) {
          cleared.add(message.id);
        }
      }
      clearedActivityIdsByTab.set(key, cleared);
      renderMessages(latestState);
      messageCount.textContent = `${visibleMessages(latestState).length} messages`;
    });

    applyTabsCollapsedState();
    refresh();
    setInterval(refresh, 900);
  </script>
</body>
</html>
)HTML");

    return html.arg(cssColor(theme->window.background))
        .arg(cssColor(theme->splits.background))
        .arg(cssColor(theme->splits.header.background))
        .arg(cssColor(theme->splits.header.focusedBackground))
        .arg(cssColor(theme->splits.header.text))
        .arg(cssColor(theme->splits.header.focusedText))
        .arg(cssColor(divider))
        .arg(cssColor(theme->window.text))
        .arg(cssColor(muted))
        .arg(cssColor(theme->messages.backgrounds.regular))
        .arg(cssColor(theme->messages.backgrounds.alternate))
        .arg(cssColor(theme->messages.textColors.regular))
        .arg(cssColor(theme->messages.textColors.system))
        .arg(cssColor(theme->tabs.regular.backgrounds.regular))
        .arg(cssColor(theme->tabs.regular.text))
        .arg(cssColor(theme->tabs.regular.line.regular))
        .arg(cssColor(theme->tabs.selected.backgrounds.regular))
        .arg(cssColor(theme->tabs.selected.text))
        .arg(cssColor(selectedLine))
        .arg(cssColor(theme->scrollbars.background))
        .arg(cssColor(theme->scrollbars.thumb))
        .arg(cssColor(theme->scrollbars.thumbSelected))
        .arg(twitchIcon)
        .arg(kickIcon)
        .arg(youtubeIcon)
        .arg(tiktokIcon)
        .toUtf8();
}

QByteArray ObsBrowserDockServer::overlayPageHtml() const
{
    const auto twitchIcon = platformIconDataUrl(MessagePlatform::AnyOrTwitch);
    const auto kickIcon = platformIconDataUrl(MessagePlatform::Kick);
    const auto youtubeIcon = platformIconDataUrl(MessagePlatform::YouTube);
    const auto tiktokIcon = platformIconDataUrl(MessagePlatform::TikTok);

    return (QString::fromUtf8(R"OBSOVERLAY(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Mergerino OBS Chat Overlay</title>
<link rel="preconnect" href="https://cdn.7tv.app" crossorigin>
<link rel="dns-prefetch" href="https://cdn.7tv.app">
<style>
:root {
  --font-family: "Segoe UI", sans-serif;
  --font-size: 28px;
  --font-weight: 600;
  --text: #fff;
  --background: rgba(17, 17, 17, .667);
  --shadow: 0 1px 4px rgba(0, 0, 0, .824);
  --spacing: 4px;
  --radius: 8px;
  --emote-size: 34px;
  --fade-duration: 500ms;
}
* { box-sizing: border-box; }
html, body {
  width: 100%;
  height: 100%;
  margin: 0;
  overflow: hidden;
  background: transparent !important;
}
body {
  color: var(--text);
  font-family: var(--font-family);
  font-size: var(--font-size);
  font-weight: var(--font-weight);
  line-height: 1.28;
  text-shadow: var(--shadow);
}
#feed {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  gap: var(--spacing);
  padding: 12px;
  overflow: hidden;
  pointer-events: none;
}
.message {
  flex: 0 0 auto;
  min-width: 0;
  padding: .36em .52em .42em;
  overflow-wrap: anywhere;
  background: var(--background);
  border-radius: var(--radius);
  opacity: 1;
  transform: translate3d(0, 0, 0);
}
.message.loading-media { display: none; }
#feed[data-platform-style="accent-line"] .message {
  border-left: max(3px, .13em) solid var(--accent, #888);
}
.message.has-highlight {
  background:
    linear-gradient(90deg, var(--highlight), transparent 70%),
    var(--background);
}
#feed[data-highlights="false"] .message {
  background: var(--background);
}
#feed[data-message-animations="true"] .message.entering {
  animation: overlay-message-in 280ms ease-out both;
}
.message.leaving {
  opacity: 0;
  transform: translateY(-10px);
  transition:
    opacity var(--fade-duration) ease,
    transform var(--fade-duration) ease;
}
@keyframes overlay-message-in {
  from { opacity: 0; transform: translate3d(0, 10px, 0); }
  to { opacity: 1; transform: translate3d(0, 0, 0); }
}
.header {
  display: inline-flex;
  align-items: center;
  gap: .24em;
  max-width: 100%;
  margin-right: .18em;
  vertical-align: baseline;
}
.timestamp {
  flex: 0 0 auto;
  opacity: .72;
  font-size: .72em;
}
.platform-icon {
  width: .9em;
  height: .9em;
  object-fit: contain;
  flex: 0 0 auto;
}
.badges {
  display: inline-flex;
  align-items: center;
  gap: .1em;
  flex: 0 0 auto;
}
.badge {
  width: .86em;
  height: .86em;
  object-fit: contain;
}
.author {
  min-width: 0;
  overflow: visible;
  white-space: nowrap;
  color: var(--author-color, var(--text));
}
.author.has-paint {
  position: relative;
  display: inline-flex;
  align-items: center;
}
.author-paint {
  width: auto;
  height: 1.18em;
  object-fit: contain;
  vertical-align: -.18em;
}
.author.has-live-paint .author-text {
  display: inline;
  color: transparent;
  -webkit-text-fill-color: transparent;
  text-shadow: none;
}
.author-paint-live {
  position: absolute;
  left: 0;
  top: 0;
  height: 100%;
  display: grid;
  align-items: center;
  pointer-events: none;
  filter: var(--author-paint-filter, none);
}
.author-paint-live-layer {
  grid-area: 1 / 1;
  white-space: pre;
  color: transparent;
  text-shadow: none;
  -webkit-text-fill-color: transparent;
  background-position: center;
  background-repeat: no-repeat;
  background-size: 100% 100%;
  background-clip: text;
  -webkit-background-clip: text;
}
.author-paint-canvas {
  position: absolute;
  pointer-events: none;
  filter: var(--author-paint-filter, none);
}
.author.has-paint .author-text { display: none; }
.author.has-live-paint .author-text { display: inline; }
.author.has-canvas-paint:not(.canvas-paint-ready) .author-text {
  color: inherit;
  -webkit-text-fill-color: currentColor;
}
.author::after { content: ":"; color: inherit; }
.message.system .author::after { content: ""; }
.body {
  display: inline;
  white-space: pre-wrap;
}
.message.action .body { font-style: italic; }
.emote,
.layered-emote {
  width: auto;
  height: var(--emote-size);
  vertical-align: middle;
}
.emote {
  object-fit: contain;
  margin: -.18em .03em;
}
.layered-emote {
  position: relative;
  display: inline-block;
  margin: -.18em .03em;
}
.layered-emote .emote-layer {
  position: absolute;
  inset: 0 auto auto 0;
  width: auto;
  height: 100%;
  margin: 0;
  object-fit: contain;
}
.layered-emote .emote-layer:first-child { position: relative; }
.seventv-emote-code,
.seventv-layer-fallback { display: none; }
.run-space { white-space: pre; }
#feed[data-timestamps="false"] .timestamp,
#feed:not([data-platform-style="logos"]) .platform-icon,
#feed[data-badges="false"] .badges { display: none; }
#feed[data-seventv-badges="false"] .badge[data-provider="seventv"],
#feed[data-seventv-emotes="false"] .seventv-emote-image,
#feed[data-seventv-emotes="false"] .emote-layer[data-provider="seventv"],
#feed[data-seventv-emotes="false"] .layered-emote[data-seventv-only="true"],
#feed[data-seventv-paints="false"] .author-paint,
#feed[data-seventv-paints="false"] .author-paint-canvas,
#feed[data-seventv-paints="false"] .author-paint-live { display: none; }
#feed[data-seventv-emotes="false"] .seventv-emote-code,
#feed[data-seventv-emotes="false"] .layered-emote[data-seventv-only="true"] + .seventv-layer-fallback,
#feed[data-seventv-paints="false"] .author.has-paint .author-text {
  display: inline;
}
#feed[data-seventv-paints="false"] .author.has-live-paint .author-text {
  color: inherit;
  -webkit-text-fill-color: currentColor;
  background-image: none !important;
  filter: none;
}
#feed[data-seventv-emotes="false"] .emote-layer.non-seventv-base {
  position: relative;
}
#feed[data-username-colors="false"] .author { color: var(--text); }
#paint-animation-sources {
  position: fixed;
  left: 0;
  top: 0;
  width: 1px;
  height: 1px;
  overflow: hidden;
  pointer-events: none;
  z-index: -1;
}
.paint-animation-source {
  position: absolute;
  width: 1px;
  height: 1px;
  opacity: .001;
}
#status {
  position: fixed;
  top: 12px;
  left: 12px;
  z-index: 20;
  display: none;
  max-width: calc(100% - 24px);
  padding: 8px 11px;
  color: #fff;
  font: 600 14px/1.35 "Segoe UI", sans-serif;
  text-shadow: 0 1px 2px #000;
  background: rgba(12, 12, 15, .82);
  border: 1px solid rgba(255, 255, 255, .18);
  border-radius: 7px;
}
body.preview #status.visible { display: block; }
</style>
</head>
<body>
<div id="feed" aria-live="polite"></div>
<div id="paint-animation-sources" aria-hidden="true"></div>
<div id="status"></div>
<script>
(function () {
  "use strict";

  const preview = new URLSearchParams(location.search).get("preview") === "1";
  if (preview) {
    document.body.classList.add("preview");
  }

  const feed = document.getElementById("feed");
  const paintAnimationSources =
    document.getElementById("paint-animation-sources");
  const status = document.getElementById("status");
  const nodes = new Map();
  const expired = new Set();
  const platformIcons = {
    twitch: "%1",
    kick: "%2",
    youtube: "%3",
    tiktok: "%4"
  };
  let config = {};
  let sourceKey = "";

  function setStatus(text) {
    status.textContent = text || "";
    status.classList.toggle("visible", preview && !!text);
  }

  function setCss(name, value) {
    document.documentElement.style.setProperty(name, value);
  }

  function applyConfig(next) {
    config = next || {};
    setCss("--font-family", config.fontFamily || "Segoe UI");
    setCss("--font-size", String(config.fontSize || 28) + "px");
    setCss("--font-weight", String(config.fontWeight || 600));
    setCss("--text", config.textColor || "#fff");
    setCss("--background", config.backgroundColor || "transparent");
    setCss("--shadow", "0 1px " + String(config.shadowBlur || 0) +
      "px " + (config.shadowColor || "transparent"));
    setCss("--spacing", String(config.messageSpacing || 0) + "px");
    setCss("--radius", String(config.borderRadius || 0) + "px");
    setCss("--emote-size", String(config.emoteSize || 34) + "px");
    setCss("--fade-duration", String(config.fadeDurationMs || 0) + "ms");

    const platformStyle =
      ["logos", "accent-line", "none"].includes(config.platformStyle)
        ? config.platformStyle
        : "logos";
    feed.dataset.timestamps = String(!!config.showTimestamps);
    feed.dataset.platformStyle = platformStyle;
    feed.dataset.badges = String(!!config.showBadges);
    feed.dataset.seventvEmotes = String(!!config.showSevenTVEmotes);
    feed.dataset.seventvBadges = String(!!config.showSevenTVBadges);
    feed.dataset.seventvPaints = String(!!config.showSevenTVPaints);
    feed.dataset.usernameColors = String(!!config.useUsernameColors);
    feed.dataset.highlights = String(!!config.showHighlights);
    feed.dataset.messageAnimations =
      String(config.messageAnimations !== false);
    feed.style.justifyContent =
      config.newestAtBottom === false ? "flex-start" : "flex-end";
  }

  function makeImage(meta, className) {
    const img = document.createElement("img");
    img.className = className;
    img.src = meta.image2x || meta.image1x || meta.image3x || "";
    img.alt = meta.name || "";
    img.title = meta.title || meta.name || "";
    img.draggable = false;
    img.decoding = "async";
    img.referrerPolicy = "no-referrer";
    if (meta.provider) {
      img.dataset.provider = meta.provider;
    }
    if (meta.backgroundColor) {
      img.style.backgroundColor = String(meta.backgroundColor);
    }
    return img;
  }

  function updateBadges(container, message) {
    if (!container) {
      return;
    }
    const badgeData =
      message.metadata && Array.isArray(message.metadata.badges)
        ? message.metadata.badges.filter(function (badge) {
            return badge && badge.image1x;
          })
        : [];
    const signature = JSON.stringify(badgeData.map(function (badge) {
      return [
        badge.id || "",
        badge.image1x || "",
        badge.image2x || "",
        badge.image3x || "",
        badge.name || "",
        badge.title || "",
        badge.provider || "",
        badge.backgroundColor || ""
      ];
    }));
    if (container.dataset.badgeSignature === signature) {
      return;
    }

    const fragment = document.createDocumentFragment();
    for (const badge of badgeData) {
      fragment.appendChild(makeImage(badge, "badge"));
    }
    container.replaceChildren(fragment);
    container.dataset.badgeSignature = signature;
  }

  function appendRuns(body, message) {
    const metadata = message.metadata || {};
    const runs = Array.isArray(metadata.runs) ? metadata.runs : [];
    if (!runs.length) {
      body.textContent = message.text || "";
      return;
    }

    for (const run of runs) {
      if (typeof run.text === "string") {
        body.appendChild(document.createTextNode(run.text));
        continue;
      }

      if (run.emote && run.emote.image1x) {
        const image = makeImage(run.emote, "emote");
        if (run.emote.provider === "seventv") {
          image.classList.add("seventv-emote-image");
          const emoteRun = document.createElement("span");
          emoteRun.className = "seventv-emote-run";
          emoteRun.appendChild(image);
          const code = document.createElement("span");
          code.className = "seventv-emote-code";
          code.textContent = run.emote.name || "";
          emoteRun.appendChild(code);
          body.appendChild(emoteRun);
        } else {
          body.appendChild(image);
        }
        if (run.trailingSpace) {
          body.appendChild(document.createTextNode(" "));
        }
        continue;
      }

      if (Array.isArray(run.layers) && run.layers.length) {
        const wrapper = document.createElement("span");
        wrapper.className = "layered-emote";
        const visibleLayers = run.layers.filter(function (layer) {
          return layer && layer.image1x;
        });
        const sevenTVOnly = visibleLayers.length > 0 &&
          visibleLayers.every(function (layer) {
            return layer.provider === "seventv";
          });
        wrapper.dataset.seventvOnly = String(sevenTVOnly);
        let foundNonSevenTVBase = false;
        for (const layer of visibleLayers) {
          const image = makeImage(layer, "emote-layer");
          if (layer.provider !== "seventv" && !foundNonSevenTVBase) {
            image.classList.add("non-seventv-base");
            foundNonSevenTVBase = true;
          }
          wrapper.appendChild(image);
        }
        if (wrapper.childElementCount) {
          body.appendChild(wrapper);
          if (sevenTVOnly) {
            const fallback = document.createElement("span");
            fallback.className = "seventv-layer-fallback";
            fallback.textContent = visibleLayers[0].name || "";
            body.appendChild(fallback);
          }
          if (run.trailingSpace) {
            body.appendChild(document.createTextNode(" "));
          }
        }
      }
    }
  }

  function updateBody(body, message) {
    if (!body) {
      return false;
    }
    const runs = message.metadata && Array.isArray(message.metadata.runs)
      ? message.metadata.runs
      : [];
    const signature = JSON.stringify({
      text: message.text || "",
      runs: runs
    });
    if (body.dataset.bodySignature === signature) {
      return false;
    }
    body.replaceChildren();
    appendRuns(body, message);
    body.dataset.bodySignature = signature;
    return true;
  }

  function messageAssetSignature(message) {
    return JSON.stringify({
      paint: message.authorPaint || {},
      badges: message.metadata && Array.isArray(message.metadata.badges)
        ? message.metadata.badges
        : [],
      runs: message.metadata && Array.isArray(message.metadata.runs)
        ? message.metadata.runs
        : []
    });
  }

  function waitForImage(image) {
    return new Promise(function (resolve) {
      if (!image || !image.src || image.complete) {
        resolve();
        return;
      }
      image.addEventListener("load", resolve, { once: true });
      image.addEventListener("error", resolve, { once: true });
    });
  }

  function settleMessageMedia(card, message, isNew) {
    const generation = Number(card.dataset.mediaGeneration || 0) + 1;
    card.dataset.mediaGeneration = String(generation);
    if (isNew) {
      card.classList.add("loading-media");
    }

    const images = Array.from(card.querySelectorAll("img"));
    const paint = message.authorPaint || {};
    const animationLayers = Array.isArray(paint.animationLayers)
      ? paint.animationLayers
      : [];
    for (const layer of animationLayers) {
      if (layer && layer.imageUrl) {
        images.push(animatedPaintImage(layer.imageUrl));
      }
    }

    const ready = Promise.all(images.map(waitForImage));
    const timeout = new Promise(function (resolve) {
      setTimeout(resolve, 1800);
    });
    return Promise.race([ready, timeout]).then(function () {
      return Number(card.dataset.mediaGeneration || 0) === generation;
    });
  }

  function revealNewMessage(card, delay) {
    setTimeout(function () {
      if (!card.isConnected) {
        return;
      }
      card.classList.remove("loading-media");
      if (config.messageAnimations === false) {
        return;
      }
      card.classList.add("entering");
      let finished = false;
      const finishEntry = function () {
        if (finished) {
          return;
        }
        finished = true;
        card.classList.remove("entering");
      };
      card.addEventListener("animationend", finishEntry, { once: true });
      setTimeout(finishEntry, 360);
    }, delay);
  }

)OBSOVERLAY") +
           QString::fromUtf8(R"OBSOVERLAY(  const animatedPaintImages = new Map();

  function animatedPaintImage(url) {
    const key = String(url || "");
    let image = animatedPaintImages.get(key);
    if (!image) {
      image = new Image();
      image.className = "paint-animation-source";
      image.decoding = "async";
      image.alt = "";
      image.setAttribute("aria-hidden", "true");
      paintAnimationSources.appendChild(image);
      image.src = key;
      animatedPaintImages.set(key, image);
    }
    return image;
  }

  function paintLayerOpacity(value) {
    const opacity = Number(value);
    return Number.isFinite(opacity)
      ? Math.min(1, Math.max(0, opacity))
      : 1;
  }

  function renderAnimatedPaintCanvases() {
    for (const canvas of document.querySelectorAll(
      ".author-paint-canvas")) {
      const author = canvas.parentElement;
      const authorText = author &&
        author.querySelector(".author-text");
      const layers = Array.isArray(canvas.paintLayers)
        ? canvas.paintLayers
        : [];
      if (!author || !authorText || !layers.length) {
        continue;
      }

      const textRect = authorText.getBoundingClientRect();
      const authorRect = author.getBoundingClientRect();
      const width = textRect.width;
      const height = textRect.height;
      if (width <= 0 || height <= 0) {
        continue;
      }

      let ready = true;
      for (const layer of layers) {
        if (!layer.imageUrl) {
          continue;
        }
        const image = animatedPaintImage(layer.imageUrl);
        if (!image.complete || !image.naturalWidth) {
          ready = false;
          break;
        }
      }
      canvas.style.visibility = ready ? "visible" : "hidden";
      author.classList.toggle("canvas-paint-ready", ready);
      if (!ready) {
        continue;
      }

      const scale = Math.max(1, Number(window.devicePixelRatio) || 1);
      const pixelWidth = Math.max(1, Math.ceil(width * scale));
      const pixelHeight = Math.max(1, Math.ceil(height * scale));
      if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) {
        canvas.width = pixelWidth;
        canvas.height = pixelHeight;
      }
      canvas.style.left = (textRect.left - authorRect.left) + "px";
      canvas.style.top = (textRect.top - authorRect.top) + "px";
      canvas.style.width = width + "px";
      canvas.style.height = height + "px";

      const context = canvas.getContext("2d");
      if (!context) {
        continue;
      }
      context.setTransform(scale, 0, 0, scale, 0, 0);
      context.clearRect(0, 0, width, height);
      context.imageSmoothingEnabled = true;
      context.imageSmoothingQuality = "high";
      context.globalCompositeOperation = "source-over";

      for (const layer of layers) {
        context.globalAlpha = paintLayerOpacity(layer.opacity);
        if (layer.backgroundColor) {
          context.fillStyle = String(layer.backgroundColor);
          context.fillRect(0, 0, width, height);
        }
        if (layer.imageUrl) {
          context.drawImage(
            animatedPaintImage(layer.imageUrl), 0, 0, width, height);
        }
      }

      const style = getComputedStyle(authorText);
      const label = authorText.textContent || "";
      context.globalAlpha = 1;
      context.globalCompositeOperation = "destination-in";
      context.fillStyle = "#fff";
      context.font = [
        style.fontStyle,
        style.fontVariant,
        style.fontWeight,
        style.fontSize,
        style.fontFamily
      ].join(" ");
      context.textAlign = "left";
      context.textBaseline = "alphabetic";
      const metrics = context.measureText(label);
      const fontSize = Number.parseFloat(style.fontSize) || height;
      const ascent = metrics.actualBoundingBoxAscent || fontSize * 0.8;
      const descent = metrics.actualBoundingBoxDescent || fontSize * 0.2;
      const baseline = (height + ascent - descent) / 2;
      context.fillText(label, 0, baseline);
      context.globalCompositeOperation = "source-over";
    }
  }

  function updateAuthorPaint(author, message) {
    if (!author) {
      return;
    }

    const paint = message.authorPaint || {};
    let paintImage = author.querySelector(".author-paint");
    let paintCanvas = author.querySelector(".author-paint-canvas");
    let livePaint = author.querySelector(".author-paint-live");
    const authorText = author.querySelector(".author-text");

    if (!paint.image) {
      if (paintImage) {
        paintImage.remove();
      }
      if (paintCanvas) {
        paintCanvas.remove();
      }
      if (livePaint) {
        livePaint.remove();
      }
      if (authorText) {
        authorText.style.backgroundImage = "";
      }
      author.style.removeProperty("--author-paint-filter");
      author.classList.remove(
        "has-paint", "has-live-paint", "has-canvas-paint",
        "canvas-paint-ready");
      return;
    }

    const animationLayers =
      paint.animated && Array.isArray(paint.animationLayers)
        ? paint.animationLayers.filter(function (layer) {
            return layer && (layer.backgroundColor ||
              layer.backgroundImage || layer.imageUrl);
          })
        : [];
    if (animationLayers.length && authorText) {
      if (paintCanvas) {
        paintCanvas.remove();
        paintCanvas = null;
      }
      author.classList.remove("has-canvas-paint", "canvas-paint-ready");
      if (paintImage) {
        paintImage.remove();
      }
      authorText.style.backgroundImage = "";
      const signature = JSON.stringify(animationLayers);
      if (!livePaint) {
        livePaint = document.createElement("span");
        livePaint.className = "author-paint-live";
        livePaint.setAttribute("aria-hidden", "true");
        author.appendChild(livePaint);
      }
      if (livePaint.dataset.paintSignature !== signature) {
        livePaint.replaceChildren();
        for (const layer of animationLayers) {
          const visual = document.createElement("span");
          visual.className = "author-paint-live-layer";
          visual.textContent = authorText.textContent || "";
          const opacity = Number(layer.opacity);
          visual.style.opacity = String(
            Number.isFinite(opacity)
              ? Math.min(1, Math.max(0, opacity))
              : 1);
          if (layer.backgroundColor) {
            visual.style.backgroundColor = String(layer.backgroundColor);
          }
          if (layer.imageUrl) {
            visual.dataset.animatedPaintLayer = "true";
            visual.style.backgroundImage =
              "url(" + JSON.stringify(String(layer.imageUrl)) + ")";
          } else if (layer.backgroundImage) {
            visual.style.backgroundImage = String(layer.backgroundImage);
          }
          livePaint.appendChild(visual);
        }
        livePaint.dataset.paintSignature = signature;
      }

      author.style.setProperty(
        "--author-paint-filter", String(paint.filter || "none"));
      author.classList.add("has-paint", "has-live-paint");
      return;
    }

    author.classList.remove(
      "has-live-paint", "has-canvas-paint", "canvas-paint-ready");
    author.style.removeProperty("--author-paint-filter");
    if (authorText) {
      authorText.style.backgroundImage = "";
    }
    if (paintCanvas) {
      paintCanvas.remove();
    }
    if (livePaint) {
      livePaint.remove();
    }

    if (!paintImage) {
      paintImage = document.createElement("img");
      paintImage.className = "author-paint";
      paintImage.alt = "";
      paintImage.draggable = false;
      paintImage.setAttribute("aria-hidden", "true");
      author.appendChild(paintImage);
    }
    const paintFrame = String(paint.image);
    if (paintImage.getAttribute("src") !== paintFrame) {
      paintImage.src = paintFrame;
    }

    author.classList.add("has-paint");
  }

  function createMessage(message) {
    const card = document.createElement("article");
    card.className = "message";
    card.dataset.messageId = message.id;
    card.dataset.addedAt = String(Date.now());
    card.style.setProperty(
      "--accent", message.platformAccentColor || "#888");
    if (message.highlightColor) {
      card.classList.add("has-highlight");
      card.style.setProperty("--highlight", message.highlightColor);
    }
    card.classList.toggle("system", !!message.system);
    card.classList.toggle("action", !!message.action);

    const header = document.createElement("span");
    header.className = "header";

    const timestamp = document.createElement("span");
    timestamp.className = "timestamp";
    timestamp.textContent = message.timestamp || "";
    header.appendChild(timestamp);

    const iconUrl = platformIcons[message.platform];
    const icon = document.createElement("img");
    icon.className = "platform-icon";
    icon.src = iconUrl || "";
    icon.alt = message.platform || "";
    icon.title = message.platform || "";
    icon.draggable = false;
    header.appendChild(icon);

    const badges = document.createElement("span");
    badges.className = "badges";
    updateBadges(badges, message);
    header.appendChild(badges);

    const author = document.createElement("span");
    author.className = "author";
    const authorLabel =
      message.author || (message.system ? "Mergerino" : "Unknown");
    author.title = authorLabel;
    author.dataset.authorLabel = authorLabel;
    author.style.setProperty(
      "--author-color", message.authorColor || "var(--text)");

    const authorText = document.createElement("span");
    authorText.className = "author-text";
    authorText.textContent = authorLabel;
    author.appendChild(authorText);

    updateAuthorPaint(author, message);
    header.appendChild(author);
    card.appendChild(header);

    const body = document.createElement("span");
    body.className = "body";
    updateBody(body, message);
    card.appendChild(body);
    card.dataset.assetSignature = messageAssetSignature(message);
    return card;
  }

  function removeMessage(id, markExpired, immediate) {
    const node = nodes.get(id);
    if (!node) {
      return;
    }
    nodes.delete(id);
    if (markExpired) {
      expired.add(id);
    }
    if (immediate || !Number(config.fadeDurationMs)) {
      node.remove();
      return;
    }
    node.classList.remove("entering");
    node.classList.add("leaving");
    setTimeout(function () {
      node.remove();
    }, Number(config.fadeDurationMs) + 40);
  }

  function clearMessages(immediate) {
    for (const id of Array.from(nodes.keys())) {
      removeMessage(id, false, immediate);
    }
  }

  function sync(state) {
    applyConfig(state.config);

    if (sourceKey !== (state.sourceKey || "")) {
      clearMessages(true);
      expired.clear();
      sourceKey = state.sourceKey || "";
    }

    if (!state.enabled) {
      clearMessages(false);
      setStatus("The OBS chat overlay is disabled in Mergerino settings.");
      return;
    }
    if (!state.ready) {
      clearMessages(false);
      setStatus(state.emptyMessage || "Waiting for a Mergerino chat source...");
      return;
    }

    const messages = Array.isArray(state.messages) ? state.messages : [];
    const newEntries = [];
    const desired = new Set(messages.map(function (message) {
      return message.id;
    }));

    for (const id of Array.from(nodes.keys())) {
      if (!desired.has(id)) {
        removeMessage(id, false, false);
      }
    }
    for (const id of Array.from(expired)) {
      if (!desired.has(id)) {
        expired.delete(id);
      }
    }

    for (const message of messages) {
      if (!message.id || expired.has(message.id)) {
        continue;
      }

      const existing = nodes.get(message.id);
      if (existing) {
        const assetSignature = messageAssetSignature(message);
        if (existing.dataset.assetSignature !== assetSignature) {
          updateAuthorPaint(existing.querySelector(".author"), message);
          updateBadges(existing.querySelector(".badges"), message);
          updateBody(existing.querySelector(".body"), message);
          existing.dataset.assetSignature = assetSignature;
          const stillLoading =
            existing.classList.contains("loading-media");
          settleMessageMedia(existing, message, stillLoading).then(
            function (settled) {
              if (settled && stillLoading) {
                revealNewMessage(existing, 0);
              }
            });
        }
        continue;
      }

      const node = createMessage(message);
      nodes.set(message.id, node);
      feed.appendChild(node);
      newEntries.push({ node: node, message: message });
    }

    const ordered = config.newestAtBottom === false
      ? messages.slice().reverse()
      : messages;
    for (const message of ordered) {
      const node = nodes.get(message.id);
      if (node) {
        feed.appendChild(node);
      }
    }
    if (newEntries.length) {
      Promise.all(newEntries.map(function (entry) {
        return settleMessageMedia(entry.node, entry.message, true);
      })).then(function (settled) {
        newEntries.forEach(function (entry, index) {
          if (settled[index]) {
            revealNewMessage(entry.node, index * 70);
          }
        });
      });
    }

    setStatus(messages.length
      ? state.paneTitle
      : "Connected to " + state.paneTitle + " - waiting for messages...");
  }

  function expireMessages() {
    const lifetime = Number(config.lifetimeSeconds || 0);
    if (lifetime <= 0) {
      return;
    }
    const cutoff = Date.now() - lifetime * 1000;
    for (const [id, node] of Array.from(nodes.entries())) {
      if (Number(node.dataset.addedAt || 0) <= cutoff) {
        removeMessage(id, true, false);
      }
    }
  }

  let paintRefreshPhase = false;
  function refreshAnimatedPaintFrames() {
    paintRefreshPhase = !paintRefreshPhase;
    const position = paintRefreshPhase
      ? "50% 50%"
      : "calc(50% + 0.02px) 50%";
    for (const animatedLayer of document.querySelectorAll(
      ".author-paint-live-layer[data-animated-paint-layer='true']")) {
      animatedLayer.style.backgroundPosition = position;
    }
  }

  async function refresh() {
    try {
      const response = await fetch("/obs-overlay/state", {
        cache: "no-store"
      });
      if (!response.ok) {
        throw new Error("HTTP " + response.status);
      }
      sync(await response.json());
    } catch (error) {
      clearMessages(false);
      setStatus("Mergerino overlay unavailable. Keep the app open.");
    }
  }

  refresh();
  setInterval(refreshAnimatedPaintFrames, 40);
  setInterval(refresh, 500);
  setInterval(expireMessages, 250);
}());
</script>
</body>
</html>)OBSOVERLAY"))
        .arg(twitchIcon)
        .arg(kickIcon)
        .arg(youtubeIcon)
        .arg(tiktokIcon)
        .toUtf8();
}

QByteArray ObsBrowserDockServer::overlayStateJson() const
{
    auto &settings = *getSettings();
    const auto enabled = settings.obsOverlayEnabled.getValue();
    auto *page = this->resolveOverlayPage(enabled);
    auto tabName = overlayPageTitle(page);
    if (tabName.isEmpty())
    {
        tabName = settings.obsOverlayTabName.getValue().trimmed();
    }
    if (tabName.isEmpty())
    {
        tabName = linkedOverlayAccounts().preferredTabName();
    }
    auto *window = this->dockWindow();
    const auto selectedTabIndex = window != nullptr && page != nullptr
                                      ? window->getNotebook().indexOf(page)
                                      : -1;
    auto *split = this->resolveOverlaySplit(page);
    const auto maxMessages =
        std::clamp(settings.obsOverlayMaxMessages.getValue(), 1, 50);

    const QJsonObject config{
        {QStringLiteral("fontFamily"),
         settings.obsOverlayFontFamily.getValue()},
        {QStringLiteral("fontSize"),
         std::clamp(settings.obsOverlayFontSize.getValue(), 8, 96)},
        {QStringLiteral("fontWeight"),
         std::clamp(settings.obsOverlayFontWeight.getValue(), 100, 900)},
        {QStringLiteral("textColor"),
         configuredCssColor(settings.obsOverlayTextColor.getValue(),
                            QColor(Qt::white))},
        {QStringLiteral("backgroundColor"),
         configuredCssColor(settings.obsOverlayBackgroundColor.getValue(),
                            settings.obsOverlayBackgroundOpacity.getValue(),
                            QColor(17, 17, 17))},
        {QStringLiteral("shadowColor"),
         configuredCssColor(settings.obsOverlayShadowColor.getValue(),
                            settings.obsOverlayShadowOpacity.getValue(),
                            QColor(Qt::black))},
        {QStringLiteral("shadowBlur"),
         std::clamp(settings.obsOverlayShadowBlur.getValue(), 0, 24)},
        {QStringLiteral("messageSpacing"),
         std::clamp(settings.obsOverlayMessageSpacing.getValue(), 0, 40)},
        {QStringLiteral("borderRadius"),
         std::clamp(settings.obsOverlayBorderRadius.getValue(), 0, 40)},
        {QStringLiteral("emoteSize"),
         std::clamp(settings.obsOverlayEmoteSize.getValue(), 12, 96)},
        {QStringLiteral("maxMessages"), maxMessages},
        {QStringLiteral("lifetimeSeconds"),
         std::clamp(settings.obsOverlayMessageLifetime.getValue(), 0, 300)},
        {QStringLiteral("fadeDurationMs"),
         std::clamp(settings.obsOverlayFadeDuration.getValue(), 0, 5000)},
        {QStringLiteral("messageAnimations"),
         obsOverlayMessageAnimationsSetting().getValue()},
        {QStringLiteral("newestAtBottom"),
         settings.obsOverlayNewestAtBottom.getValue()},
        {QStringLiteral("useUsernameColors"),
         settings.obsOverlayUseUsernameColors.getValue()},
        {QStringLiteral("showTimestamps"),
         settings.obsOverlayShowTimestamps.getValue()},
        {QStringLiteral("platformStyle"),
         normalizedOverlayPlatformStyle(
             settings.obsOverlayPlatformStyle.getValue())},
        {QStringLiteral("showBadges"),
         settings.obsOverlayShowBadges.getValue()},
        {QStringLiteral("showSevenTVEmotes"),
         settings.obsOverlayShowSevenTVEmotes.getValue()},
        {QStringLiteral("showSevenTVBadges"),
         settings.obsOverlayShowSevenTVBadges.getValue()},
        {QStringLiteral("showSevenTVPaints"),
         settings.obsOverlayShowSevenTVPaints.getValue()},
        {QStringLiteral("showHighlights"),
         settings.obsOverlayShowHighlights.getValue()},
    };

    const auto sourceKey =
        split == nullptr ? QString()
                         : QStringLiteral("%1-%2")
                               .arg(selectedTabIndex)
                               .arg(reinterpret_cast<quintptr>(split), 0, 16);

    QJsonObject root{
        {QStringLiteral("enabled"), enabled},
        {QStringLiteral("ready"), enabled && split != nullptr},
        {QStringLiteral("sourceTabName"), tabName},
        {QStringLiteral("selectedTab"), selectedTabIndex},
        {QStringLiteral("paneTitle"), splitLabel(split)},
        {QStringLiteral("sourceKey"), sourceKey},
        {QStringLiteral("config"), config},
        {QStringLiteral("messages"), QJsonArray{}},
    };

    if (!enabled)
    {
        root.insert(QStringLiteral("emptyMessage"),
                    QStringLiteral("The OBS chat overlay is disabled."));
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }

    if (page == nullptr)
    {
        root.insert(
            QStringLiteral("emptyMessage"),
            QStringLiteral(
                "No chat tab is available. Open a linked account's chat or "
                "choose an existing chat tab in OBS settings."));
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }

    if (split == nullptr)
    {
        root.insert(
            QStringLiteral("emptyMessage"),
            QStringLiteral("This tab does not have a chat split to show."));
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }

    auto &snapshot = split->getChannelView().getMessagesSnapshot();
    QJsonArray messages;
    const auto begin =
        snapshot.size() > OBS_DOCK_MESSAGE_LIMIT
            ? snapshot.end() -
                  static_cast<std::ptrdiff_t>(OBS_DOCK_MESSAGE_LIMIT)
            : snapshot.begin();

    for (auto it = begin; it != snapshot.end(); ++it)
    {
        const auto &layout = *it;
        if (!layout)
        {
            continue;
        }

        const auto *message = layout->getMessage();
        if (message == nullptr)
        {
            continue;
        }
        const bool moderation =
            message->flags.has(MessageFlag::ModerationAction);
        if (!settings.obsOverlayShowModerationMessages.getValue() &&
            moderation)
        {
            continue;
        }
        if (!settings.obsOverlayShowSystemMessages.getValue() &&
            !moderation && isOverlaySystemOrEventMessage(*message))
        {
            continue;
        }

        messages.push_back(serializeBrowserMessage(*message));
        while (messages.size() > maxMessages)
        {
            messages.removeAt(0);
        }
    }

    root.insert(QStringLiteral("messages"), messages);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray ObsBrowserDockServer::dockStateJson(const QString &view,
                                               int requestedTabIndex) const
{
    const auto resolvedView = normalizedDockView(view);
    auto *window = this->dockWindow();
    QJsonArray tabs;
    int selectedTabIndex = -1;

    if (window != nullptr)
    {
        auto &notebook = window->getNotebook();
        selectedTabIndex = normalizedDockTabIndex(
            requestedTabIndex, notebook.getPageCount(),
            notebook.getSelectedIndex());

        for (int i = 0; i < notebook.getPageCount(); ++i)
        {
            auto *page = dynamic_cast<SplitContainer *>(notebook.getPageAt(i));
            if (page == nullptr)
            {
                continue;
            }

            auto *tab = page->getTab();
            QString title = tab != nullptr ? tab->getTitle() : QString();
            if (title.isEmpty())
            {
                title = QStringLiteral("Tab %1").arg(i + 1);
            }

            tabs.push_back(QJsonObject{
                {QStringLiteral("index"), i},
                {QStringLiteral("title"), title},
                {QStringLiteral("platforms"), pagePlatforms(page)},
            });
        }
    }

    auto *page = this->selectedPage(selectedTabIndex);
    auto *split = this->resolveSplit(page, resolvedView);
    const auto currentPlatforms = pagePlatforms(page);

    QJsonObject root{
        {QStringLiteral("view"), resolvedView},
        {QStringLiteral("localUrl"),
         ObsBrowserDockServer::dockUrl(resolvedView, selectedTabIndex)},
        {QStringLiteral("ready"), split != nullptr},
        {QStringLiteral("paneTitle"), splitLabel(split)},
        {QStringLiteral("selectedTab"), selectedTabIndex},
        {QStringLiteral("tabs"), tabs},
        {QStringLiteral("platforms"), currentPlatforms},
        {QStringLiteral("messageCount"), 0},
    };

    if (page == nullptr)
    {
        root.insert(QStringLiteral("emptyMessage"),
                    QStringLiteral("No Mergerino tabs are open in the main window."));
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }

    if (split == nullptr)
    {
        if (resolvedView == QStringLiteral("activity"))
        {
            root.insert(QStringLiteral("emptyMessage"),
                        QStringLiteral("This tab does not have an activity pane."));
        }
        else
        {
            root.insert(QStringLiteral("emptyMessage"),
                        QStringLiteral("This tab does not have a chat split to show."));
        }
        return QJsonDocument(root).toJson(QJsonDocument::Compact);
    }

    if (const auto channel = split->getChannel())
    {
        if (auto *merged = dynamic_cast<MergedChannel *>(channel.get()))
        {
            const auto &config = merged->config();
            QJsonObject youtube{
                {QStringLiteral("enabled"), config.youtubeEnabled},
                {QStringLiteral("source"), config.youtubeStreamUrl},
                {QStringLiteral("present"),
                 merged->youtubeLiveChat() != nullptr},
            };

            if (auto *liveChat = merged->youtubeLiveChat())
            {
                youtube.insert(QStringLiteral("live"), liveChat->isLive());
                youtube.insert(QStringLiteral("videoId"), liveChat->videoId());
                youtube.insert(QStringLiteral("statusText"),
                               liveChat->statusText());
                youtube.insert(QStringLiteral("liveTitle"),
                               liveChat->liveTitle());
                youtube.insert(QStringLiteral("viewerCount"),
                               QString::number(liveChat->liveViewerCount()));
            }

            root.insert(QStringLiteral("youtube"), youtube);
        }
    }

    auto &snapshot = split->getChannelView().getMessagesSnapshot();
    QJsonArray messages;
    const auto begin =
        snapshot.size() > OBS_DOCK_MESSAGE_LIMIT
            ? snapshot.end() -
                  static_cast<std::ptrdiff_t>(OBS_DOCK_MESSAGE_LIMIT)
            : snapshot.begin();

    for (auto it = begin; it != snapshot.end(); ++it)
    {
        const auto &layout = *it;
        if (!layout)
        {
            continue;
        }

        const auto *message = layout->getMessage();
        if (message == nullptr)
        {
            continue;
        }

        messages.push_back(serializeBrowserMessage(*message));
    }

    root.insert(QStringLiteral("messageCount"), messages.size());
    root.insert(QStringLiteral("messages"), messages);

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

}  // namespace chatterino
